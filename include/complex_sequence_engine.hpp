#pragma once

// Multi-Step Workflow / Complex Sequence Engine (up to 16 concurrent sequences, 128 steps each).
// Runs on a dedicated event-driven background thread (0.00% CPU while idle) using TimingEngine's
// persistent high-resolution waitable timer + auto-reset wake event + Eco adaptive hybrid tail,
// serializing all hardware outputs through EngineSharedState::auxOutputQueue.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core_contracts.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mask::workflows {

using PixelSampleFn     = bool(*)(int32_t x, int32_t y, uint8_t& r, uint8_t& g, uint8_t& b) noexcept;
using ToggleMacroFn     = void(*)(uint8_t macroSlot) noexcept;
using CommandApprovalFn = void(*)(uint8_t sequenceIndex, uint16_t stepIndex, bool isShellCommand, const wchar_t* commandText) noexcept;

inline constexpr uint32_t kComplexSequencesMagic     = 0x4D534551U; // 'MSEQ'
inline constexpr uint32_t kComplexSequencesVersionV1 = 1U;
inline constexpr uint32_t kComplexSequencesVersion   = 2U;
inline constexpr size_t   kMaxLoopNesting            = 8U;

/**
 * @brief Multi-Step Complex Sequence Engine supporting up to 16 concurrent workflows (128 steps each).
 *
 * - Runs on a dedicated event-driven background thread (0.00% CPU when idle).
 * - Uses a persistent high-resolution waitable timer + auto-reset wake event for instant cancellation.
 * - Zero heap allocations on the execution path (fixed loop stack, fixed key-hold bitsets, fixed async launch slots).
 */
class ComplexSequenceEngine {
public:
    explicit ComplexSequenceEngine(
        mask::core::EngineSharedState& sharedState,
        PixelSampleFn pixelSampler = nullptr
    ) noexcept;
    ~ComplexSequenceEngine() noexcept;

    ComplexSequenceEngine(const ComplexSequenceEngine&) = delete;
    ComplexSequenceEngine& operator=(const ComplexSequenceEngine&) = delete;

    void Start();
    void Stop() noexcept;

    void SetPixelSampler(PixelSampleFn pixelSampler) noexcept;
    void SetToggleMacroCallback(ToggleMacroFn toggleMacro) noexcept;
    void SetCommandApprovalCallback(CommandApprovalFn cb) noexcept;
    void ResolveCommandApproval(uint8_t sequenceIndex, bool allowAndTrust) noexcept;

    void UpdateConfig(size_t index, const mask::core::ComplexSequenceConfig& cfg) noexcept;
    void UpdateAllConfigs(
        const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& cfgs
    ) noexcept;

    void NotifyPhysicalKeyTransition(uint16_t vk, bool isDown) noexcept;
    void TriggerSequence(size_t index) noexcept;
    void AbortAll() noexcept;

    [[nodiscard]] uint16_t GetActiveSequenceBitmask() const noexcept;
    [[nodiscard]] uint16_t GetCurrentStepIndex(size_t index) const noexcept;
    [[nodiscard]] bool     IsSequenceWaitingForApproval(size_t index) const noexcept;

    /**
     * @brief Deterministic offline verification of ComplexSequenceEngine conditionals,
     *        loop nesting, and untrusted imported command approval gating.
     */
    [[nodiscard]] static bool RunControlFlowSelfTest() noexcept;

    [[nodiscard]] static bool SaveSequences(
        const wchar_t* path,
        const std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& cfgs
    ) noexcept;

    [[nodiscard]] static bool LoadSequences(
        const wchar_t* path,
        std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>& outCfgs
    ) noexcept;

    /**
     * @brief Verifies v1 -> v2 sequences.bin binary migration (preserving uncapped timings and
     *        kSeqFlagUntrustedImport) using %TEMP%.
     */
    [[nodiscard]] static bool RunSaveMigrationSelfTest() noexcept;

private:
    static constexpr uint32_t kMaxImmediateStepsPerTick = 64U;
    static constexpr uint32_t kColorPollIntervalMs      = 4U;

    struct LoopFrame {
        uint16_t startStep{0};
        uint32_t remainingIterations{0};
        bool     infinite{false};
    };

    struct SequenceRuntimeState {
        bool     active{false};
        bool     toggleLatched{false};
        bool     manualOneShot{false};
        bool     waitingForTimer{false};
        bool     waitingForColor{false};
        bool     waitingForCommandApproval{false};
        bool     approvalPromptSent{false};
        uint16_t approvalPendingStep{0};
        uint16_t currentStep{0};
        uint64_t waitDeadlineQpc{0};
        uint64_t phaseAnchorQpc{0};
        uint64_t nextColorPollQpc{0};
        uint64_t colorTimeoutQpc{0}; // 0 = no timeout
        uint32_t lastSeenToggleEdges{0};
        uint64_t rngState{0x9E3779B97F4A7C15ULL};
        uint8_t  loopDepth{0};
        std::array<LoopFrame, kMaxLoopNesting> loopStack{};
        std::array<uint64_t, 4> heldVkBits{};
    };

    static DWORD WINAPI ThreadEntry(LPVOID param) noexcept;
    void SignalWake() const noexcept;

    [[nodiscard]] static uint64_t NowQpc() noexcept;
    [[nodiscard]] static uint64_t NowTsc() noexcept;
    [[nodiscard]] uint64_t MsToQpc(uint64_t ms) const noexcept;
    [[nodiscard]] uint64_t QpcDeltaToNs(uint64_t deltaQpc) const noexcept;

    void PublishTriggerCache(size_t index, const mask::core::ComplexSequenceConfig& cfg) noexcept;
    void PushAuxCommand(const mask::core::AuxOutputCommand& cmd) noexcept;
    [[nodiscard]] bool IsKeyHeldByOtherSequence(size_t ignoreIndex, uint16_t vk) const noexcept;
    void ReleaseSequenceHeldKeys(size_t index) noexcept;
    void StopSequenceSlot(size_t index) noexcept;
    void AbortAllWorker() noexcept;
    void StartSequenceSlot(size_t index, bool manualOneShot) noexcept;
    void SyncDirtyConfigs() noexcept;
    void DispatchAsyncLaunch(const wchar_t text[128], bool isCommand) noexcept;
    void ProcessPendingTriggers() noexcept;
    [[nodiscard]] bool EvaluateCondition(
        const mask::core::ComplexStep& step,
        SequenceRuntimeState& rt
    ) const noexcept;
    [[nodiscard]] static uint16_t FindConditionalJumpTarget(
        const mask::core::ComplexSequenceConfig& cfg,
        uint16_t fromStep,
        bool stopAtElse
    ) noexcept;
    [[nodiscard]] uint64_t AdvanceSequence(size_t index, uint64_t nowQpc) noexcept;
    void WorkerLoop() noexcept;

    using ConfigArray = std::array<mask::core::ComplexSequenceConfig, mask::core::kMaxComplexSequences>;

    mask::core::EngineSharedState& sharedState_;
    std::atomic<PixelSampleFn>     pixelSampler_{nullptr};
    std::atomic<ToggleMacroFn>     toggleMacroFn_{nullptr};
    std::atomic<CommandApprovalFn> commandApprovalCb_{nullptr};

    SRWLOCK                                                            configLock_{};
    std::unique_ptr<ConfigArray>                                       configs_;
    std::unique_ptr<ConfigArray>                                       workerConfigs_;
    std::array<SequenceRuntimeState, mask::core::kMaxComplexSequences> runtime_{};

    // Wait-free trigger cache read by NotifyPhysicalKeyTransition
    std::array<std::atomic<uint16_t>, mask::core::kMaxComplexSequences> cachedTriggerVk_{};
    std::array<std::atomic<uint8_t>,  mask::core::kMaxComplexSequences> cachedTriggerMeta_{};
    std::array<std::atomic<uint8_t>,  mask::core::kMaxComplexSequences> heldTriggerState_{};
    std::array<std::atomic<uint32_t>, mask::core::kMaxComplexSequences> toggleEdgeCounts_{};

    // Lock-free command approval resolution & waiting state
    std::array<std::atomic<uint8_t>, mask::core::kMaxComplexSequences> pendingApprovalDecision_{};
    std::array<std::atomic<bool>,    mask::core::kMaxComplexSequences> waitingApprovalTelemetry_{};

    // Telemetry
    std::atomic<uint16_t> activeSequenceBitmask_{0};
    std::array<std::atomic<uint16_t>, mask::core::kMaxComplexSequences> currentStepIndices_{};

    // Lock-free command masks
    std::atomic<uint16_t> configDirtyMask_{0};
    std::atomic<uint16_t> pendingStartMask_{0};
    std::atomic<uint16_t> pendingManualTriggerMask_{0};
    std::atomic<bool>     abortRequested_{false};
    std::atomic<bool>     running_{false};
    std::atomic<bool>     stopRequested_{false};

    // Offline self-test hook so command approval tests never spawn real OS processes
    bool     dryRunLaunchesForTest_{false};
    uint32_t testLaunchCount_{0};
    bool     testLastLaunchWasCmd_{false};
    wchar_t  testLastLaunchText_[128]{};

    uint64_t qpcFreqHz_{10'000'000ULL};
    HANDLE   wakeEvent_{nullptr};
    HANDLE   workerThread_{nullptr};
};

} // namespace mask::workflows
