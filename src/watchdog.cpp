#include "watchdog.hpp"

namespace mask::safety {

std::atomic<Watchdog*> Watchdog::s_activeWatchdogInstance{nullptr};

Watchdog::Watchdog(mask::core::EngineSharedState& sharedState, SafeStopCallback onSafeStop) noexcept
    : sharedState_(sharedState)
    , onSafeStop_(onSafeStop) {
    s_activeWatchdogInstance.store(this, std::memory_order_release);
    InstallCrashAndConsoleFailSafeHandlers();
}

Watchdog::~Watchdog() noexcept {
    Stop();
    Watchdog* expected = this;
    (void)s_activeWatchdogInstance.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void Watchdog::SetAuxCleanupCallback(AuxCleanupCallback cb) noexcept {
    auxCleanupCallback_.store(cb, std::memory_order_release);
}

void Watchdog::Start() {
    watchdogRunning_.store(true, std::memory_order_release);
    workerThread_ = std::thread([this]() { WatchdogThreadMain(); });
}

void Watchdog::Stop() noexcept {
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

void Watchdog::SafeStop(mask::core::SafeStopReason reason) noexcept {
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

void Watchdog::ExecuteEmergencyRecovery(bool tearDownVirtualControllers) noexcept {
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

void Watchdog::ReleaseAllSyntheticHeldInputs() noexcept {
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

    // Safe synchronous driver-level neutralizer fallback even if the output thread is stalled/stopped.
    mask::bridge::HIDMaestroHost::Instance().ReleaseAllNeutralHardware();
}

void Watchdog::ClearVirtualEchoTokens() noexcept {
    mask::input::InputDispatcher::ClearVirtualEchoTokens(sharedState_);
}

LONG WINAPI Watchdog::UnhandledSehCrashFilter(EXCEPTION_POINTERS* /*exceptionInfo*/) noexcept {
    if (auto* wd = s_activeWatchdogInstance.load(std::memory_order_acquire)) {
        wd->ExecuteEmergencyRecovery(true);
    } else {
        mask::bridge::HIDMaestroHost::Instance().EmergencyTeardownHardware(false);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

BOOL WINAPI Watchdog::ConsoleCtrlFailSafeHandler(DWORD ctrlType) noexcept {
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

void Watchdog::InstallCrashAndConsoleFailSafeHandlers() noexcept {
    SetUnhandledExceptionFilter(UnhandledSehCrashFilter);
    SetConsoleCtrlHandler(ConsoleCtrlFailSafeHandler, TRUE);
}

void Watchdog::WatchdogThreadMain() noexcept {
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

} // namespace mask::safety
