#pragma once

#include "core_contracts.hpp"
#include "timing_engine.hpp"
#include "input_pipeline.hpp"

#include <immintrin.h>
#include <intrin.h>
#include <cstdint>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

namespace mask::runtime {

/**
 * @brief Pre-computed TSC step entry for Multi-Key Sequence execution.
 */
struct SequenceStepTsc {
    uint64_t dwellTsc{0};
    uint64_t recoveryTsc{0};
    uint16_t outputVk{'1'};
    uint8_t  isMouse{0};
    uint8_t  enabled{1};
    uint8_t  padding[4]{0, 0, 0, 0};
};

/**
 * @brief Cache-Aligned Struct-of-Arrays (SoA) Macro Arena for 64 concurrent slots.
 * Layouts are aligned to 64-byte boundaries for single-instruction AVX2 vector loads.
 */
struct alignas(mask::core::kCacheLineSize) MacroArenaSoA {
    // Hot SIMD-evaluated arrays (64 x uint64_t = 512 bytes = 8 cache lines)
    alignas(mask::core::kCacheLineSize) std::array<uint64_t, mask::core::kMaxMacroSlots> nextDeadlineTsc{};
    alignas(mask::core::kCacheLineSize) std::array<uint64_t, mask::core::kMaxMacroSlots> dwellDurationTsc{};
    alignas(mask::core::kCacheLineSize) std::array<uint64_t, mask::core::kMaxMacroSlots> recoveryDurationTsc{};

    // Per-slot configuration and phase metadata
    alignas(mask::core::kCacheLineSize) std::array<mask::core::SlotPhase, mask::core::kMaxMacroSlots> phase{};
    alignas(mask::core::kCacheLineSize) std::array<uint16_t, mask::core::kMaxMacroSlots> triggerVk{};
    alignas(mask::core::kCacheLineSize) std::array<uint16_t, mask::core::kMaxMacroSlots> outputVk{};
    alignas(mask::core::kCacheLineSize) std::array<uint16_t, mask::core::kMaxMacroSlots> activeDispatchedVk{};
    alignas(mask::core::kCacheLineSize) std::array<uint8_t,  mask::core::kMaxMacroSlots> activeDispatchedIsMouse{};
    alignas(mask::core::kCacheLineSize) std::array<uint8_t,  mask::core::kMaxMacroSlots> mode{};
    alignas(mask::core::kCacheLineSize) std::array<uint8_t,  mask::core::kMaxMacroSlots> isMouse{};
    alignas(mask::core::kCacheLineSize) std::array<int32_t,  mask::core::kMaxMacroSlots> mouseDeltaX{};
    alignas(mask::core::kCacheLineSize) std::array<int32_t,  mask::core::kMaxMacroSlots> mouseDeltaY{};
    alignas(mask::core::kCacheLineSize) std::array<uint32_t, mask::core::kMaxMacroSlots> targetRepeatCount{};
    alignas(mask::core::kCacheLineSize) std::array<uint32_t, mask::core::kMaxMacroSlots> remainingRepeats{};
    alignas(mask::core::kCacheLineSize) std::array<uint8_t,  mask::core::kMaxMacroSlots> sequenceStepIndex{};
    alignas(mask::core::kCacheLineSize) std::array<uint8_t,  mask::core::kMaxMacroSlots> sequenceStepCount{};
    alignas(mask::core::kCacheLineSize) std::array<uint8_t,  mask::core::kMaxMacroSlots> humanizePct{};
    alignas(mask::core::kCacheLineSize) std::array<uint16_t, mask::core::kMaxMacroSlots> socdAssertedVk{}; // Key a snap-tap slot currently holds down (0 = none)
    alignas(mask::core::kCacheLineSize) std::array<std::array<SequenceStepTsc, mask::core::kMaxSequenceSteps>, mask::core::kMaxMacroSlots> sequenceSteps{};

    // Bitmasks for branchless active-slot evaluation
    uint64_t configuredMask{0}; // Bit i = 1 if slot i is configured and enabled
    uint64_t activeMask{0};     // Bit i = 1 if slot i is currently running Phase 1 or Phase 2
    uint64_t toggleMask{0};     // Bit i = 1 if an InfiniteToggleLoop or Sequence slot is toggled ON
};

/**
 * @brief SIMD & Data-Oriented Macro Runtime.
 * Detects AVX2 + BMI2 via __cpuid at startup, evaluates 64 macro slot deadlines using
 * either AVX2+BMI2 (_mm256_cmpgt_epi64, _mm256_movemask_pd, _tzcnt_u64) or an SSE2 baseline,
 * enforces a 50us minimum pulse floor guard, and executes all 5 modes.
 *
 * Timing model: every slot alternates between a press phase ("dwell") and a release phase ("recovery").
 *   - Deadlines are scheduled from the previous *ideal* deadline, not from the moment the engine got
 *     around to it, so lateness never accumulates and the long-run rate is exactly what was configured.
 *   - A recovery time of 0 is a true 100% duty cycle: the output goes down once and stays down. No release
 *     event is ever sent between cycles. In Sequence mode a 0 gap swaps to the next key in a single report.
 *
 * Modes:
 *   0: Hold-to-Loop (While Held)
 *   1: Infinite Toggle Loop (On/Off Toggle)
 *   2: Repeat N Times (Burst Mode)
 *   3: Multi-Key Sequence Mode (Up to 8 steps with per-step keys & microsecond delays)
 *   4: SOCD Pair (Simultaneous Opposing Cardinal Directions - Last Input Priority)
 */
class alignas(mask::core::kCacheLineSize) MacroRuntime {
public:
    using ComplexSeqTriggerCallback = void(*)(uint8_t sequenceIndex) noexcept;

    MacroRuntime(
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

    ~MacroRuntime() noexcept = default;

    MacroRuntime(const MacroRuntime&) = delete;
    MacroRuntime& operator=(const MacroRuntime&) = delete;

    void ResetArena() noexcept {
        constexpr uint64_t kMaxSigned64 = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        minPulseFloorTsc_ = timingEngine_.NanosecondsToTsc(mask::core::kMinimumPulseFloorNs);
        arena_.nextDeadlineTsc.fill(kMaxSigned64);
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

    /**
     * @brief Registers an optional callback invoked when `AuxActionKind::TriggerComplexSeq` is drained.
     */
    void SetComplexSeqTriggerCallback(ComplexSeqTriggerCallback cb) noexcept {
        complexSeqTriggerCallback_.store(cb, std::memory_order_release);
    }

    /**
     * @brief Detects AVX2 and BMI2 instruction set support via __cpuidex(leaf 7, subleaf 0).
     */
    void DetectCpuCapabilities() noexcept {
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
            const bool hasBmi1 = (cpuInfo[1] & (1 << 3)) != 0; // EBX bit 3: BMI1 (_tzcnt_u64)
            hasAvx2Bmi2_ = (hasAvx2 && hasBmi2 && hasBmi1);
        } else {
            hasAvx2Bmi2_ = false;
        }
    }

    [[nodiscard]] bool HasAvx2Bmi2() const noexcept {
        return hasAvx2Bmi2_;
    }

    /**
     * @brief Installs or updates a MacroConfig slot in the SoA arena.
     * Applies the mandatory 50us minimum pulse-floor guard to prevent OS input queue saturation
     * (a release time of exactly 0 is kept as-is: that is the 100% duty cycle setting).
     *
     * If only timings, repeat count, pointer movement or output keys changed, the slot is updated in place
     * and keeps running, so speed and duty cycle can be tuned live. Changing the trigger, the mode or the
     * enabled state stops the slot and releases anything it was holding.
     */
    bool ConfigureSlot(const mask::core::MacroConfig& cfg) noexcept {
        const size_t slot = cfg.slotIndex;
        if (slot >= mask::core::kMaxMacroSlots) {
            return false;
        }

        const uint64_t bit = (1ULL << slot);
        const bool wasConfigured = (arena_.configuredMask & bit) != 0ULL;
        const bool nowConfigured = cfg.enabled && cfg.triggerVk != 0;
        const uint8_t newMode = static_cast<uint8_t>(std::min<unsigned>(cfg.mode, 4U));
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

        const uint16_t outVk = static_cast<uint16_t>(cfg.outputVk & 0xFFU);
        arena_.dwellDurationTsc[slot]    = timingEngine_.NanosecondsToTsc(mask::core::ClampPressNs(cfg.dwellDurationNs));
        arena_.recoveryDurationTsc[slot] = timingEngine_.NanosecondsToTsc(mask::core::ClampReleaseNs(cfg.recoveryDurationNs));
        arena_.triggerVk[slot]           = static_cast<uint16_t>(cfg.triggerVk & 0xFFU);
        arena_.outputVk[slot]            = outVk;
        arena_.mode[slot]                = newMode;
        arena_.isMouse[slot]             = mask::input::InputDispatcher::IsMouseVirtualKey(outVk) ? 1U : 0U;
        arena_.mouseDeltaX[slot]         = cfg.mouseDeltaX;
        arena_.mouseDeltaY[slot]         = cfg.mouseDeltaY;
        arena_.targetRepeatCount[slot]   = std::clamp<uint32_t>(cfg.repeatCount, 1U, 9999U);
        arena_.sequenceStepCount[slot]   = std::clamp<uint8_t>(cfg.stepCount, 1U, static_cast<uint8_t>(mask::core::kMaxSequenceSteps));
        arena_.humanizePct[slot]         = std::min<uint8_t>(mask::core::HumanizePercent(cfg), uint8_t{50});

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
            // Keep phase, deadline and counters: the new values apply from the next phase boundary.
            // A slot parked at 100% duty has no boundary coming, so give it one right now.
            if ((arena_.activeMask & bit) != 0ULL && arena_.nextDeadlineTsc[slot] >= kNoDeadline) {
                arena_.nextDeadlineTsc[slot] = mask::timing::TimingEngine::ReadTsc();
            }
            arena_.remainingRepeats[slot] = std::min(arena_.remainingRepeats[slot], arena_.targetRepeatCount[slot]);
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

    /**
     * @brief Drains pending UI configuration updates and physical hook trigger events.
     */
    void ProcessPendingQueues(uint64_t currentTsc) noexcept {
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

    /**
     * @brief Evaluates all 64 macro slots using either the AVX2+BMI2 fast-path or SSE2 baseline,
     * and executes Phase 1 / Phase 2 transitions for every expired deadline.
     */
    void EvaluateAndExecute(uint64_t currentTsc) noexcept {
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

    /**
     * @brief Finds the earliest upcoming TSC deadline across all active macro slots and pending
     * auxiliary timed releases, or UINT64_MAX when nothing is scheduled.
     */
    [[nodiscard]] uint64_t GetEarliestActiveDeadlineTsc() const noexcept {
        uint64_t mask = arena_.activeMask;
        uint64_t earliest = std::numeric_limits<uint64_t>::max();
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

    /**
     * @brief Programmatically triggers a configured macro slot (e.g., from a Color Trigger or Complex Sequence).
     */
    void TriggerMacroSlot(uint8_t slotIndex, uint64_t currentTsc) noexcept {
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
                    if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE ||
                        arena_.recoveryDurationTsc[slot] == 0ULL) {
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

    [[nodiscard]] uint32_t GetActiveSlotCount() const noexcept {
        return static_cast<uint32_t>(__popcnt64(arena_.activeMask));
    }

    [[nodiscard]] uint64_t GetActiveSlotBitmask() const noexcept {
        return arena_.activeMask;
    }

    [[nodiscard]] uint64_t GetToggledSlotBitmask() const noexcept {
        return arena_.toggleMask;
    }

    [[nodiscard]] uint32_t GetConfiguredSlotCount() const noexcept {
        return static_cast<uint32_t>(__popcnt64(arena_.configuredMask));
    }

    [[nodiscard]] uint64_t GetTotalHookEventsProcessed() const noexcept {
        return totalHookEventsProcessed_;
    }

    void AbortAllActiveSlots() noexcept {
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

private:
    static constexpr uint64_t kNoDeadline          = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    static constexpr size_t   kMaxAuxTimedReleases = 16;
    static constexpr size_t   kMaxAuxBatchPerPass  = 64;

    struct AuxTimedRelease {
        uint64_t releaseDeadlineTsc{0};
        uint16_t vk{0};
        bool     active{false};
        uint8_t  reserved[5]{0, 0, 0, 0, 0};
    };

    void ExecuteAuxOutputCommand(const mask::core::AuxOutputCommand& cmd, uint64_t currentTsc) noexcept {
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

    void ScheduleAuxTimedRelease(uint16_t vk, uint64_t releaseDeadlineTsc) noexcept {
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

    void CancelAuxTimedReleaseForVk(uint16_t vk) noexcept {
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

    void ProcessAuxTimedReleases(uint64_t currentTsc) noexcept {
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

    void ReleaseAllAuxTimedReleases() noexcept {
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

    [[nodiscard]] bool TriggersSuppressed() const noexcept {
        return sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U ||
               sharedState_.suppressTriggers.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsPhysicallyHeld(uint16_t vk) const noexcept {
        return sharedState_.physicalKeyState[vk & 0xFFU].load(std::memory_order_relaxed) != 0U;
    }

    /**
     * @brief Next deadline, measured from the previous ideal deadline so that lateness does not accumulate.
     * Falls back to "now" when there is no previous deadline or the engine is more than one phase behind
     * (which avoids firing a burst of catch-up events after a stall).
     */
    [[nodiscard]] static uint64_t NextDeadline(uint64_t previousTargetTsc, uint64_t nowTsc, uint64_t durationTsc) noexcept {
        if (previousTargetTsc < kNoDeadline && previousTargetTsc <= nowTsc) {
            const uint64_t ideal = previousTargetTsc + durationTsc;
            if (ideal > nowTsc) {
                return ideal;
            }
        }
        return nowTsc + durationTsc;
    }
    /**
     * @brief AVX2 + BMI2 branchless deadline evaluator across active 4-slot groups.
     */
    [[nodiscard]] uint64_t EvaluateExpiredMaskAvx2Bmi2(uint64_t currentTsc) const noexcept {
        const uint64_t active = arena_.activeMask;
        if (active == 0ULL) {
            return 0ULL;
        }

        const __m256i vNowPlusOne = _mm256_set1_epi64x(static_cast<long long>(currentTsc + 1ULL));
        const uint64_t* __restrict deadlines = arena_.nextDeadlineTsc.data();

        auto evalGroup = [&](size_t group) noexcept -> uint64_t {
            const __m256i vDeadlines = _mm256_load_si256(reinterpret_cast<const __m256i*>(deadlines + (group * 4)));
            const __m256i vCmp = _mm256_cmpgt_epi64(vNowPlusOne, vDeadlines);
            const int mask4 = _mm256_movemask_pd(_mm256_castsi256_pd(vCmp));
            return static_cast<uint64_t>(static_cast<uint32_t>(mask4) & 0xFU) << (group * 4);
        };

        if ((active >> 4) == 0ULL) {
            return evalGroup(0) & active;
        }

        uint64_t expiredMask = 0ULL;
        for (size_t group = 0; group < 16; ++group) {
            if (((active >> (group * 4)) & 0xFULL) == 0ULL) {
                continue;
            }
            expiredMask |= evalGroup(group);
        }

        return expiredMask & active;
    }

    /**
     * @brief SSE2 baseline evaluator across active 2-slot pairs for CPUs without AVX2.
     */
    [[nodiscard]] uint64_t EvaluateExpiredMaskSse2(uint64_t currentTsc) const noexcept {
        const uint64_t active = arena_.activeMask;
        if (active == 0ULL) {
            return 0ULL;
        }

        const __m128i vNowPlusOne = _mm_set1_epi64x(static_cast<long long>(currentTsc + 1ULL));
        const __m128i vSignFlip   = _mm_set1_epi64x(static_cast<long long>(0x8000000000000000ULL));
        const __m128i vNowFlipped = _mm_xor_si128(vNowPlusOne, vSignFlip);
        const uint64_t* __restrict deadlines = arena_.nextDeadlineTsc.data();

        auto evalPair = [&](size_t pair) noexcept -> uint64_t {
            const __m128i vDead = _mm_load_si128(reinterpret_cast<const __m128i*>(deadlines + (pair * 2)));
            const __m128i vDeadFlipped = _mm_xor_si128(vDead, vSignFlip);

            const __m128i cmp32  = _mm_cmpgt_epi32(vNowFlipped, vDeadFlipped);
            const __m128i eq32   = _mm_cmpeq_epi32(vNowFlipped, vDeadFlipped);
            const __m128i cmpLow = _mm_shuffle_epi32(cmp32, _MM_SHUFFLE(2, 2, 0, 0));
            const __m128i cmpHi  = _mm_shuffle_epi32(cmp32, _MM_SHUFFLE(3, 3, 1, 1));
            const __m128i eqHi   = _mm_shuffle_epi32(eq32,  _MM_SHUFFLE(3, 3, 1, 1));
            const __m128i gt64   = _mm_or_si128(cmpHi, _mm_and_si128(eqHi, cmpLow));

            const int mask2 = _mm_movemask_pd(_mm_castsi128_pd(gt64));
            return static_cast<uint64_t>(static_cast<uint32_t>(mask2) & 0x3U) << (pair * 2);
        };

        if ((active >> 2) == 0ULL) {
            return evalPair(0) & active;
        }

        uint64_t expiredMask = 0ULL;
        for (size_t pair = 0; pair < 32; ++pair) {
            if (((active >> (pair * 2)) & 0x3ULL) == 0ULL) {
                continue;
            }
            expiredMask |= evalPair(pair);
        }

        return expiredMask & active;
    }

    [[nodiscard]] __forceinline uint64_t NextSetBitIndex(uint64_t mask) const noexcept {
        if (hasAvx2Bmi2_) {
            return _tzcnt_u64(mask);
        }
        unsigned long index = 0;
        _BitScanForward64(&index, mask);
        return static_cast<uint64_t>(index);
    }

    /**
     * @brief Handles an incoming physical key/button transition from the input queue.
     */
    void HandleTriggerTransition(const mask::core::InputTriggerEvent& ev, uint64_t currentTsc) noexcept {
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
                            // Between presses, or holding at 100% duty: stop now. In the middle of a timed
                            // press: AdvanceSlotStateMachine finishes that press cleanly and then stops.
                            if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE ||
                                arena_.recoveryDurationTsc[slot] == 0ULL) {
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

    /**
     * @brief Deterministic SOCD (Last-Input Priority / Snap Tap) resolution.
     * The slot asserts at most one of its two keys: the most recently pressed one that is still held.
     * Switching from one to the other is a single report, so there is no neutral frame and no overlap.
     */
    void ResolveSocdLastInputPriority(size_t slot, uint16_t changedVk, bool isDown) noexcept {
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

    /**
     * @brief Stops everything whose output depends on a physical key staying held (Hold slots and snap-tap).
     * Toggles, bursts and sequences keep running.
     */
    void ReleaseTriggerFollowingSlots() noexcept {
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

    void ReleaseSocdSlot(size_t slot) noexcept {
        const uint16_t assertedVk = arena_.socdAssertedVk[slot];
        if (assertedVk != 0U) {
            dispatcher_.DispatchKeyTransition(assertedVk, false);
            arena_.socdAssertedVk[slot] = 0;
        }
    }

    /**
     * @brief Ensures any Hold-to-Loop slot is synced with live physical trigger state
     * so triggers are NEVER ignored or stuck even if an edge event was missed.
     */
    void SyncHoldTriggerStates(uint64_t currentTsc, bool allowStart) noexcept {
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
                // Level-triggered auto-start: physical trigger is held and the slot is idle
                if (!isActive && allowStart) {
                    StartSlot(slot, currentTsc);
                }
            } else if (isActive) {
                // Trigger is no longer held. A timed press in progress is allowed to finish (the dwell
                // deadline stops the slot); between presses, or at 100% duty, stop immediately.
                if (arena_.phase[slot] != mask::core::SlotPhase::PHASE_1_DWELL_ACTIVE ||
                    arena_.recoveryDurationTsc[slot] == 0ULL) {
                    StopAndReleaseSlot(slot);
                }
            }
        }
    }

    /**
     * @brief Advances a single slot's split-phase state machine when its TSC deadline expires.
     */
    void AdvanceSlotStateMachine(size_t slot, uint64_t currentTsc) noexcept {
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
                    const uint8_t count = std::max<uint8_t>(1U, arena_.sequenceStepCount[slot]);
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

                const uint64_t recoveryTsc = arena_.recoveryDurationTsc[slot];
                if (recoveryTsc == 0ULL) {
                    if (mask::input::InputDispatcher::IsWheelVirtualKey(arena_.outputVk[slot])) {
                        if (mode == mask::core::MacroMode::RepeatNTimes) {
                            if (arena_.remainingRepeats[slot] > 1U) {
                                --arena_.remainingRepeats[slot];
                            } else {
                                arena_.remainingRepeats[slot] = 0U;
                                StopAndReleaseSlot(slot);
                                return;
                            }
                        }
                        EnterPressPhase(slot, targetTsc, currentTsc, /*seamless=*/false);
                        return;
                    }
                    // ---- 100% duty cycle: the output simply stays down. No release, no re-press. -----
                    if (mode == mask::core::MacroMode::RepeatNTimes) {
                        if (arena_.remainingRepeats[slot] > 1U) {
                            --arena_.remainingRepeats[slot];
                        } else {
                            arena_.remainingRepeats[slot] = 0U;
                            StopAndReleaseSlot(slot);
                            return;
                        }
                    }
                    const bool movesPointer = (arena_.mouseDeltaX[slot] != 0 || arena_.mouseDeltaY[slot] != 0);
                    if (movesPointer) {
                        dispatcher_.DispatchMouseMoveUncoalesced(arena_.mouseDeltaX[slot], arena_.mouseDeltaY[slot]);
                    }
                    // Nothing periodic left to do? Then park: the engine sleeps until the trigger changes
                    // instead of waking every cycle just to keep holding.
                    const bool needsTick = movesPointer || mode == mask::core::MacroMode::RepeatNTimes;
                    arena_.nextDeadlineTsc[slot] = needsTick
                        ? NextDeadline(targetTsc, currentTsc, ApplyHumanizeJitterTsc(arena_.dwellDurationTsc[slot], arena_.humanizePct[slot]))
                        : kNoDeadline;
                    return;
                }

                ReleaseSlotOutput(slot);
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
                            const uint8_t count = std::max<uint8_t>(1U, arena_.sequenceStepCount[slot]);
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

    void StartSlot(size_t slot, uint64_t currentTsc) noexcept {
        arena_.activeMask |= (1ULL << slot);
        EnterPressPhase(slot, kNoDeadline, currentTsc, /*seamless=*/false);
    }

    /**
     * @brief Presses the slot's output (or the current sequence step's key) and arms the press deadline.
     * With `seamless`, whatever the slot is still holding is swapped for the new key in one report; if it
     * is the same key it just stays down.
     */
    void EnterPressPhase(size_t slot, uint64_t previousTargetTsc, uint64_t currentTsc, bool seamless) noexcept {
        const auto mode = static_cast<mask::core::MacroMode>(arena_.mode[slot]);

        uint16_t outVk = arena_.outputVk[slot];
        uint8_t  outIsMouse = arena_.isMouse[slot];
        uint64_t dwellTsc = arena_.dwellDurationTsc[slot];

        if (mode == mask::core::MacroMode::Sequence) {
            const uint8_t count = std::max<uint8_t>(1U, arena_.sequenceStepCount[slot]);
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

    void ReleaseSlotOutput(size_t slot) noexcept {
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

    void StopAndReleaseSlot(size_t slot) noexcept {
        ReleaseSlotOutput(slot); // no-op unless this slot is holding something
        const uint64_t bit = (1ULL << slot);
        arena_.activeMask &= ~bit;
        arena_.toggleMask &= ~bit;
        arena_.phase[slot] = mask::core::SlotPhase::IDLE;
        arena_.nextDeadlineTsc[slot] = kNoDeadline;
    }

    [[nodiscard]] __forceinline uint64_t NextRandomU64() noexcept {
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

    [[nodiscard]] __forceinline uint64_t ApplyHumanizeJitterTsc(uint64_t baseTsc, uint8_t pct) noexcept {
        if (pct == 0U || baseTsc == 0ULL) {
            return baseTsc;
        }
        const uint64_t p = static_cast<uint64_t>(std::min<uint8_t>(pct, uint8_t{50}));
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

    mask::core::EngineSharedState&  sharedState_;
    mask::timing::TimingEngine&     timingEngine_;
    mask::input::InputDispatcher&   dispatcher_;
    MacroArenaSoA                   arena_{};
    bool                            hasAvx2Bmi2_{false};
    bool                            wasSuppressed_{false};
    uint64_t                        totalHookEventsProcessed_{0};
    uint64_t                                      prngState_{0x9E3779B97F4A7C15ULL};
    uint64_t                                      minPulseFloorTsc_{0};
    std::array<AuxTimedRelease, kMaxAuxTimedReleases> auxTimedReleases_{};
    uint16_t                                      auxTimedActiveMask_{0};
    std::atomic<ComplexSeqTriggerCallback>        complexSeqTriggerCallback_{nullptr};
};

} // namespace mask::runtime
