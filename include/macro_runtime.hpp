#pragma once

#include "core_contracts.hpp"
#include "timing_engine.hpp"
#include "input_pipeline.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <limits>

namespace mask::runtime {

/**
 * @brief Isolated AVX2 deadline evaluation entry point (compiled solely in `src/macro_runtime_avx2.cpp` with `/arch:AVX2`).
 */
[[nodiscard]] uint64_t EvaluateExpiredMaskAvx2Bmi2Impl(
    const uint64_t* deadlines,
    uint64_t activeMask,
    uint64_t currentTsc
) noexcept;

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
 * Layouts are aligned to 64-byte boundaries for single-instruction AVX2/SSE2 vector loads.
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
    alignas(mask::core::kCacheLineSize) std::array<uint16_t, mask::core::kMaxMacroSlots> socdAssertedVk{};
    alignas(mask::core::kCacheLineSize) std::array<std::array<SequenceStepTsc, mask::core::kMaxSequenceSteps>, mask::core::kMaxMacroSlots> sequenceSteps{};

    // Bitmasks for branchless active-slot evaluation
    uint64_t configuredMask{0}; // Bit i = 1 if slot i is configured and enabled
    uint64_t activeMask{0};     // Bit i = 1 if slot i is currently running Phase 1 or Phase 2
    uint64_t toggleMask{0};     // Bit i = 1 if an InfiniteToggleLoop or Sequence slot is toggled ON
};

/**
 * @brief SIMD & Data-Oriented Macro Runtime.
 * Detects AVX2 + BMI2 via __cpuid at startup, evaluates 64 macro slot deadlines using
 * either isolated AVX2 (`src/macro_runtime_avx2.cpp`) or an SSE2 baseline, respects raw unrestricted
 * dwell/recovery timings (including safe 0 ms / >1000 CPS autoclicking), and executes all 5 modes.
 */
class alignas(mask::core::kCacheLineSize) MacroRuntime {
public:
    using ComplexSeqTriggerCallback = void(*)(uint8_t sequenceIndex) noexcept;

    MacroRuntime(
        mask::core::EngineSharedState& sharedState,
        mask::timing::TimingEngine& timingEngine,
        mask::input::InputDispatcher& dispatcher
    ) noexcept;

    ~MacroRuntime() noexcept = default;

    MacroRuntime(const MacroRuntime&) = delete;
    MacroRuntime& operator=(const MacroRuntime&) = delete;

    void ResetArena() noexcept;
    void SetComplexSeqTriggerCallback(ComplexSeqTriggerCallback cb) noexcept;
    void DetectCpuCapabilities() noexcept;

    [[nodiscard]] bool HasAvx2Bmi2() const noexcept {
        return hasAvx2Bmi2_;
    }

    bool ConfigureSlot(const mask::core::MacroConfig& cfg) noexcept;
    void ProcessPendingQueues(uint64_t currentTsc) noexcept;
    void EvaluateAndExecute(uint64_t currentTsc) noexcept;

    [[nodiscard]] uint64_t GetEarliestActiveDeadlineTsc() const noexcept;
    void TriggerMacroSlot(uint8_t slotIndex, uint64_t currentTsc) noexcept;

    [[nodiscard]] uint32_t GetActiveSlotCount() const noexcept;

    [[nodiscard]] uint64_t GetActiveSlotBitmask() const noexcept {
        return arena_.activeMask;
    }

    [[nodiscard]] uint64_t GetToggledSlotBitmask() const noexcept {
        return arena_.toggleMask;
    }

    [[nodiscard]] uint32_t GetConfiguredSlotCount() const noexcept;

    [[nodiscard]] uint64_t GetTotalHookEventsProcessed() const noexcept {
        return totalHookEventsProcessed_;
    }

    void AbortAllActiveSlots() noexcept;

    [[nodiscard]] static bool RunSimdEquivalenceSelfTest() noexcept;

private:
    static constexpr uint64_t kNoDeadline          = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    static constexpr size_t   kMaxAuxTimedReleases = 16;
    static constexpr size_t   kMaxAuxBatchPerPass  = 64;

    struct AuxTimedRelease {
        uint64_t releaseDeadlineTsc{0};
        uint16_t vk{0};
        bool     active{false};
        uint8_t  reserved[5]{0, 0, 0, 0, 0};
    };

    void ExecuteAuxOutputCommand(const mask::core::AuxOutputCommand& cmd, uint64_t currentTsc) noexcept;
    void ScheduleAuxTimedRelease(uint16_t vk, uint64_t releaseDeadlineTsc) noexcept;
    void CancelAuxTimedReleaseForVk(uint16_t vk) noexcept;
    void ProcessAuxTimedReleases(uint64_t currentTsc) noexcept;
    void ReleaseAllAuxTimedReleases() noexcept;

    [[nodiscard]] bool TriggersSuppressed() const noexcept;
    [[nodiscard]] bool IsPhysicallyHeld(uint16_t vk) const noexcept;
    [[nodiscard]] static uint64_t NextDeadline(uint64_t previousTargetTsc, uint64_t nowTsc, uint64_t durationTsc) noexcept;

    [[nodiscard]] uint64_t EvaluateExpiredMaskAvx2Bmi2(uint64_t currentTsc) const noexcept;
    [[nodiscard]] uint64_t EvaluateExpiredMaskSse2(uint64_t currentTsc) const noexcept;
    [[nodiscard]] static uint64_t NextSetBitIndex(uint64_t mask) noexcept;

    void HandleTriggerTransition(const mask::core::InputTriggerEvent& ev, uint64_t currentTsc) noexcept;
    void ResolveSocdLastInputPriority(size_t slot, uint16_t changedVk, bool isDown) noexcept;
    void ReleaseTriggerFollowingSlots() noexcept;
    void ReleaseSocdSlot(size_t slot) noexcept;
    void SyncHoldTriggerStates(uint64_t currentTsc, bool allowStart) noexcept;
    void AdvanceSlotStateMachine(size_t slot, uint64_t currentTsc) noexcept;
    void StartSlot(size_t slot, uint64_t currentTsc) noexcept;
    void EnterPressPhase(size_t slot, uint64_t previousTargetTsc, uint64_t currentTsc, bool seamless) noexcept;
    void ReleaseSlotOutput(size_t slot) noexcept;
    void StopAndReleaseSlot(size_t slot) noexcept;

    [[nodiscard]] uint64_t NextRandomU64() noexcept;
    [[nodiscard]] uint64_t ApplyHumanizeJitterTsc(uint64_t baseTsc, uint8_t pct) noexcept;

    mask::core::EngineSharedState&                    sharedState_;
    mask::timing::TimingEngine&                       timingEngine_;
    mask::input::InputDispatcher&                     dispatcher_;
    MacroArenaSoA                                     arena_{};
    bool                                              hasAvx2Bmi2_{false};
    bool                                              wasSuppressed_{false};
    uint64_t                                          totalHookEventsProcessed_{0};
    uint64_t                                          prngState_{0x9E3779B97F4A7C15ULL};
    uint64_t                                          minPulseFloorTsc_{0};
    std::array<AuxTimedRelease, kMaxAuxTimedReleases> auxTimedReleases_{};
    uint16_t                                          auxTimedActiveMask_{0};
    std::atomic<ComplexSeqTriggerCallback>            complexSeqTriggerCallback_{nullptr};
};

} // namespace mask::runtime
