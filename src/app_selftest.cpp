#include "app.hpp"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace mask::app {

int App::RunSelfTest() noexcept {
    model_.ResolveProfilePaths();
    model_.InitializeDefaults();

    seqEngine_.SetToggleMacroCallback(&App::OnToggleMacroSlotFromSeqStatic);
    SyncAllEnginesFromModel();
    model_.RefreshDevicesAndMonitors(&remapEngine_);

    pixelEngine_.Start();
    seqEngine_.Start();
    remapEngine_.Start();

    mask::input::GameInputGlobalContext::perDeviceCallback.store(
        &App::OnPhysicalDeviceTransitionStatic, std::memory_order_release);

    watchdog_ = std::make_unique<mask::safety::Watchdog>(sharedState_, &App::OnWatchdogSafeStopStatic);
    watchdog_->SetAuxCleanupCallback(&App::OnWatchdogAuxCleanupStatic);
    watchdog_->Start();

    outputThreadStop_.store(false, std::memory_order_release);
    std::atomic<bool> outputReady{false};
    outputThread_ = std::thread([this, &outputReady]() noexcept {
        OutputThreadMain(&outputReady);
    });
    while (!outputReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    std::atomic<bool> inputReady{false};
    gameInputThread_ = std::thread([this, &inputReady]() noexcept {
        GameInputThreadMain(&inputReady);
    });
    while (!inputReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    std::atomic<bool> engineReady{false};
    engineThread_ = std::thread([this, &engineReady]() noexcept {
        EngineThreadMain(true, &engineReady);
    });
    while (!engineReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    auto& host = mask::bridge::HIDMaestroHost::Instance();
    host.LoadAndInitialize(L"logitech-g502-hero", L"keychron-q6");
    Sleep(180);

    const mask::core::TelemetrySnapshot snap = sharedState_.ReadTelemetry();
    bool hmLive = false, hmElev = false, hmDrv = false, hmDeliv = false, hmRelExit = false;
    int32_t mIdx = -1, kIdx = -1;
    uint8_t kbLeds = 0;
    host.QueryDriverTelemetry(hmLive, hmElev, hmDrv, mIdx, kIdx, hmDeliv, hmRelExit, kbLeds);

    const bool simdOk     = mask::runtime::MacroRuntime::RunSimdEquivalenceSelfTest();
    const bool seqLogicOk = mask::workflows::ComplexSequenceEngine::RunControlFlowSelfTest();
    double visionAvgUs    = 0.0;
    const bool trigOk     = mask::triggers::PixelTriggerEngine::RunTriggerSelfTest(visionAvgUs);
    const bool remapOk    = mask::remap::RemapEngine::RunRemapSelfTest();

    const bool profMigOk  = mask::app::storage::RunProfileMigrationSelfTest();
    const bool seqMigOk   = mask::workflows::ComplexSequenceEngine::RunSaveMigrationSelfTest();
    const bool trigMigOk  = mask::triggers::PixelTriggerEngine::RunSaveMigrationSelfTest();
    const bool remapMigOk = mask::remap::RemapEngine::RunSaveMigrationSelfTest();
    const bool setMigOk   = mask::settings::RunSettingsMigrationSelfTest();
    const bool jsonOk     = mask::profile_json::RunProfileJsonSelfTest();

    const bool allOk = simdOk && seqLogicOk && trigOk && remapOk &&
                       profMigOk && seqMigOk && trigMigOk && remapMigOk &&
                       setMigOk && jsonOk && (snap.calibratedTscFreqHz > 0ULL);

    std::printf("\nMask self-test\n");
    std::printf("  Timer frequency     : %.4f GHz\n", static_cast<double>(snap.calibratedTscFreqHz) / 1e9);
    std::printf("  Fast path           : %s\n", snap.avx2Bmi2Active ? "AVX2 + BMI2" : "SSE2");
    std::printf("  Microsoft GameInput : %s (%u physical devices)\n",
                sharedState_.gameInputConnected.load(std::memory_order_acquire) ? "active" : "fallback polling",
                static_cast<unsigned>(snap.gameInputDeviceCount));
    std::printf("  Virtual devices     : driver installed=%s | elevated=%s | live=%s\n",
                hmDrv ? "yes" : "no", hmElev ? "yes" : "no", hmLive ? "yes" : "no");
    std::printf("  Bridge status       : %ls\n", host.GetLastStatusMessage());
    std::printf("  Timing error p50    : %llu ns\n", static_cast<unsigned long long>(snap.p50JitterNs));
    std::printf("  Timing error p99    : %llu ns\n", static_cast<unsigned long long>(snap.p99JitterNs));
    std::printf("  Timing error max    : %llu ns\n", static_cast<unsigned long long>(snap.maxJitterNs));
    std::printf("  Jitter histogram    : <100ns=%u | 100-500ns=%u | 500ns-2us=%u | >=2us=%u\n",
                static_cast<unsigned>(snap.jitterHistogram[0]),
                static_cast<unsigned>(snap.jitterHistogram[1]),
                static_cast<unsigned>(snap.jitterHistogram[2]),
                static_cast<unsigned>(snap.jitterHistogram[3]));
    std::printf("  SIMD equivalence    : %s\n", simdOk ? "PASS" : "FAIL");
    std::printf("  Sequence logic      : %s\n", seqLogicOk ? "PASS" : "FAIL");
    std::printf("  Trigger logic       : %s (%.2f us/frame)\n", trigOk ? "PASS" : "FAIL", visionAvgUs);
    std::printf("  Remap logic         : %s\n", remapOk ? "PASS" : "FAIL");
    std::printf("  Profiles migration  : %s\n", profMigOk ? "PASS" : "FAIL");
    std::printf("  Sequences migration : %s\n", seqMigOk ? "PASS" : "FAIL");
    std::printf("  Triggers migration  : %s\n", trigMigOk ? "PASS" : "FAIL");
    std::printf("  Remaps migration    : %s\n", remapMigOk ? "PASS" : "FAIL");
    std::printf("  Settings migration  : %s\n", setMigOk ? "PASS" : "FAIL");
    std::printf("  Profile JSON        : %s\n", jsonOk ? "PASS" : "FAIL");
    std::printf("  Overall             : %s\n", allOk ? "PASS" : "FAIL");
    std::fflush(stdout);

    mask::input::GameInputGlobalContext::perDeviceCallback.store(nullptr, std::memory_order_release);
    sharedState_.engineRunning.store(false, std::memory_order_release);
    mask::input::WakeEngine();
    mask::input::WakeWatchdog();

    pixelEngine_.Stop();
    seqEngine_.Stop();
    remapEngine_.Stop();

    if (engineThread_.joinable()) engineThread_.join();
    if (gameInputThread_.joinable()) gameInputThread_.join();

    outputThreadStop_.store(true, std::memory_order_release);
    mask::input::WakeOutput();
    if (outputThread_.joinable()) outputThread_.join();

    if (watchdog_) {
        watchdog_->ExecuteEmergencyRecovery(false);
        watchdog_->Stop();
        watchdog_.reset();
    }

    return allOk ? 0 : 1;
}

int App::RunBenchmark() noexcept {
    auto& host = mask::bridge::HIDMaestroHost::Instance();
    (void)host.LoadAndInitialize(L"logitech-g502-hero", L"keychron-q6");

    const bool liveHardware = host.AreControllersLive();
    sharedState_.benchDryRunOutput.store(!liveHardware, std::memory_order_release);
    sharedState_.automationPaused.store(false, std::memory_order_release);
    sharedState_.suppressTriggers.store(false, std::memory_order_release);
    sharedState_.captureBindTarget.store(0U, std::memory_order_release);
    sharedState_.outputFailureStreak.store(0U, std::memory_order_release);
    sharedState_.benchCompletedCount.store(0ULL, std::memory_order_release);
    sharedState_.benchTotalBatches.store(0ULL, std::memory_order_release);
    sharedState_.benchLastCompleteTsc.store(0ULL, std::memory_order_release);
    sharedState_.benchLastCompleteToken.store(0ULL, std::memory_order_release);
    sharedState_.engineRunning.store(true, std::memory_order_release);

    for (auto& w : sharedState_.watchedVkBits) {
        w.store(0ULL, std::memory_order_release);
    }

    outputThreadStop_.store(false, std::memory_order_release);
    std::atomic<bool> outputReady{false};
    outputThread_ = std::thread([this, &outputReady]() noexcept {
        OutputThreadMain(&outputReady);
    });
    while (!outputReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    std::atomic<bool> engineReady{false};
    engineThread_ = std::thread([this, &engineReady]() noexcept {
        EngineThreadMain(false, &engineReady);
    });
    while (!engineReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    // Configure Autoclicker slot 0: HoldToLoop triggered by VK_F24 emitting keyboard 'B'
    constexpr uint16_t kBenchTriggerVk = VK_F24;
    MacroConfig benchSlot{};
    benchSlot.slotIndex          = 0U;
    benchSlot.enabled            = true;
    benchSlot.mode               = static_cast<uint8_t>(MacroMode::HoldToLoop);
    benchSlot.triggerVk          = kBenchTriggerVk;
    benchSlot.outputVk           = 'B';
    benchSlot.isMouse            = false;
    benchSlot.dwellDurationNs    = 5'000'000'000ULL; // 5 s so Hold dwells cleanly until release edge
    benchSlot.recoveryDurationNs = 5'000'000ULL;
    benchSlot.reserved[0]        = 1U;

    while (!sharedState_.uiCommandQueue.try_push(benchSlot)) {
        _mm_pause();
    }
    mask::input::WakeEngine();
    while (!sharedState_.uiCommandQueue.empty()) {
        Sleep(1);
    }
    const uint64_t hbSync = sharedState_.heartbeatCounter.load(std::memory_order_acquire);
    while (sharedState_.heartbeatCounter.load(std::memory_order_acquire) == hbSync) {
        mask::input::WakeEngine();
        Sleep(1);
    }

    // ---- Test 1: Input-to-Output Latency (500 measured iterations) -----------------
    constexpr size_t kWarmupIters   = 10;
    constexpr size_t kMeasuredIters = 500;
    std::array<double, kMeasuredIters> latenciesUs{};
    size_t recorded = 0;
    const uint64_t spinTimeoutTsc = timingEngine_.NanosecondsToTsc(2'000'000'000ULL);

    auto waitForCount = [&](uint64_t targetCount) noexcept -> bool {
        const uint64_t deadline = mask::timing::TimingEngine::ReadTsc() + spinTimeoutTsc;
        while (sharedState_.benchCompletedCount.load(std::memory_order_acquire) < targetCount) {
            _mm_pause();
            if (mask::timing::TimingEngine::ReadTsc() >= deadline) {
                return false;
            }
        }
        return true;
    };

    for (size_t iter = 0; iter < kWarmupIters + kMeasuredIters; ++iter) {
        const uint64_t expectedPress = sharedState_.benchCompletedCount.load(std::memory_order_acquire) + 1ULL;
        sharedState_.physicalKeyState[kBenchTriggerVk].store(1U, std::memory_order_release);
        const uint64_t t0 = mask::timing::TimingEngine::ReadTsc();
        const mask::core::InputTriggerEvent downEv{ t0, kBenchTriggerVk, true, {0, 0, 0, 0, 0} };
        while (!sharedState_.inputEventQueue.try_push(downEv)) {
            _mm_pause();
        }
        mask::input::WakeEngine();

        const bool pressOk = waitForCount(expectedPress);
        const uint64_t t1 = sharedState_.benchLastCompleteTsc.load(std::memory_order_acquire);
        if (pressOk && iter >= kWarmupIters && recorded < kMeasuredIters) {
            const uint64_t deltaTsc = (t1 >= t0) ? (t1 - t0) : 0ULL;
            latenciesUs[recorded++] = static_cast<double>(timingEngine_.TscToNanoseconds(deltaTsc)) / 1000.0;
        }

        const uint64_t expectedRel = sharedState_.benchCompletedCount.load(std::memory_order_acquire) + 1ULL;
        sharedState_.physicalKeyState[kBenchTriggerVk].store(0U, std::memory_order_release);
        const mask::core::InputTriggerEvent upEv{
            mask::timing::TimingEngine::ReadTsc(), kBenchTriggerVk, false, {0, 0, 0, 0, 0}
        };
        while (!sharedState_.inputEventQueue.try_push(upEv)) {
            _mm_pause();
        }
        mask::input::WakeEngine();
        (void)waitForCount(expectedRel);
    }

    // Join engine thread cleanly before Test 2 so outputBatchQueue preserves strict SPSC producer ownership.
    sharedState_.engineRunning.store(false, std::memory_order_release);
    mask::input::WakeEngine();
    if (engineThread_.joinable()) {
        engineThread_.join();
    }

    double p50Us = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
    if (recorded > 0) {
        std::sort(latenciesUs.begin(), latenciesUs.begin() + static_cast<ptrdiff_t>(recorded));
        const size_t idx50 = (recorded - 1U) / 2U;
        const size_t idx99 = ((recorded - 1U) * 99U) / 100U;
        p50Us = latenciesUs[idx50];
        p99Us = latenciesUs[idx99];
        maxUs = latenciesUs[recorded - 1U];
    }

    // ---- Test 2: Sustained Max Clicks Per Second (10,000 clicks = 20,000 reports) --
    mask::input::InputDispatcher benchDispatcher(sharedState_, true);
    benchDispatcher.SetBatchQueueMode(true);
    benchDispatcher.SetAutoWake(false);

    auto pushBatchClicks = [&](uint64_t clicks) noexcept {
        for (uint64_t c = 0; c < clicks; ++c) {
            while (!benchDispatcher.EnqueueMouseItem(mask::bridge::hm::MouseBtn_Left, 0, 0, 0, 0, 0ULL)) {
                benchDispatcher.FlushBatchWake();
                _mm_pause();
            }
            while (!benchDispatcher.EnqueueMouseItem(mask::bridge::hm::MouseBtn_None, 0, 0, 0, 0, 0ULL)) {
                benchDispatcher.FlushBatchWake();
                _mm_pause();
            }
        }
        benchDispatcher.FlushBatchWake();
    };

    // Warm up 64 clicks
    const uint64_t warmTarget = sharedState_.benchCompletedCount.load(std::memory_order_acquire) + 128ULL;
    pushBatchClicks(64ULL);
    (void)waitForCount(warmTarget);

    constexpr uint64_t kBenchClicks       = 10'000ULL;
    constexpr uint64_t kBenchTotalReports = kBenchClicks * 2ULL;
    constexpr uint64_t kClicksPerBatch    = 32ULL; // 64 Down+Up OutputBatchItems per batch

    const uint64_t batchesBefore = sharedState_.benchTotalBatches.load(std::memory_order_acquire);
    const uint64_t countBefore   = sharedState_.benchCompletedCount.load(std::memory_order_acquire);
    const uint64_t targetCount   = countBefore + kBenchTotalReports;

    const uint64_t cpsStartTsc = mask::timing::TimingEngine::ReadTsc();
    uint64_t remaining = kBenchClicks;
    while (remaining > 0ULL) {
        const uint64_t chunk = (std::min<uint64_t>)(remaining, kClicksPerBatch);
        pushBatchClicks(chunk);
        remaining -= chunk;
    }
    const bool cpsOk = waitForCount(targetCount);
    const uint64_t cpsEndTsc = mask::timing::TimingEngine::ReadTsc();
    const uint64_t batchesAfter = sharedState_.benchTotalBatches.load(std::memory_order_acquire);

    const uint64_t elapsedNs  = (cpsEndTsc > cpsStartTsc)
        ? timingEngine_.TscToNanoseconds(cpsEndTsc - cpsStartTsc)
        : 1ULL;
    const double elapsedMs    = static_cast<double>(elapsedNs) / 1e6;
    const double elapsedSec   = static_cast<double>(elapsedNs) / 1e9;
    const double clicksPerSec = (elapsedSec > 0.0) ? (static_cast<double>(kBenchClicks) / elapsedSec) : 0.0;
    const double reportsPerSec = (elapsedSec > 0.0) ? (static_cast<double>(kBenchTotalReports) / elapsedSec) : 0.0;

    outputThreadStop_.store(true, std::memory_order_release);
    mask::input::WakeOutput();
    if (outputThread_.joinable()) {
        outputThread_.join();
    }

    // Restore state cleanly
    sharedState_.physicalKeyState[kBenchTriggerVk].store(0U, std::memory_order_release);
    sharedState_.automationPaused.store(false, std::memory_order_release);
    sharedState_.benchDryRunOutput.store(false, std::memory_order_release);
    sharedState_.outputFailureStreak.store(0U, std::memory_order_release);

    std::printf("\nMask benchmark\n");
    std::printf("  Pipeline mode       : %s\n",
                liveHardware
                    ? "[Live HIDMaestro UMDF2 Hardware]"
                    : "[CoreCLR HMBridge_SubmitBatch Dry-Run Pipeline]");
    std::printf("  Bridge status       : %ls\n", host.GetLastStatusMessage());
    std::printf("  Timer frequency     : %.4f GHz\n", static_cast<double>(timingEngine_.GetTscFrequencyHz()) / 1e9);
    std::printf("  Test 1 - Input-to-Output Latency (%zu samples, vk='B'):\n", recorded);
    std::printf("    p50               : %.2f us\n", p50Us);
    std::printf("    p99               : %.2f us\n", p99Us);
    std::printf("    max               : %.2f us\n", maxUs);
    std::printf("  Test 2 - Sustained Max CPS (%llu clicks = %llu HID reports, %llu batches):\n",
                static_cast<unsigned long long>(kBenchClicks),
                static_cast<unsigned long long>(kBenchTotalReports),
                static_cast<unsigned long long>(batchesAfter - batchesBefore));
    std::printf("    Completed         : %s\n", cpsOk ? "yes" : "timeout");
    std::printf("    Elapsed           : %.3f ms\n", elapsedMs);
    std::printf("    Sustained CPS     : %.0f clicks/sec\n", clicksPerSec);
    std::printf("    HID report rate   : %.0f reports/sec\n", reportsPerSec);
    std::fflush(stdout);

    return (recorded == kMeasuredIters && cpsOk) ? 0 : 1;
}

} // namespace mask::app
