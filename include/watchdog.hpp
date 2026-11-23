#pragma once

#include "core_contracts.hpp"
#include "input_pipeline.hpp"
#include "hidmaestro_bridge.hpp"

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <thread>

namespace mask::safety {

/**
 * @brief Crash-Proof Driver-Level Anti-Lockout Watchdog for HIDMaestro v2.1.0 + Microsoft GameInput.
 *
 * Runs on its own high-priority thread, independent of the engine, and guarantees that no synthetic key or
 * button is ever left held down:
 *
 *   1. Panic chord (L-Ctrl + R-Ctrl + Esc): the input thread signals this thread, which reacts immediately
 *      (it waits on an event, it does not poll for it).
 *   2. Engine stall: if the engine heartbeat stops advancing for kStallTimeoutMs.
 *
 *   In both cases it performs a *safe stop*: every output is released with neutral hardware reports,
 *   automation is paused and running macros are flushed. The app itself keeps running, so a hiccup never
 *   costs the user their session; F8 (or the button in the window) resumes.
 *
 *   3. Process crash / console close / shutdown: neutral reports, then the virtual controllers are disposed
 *      (`SetUnhandledExceptionFilter`, `SetConsoleCtrlHandler`).
 */
class Watchdog {
public:
    using SafeStopCallback   = void (*)(mask::core::SafeStopReason reason) noexcept;
    using AuxCleanupCallback = void (*)() noexcept;

    explicit Watchdog(mask::core::EngineSharedState& sharedState, SafeStopCallback onSafeStop = nullptr) noexcept
        : sharedState_(sharedState)
        , onSafeStop_(onSafeStop) {
        s_activeWatchdogInstance.store(this, std::memory_order_release);
        InstallCrashAndConsoleFailSafeHandlers();
    }

    ~Watchdog() noexcept {
        Stop();
        s_activeWatchdogInstance.store(nullptr, std::memory_order_release);
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    /**
     * @brief Registers an optional cleanup hook invoked during SafeStop and ExecuteEmergencyRecovery
     * so auxiliary engines (SuppressionGate, ComplexSequenceEngine, PixelTriggerEngine) neutralize immediately.
     */
    void SetAuxCleanupCallback(AuxCleanupCallback cb) noexcept {
        auxCleanupCallback_.store(cb, std::memory_order_release);
    }

    void Start() {
        watchdogRunning_.store(true, std::memory_order_release);
        workerThread_ = std::thread([this]() { WatchdogThreadMain(); });
    }

    void Stop() noexcept {
        const bool wasRunning = watchdogRunning_.exchange(false, std::memory_order_acq_rel);
        mask::input::WakeWatchdog();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        if (wasRunning) {
            // Always perform a final clean hardware neutral release on shutdown
            ReleaseAllSyntheticHeldInputs();
            ClearVirtualEchoTokens();
        }
    }

    /**
     * @brief Releases every output, pauses automation and flushes running macros. Recoverable: nothing is
     * torn down, and resuming (F8) picks up exactly where the user left their configuration.
     */
    void SafeStop(mask::core::SafeStopReason reason) noexcept {
        sharedState_.automationPaused.store(true, std::memory_order_release);
        sharedState_.flushAllRequested.store(true, std::memory_order_release);
        sharedState_.safeStopReason.store(static_cast<uint8_t>(reason), std::memory_order_release);
        if (const AuxCleanupCallback cleanup = auxCleanupCallback_.load(std::memory_order_acquire)) {
            cleanup();
        }
        ReleaseAllSyntheticHeldInputs();
        mask::input::WakeEngine();
        if (onSafeStop_ != nullptr) {
            onSafeStop_(reason);
        }
    }

    /**
     * @brief Final recovery for process exit or a crash:
     *   1. Stops the Microsoft GameInput reading callback.
     *   2. Invokes registered auxiliary cleanup callback (unhooks SuppressionGate, aborts sequences/triggers).
     *   3. Zeroes all synthetic held states and transmits neutral reports to HIDMaestro virtual controllers.
     *   4. Optionally tears down HIDMaestro virtual controllers and sweeps orphaned devnodes.
     */
    void ExecuteEmergencyRecovery(bool tearDownVirtualControllers = false) noexcept {
        IGameInput* gi = mask::input::GameInputGlobalContext::gameInputInstance.load(std::memory_order_acquire);
        const GameInputCallbackToken tok = mask::input::GameInputGlobalContext::readingToken.exchange(0, std::memory_order_acq_rel);
        if (gi != nullptr && tok != 0) {
            gi->StopCallback(tok);
        }

        if (const AuxCleanupCallback cleanup = auxCleanupCallback_.load(std::memory_order_acquire)) {
            cleanup();
        }

        ReleaseAllSyntheticHeldInputs();

        if (tearDownVirtualControllers) {
            mask::bridge::HIDMaestroHost::Instance().EmergencyTeardownHardware(false);
        }
    }

    /**
     * @brief Clears all 256 `syntheticHeldState` slots, stamps virtual echo UP tokens for any keys
     * currently held so live hooks pass the release through, and commands HIDMaestro v2.1.0 to emit
     * zeroed neutral reports on both the virtual mouse and virtual keyboard.
     */
    void ReleaseAllSyntheticHeldInputs() noexcept {
        for (uint16_t vk = 0; vk < 256; ++vk) {
            if (sharedState_.syntheticHeldState[vk].exchange(0U, std::memory_order_acq_rel) != 0U) {
                mask::input::InputDispatcher::StampVirtualEchoToken(sharedState_, vk, false);
            }
        }
        for (auto& w : sharedState_.syntheticKeyBits) {
            w.store(0ULL, std::memory_order_release);
        }
        sharedState_.activeMouseButtonMask.store(0U, std::memory_order_release);
        sharedState_.activeKeyModifierMask.store(0U, std::memory_order_release);

        mask::bridge::HIDMaestroHost::Instance().ReleaseAllNeutralHardware();
    }

    /**
     * @brief Resets all per-VK virtual echo token counters and timestamps cleanly on shutdown.
     */
    void ClearVirtualEchoTokens() noexcept {
        mask::input::InputDispatcher::ClearVirtualEchoTokens(sharedState_);
    }

private:
    static constexpr DWORD kTickMs         = 25;  // heartbeat sampling period
    static constexpr DWORD kStallTimeoutMs = 400; // engine silence that counts as a stall

    inline static std::atomic<Watchdog*> s_activeWatchdogInstance{nullptr};

    static LONG WINAPI UnhandledSehCrashFilter(EXCEPTION_POINTERS* /*exceptionInfo*/) noexcept {
        if (auto* wd = s_activeWatchdogInstance.load(std::memory_order_acquire)) {
            wd->ExecuteEmergencyRecovery(true);
        } else {
            mask::bridge::HIDMaestroHost::Instance().EmergencyTeardownHardware(false);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    static BOOL WINAPI ConsoleCtrlFailSafeHandler(DWORD ctrlType) noexcept {
        switch (ctrlType) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                if (auto* wd = s_activeWatchdogInstance.load(std::memory_order_acquire)) {
                    wd->sharedState_.engineRunning.store(false, std::memory_order_release);
                    mask::input::WakeEngine();
                    wd->ExecuteEmergencyRecovery(true);
                } else {
                    mask::bridge::HIDMaestroHost::Instance().EmergencyTeardownHardware(false);
                }
                return FALSE;
            default:
                return FALSE;
        }
    }

    static void InstallCrashAndConsoleFailSafeHandlers() noexcept {
        SetUnhandledExceptionFilter(UnhandledSehCrashFilter);
        SetConsoleCtrlHandler(ConsoleCtrlFailSafeHandler, TRUE);
    }

    void WatchdogThreadMain() noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        const HANDLE wakeEvent = mask::input::GameInputGlobalContext::watchdogWakeEvent;

        uint64_t lastHeartbeat = sharedState_.heartbeatCounter.load(std::memory_order_acquire);
        DWORD    silentTicks = 0;     // consecutive full ticks without engine progress
        bool     stallReported = false;

        while (watchdogRunning_.load(std::memory_order_acquire) &&
               sharedState_.engineRunning.load(std::memory_order_acquire)) {

            // Sleeps until the next tick, or returns at once when the panic chord (or shutdown) signals.
            bool fullTick = true;
            if (wakeEvent != nullptr) {
                fullTick = (WaitForSingleObject(wakeEvent, kTickMs) == WAIT_TIMEOUT);
            } else {
                Sleep(kTickMs);
            }

            // 1. Physical Emergency Panic Chord (L-Ctrl + R-Ctrl + Esc)
            if (sharedState_.emergencyPanicTriggered.exchange(false, std::memory_order_acq_rel)) {
                SafeStop(mask::core::SafeStopReason::PanicChord);
                continue;
            }

            // 2. Real-Time Engine Heartbeat Progression. Counted in ticks this thread actually waited
            //    through (not wall-clock time), so waking from sleep/hibernate is never mistaken for a stall.
            const uint64_t currentHeartbeat = sharedState_.heartbeatCounter.load(std::memory_order_acquire);
            if (currentHeartbeat != lastHeartbeat) {
                lastHeartbeat = currentHeartbeat;
                silentTicks = 0;
                stallReported = false;
            } else if (currentHeartbeat > 0 && fullTick) {
                ++silentTicks;
                if (!stallReported && silentTicks * kTickMs >= kStallTimeoutMs) {
                    stallReported = true; // one safe stop per stall, not one per tick
                    SafeStop(mask::core::SafeStopReason::EngineStall);
                }
            }
        }
    }

    mask::core::EngineSharedState&  sharedState_;
    SafeStopCallback                onSafeStop_{nullptr};
    std::atomic<AuxCleanupCallback> auxCleanupCallback_{nullptr};
    std::atomic<bool>               watchdogRunning_{false};
    std::thread                     workerThread_{};
};

} // namespace mask::safety
