#include "app.hpp"

#include <GameInput.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <limits>

namespace mask::app {

void App::RunStartupPacingSelfTest() noexcept {
    const uint64_t stepTsc = timingEngine_.NanosecondsToTsc(250'000ULL);
    uint64_t targetTsc = mask::timing::TimingEngine::ReadTsc() + stepTsc;
    for (int i = 0; i < 20; ++i) {
        const uint64_t actualTsc = timingEngine_.WaitUntilTsc(targetTsc);
        timingEngine_.RecordJitterSampleTsc(targetTsc, actualTsc);
        targetTsc = actualTsc + stepTsc;
    }
}

void App::EngineThreadMain(bool selfTest, std::atomic<bool>* engineReadyFlag) noexcept {
    timingEngine_.BoostCurrentThreadRealtimePriority();

    mask::input::GameInputGlobalContext::echoSuppressionWindowTsc.store(
        timingEngine_.NanosecondsToTsc(2'000'000ULL), std::memory_order_release);

    mask::input::InputDispatcher dispatcher(sharedState_, true);
    dispatcher.SetBatchQueueMode(true);
    dispatcher.SetAutoWake(false);

    mask::runtime::MacroRuntime runtime(sharedState_, timingEngine_, dispatcher);
    runtime.SetComplexSeqTriggerCallback(&App::OnTriggerComplexSeqStatic);

    uint32_t lastSpinPolicy = sharedState_.spinPolicy.load(std::memory_order_relaxed);
    timingEngine_.SetSpinPolicy(lastSpinPolicy);

    RunStartupPacingSelfTest();

    if (engineReadyFlag != nullptr) {
        engineReadyFlag->store(true, std::memory_order_release);
    }

    const HANDLE wakeEvent = mask::input::GameInputGlobalContext::engineWakeEvent;
    constexpr DWORD    kIdleHeartbeatMs     = 10;
    constexpr uint64_t kMaxSliceNs          = 10'000'000ULL;
    const uint64_t     periodicTelemetryTsc = timingEngine_.NanosecondsToTsc(100'000'000ULL);

    uint64_t loopCounter    = 0;
    uint64_t lastActive     = ~0ULL;
    uint64_t lastToggled    = ~0ULL;
    bool     lastPaused     = false;
    uint8_t  lastLayer      = 0U;
    bool     lastUnlocked   = true;
    uint32_t lastDevCount   = ~0U;
    uint64_t lastDispatched = ~0ULL;
    uint64_t lastPublishTsc = 0;
    bool     firstPublish   = true;

    while (sharedState_.engineRunning.load(std::memory_order_acquire)) {
        sharedState_.heartbeatCounter.fetch_add(1ULL, std::memory_order_release);

        const uint32_t sp = sharedState_.spinPolicy.load(std::memory_order_relaxed);
        if (sp != lastSpinPolicy) {
            lastSpinPolicy = sp;
            timingEngine_.SetSpinPolicy(sp);
        }

        const uint64_t nowTsc = mask::timing::TimingEngine::ReadTsc();
        runtime.ProcessPendingQueues(nowTsc);
        runtime.EvaluateAndExecute(nowTsc);

        // Flush all transitions emitted in this tick to the dedicated output thread in a single wake.
        dispatcher.FlushBatchWake();
        ++loopCounter;

        const uint64_t active      = runtime.GetActiveSlotBitmask();
        const uint64_t toggled     = runtime.GetToggledSlotBitmask();
        const bool     paused      = sharedState_.automationPaused.load(std::memory_order_relaxed);
        const uint8_t  layer       = sharedState_.activeRemapLayer.load(std::memory_order_relaxed);
        const bool     unlocked    = sharedState_.remapUnlocked.load(std::memory_order_relaxed);
        const uint32_t devCount    = mask::input::GameInputGlobalContext::activePhysicalDeviceCount.load(std::memory_order_relaxed);
        const uint64_t dispatched  = dispatcher.GetTotalDispatched();
        const bool     hwOpen      = settingsHwTabOpen_.load(std::memory_order_relaxed);
        const bool     periodicDue = (nowTsc - lastPublishTsc >= periodicTelemetryTsc) &&
                                     (hwOpen || (active != 0ULL && dispatched != lastDispatched));

        if (firstPublish || active != lastActive || toggled != lastToggled || paused != lastPaused ||
            layer != lastLayer || unlocked != lastUnlocked ||
            devCount != lastDevCount || periodicDue || (selfTest && (loopCounter & 0x3FULL) == 0ULL)) {
            mask::core::TelemetrySnapshot snap{};
            timingEngine_.ComputeJitterPercentiles(snap.p50JitterNs, snap.p99JitterNs, snap.maxJitterNs);
            timingEngine_.GetJitterHistogram(snap.jitterHistogram);
            snap.calibratedTscFreqHz      = timingEngine_.GetTscFrequencyHz();
            snap.totalEventsDispatched    = dispatched;
            snap.totalHookEventsProcessed = runtime.GetTotalHookEventsProcessed();
            snap.engineLoopIterations     = loopCounter;
            snap.activeSlotBitmask        = active;
            snap.toggledSlotBitmask       = toggled;
            snap.activeSlotCount          = runtime.GetActiveSlotCount();
            snap.avx2Bmi2Active           = runtime.HasAvx2Bmi2();
            snap.gameInputDeviceCount     = devCount;
            sharedState_.PublishTelemetry(snap);

            firstPublish   = false;
            lastActive     = active;
            lastToggled    = toggled;
            lastPaused     = paused;
            lastLayer      = layer;
            lastUnlocked   = unlocked;
            lastDevCount   = devCount;
            lastDispatched = dispatched;
            lastPublishTsc = nowTsc;
            NotifyUi();
        }

        if (!sharedState_.inputEventQueue.empty() || !sharedState_.uiCommandQueue.empty() ||
            !sharedState_.auxOutputQueue.empty()) {
            continue;
        }

        const uint64_t earliest = runtime.GetEarliestActiveDeadlineTsc();
        if (earliest == (std::numeric_limits<uint64_t>::max)()) {
            WaitForSingleObject(wakeEvent, kIdleHeartbeatMs);
        } else {
            (void)timingEngine_.WaitUntilTscOrEvent(earliest, wakeEvent, kMaxSliceNs);
        }
    }

    runtime.AbortAllActiveSlots();
    dispatcher.FlushBatchWake();
    timingEngine_.RevertThreadRealtimePriority();
}

void App::OutputThreadMain(std::atomic<bool>* outputReadyFlag) noexcept {
    HANDLE mmcssHandle = mask::timing::TimingEngine::BoostThreadToProAudio();

    if (outputReadyFlag != nullptr) {
        outputReadyFlag->store(true, std::memory_order_release);
    }

    const HANDLE wakeEvent = mask::input::GameInputGlobalContext::outputWakeEvent;
    auto& host = mask::bridge::HIDMaestroHost::Instance();
    std::array<mask::core::OutputBatchItem, 64> batch{};

    for (;;) {
        int32_t count = 0;
        uint64_t highestBenchToken = 0ULL;
        while (count < static_cast<int32_t>(batch.size()) &&
               sharedState_.outputBatchQueue.try_pop(batch[static_cast<size_t>(count)])) {
            const uint64_t tok = batch[static_cast<size_t>(count)].benchToken;
            if (tok > highestBenchToken) {
                highestBenchToken = tok;
            }
            ++count;
        }

        if (count > 0) {
            const bool dryRun = sharedState_.benchDryRunOutput.load(std::memory_order_relaxed);
            const bool ok = host.SubmitBatchHardware(batch.data(), count, dryRun);
            const uint64_t doneTsc = mask::timing::TimingEngine::ReadTsc();

            if (!dryRun) {
                if (ok) {
                    if (sharedState_.outputFailureStreak.load(std::memory_order_relaxed) != 0U) {
                        sharedState_.outputFailureStreak.store(0U, std::memory_order_relaxed);
                    }
                } else {
                    const uint32_t streak = sharedState_.outputFailureStreak.load(std::memory_order_relaxed);
                    if (streak < 1'000'000U) {
                        sharedState_.outputFailureStreak.store(streak + 1U, std::memory_order_relaxed);
                    }
                }
            }

            sharedState_.benchLastCompleteTsc.store(doneTsc, std::memory_order_release);
            if (highestBenchToken != 0ULL) {
                sharedState_.benchLastCompleteToken.store(highestBenchToken, std::memory_order_release);
            }
            sharedState_.benchTotalBatches.fetch_add(1ULL, std::memory_order_release);
            sharedState_.benchCompletedCount.fetch_add(static_cast<uint64_t>(count), std::memory_order_release);
            continue;
        }

        if (outputThreadStop_.load(std::memory_order_acquire) &&
            sharedState_.outputBatchQueue.empty()) {
            break;
        }

        if (wakeEvent != nullptr) {
            // Double-check emptiness before blocking so no wakeup between pop and wait is missed.
            if (sharedState_.outputBatchQueue.empty()) {
                WaitForSingleObject(wakeEvent, 100U);
            }
        } else {
            Sleep(1);
        }
    }

    mask::timing::TimingEngine::RevertThreadProAudio(mmcssHandle);
}

void App::GameInputThreadMain(std::atomic<bool>* inputReadyFlag) noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    IGameInput* gameInput = nullptr;
    const HRESULT hr = GameInputCreate(&gameInput);
    if (SUCCEEDED(hr) && gameInput != nullptr) {
        gameInput->SetFocusPolicy(GameInputDefaultFocusPolicy);
        mask::input::GameInputGlobalContext::gameInputInstance.store(gameInput, std::memory_order_release);
        sharedState_.gameInputConnected.store(true, std::memory_order_release);

        GameInputCallbackToken token = 0;
        const HRESULT cbHr = gameInput->RegisterReadingCallback(
            nullptr,
            GameInputKindKeyboard | GameInputKindMouse,
            0.0f,
            nullptr,
            mask::input::OnGameInputReadingCallback,
            &token
        );
        if (SUCCEEDED(cbHr)) {
            mask::input::GameInputGlobalContext::readingToken.store(token, std::memory_order_release);
        }
    }

    if (inputReadyFlag != nullptr) {
        inputReadyFlag->store(true, std::memory_order_release);
    }

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    std::array<uint8_t, 256> polledState{};
    uint32_t armedCaptureSerial = 0;

    while (sharedState_.engineRunning.load(std::memory_order_acquire)) {
        if (gameInput != nullptr) {
            IGameInputReading* kbReading = nullptr;
            if (SUCCEEDED(gameInput->GetCurrentReading(GameInputKindKeyboard, nullptr, &kbReading)) && kbReading != nullptr) {
                mask::input::ProcessGameInputReading(kbReading);
                kbReading->Release();
            }
            IGameInputReading* msReading = nullptr;
            if (SUCCEEDED(gameInput->GetCurrentReading(GameInputKindMouse, nullptr, &msReading)) && msReading != nullptr) {
                mask::input::ProcessGameInputReading(msReading);
                msReading->Release();
            }
        }

        unsigned int aux = 0;
        const uint64_t nowTsc = __rdtscp(&aux);
        const uint8_t bindTarget = sharedState_.captureBindTarget.load(std::memory_order_relaxed);

        if (bindTarget != 0U) {
            const uint32_t serial = sharedState_.captureSerial.load(std::memory_order_acquire);
            if (serial != armedCaptureSerial) {
                armedCaptureSerial = serial;
                for (uint16_t vk = 1; vk < 255; ++vk) {
                    polledState[vk] = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0 ? 1U : 0U;
                }
            } else {
                for (uint16_t vk = 1; vk < 255; ++vk) {
                    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                        mask::input::InputDispatcher::IsWheelVirtualKey(vk)) {
                        continue;
                    }
                    if (sharedState_.syntheticHeldState[vk].load(std::memory_order_relaxed) != 0U) {
                        polledState[vk] = 1U;
                        continue;
                    }
                    const uint8_t cur = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0 ? 1U : 0U;
                    if (cur != polledState[vk]) {
                        polledState[vk] = cur;
                        if (cur != 0U) {
                            const bool isMouse = mask::input::InputDispatcher::IsMouseVirtualKey(vk);
                            mask::input::UpdatePhysicalAndEnqueueTransition(&sharedState_, vk, true, nowTsc, isMouse);
                            break;
                        }
                    }
                }
            }
        } else {
            for (unsigned word = 0; word < 4; ++word) {
                uint64_t bits = sharedState_.watchedVkBits[word].load(std::memory_order_relaxed);
                while (bits != 0ULL) {
                    unsigned long index = 0;
                    _BitScanForward64(&index, bits);
                    bits &= (bits - 1ULL);
                    const uint8_t bvk = static_cast<uint8_t>(word * 64U + index);
                    if (sharedState_.syntheticHeldState[bvk].load(std::memory_order_relaxed) != 0U) {
                        continue;
                    }
                    const bool down = (GetAsyncKeyState(static_cast<int>(bvk)) & 0x8000) != 0;
                    const uint8_t cur = down ? 1U : 0U;
                    const uint8_t prev = sharedState_.physicalKeyState[bvk].load(std::memory_order_relaxed);
                    if (cur != prev) {
                        const bool isMouse = mask::input::InputDispatcher::IsMouseVirtualKey(bvk);
                        mask::input::UpdatePhysicalAndEnqueueTransition(&sharedState_, bvk, down, nowTsc, isMouse);
                        seqEngine_.NotifyPhysicalKeyTransition(bvk, down);
                    }
                    polledState[bvk] = cur;
                }
            }
        }

        const bool armed = bindTarget != 0U ||
                           (sharedState_.anyMacroEnabled.load(std::memory_order_relaxed) &&
                            !sharedState_.automationPaused.load(std::memory_order_relaxed));
        const uint32_t armedUs = std::clamp(sharedState_.armedPollIntervalUs.load(std::memory_order_relaxed), 500U, 10'000U);
        const uint32_t sleepUs = armed ? armedUs : 4000U;
        if (timer != nullptr) {
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -static_cast<LONGLONG>(sleepUs) * 10LL;
            SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, (std::max<DWORD>)(2, sleepUs / 500U));
        } else {
            Sleep((std::max<DWORD>)(1, sleepUs / 1000U));
        }
    }

    if (timer != nullptr) {
        CloseHandle(timer);
    }

    if (gameInput != nullptr) {
        const GameInputCallbackToken tok = mask::input::GameInputGlobalContext::readingToken.exchange(0, std::memory_order_acq_rel);
        if (tok != 0) {
            gameInput->UnregisterCallback(tok, 50'000ULL);
        }
        mask::input::GameInputGlobalContext::gameInputInstance.store(nullptr, std::memory_order_release);
        gameInput->Release();
    }
}

} // namespace mask::app
