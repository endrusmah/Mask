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
 *   1. Panic chord (L-Ctrl + R-Ctrl + Esc): the input thread signals this thread, which reacts immediately.
 *   2. Engine stall: if the engine heartbeat stops advancing for kStallTimeoutMs.
 *   3. Process crash / console close / shutdown: neutral reports, then virtual controllers are disposed.
 */
class Watchdog {
public:
    using SafeStopCallback   = void (*)(mask::core::SafeStopReason reason) noexcept;
    using AuxCleanupCallback = void (*)() noexcept;

    explicit Watchdog(mask::core::EngineSharedState& sharedState, SafeStopCallback onSafeStop = nullptr) noexcept;
    ~Watchdog() noexcept;

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    void SetAuxCleanupCallback(AuxCleanupCallback cb) noexcept;

    void Start();
    void Stop() noexcept;

    void SafeStop(mask::core::SafeStopReason reason) noexcept;
    void ExecuteEmergencyRecovery(bool tearDownVirtualControllers = false) noexcept;
    void ReleaseAllSyntheticHeldInputs() noexcept;
    void ClearVirtualEchoTokens() noexcept;

private:
    static constexpr DWORD kTickMs         = 25;  // heartbeat sampling period
    static constexpr DWORD kStallTimeoutMs = 400; // engine silence that counts as a stall

    static std::atomic<Watchdog*> s_activeWatchdogInstance;

    static LONG WINAPI UnhandledSehCrashFilter(EXCEPTION_POINTERS* exceptionInfo) noexcept;
    static BOOL WINAPI ConsoleCtrlFailSafeHandler(DWORD ctrlType) noexcept;
    static void InstallCrashAndConsoleFailSafeHandlers() noexcept;

    void WatchdogThreadMain() noexcept;

    mask::core::EngineSharedState&  sharedState_;
    SafeStopCallback                onSafeStop_{nullptr};
    std::atomic<AuxCleanupCallback> auxCleanupCallback_{nullptr};
    std::atomic<bool>               watchdogRunning_{false};
    std::thread                     workerThread_{};
};

} // namespace mask::safety
