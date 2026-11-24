#include "macro_runtime.hpp"

#include <emmintrin.h> // Baseline x64 SSE2 only (zero AVX2 / BMI1 instructions)
#include <intrin.h>
#include <algorithm>

namespace mask::runtime {

namespace {

[[nodiscard]] inline uint32_t PopCount64(uint64_t x) noexcept {
    x -= (x >> 1) & 0x5555555555555555ULL;
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<uint32_t>((x * 0x0101010101010101ULL) >> 56);
}

[[nodiscard]] bool DetectAvx2Bmi2Support() noexcept {
    int cpuInfo[4]{0, 0, 0, 0};
    __cpuid(cpuInfo, 0);
    const int maxLeaf = cpuInfo[0];

    bool osUsesXSaveXRestor = false;
    if (maxLeaf >= 1) {
        __cpuid(cpuInfo, 1);
        osUsesXSaveXRestor = (cpuInfo[2] & (1 << 27)) != 0; // OSXSAVE
        const bool avxSupported = (cpuInfo[2] & (1 << 28)) != 0;
        if (osUsesXSaveXRestor && avxSupported) {
            const unsigned long long xcrFeatureMask = _xgetbv(_XCR_XFEATURE_ENABLED_MASK);
            osUsesXSaveXRestor = ((xcrFeatureMask & 0x6ULL) == 0x6ULL);
        }
    }

    if (maxLeaf >= 7 && osUsesXSaveXRestor) {
        __cpuidex(cpuInfo, 7, 0);
        const bool hasAvx2 = (cpuInfo[1] & (1 << 5)) != 0; // EBX bit 5: AVX2
        const bool hasBmi2 = (cpuInfo[1] & (1 << 8)) != 0; // EBX bit 8: BMI2
        const bool hasBmi1 = (cpuInfo[1] & (1 << 3)) != 0; // EBX bit 3: BMI1
        return (hasAvx2 && hasBmi2 && hasBmi1);
    }
    return false;
}

/**
 * @brief SSE2 baseline evaluator across active 2-slot pairs for CPUs without AVX2.
 * Performs exact unsigned 64-bit `currentTsc >= deadlines[i]` by flipping bit 31 of both
 * 32-bit halves (`0x8000000080000000`) and computing `!(dead > now)`.
 */
[[nodiscard]] uint64_t EvaluateExpiredMaskSse2Impl(
    const uint64_t* deadlines,
    uint64_t activeMask,
    uint64_t currentTsc
) noexcept {
    if (activeMask == 0ULL || deadlines == nullptr) {
        return 0ULL;
    }

    const __m128i vSignFlip   = _mm_set1_epi64x(static_cast<long long>(0x8000000080000000ULL));
    const __m128i vNowFlipped = _mm_xor_si128(
        _mm_set1_epi64x(static_cast<long long>(currentTsc)),
        vSignFlip);

    auto evalPair = [&](size_t pair) noexcept -> uint64_t {
        const __m128i vDead        = _mm_loadu_si128(reinterpret_cast<const __m128i*>(deadlines + (pair * 2)));
        const __m128i vDeadFlipped = _mm_xor_si128(vDead, vSignFlip);

        const __m128i cmp32       = _mm_cmpgt_epi32(vDeadFlipped, vNowFlipped);
        const __m128i eq32        = _mm_cmpeq_epi32(vDeadFlipped, vNowFlipped);
        const __m128i cmpLow      = _mm_shuffle_epi32(cmp32, _MM_SHUFFLE(2, 2, 0, 0));
        const __m128i cmpHi       = _mm_shuffle_epi32(cmp32, _MM_SHUFFLE(3, 3, 1, 1));
        const __m128i eqHi        = _mm_shuffle_epi32(eq32,  _MM_SHUFFLE(3, 3, 1, 1));
        const __m128i deadGtNow64 = _mm_or_si128(cmpHi, _mm_and_si128(eqHi, cmpLow));

        const uint32_t gtMask2 = static_cast<uint32_t>(_mm_movemask_pd(_mm_castsi128_pd(deadGtNow64))) & 0x3U;
        const uint32_t geMask2 = (~gtMask2) & 0x3U;
        return static_cast<uint64_t>(geMask2) << (pair * 2);
    };

    if ((activeMask >> 2) == 0ULL) {
        return evalPair(0) & activeMask;
    }

    uint64_t expiredMask = 0ULL;
    for (size_t pair = 0; pair < 32; ++pair) {
        if (((activeMask >> (pair * 2)) & 0x3ULL) == 0ULL) {
            continue;
        }
        expiredMask |= evalPair(pair);
    }

    return expiredMask & activeMask;
}

} // namespace

MacroRuntime::MacroRuntime(
    mask::core::EngineSharedState& sharedState,
    mask::timing::TimingEngine& timingEngine,
    mask::input::InputDispatcher& dispatcher
) noexcept
    : sharedState_(sharedState)
    , timingEngine_(timingEngine)
    , dispatcher_(dispatcher) {
    DetectCpuCapabilities();
    unsigned int aux = 0;
    const uint64_t seed = __rdtscp(&aux) ^ reinterpret_cast<uintptr_t>(this);
    prngState_ = (seed != 0ULL) ? seed : 0x9E3779B97F4A7C15ULL;
    minPulseFloorTsc_ = timingEngine_.NanosecondsToTsc(mask::core::kMinimumPulseFloorNs);
    ResetArena();
}

void MacroRuntime::ResetArena() noexcept {
    minPulseFloorTsc_ = timingEngine_.NanosecondsToTsc(mask::core::kMinimumPulseFloorNs);
    arena_.nextDeadlineTsc.fill(kNoDeadline);
    arena_.dwellDurationTsc.fill(0);
    arena_.recoveryDurationTsc.fill(0);
    arena_.phase.fill(mask::core::SlotPhase::IDLE);
    arena_.triggerVk.fill(0);
    arena_.outputVk.fill(0);
    arena_.activeDispatchedVk.fill(0);
    arena_.activeDispatchedIsMouse.fill(0);
    arena_.mode.fill(0);
    arena_.isMouse.fill(0);
    arena_.mouseDeltaX.fill(0);
    arena_.mouseDeltaY.fill(0);
    arena_.targetRepeatCount.fill(1);
    arena_.remainingRepeats.fill(0);
    arena_.sequenceStepIndex.fill(0);
    arena_.sequenceStepCount.fill(4);
    arena_.humanizePct.fill(0);
    arena_.socdAssertedVk.fill(0);
    arena_.configuredMask = 0;
    arena_.activeMask = 0;
    arena_.toggleMask = 0;
    auxTimedReleases_.fill(AuxTimedRelease{});
    auxTimedActiveMask_ = 0U;
}

void MacroRuntime::SetComplexSeqTriggerCallback(ComplexSeqTriggerCallback cb) noexcept {
    complexSeqTriggerCallback_.store(cb, std::memory_order_release);
}

void MacroRuntime::DetectCpuCapabilities() noexcept {
    hasAvx2Bmi2_ = DetectAvx2Bmi2Support();
}

bool MacroRuntime::ConfigureSlot(const mask::core::MacroConfig& cfg) noexcept {
    const size_t slot = cfg.slotIndex;
    if (slot >= mask::core::kMaxMacroSlots) {
        return false;
    }

    const uint64_t bit = (1ULL << slot);
    const bool wasConfigured = (arena_.configuredMask & bit) != 0ULL;
    const bool nowConfigured = cfg.enabled && cfg.triggerVk != 0;
    const uint8_t newMode = static_cast<uint8_t>((std::min<unsigned>)(cfg.mode, 4U));
    const bool isSocd = static_cast<mask::core::MacroMode>(newMode) == mask::core::MacroMode::SocdPair;

    const bool updateInPlace =
        wasConfigured && nowConfigured && !isSocd &&
        arena_.mode[slot] == newMode &&
        arena_.triggerVk[slot] == static_cast<uint16_t>(cfg.triggerVk & 0xFFU);

    if (!updateInPlace) {
        StopAndReleaseSlot(slot);
        ReleaseSocdSlot(slot);
    }

    if (!nowConfigured) {
        arena_.configuredMask &= ~bit;
        arena_.humanizePct[slot] = 0;
        return true;
    }

    // Unrestricted timing & repeatCount: honor raw dwellDurationNs, recoveryDurationNs, and repeatCount >= 1
    const uint16_t outVk = static_cast<uint16_t>(cfg.outputVk & 0xFFU);
    arena_.dwellDurationTsc[slot]    = timingEngine_.NanosecondsToTsc(mask::core::ClampPressNs(cfg.dwellDurationNs));
    arena_.recoveryDurationTsc[slot] = timingEngine_.NanosecondsToTsc(mask::core::ClampReleaseNs(cfg.recoveryDurationNs));
    arena_.triggerVk[slot]           = static_cast<uint16_t>(cfg.triggerVk & 0xFFU);
    arena_.outputVk[slot]            = outVk;
    arena_.mode[slot]                = newMode;
    arena_.isMouse[slot]             = mask::input::InputDispatcher::IsMouseVirtualKey(outVk) ? 1U : 0U;
    arena_.mouseDeltaX[slot]         = cfg.mouseDeltaX;
    arena_.mouseDeltaY[slot]         = cfg.mouseDeltaY;
    arena_.targetRepeatCount[slot]   = (std::max<uint32_t>)(cfg.repeatCount, 1U);
    arena_.sequenceStepCount[slot]   = std::clamp<uint8_t>(cfg.stepCount, 1U, static_cast<uint8_t>(mask::core::kMaxSequenceSteps));
    arena_.humanizePct[slot]         = (std::min<uint8_t>)(mask::core::HumanizePercent(cfg), uint8_t{50});

    for (size_t s = 0; s < mask::core::kMaxSequenceSteps; ++s) {
        const auto& stepCfg = cfg.steps[s];
        auto& step = arena_.sequenceSteps[slot][s];
        const uint16_t stepVk = static_cast<uint16_t>(stepCfg.outputVk & 0xFFU);
        step.dwellTsc    = timingEngine_.NanosecondsToTsc(mask::core::ClampPressNs(stepCfg.dwellDurationNs));
        step.recoveryTsc = timingEngine_.NanosecondsToTsc(mask::core::ClampReleaseNs(stepCfg.recoveryDurationNs));
        step.outputVk    = stepVk;
        step.isMouse     = mask::input::InputDispatcher::IsMouseVirtualKey(stepVk) ? 1U : 0U;
        step.enabled     = stepCfg.enabled ? 1U : 0U;
    }

    if (updateInPlace) {
        if ((arena_.activeMask & bit) != 0ULL && arena_.nextDeadlineTsc[slot] >= kNoDeadline) {
            arena_.nextDeadlineTsc[slot] = mask::timing::TimingEngine::ReadTsc();
        }
        arena_.remainingRepeats[slot] = (std::min)(arena_.remainingRepeats[slot], arena_.targetRepeatCount[slot]);
        if (arena_.sequenceStepIndex[slot] >= arena_.sequenceStepCount[slot]) {
            arena_.sequenceStepIndex[slot] = 0;
        }
    } else {
        arena_.remainingRepeats[slot]  = 0;
        arena_.sequenceStepIndex[slot] = 0;
    }

    arena_.configuredMask |= bit;
    return true;
}

void MacroRuntime::ProcessPendingQueues(uint64_t currentTsc) noexcept {
    // Check if UI requested a non-destructive soft flush of all active macros
    if (sharedState_.flushAllRequested.exchange(false, std::memory_order_acq_rel)) {
        AbortAllActiveSlots();
    }

    // 1. Drain UI command queue
    mask::core::MacroConfig cmd{};
    while (sharedState_.uiCommandQueue.try_pop(cmd)) {
        ConfigureSlot(cmd);
    }

    bool paused = sharedState_.automationPaused.load(std::memory_order_relaxed);
    const bool capturingBind = (sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U);
    const bool suppressed = TriggersSuppressed();

    // The user started typing in the app or binding a key: let go of everything that follows a held
    // physical key, so a held virtual key cannot auto-repeat into the text field.
    if (suppressed && !wasSuppressed_) {
        ReleaseTriggerFollowingSlots();
    }
    wasSuppressed_ = suppressed;

    // 2. Drain physical trigger events
    const uint16_t pauseVk   = sharedState_.globalPauseVk.load(std::memory_order_relaxed);
    const uint16_t stopAllVk = sharedState_.globalStopAllVk.load(std::memory_order_relaxed);
    mask::core::InputTriggerEvent ev{};
    while (sharedState_.inputEventQueue.try_pop(ev)) {
        ++totalHookEventsProcessed_;

        // Configurable global pause hotkey toggles automation pause state cleanly (unless binding a key)
        if (pauseVk != 0U && !capturingBind && ev.isDown && ev.vkCode == pauseVk) {
            const bool newPaused = !sharedState_.automationPaused.load(std::memory_order_relaxed);
            sharedState_.automationPaused.store(newPaused, std::memory_order_release);
            if (newPaused) {
                AbortAllActiveSlots();
            } else {
                sharedState_.safeStopReason.store(0U, std::memory_order_release);
            }
            paused = newPaused;
            continue;
        }

        // Optional global stop-all hotkey immediately aborts all active macros/releases
        if (stopAllVk != 0U && !capturingBind && ev.isDown && ev.vkCode == stopAllVk) {
            AbortAllActiveSlots();
            continue;
        }

        if (paused || suppressed) {
            continue;
        }

        HandleTriggerTransition(ev, currentTsc);
    }

    // 3. Drain auxiliary lock-free output queue (bounded batch per pass so engine stays responsive)
    mask::core::AuxOutputCommand auxCmd{};
    size_t auxProcessed = 0;
    while (auxProcessed < kMaxAuxBatchPerPass && sharedState_.auxOutputQueue.try_pop(auxCmd)) {
        ++auxProcessed;
        const auto action = static_cast<mask::core::AuxActionKind>(auxCmd.kind);
        if (paused && action != mask::core::AuxActionKind::KeyUp) {
            continue;
        }
        ExecuteAuxOutputCommand(auxCmd, currentTsc);
    }
    if (auxProcessed == kMaxAuxBatchPerPass && !sharedState_.auxOutputQueue.empty()) {
        mask::input::WakeEngine();
    }
}

void MacroRuntime::EvaluateAndExecute(uint64_t currentTsc) noexcept {
    // Always service pending timed auxiliary key releases even if no macro slot is active
    if (auxTimedActiveMask_ != 0U) {
        ProcessAuxTimedReleases(currentTsc);
    }

    if (sharedState_.automationPaused.load(std::memory_order_relaxed)) [[unlikely]] {
        return;
    }

    // Refresh hold-based slots from physicalKeyState in case an edge event was missed
    SyncHoldTriggerStates(currentTsc, !TriggersSuppressed());

    if (arena_.activeMask == 0ULL) {
        return;
    }

    const uint64_t expiredMask = hasAvx2Bmi2_
        ? EvaluateExpiredMaskAvx2Bmi2(currentTsc)
        : EvaluateExpiredMaskSse2(currentTsc);

    uint64_t workMask = expiredMask & arena_.activeMask;
    while (workMask != 0ULL) {
        const uint64_t slotIdx = NextSetBitIndex(workMask);
        workMask &= (workMask - 1ULL); // Clear lowest set bit
        AdvanceSlotStateMachine(static_cast<size_t>(slotIdx), currentTsc);
    }
}

uint64_t MacroRuntime::GetEarliestActiveDeadlineTsc() const noexcept {
    uint64_t mask = arena_.activeMask;
    uint64_t earliest = (std::numeric_limits<uint64_t>::max)();
    while (mask != 0ULL) {
        const uint64_t idx = NextSetBitIndex(mask);
        mask &= (mask - 1ULL);
        const uint64_t d = arena_.nextDeadlineTsc[static_cast<size_t>(idx)];
        if (d < kNoDeadline && d < earliest) {
            earliest = d;
        }
    }
    uint32_t auxMask = auxTimedActiveMask_;
    while (auxMask != 0U) {
        unsigned long idx = 0;
        _BitScanForward(&idx, auxMask);
        auxMask &= (auxMask - 1U);
        const uint64_t d = auxTimedReleases_[idx].releaseDeadlineTsc;
        if (d < kNoDeadline && d < earliest) {
            earliest = d;
        }
    }
    return earliest;
}

void MacroRuntime::TriggerMacroSlot(uint8_t slotIndex, uint64_t currentTsc) noexcept {
    const size_t slot = static_cast<size_t>(slotIndex);
    if (slot >= mask::core::kMaxMacroSlots) {
        return;
    }
    const uint64_t slotBit = (1ULL << slot);
    if ((arena_.configuredMask & slotBit) == 0ULL) {
        return;
    }

    const bool isActive = (arena_.activeMask & slotBit) != 0ULL;
    const auto mode = static_cast<mask::core::MacroMode>(arena_.mode[slot]);
    switch (mode) {
        case mask::core::MacroMode::HoldToLoop:
            if (!isActive) {
                StartSlot(slot, currentTsc);
            } else {
                StopAndReleaseSlot(slot);
            }
            break;

        case mask::core::MacroMode::InfiniteToggleLoop:
            if ((arena_.toggleMask & slotBit) != 0ULL) {
                arena_.toggleMask &= ~slotBit;
                if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE) {
                    StopAndReleaseSlot(slot);
                }
            } else if (!isActive) {
                arena_.toggleMask |= slotBit;
                StartSlot(slot, currentTsc);
            }
            break;

        case mask::core::MacroMode::RepeatNTimes:
            if (isActive) {
                StopAndReleaseSlot(slot);
            } else {
                arena_.remainingRepeats[slot] = arena_.targetRepeatCount[slot];
                StartSlot(slot, currentTsc);
            }
            break;

        case mask::core::MacroMode::Sequence:
            if ((arena_.toggleMask & slotBit) != 0ULL) {
                arena_.toggleMask &= ~slotBit;
                if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE) {
                    StopAndReleaseSlot(slot);
                }
            } else if (!isActive) {
                arena_.toggleMask |= slotBit;
                arena_.sequenceStepIndex[slot] = 0;
                StartSlot(slot, currentTsc);
            }
            break;

        case mask::core::MacroMode::SocdPair:
        default:
            break;
    }
}

uint32_t MacroRuntime::GetActiveSlotCount() const noexcept {
    return PopCount64(arena_.activeMask);
}

uint32_t MacroRuntime::GetConfiguredSlotCount() const noexcept {
    return PopCount64(arena_.configuredMask);
}

void MacroRuntime::AbortAllActiveSlots() noexcept {
    uint64_t mask = arena_.activeMask;
    while (mask != 0ULL) {
        const size_t slot = static_cast<size_t>(NextSetBitIndex(mask));
        mask &= (mask - 1ULL);
        StopAndReleaseSlot(slot);
    }
    arena_.toggleMask = 0ULL;

    // Snap-tap slots hold keys without being "active", so they need their own sweep.
    uint64_t configured = arena_.configuredMask;
    while (configured != 0ULL) {
        const size_t slot = static_cast<size_t>(NextSetBitIndex(configured));
        configured &= (configured - 1ULL);
        ReleaseSocdSlot(slot);
    }

    ReleaseAllAuxTimedReleases();
}

void MacroRuntime::ExecuteAuxOutputCommand(const mask::core::AuxOutputCommand& cmd, uint64_t currentTsc) noexcept {
    switch (static_cast<mask::core::AuxActionKind>(cmd.kind)) {
        case mask::core::AuxActionKind::KeyDown:
            if (cmd.vk != 0U) {
                dispatcher_.DispatchKeyTransition(cmd.vk, true);
            }
            break;

        case mask::core::AuxActionKind::KeyUp:
            if (cmd.vk != 0U) {
                CancelAuxTimedReleaseForVk(cmd.vk);
                dispatcher_.DispatchKeyTransition(cmd.vk, false);
            }
            break;

        case mask::core::AuxActionKind::KeyClick:
            if (cmd.vk != 0U) {
                dispatcher_.DispatchKeyTransition(cmd.vk, true);
                if (!mask::input::InputDispatcher::IsWheelVirtualKey(cmd.vk)) {
                    const uint32_t holdMs = (cmd.holdMs != 0U) ? cmd.holdMs : 15U;
                    const uint64_t holdTsc = timingEngine_.NanosecondsToTsc(
                        static_cast<uint64_t>(holdMs) * 1'000'000ULL);
                    ScheduleAuxTimedRelease(cmd.vk, currentTsc + holdTsc);
                }
            }
            break;

        case mask::core::AuxActionKind::MouseMoveRel:
            dispatcher_.DispatchMouseMoveUncoalesced(cmd.argX, cmd.argY);
            break;

        case mask::core::AuxActionKind::MouseMoveAbs:
            dispatcher_.DispatchAbsoluteCursorMove(cmd.argX, cmd.argY);
            break;

        case mask::core::AuxActionKind::MouseScroll:
            dispatcher_.DispatchScrollWheel(cmd.argX, cmd.argY);
            break;

        case mask::core::AuxActionKind::SetLayer: {
            const uint8_t layer = (cmd.auxIndex < mask::core::kMaxRemapLayers)
                ? cmd.auxIndex
                : static_cast<uint8_t>(mask::core::kMaxRemapLayers - 1U);
            sharedState_.activeRemapLayer.store(layer, std::memory_order_release);
            break;
        }

        case mask::core::AuxActionKind::ToggleUnlock: {
            const bool cur = sharedState_.remapUnlocked.load(std::memory_order_relaxed);
            sharedState_.remapUnlocked.store(!cur, std::memory_order_release);
            break;
        }

        case mask::core::AuxActionKind::TriggerMacroSlot:
            TriggerMacroSlot(cmd.auxIndex, currentTsc);
            break;

        case mask::core::AuxActionKind::TriggerComplexSeq:
            if (const ComplexSeqTriggerCallback cb = complexSeqTriggerCallback_.load(std::memory_order_acquire)) {
                cb(cmd.auxIndex);
            }
            break;

        default:
            break;
    }
}

void MacroRuntime::ScheduleAuxTimedRelease(uint16_t vk, uint64_t releaseDeadlineTsc) noexcept {
    // If this VK already has a pending release, extend/refresh its deadline in place
    uint32_t active = auxTimedActiveMask_;
    while (active != 0U) {
        unsigned long idx = 0;
        _BitScanForward(&idx, active);
        active &= (active - 1U);
        if (auxTimedReleases_[idx].vk == vk) {
            auxTimedReleases_[idx].releaseDeadlineTsc = releaseDeadlineTsc;
            return;
        }
    }

    // Find first free slot in our 16-entry fixed table
    size_t targetIdx = kMaxAuxTimedReleases;
    for (size_t i = 0; i < kMaxAuxTimedReleases; ++i) {
        if (((auxTimedActiveMask_ >> i) & 1U) == 0U) {
            targetIdx = i;
            break;
        }
    }

    // If all 16 slots are occupied, release the earliest-expiring entry cleanly and reuse it
    if (targetIdx == kMaxAuxTimedReleases) {
        targetIdx = 0;
        uint64_t earliest = auxTimedReleases_[0].releaseDeadlineTsc;
        for (size_t i = 1; i < kMaxAuxTimedReleases; ++i) {
            if (auxTimedReleases_[i].releaseDeadlineTsc < earliest) {
                earliest = auxTimedReleases_[i].releaseDeadlineTsc;
                targetIdx = i;
            }
        }
        if (auxTimedReleases_[targetIdx].active && auxTimedReleases_[targetIdx].vk != 0U) {
            dispatcher_.DispatchKeyTransition(auxTimedReleases_[targetIdx].vk, false);
        }
    }

    auto& entry = auxTimedReleases_[targetIdx];
    entry.releaseDeadlineTsc = releaseDeadlineTsc;
    entry.vk = vk;
    entry.active = true;
    auxTimedActiveMask_ |= static_cast<uint16_t>(1U << targetIdx);
}

void MacroRuntime::CancelAuxTimedReleaseForVk(uint16_t vk) noexcept {
    uint32_t active = auxTimedActiveMask_;
    while (active != 0U) {
        unsigned long idx = 0;
        _BitScanForward(&idx, active);
        active &= (active - 1U);
        auto& entry = auxTimedReleases_[idx];
        if (entry.vk == vk) {
            entry.active = false;
            entry.vk = 0;
            entry.releaseDeadlineTsc = 0;
            auxTimedActiveMask_ &= static_cast<uint16_t>(~(1U << idx));
        }
    }
}

void MacroRuntime::ProcessAuxTimedReleases(uint64_t currentTsc) noexcept {
    uint32_t active = auxTimedActiveMask_;
    while (active != 0U) {
        unsigned long idx = 0;
        _BitScanForward(&idx, active);
        active &= (active - 1U);
        auto& entry = auxTimedReleases_[idx];
        if (entry.active && currentTsc >= entry.releaseDeadlineTsc) {
            const uint16_t vk = entry.vk;
            entry.active = false;
            entry.vk = 0;
            entry.releaseDeadlineTsc = 0;
            auxTimedActiveMask_ &= static_cast<uint16_t>(~(1U << idx));
            if (vk != 0U) {
                dispatcher_.DispatchKeyTransition(vk, false);
            }
        }
    }
}

void MacroRuntime::ReleaseAllAuxTimedReleases() noexcept {
    uint32_t active = auxTimedActiveMask_;
    while (active != 0U) {
        unsigned long idx = 0;
        _BitScanForward(&idx, active);
        active &= (active - 1U);
        auto& entry = auxTimedReleases_[idx];
        if (entry.active && entry.vk != 0U) {
            dispatcher_.DispatchKeyTransition(entry.vk, false);
        }
        entry.active = false;
        entry.vk = 0;
        entry.releaseDeadlineTsc = 0;
    }
    auxTimedActiveMask_ = 0U;
}

bool MacroRuntime::TriggersSuppressed() const noexcept {
    return sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U ||
           sharedState_.suppressTriggers.load(std::memory_order_relaxed);
}

bool MacroRuntime::IsPhysicallyHeld(uint16_t vk) const noexcept {
    return sharedState_.physicalKeyState[vk & 0xFFU].load(std::memory_order_relaxed) != 0U;
}

uint64_t MacroRuntime::NextDeadline(uint64_t previousTargetTsc, uint64_t nowTsc, uint64_t durationTsc) noexcept {
    if (previousTargetTsc < kNoDeadline && previousTargetTsc <= nowTsc) {
        const uint64_t ideal = previousTargetTsc + durationTsc;
        if (ideal > nowTsc) {
            return ideal;
        }
    }
    return nowTsc + durationTsc;
}

uint64_t MacroRuntime::EvaluateExpiredMaskAvx2Bmi2(uint64_t currentTsc) const noexcept {
    return EvaluateExpiredMaskAvx2Bmi2Impl(arena_.nextDeadlineTsc.data(), arena_.activeMask, currentTsc);
}

uint64_t MacroRuntime::EvaluateExpiredMaskSse2(uint64_t currentTsc) const noexcept {
    return EvaluateExpiredMaskSse2Impl(arena_.nextDeadlineTsc.data(), arena_.activeMask, currentTsc);
}

uint64_t MacroRuntime::NextSetBitIndex(uint64_t mask) noexcept {
    unsigned long index = 0;
    return _BitScanForward64(&index, mask) ? static_cast<uint64_t>(index) : 64ULL;
}

void MacroRuntime::HandleTriggerTransition(const mask::core::InputTriggerEvent& ev, uint64_t currentTsc) noexcept {
    uint64_t mask = arena_.configuredMask;
    while (mask != 0ULL) {
        const size_t slot = static_cast<size_t>(NextSetBitIndex(mask));
        mask &= (mask - 1ULL);

        const auto mode = static_cast<mask::core::MacroMode>(arena_.mode[slot]);

        // Mode 4: Deterministic SOCD (Simultaneous Opposing Cardinal Directions - Last Input Priority)
        if (mode == mask::core::MacroMode::SocdPair) {
            if (ev.vkCode == arena_.triggerVk[slot] || ev.vkCode == arena_.outputVk[slot]) {
                ResolveSocdLastInputPriority(slot, ev.vkCode, ev.isDown);
            }
            continue;
        }

        if (arena_.triggerVk[slot] != ev.vkCode) {
            continue;
        }

        const uint64_t slotBit = (1ULL << slot);
        const bool isActive = (arena_.activeMask & slotBit) != 0ULL;

        switch (mode) {
            case mask::core::MacroMode::HoldToLoop: {
                if (ev.isDown) {
                    if (!isActive) {
                        StartSlot(slot, currentTsc);
                    }
                } else {
                    // Trigger released: stop right away and release the output, so nothing is left held.
                    StopAndReleaseSlot(slot);
                }
                break;
            }

            case mask::core::MacroMode::InfiniteToggleLoop: {
                if (ev.isDown) {
                    if ((arena_.toggleMask & slotBit) != 0ULL) {
                        arena_.toggleMask &= ~slotBit;
                        // Between presses: stop now. In the middle of a timed press:
                        // AdvanceSlotStateMachine finishes that press cleanly and then stops.
                        if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE) {
                            StopAndReleaseSlot(slot);
                        }
                    } else if (!isActive) {
                        arena_.toggleMask |= slotBit;
                        StartSlot(slot, currentTsc);
                    }
                }
                break;
            }

            case mask::core::MacroMode::RepeatNTimes: {
                if (ev.isDown) {
                    if (isActive) {
                        // Pressing trigger while burst is active cancels burst cleanly
                        StopAndReleaseSlot(slot);
                    } else {
                        arena_.remainingRepeats[slot] = arena_.targetRepeatCount[slot];
                        StartSlot(slot, currentTsc);
                    }
                }
                break;
            }

            case mask::core::MacroMode::Sequence: {
                if (ev.isDown) {
                    if ((arena_.toggleMask & slotBit) != 0ULL) {
                        arena_.toggleMask &= ~slotBit;
                        if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE) {
                            StopAndReleaseSlot(slot);
                        }
                    } else if (!isActive) {
                        arena_.toggleMask |= slotBit;
                        arena_.sequenceStepIndex[slot] = 0;
                        StartSlot(slot, currentTsc);
                    }
                }
                break;
            }

            default:
                break;
        }
    }
}

void MacroRuntime::ResolveSocdLastInputPriority(size_t slot, uint16_t changedVk, bool isDown) noexcept {
    const uint16_t primaryVk  = arena_.triggerVk[slot];
    const uint16_t opposingVk = arena_.outputVk[slot];
    if (primaryVk == 0U || opposingVk == 0U || primaryVk == opposingVk) {
        return;
    }
    const uint16_t otherVk = (changedVk == primaryVk) ? opposingVk : primaryVk;

    uint16_t desiredVk = 0;
    if (isDown) {
        desiredVk = changedVk;
    } else if (IsPhysicallyHeld(otherVk)) {
        desiredVk = otherVk;
    }

    const uint16_t assertedVk = arena_.socdAssertedVk[slot];
    if (desiredVk == assertedVk) {
        return;
    }
    dispatcher_.DispatchSeamlessSwap(assertedVk, desiredVk);
    arena_.socdAssertedVk[slot] = desiredVk;
}

void MacroRuntime::ReleaseTriggerFollowingSlots() noexcept {
    uint64_t mask = arena_.configuredMask;
    while (mask != 0ULL) {
        const size_t slot = static_cast<size_t>(NextSetBitIndex(mask));
        mask &= (mask - 1ULL);
        const auto mode = static_cast<mask::core::MacroMode>(arena_.mode[slot]);
        if (mode == mask::core::MacroMode::SocdPair) {
            ReleaseSocdSlot(slot);
        } else if (mode == mask::core::MacroMode::HoldToLoop) {
            StopAndReleaseSlot(slot);
        }
    }
}

void MacroRuntime::ReleaseSocdSlot(size_t slot) noexcept {
    const uint16_t assertedVk = arena_.socdAssertedVk[slot];
    if (assertedVk != 0U) {
        dispatcher_.DispatchKeyTransition(assertedVk, false);
        arena_.socdAssertedVk[slot] = 0;
    }
}

void MacroRuntime::SyncHoldTriggerStates(uint64_t currentTsc, bool allowStart) noexcept {
    uint64_t mask = arena_.configuredMask;
    while (mask != 0ULL) {
        const size_t slot = static_cast<size_t>(NextSetBitIndex(mask));
        mask &= (mask - 1ULL);

        const auto mode = static_cast<mask::core::MacroMode>(arena_.mode[slot]);
        if (mode == mask::core::MacroMode::SocdPair) {
            // A snap-tap slot must never keep asserting a key that is no longer physically held.
            const uint16_t assertedVk = arena_.socdAssertedVk[slot];
            if (assertedVk != 0U && !IsPhysicallyHeld(assertedVk)) {
                ResolveSocdLastInputPriority(slot, assertedVk, false);
            }
            continue;
        }
        if (mode != mask::core::MacroMode::HoldToLoop) {
            continue;
        }
        const uint16_t trigVk = arena_.triggerVk[slot];
        if (trigVk == 0U) {
            continue;
        }
        const uint64_t slotBit = (1ULL << slot);
        const bool isActive = (arena_.activeMask & slotBit) != 0ULL;

        if (IsPhysicallyHeld(trigVk)) {
            if (!isActive && allowStart) {
                StartSlot(slot, currentTsc);
            }
        } else if (isActive) {
            if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE) {
                StopAndReleaseSlot(slot);
            }
        }
    }
}

void MacroRuntime::AdvanceSlotStateMachine(size_t slot, uint64_t currentTsc) noexcept {
    const uint64_t targetTsc = arena_.nextDeadlineTsc[slot];
    if (targetTsc < kNoDeadline) {
        timingEngine_.RecordJitterSampleTsc(targetTsc, currentTsc);
    }

    const uint64_t slotBit = (1ULL << slot);
    const auto mode = static_cast<mask::core::MacroMode>(arena_.mode[slot]);
    const bool toggledOn = (arena_.toggleMask & slotBit) != 0ULL;

    switch (arena_.phase[slot]) {
        case mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE: {
            // ---- End of a press. First: should the slot stop here? -------------------------------
            bool shouldStop = false;
            if (mode == mask::core::MacroMode::InfiniteToggleLoop || mode == mask::core::MacroMode::Sequence) {
                shouldStop = !toggledOn;
            } else if (mode == mask::core::MacroMode::HoldToLoop) {
                shouldStop = !IsPhysicallyHeld(arena_.triggerVk[slot]);
            }
            if (shouldStop) {
                StopAndReleaseSlot(slot);
                return;
            }

            if (mode == mask::core::MacroMode::Sequence) {
                const uint8_t count = (std::max<uint8_t>)(1U, arena_.sequenceStepCount[slot]);
                const uint8_t stepIdx = static_cast<uint8_t>(arena_.sequenceStepIndex[slot] % count);
                const uint64_t gapTsc = arena_.sequenceSteps[slot][stepIdx].recoveryTsc;
                if (gapTsc == 0ULL) {
                    // No gap after this step: go straight to the next key without a release in between.
                    arena_.sequenceStepIndex[slot] = static_cast<uint8_t>((stepIdx + 1U) % count);
                    EnterPressPhase(slot, targetTsc, currentTsc, /*seamless=*/true);
                    return;
                }
                ReleaseSlotOutput(slot);
                const uint64_t jitteredGapTsc = ApplyHumanizeJitterTsc(gapTsc, arena_.humanizePct[slot]);
                arena_.phase[slot] = mask::core::SlotPhase::PHASE_2_RECOVERY_WAIT;
                arena_.nextDeadlineTsc[slot] = NextDeadline(targetTsc, currentTsc, jitteredGapTsc);
                return;
            }

            // Always emit Up and enter PHASE_2_RECOVERY_WAIT (even when recoveryDurationTsc == 0),
            // executing at most 1 state transition per slot per pass so 0 ms / >1000 CPS autoclicking
            // never gets stuck holding down and Pause/Stop/Panic always stay responsive.
            ReleaseSlotOutput(slot);
            const uint64_t recoveryTsc = arena_.recoveryDurationTsc[slot];
            const uint64_t jitteredRecoveryTsc = ApplyHumanizeJitterTsc(recoveryTsc, arena_.humanizePct[slot]);
            arena_.phase[slot] = mask::core::SlotPhase::PHASE_2_RECOVERY_WAIT;
            arena_.nextDeadlineTsc[slot] = NextDeadline(targetTsc, currentTsc, jitteredRecoveryTsc);
            return;
        }

        case mask::core::SlotPhase::PHASE_2_RECOVERY_WAIT: {
            // ---- End of a release gap: press again, or finish. -----------------------------------
            bool keepRunning = false;
            switch (mode) {
                case mask::core::MacroMode::HoldToLoop:
                    keepRunning = IsPhysicallyHeld(arena_.triggerVk[slot]);
                    break;
                case mask::core::MacroMode::InfiniteToggleLoop:
                    keepRunning = toggledOn;
                    break;
                case mask::core::MacroMode::RepeatNTimes:
                    if (arena_.remainingRepeats[slot] > 1U) {
                        --arena_.remainingRepeats[slot];
                        keepRunning = true;
                    } else {
                        arena_.remainingRepeats[slot] = 0U;
                    }
                    break;
                case mask::core::MacroMode::Sequence:
                    keepRunning = toggledOn;
                    if (keepRunning) {
                        const uint8_t count = (std::max<uint8_t>)(1U, arena_.sequenceStepCount[slot]);
                        arena_.sequenceStepIndex[slot] = static_cast<uint8_t>((arena_.sequenceStepIndex[slot] + 1U) % count);
                    }
                    break;
                default:
                    break;
            }

            if (keepRunning) {
                EnterPressPhase(slot, targetTsc, currentTsc, /*seamless=*/false);
            } else {
                StopAndReleaseSlot(slot);
            }
            return;
        }

        default:
            // An active slot should never be idle; recover by stopping it.
            StopAndReleaseSlot(slot);
            return;
    }
}

void MacroRuntime::StartSlot(size_t slot, uint64_t currentTsc) noexcept {
    arena_.activeMask |= (1ULL << slot);
    EnterPressPhase(slot, kNoDeadline, currentTsc, /*seamless=*/false);
}

void MacroRuntime::EnterPressPhase(size_t slot, uint64_t previousTargetTsc, uint64_t currentTsc, bool seamless) noexcept {
    const auto mode = static_cast<mask::core::MacroMode>(arena_.mode[slot]);

    uint16_t outVk = arena_.outputVk[slot];
    uint8_t  outIsMouse = arena_.isMouse[slot];
    uint64_t dwellTsc = arena_.dwellDurationTsc[slot];

    if (mode == mask::core::MacroMode::Sequence) {
        const uint8_t count = (std::max<uint8_t>)(1U, arena_.sequenceStepCount[slot]);
        const auto& step = arena_.sequenceSteps[slot][arena_.sequenceStepIndex[slot] % count];
        outVk      = step.outputVk;
        outIsMouse = step.isMouse;
        dwellTsc   = step.dwellTsc;
    }

    const int32_t dx = arena_.mouseDeltaX[slot];
    const int32_t dy = arena_.mouseDeltaY[slot];
    const uint16_t heldVk = seamless ? arena_.activeDispatchedVk[slot] : uint16_t{0};

    if (mask::input::InputDispatcher::IsWheelVirtualKey(outVk)) {
        if (arena_.activeDispatchedVk[slot] != 0U) {
            ReleaseSlotOutput(slot);
        }
        dispatcher_.DispatchMouseButtonTransition(outVk, true, dx, dy);
        arena_.activeDispatchedVk[slot]      = 0;
        arena_.activeDispatchedIsMouse[slot] = 0;
    } else if (heldVk != 0U) {
        dispatcher_.DispatchSeamlessSwap(heldVk, outVk);
        dispatcher_.DispatchMouseMoveUncoalesced(dx, dy);
        arena_.activeDispatchedVk[slot]      = outVk;
        arena_.activeDispatchedIsMouse[slot] = outIsMouse;
    } else if (outVk != 0U && outIsMouse != 0U) {
        dispatcher_.DispatchMouseButtonTransition(outVk, true, dx, dy); // button + movement in one report
        arena_.activeDispatchedVk[slot]      = outVk;
        arena_.activeDispatchedIsMouse[slot] = outIsMouse;
    } else {
        if (outVk != 0U) {
            dispatcher_.DispatchKeyTransition(outVk, true);
        }
        dispatcher_.DispatchMouseMoveUncoalesced(dx, dy);
        arena_.activeDispatchedVk[slot]      = outVk;
        arena_.activeDispatchedIsMouse[slot] = (outVk != 0U) ? outIsMouse : uint8_t{0};
    }

    const uint64_t jitteredDwellTsc = ApplyHumanizeJitterTsc(dwellTsc, arena_.humanizePct[slot]);
    arena_.phase[slot] = mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE;
    arena_.nextDeadlineTsc[slot] = NextDeadline(previousTargetTsc, currentTsc, jitteredDwellTsc);
}

void MacroRuntime::ReleaseSlotOutput(size_t slot) noexcept {
    const uint16_t outVk = arena_.activeDispatchedVk[slot];
    if (outVk == 0U) {
        return;
    }
    if (arena_.activeDispatchedIsMouse[slot] != 0U) {
        if (!mask::input::InputDispatcher::IsWheelVirtualKey(outVk)) {
            dispatcher_.DispatchMouseButtonTransition(outVk, false);
        }
    } else {
        dispatcher_.DispatchKeyTransition(outVk, false);
    }
    arena_.activeDispatchedVk[slot]      = 0;
    arena_.activeDispatchedIsMouse[slot] = 0;
}

void MacroRuntime::StopAndReleaseSlot(size_t slot) noexcept {
    ReleaseSlotOutput(slot); // no-op unless this slot is holding something
    const uint64_t bit = (1ULL << slot);
    arena_.activeMask &= ~bit;
    arena_.toggleMask &= ~bit;
    arena_.phase[slot] = mask::core::SlotPhase::IDLE;
    arena_.nextDeadlineTsc[slot] = kNoDeadline;
}

uint64_t MacroRuntime::NextRandomU64() noexcept {
    uint64_t x = prngState_;
    if (x == 0ULL) [[unlikely]] {
        x = 0x9E3779B97F4A7C15ULL;
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    prngState_ = x;
    return x * 0x2545F4914F6CDD1DULL;
}

uint64_t MacroRuntime::ApplyHumanizeJitterTsc(uint64_t baseTsc, uint8_t pct) noexcept {
    if (pct == 0U || baseTsc == 0ULL) {
        return baseTsc;
    }
    const uint64_t p = static_cast<uint64_t>((std::min<uint8_t>)(pct, uint8_t{50}));
    const uint64_t q = baseTsc / 100ULL;
    const uint64_t r = baseTsc % 100ULL;
    const uint64_t minMul = 100ULL - p;
    const uint64_t maxMul = 100ULL + p;

    uint64_t minTsc = q * minMul + (r * minMul) / 100ULL;
    const uint64_t maxHiPart = (r * maxMul) / 100ULL;
    uint64_t maxTsc = (std::numeric_limits<uint64_t>::max)();
    if (q <= ((std::numeric_limits<uint64_t>::max)() - maxHiPart) / maxMul) {
        maxTsc = q * maxMul + maxHiPart;
    }

    if (minTsc < minPulseFloorTsc_) {
        minTsc = minPulseFloorTsc_;
    }
    if (maxTsc < minTsc) {
        maxTsc = minTsc;
    }

    const uint64_t span = (maxTsc - minTsc) + 1ULL;
    if (span <= 1ULL) {
        return minTsc;
    }
    uint64_t offset = 0ULL;
    (void)_umul128(NextRandomU64(), span, &offset);
    return minTsc + offset;
}

bool MacroRuntime::RunSimdEquivalenceSelfTest() noexcept {
    const bool hasAvx2 = DetectAvx2Bmi2Support();

    auto scalarEval = [](const uint64_t* deadlines, uint64_t activeMask, uint64_t currentTsc) noexcept -> uint64_t {
        uint64_t expected = 0ULL;
        for (size_t i = 0; i < mask::core::kMaxMacroSlots; ++i) {
            if (((activeMask >> i) & 1ULL) != 0ULL && currentTsc >= deadlines[i]) {
                expected |= (1ULL << i);
            }
        }
        return expected;
    };

    auto verifyCase = [&](const uint64_t* deadlines, uint64_t activeMask, uint64_t currentTsc) noexcept -> bool {
        const uint64_t expected = scalarEval(deadlines, activeMask, currentTsc);
        const uint64_t sse2Res  = EvaluateExpiredMaskSse2Impl(deadlines, activeMask, currentTsc);
        if (sse2Res != expected) {
            return false;
        }
        if (hasAvx2) {
            const uint64_t avx2Res = EvaluateExpiredMaskAvx2Bmi2Impl(deadlines, activeMask, currentTsc);
            if (avx2Res != expected || avx2Res != sse2Res) {
                return false;
            }
        }
        return true;
    };

    alignas(64) std::array<uint64_t, mask::core::kMaxMacroSlots> deadlines{};

    // 1. Edge currentTsc & edge deadlines (0, 1, sign-bit boundaries, low-32 bit 31 boundaries, INT64_MAX, UINT64_MAX)
    constexpr uint64_t kEdgeValues[] = {
        0ULL,
        1ULL,
        2ULL,
        0x7FFFFFFFULL,
        0x80000000ULL,
        0x80000001ULL,
        0xFFFFFFFFULL,
        0x100000000ULL,
        0x180000000ULL,
        0x7FFFFFFF7FFFFFFFULL,
        0x7FFFFFFF80000000ULL,
        0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        0x8000000000000001ULL,
        0x8000000080000000ULL,
        0xFFFFFFFFFFFFFFFEULL,
        0xFFFFFFFFFFFFFFFFULL
    };
    constexpr uint64_t kEdgeMasks[] = {
        0ULL,
        1ULL,
        0x3ULL,
        0xFULL,
        0x10ULL,
        0x8000000000000000ULL,
        0x5555555555555555ULL,
        0xAAAAAAAAAAAAAAAAULL,
        0xFFFFFFFFFFFFFFFFULL
    };

    for (const uint64_t now : kEdgeValues) {
        for (size_t i = 0; i < mask::core::kMaxMacroSlots; ++i) {
            deadlines[i] = kEdgeValues[i % std::size(kEdgeValues)];
        }
        for (const uint64_t activeMask : kEdgeMasks) {
            if (!verifyCase(deadlines.data(), activeMask, now)) {
                return false;
            }
        }
        // Single-bit walk across all 64 slots
        for (size_t bit = 0; bit < mask::core::kMaxMacroSlots; ++bit) {
            if (!verifyCase(deadlines.data(), 1ULL << bit, now)) {
                return false;
            }
        }
    }

    // 2. Specifically test equal upper-32 bits with crossing lower-32 bit 31 boundaries
    for (size_t i = 0; i < mask::core::kMaxMacroSlots; ++i) {
        const uint64_t hi = 0x1234567800000000ULL;
        const uint64_t lo = (i & 1U) ? 0x80000005ULL : 0x7FFFFFFFULL;
        deadlines[i] = hi | lo;
    }
    if (!verifyCase(deadlines.data(), 0xFFFFFFFFFFFFFFFFULL, 0x1234567880000000ULL)) {
        return false;
    }
    if (!verifyCase(deadlines.data(), 0xFFFFFFFFFFFFFFFFULL, 0x123456787FFFFFFFULL)) {
        return false;
    }

    // 3. Deterministic PRNG sweep of 256 random bitmasks and mixed deadlines (near-now, 64-bit, kNoDeadline)
    uint64_t rng = 0xD1B54A32D192ED03ULL;
    auto nextRng = [&rng]() noexcept -> uint64_t {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        return rng * 0x2545F4914F6CDD1DULL;
    };

    for (int trial = 0; trial < 256; ++trial) {
        const uint64_t activeMask = nextRng();
        const uint64_t baseTsc = (trial & 1)
            ? (nextRng() & 0x0000FFFFFFFFFFFFULL)
            : nextRng();

        for (size_t i = 0; i < mask::core::kMaxMacroSlots; ++i) {
            const uint64_t r = nextRng();
            switch (r & 3ULL) {
                case 0ULL: deadlines[i] = baseTsc - (r % 1024ULL); break;
                case 1ULL: deadlines[i] = baseTsc + (r % 1024ULL); break;
                case 2ULL: deadlines[i] = kNoDeadline; break;
                default:   deadlines[i] = r; break;
            }
        }
        if (!verifyCase(deadlines.data(), activeMask, baseTsc)) {
            return false;
        }
    }

    return true;
}

} // namespace mask::runtime
