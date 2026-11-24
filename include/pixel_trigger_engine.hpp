#pragma once

// Pure Ring-3 user-mode Screen / Pixel Color Trigger engine.
// ZERO process memory reading, ZERO DLL injection.
// Uses standard Windows DXGI Desktop Duplication (ID3D11Device + IDXGIOutputDuplication)
// into a reusable 32x32 D3D11_USAGE_STAGING texture with zero per-frame heap allocations,
// gracefully falling back to a persistent cached 32x32 GDI DIBSection + BitBlt ROI capture.

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
#include <type_traits>

namespace mask::triggers {

inline constexpr uint32_t kTriggersFileMagic     = 0x4D545247U; // 'MTRG'
inline constexpr uint32_t kTriggersFileVersionV1 = 1U;
inline constexpr uint32_t kTriggersFileVersion   = 2U;
inline constexpr size_t   kMaxMonitors           = 8;

struct MonitorInfoEntry {
    int32_t  left{0}, top{0}, width{1920}, height{1080};
    uint32_t dpi{96};
    bool     isPrimary{true};
    wchar_t  name[32]{L"Monitor 1"};
};

struct ColorTriggerPreviewSnapshot {
    static constexpr int kLoupeSize = 15; // 15x15 pixels (0x00RRGGBB)
    uint32_t loupePixels[kLoupeSize * kLoupeSize]{};
    uint8_t  loupeMatchMask[kLoupeSize * kLoupeSize]{}; // 0=outside zone, 1=inside zone, 3=inside zone + matched
    uint8_t  sampledR{0}, sampledG{0}, sampledB{0};
    uint32_t colorDelta{0};
    int32_t  sampleX{0}, sampleY{0};
    uint32_t lastPipelineUs{0};
    uint32_t avgPipelineUs{0};
    bool     isMatched{false};
    bool     isArmed{false};
    bool     usingDxgi{false};
    uint64_t triggersFired{0};
};

static_assert(std::is_trivially_copyable_v<ColorTriggerPreviewSnapshot>,
              "ColorTriggerPreviewSnapshot must remain trivially copyable for seqlock transfer.");

class PixelTriggerEngine {
public:
    explicit PixelTriggerEngine(mask::core::EngineSharedState& sharedState) noexcept;
    ~PixelTriggerEngine() noexcept;

    PixelTriggerEngine(const PixelTriggerEngine&) = delete;
    PixelTriggerEngine& operator=(const PixelTriggerEngine&) = delete;

    void Start();
    void Stop() noexcept;

    void SetPreviewActive(bool active, uint8_t previewSlotIndex) noexcept;
    void UpdateConfig(size_t index, const mask::core::ColorTriggerConfig& cfg) noexcept;
    void UpdateAllConfigs(
        const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& cfgs
    ) noexcept;

    [[nodiscard]] ColorTriggerPreviewSnapshot GetPreviewSnapshot() const noexcept;
    void ResetArmedStates() noexcept;

    [[nodiscard]] static int EnumerateMonitors(std::array<MonitorInfoEntry, 8>& outMonitors) noexcept;
    [[nodiscard]] static bool SamplePixelAt(
        int32_t screenX,
        int32_t screenY,
        uint8_t& outR,
        uint8_t& outG,
        uint8_t& outB
    ) noexcept;

    [[nodiscard]] static bool SaveTriggers(
        const wchar_t* path,
        const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& cfgs
    ) noexcept;
    [[nodiscard]] static bool LoadTriggers(
        const wchar_t* path,
        std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& outCfgs
    ) noexcept;

    /**
     * @brief Deterministic offline verification for MatchColor, ColorChanged (zero spam),
     *        HoldKey release, and 15x15 AnalyzeRoi throughput (<50 us/frame).
     */
    [[nodiscard]] static bool RunTriggerSelfTest(double& outAvgUs) noexcept;

    /**
     * @brief Verifies v1 -> v2 triggers.bin binary migration (legacy modes, RapidFire,
     *        coverage reserved[0], reserved[1..2] cleanup, uncapped timings) using %TEMP%.
     */
    [[nodiscard]] static bool RunSaveMigrationSelfTest() noexcept;

private:
    void SignalWake() noexcept;
    void PublishPreview(const ColorTriggerPreviewSnapshot& snap) noexcept;
    void PushAuxCommand(const mask::core::AuxOutputCommand& cmd) noexcept;

    static DWORD WINAPI WorkerThreadProc(LPVOID param) noexcept;
    void WaitForNextTick(HANDLE waitTimer, uint32_t intervalUs) noexcept;
    void WorkerLoop() noexcept;

    mask::core::EngineSharedState& sharedState_;

    SRWLOCK lifecycleLock_ = SRWLOCK_INIT;
    HANDLE  workerThread_{nullptr};
    HANDLE  wakeEvent_{nullptr};

    mutable SRWLOCK configLock_ = SRWLOCK_INIT;
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers> configs_{};
    std::atomic<uint64_t> configVersion_{0};

    std::atomic<bool>    stopRequested_{false};
    std::atomic<bool>    resetRequested_{false};
    std::atomic<bool>    previewActive_{false};
    std::atomic<uint8_t> previewSlotIndex_{0};

    std::atomic<uint64_t>       previewSeq_{0};
    ColorTriggerPreviewSnapshot previewSnapshot_{};
};

} // namespace mask::triggers
