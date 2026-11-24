#pragma once

// Multi-Keyboard, Multi-Mouse, Multi-Monitor Remapping Engine + Layer / Unlock State
// + Unified Hook-Driven RemapHookPipeline + Immediate Raw Input Enumeration & Correlation.
// Zero Win32 hooks are installed when all remap rules are disabled or automation is paused.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core_contracts.hpp"
#include "input_pipeline.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mask::remap {

inline constexpr uint32_t kRemapRulesMagic     = 0x4D524D50U; // 'MRMP'
inline constexpr uint32_t kRemapRulesVersionV1 = 1U;
inline constexpr uint32_t kRemapRulesVersion   = 2U;
inline constexpr size_t   kMaxMonitors         = 8U;

struct MonitorRectInfo {
    int32_t  left{0};
    int32_t  top{0};
    int32_t  right{1920};
    int32_t  bottom{1080};
    uint32_t dpi{96};
    bool     isPrimary{true};
    wchar_t  name[32]{L"Monitor 1"};
};

/**
 * @brief Multi-Keyboard, Multi-Mouse, Multi-Monitor Remapping Engine with Layers, Unlock Policy,
 *        Monitor Warp, Immediate Raw Input Enumeration, and Unified `RemapHookPipeline`.
 */
class RemapEngine {
public:
    explicit RemapEngine(mask::core::EngineSharedState& sharedState) noexcept;
    ~RemapEngine() noexcept;

    RemapEngine(const RemapEngine&) = delete;
    RemapEngine& operator=(const RemapEngine&) = delete;

    void Start();
    void Stop() noexcept;

    void RefreshMonitors() noexcept;
    [[nodiscard]] int GetMonitors(std::array<MonitorRectInfo, 8>& outMonitors) const noexcept;

    /**
     * @brief Immediately enumerates all physical keyboards and mice via Win32 Raw Input (`GetRawInputDeviceList`),
     * filtering out HIDMaestro virtual VID/PIDs and RDP pseudo-devices, resolving friendly product names.
     */
    void RefreshRawInputDevices() noexcept;

    /**
     * @brief Thread-safe snapshot of all physical keyboards & mice populated by Raw Input at startup.
     */
    [[nodiscard]] int GetRawDevices(
        std::array<mask::input::PhysicalDeviceInfo, mask::input::kMaxPhysicalGameInputDevices>& outList
    ) const noexcept;

    void UpdateRule(size_t index, const mask::core::RemapRuleConfig& rule) noexcept;
    void UpdateAllRules(
        const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& rules
    ) noexcept;

    // Called by GameInput per-device transition callback: records per-device correlation and
    // acts as a clean fallback ONLY when the LL hook for that device kind is not currently active.
    void OnPhysicalDeviceTransition(
        const uint8_t deviceId[32],
        uint8_t deviceKind,
        uint16_t vk,
        bool isDown,
        uint64_t nowTsc
    ) noexcept;

    void ResetStateAndSuppression() noexcept;
    void RecomputeSuppressedMask() noexcept;

    [[nodiscard]] static bool SaveRemaps(
        const wchar_t* path,
        const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& rules
    ) noexcept;

    [[nodiscard]] static bool LoadRemaps(
        const wchar_t* path,
        std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& outRules
    ) noexcept;

    /**
     * @brief Deterministic offline verification of the hook-driven remap pipeline.
     */
    [[nodiscard]] static bool RunRemapSelfTest() noexcept;

    /**
     * @brief Verifies v1 -> v2 remaps.bin binary migration using %TEMP%.
     */
    [[nodiscard]] static bool RunSaveMigrationSelfTest() noexcept;

private:
    static constexpr size_t kMaxActiveHolds  = 32U;
    static constexpr size_t kMaxRawInventory = mask::input::kMaxPhysicalGameInputDevices;
    static constexpr size_t kMaxRawHandles   = 64U;

    static std::atomic<RemapEngine*> activeInstance_;

    struct RawHandleEntry {
        HANDLE   hDevice{nullptr};
        uint8_t  deviceIdBytes[32]{};
        uint16_t vendorId{0};
        uint16_t productId{0};
        uint8_t  devKind{0}; // 1 = Keyboard, 2 = Mouse
        bool     isVirtual{false};
    };

    struct CorrelatedDevice {
        bool     hasRawId{false};
        bool     hasGiId{false};
        uint8_t  rawIdBytes[32]{};
        uint8_t  giIdBytes[32]{};
        uint16_t vendorId{0};
        uint16_t productId{0};
    };

    struct RawTransitionRecord {
        bool     valid{false};
        uint8_t  deviceIdBytes[32]{};
        uint16_t vendorId{0};
        uint16_t productId{0};
        uint64_t timestampTsc{0};
    };

    struct GiTransitionRecord {
        bool     valid{false};
        uint8_t  deviceIdBytes[32]{};
        uint64_t timestampTsc{0};
    };

    struct ActiveRemapHold {
        bool     active{false};
        bool     hasDeviceId{false};
        bool     wasSuppressed{false};
        uint8_t  deviceId[32]{};
        uint16_t sourceVk{0};
        uint16_t emittedTargetVk{0};
        uint8_t  actionKind{0};
        uint8_t  prevLayer{0};
    };

    struct ActiveHoldInfo {
        bool wasSuppressed{false};
    };

    void SyncHooks(bool wantKeyboardHook, bool wantMouseHook) noexcept;
    void RequestSyncFromHookThread(bool wantKeyboardHook, bool wantMouseHook) noexcept;
    void ShutdownHooks() noexcept;

    static DWORD WINAPI HookThreadEntry(LPVOID param) noexcept;
    static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept;
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept;

    void SyncInstalledHooks(HHOOK& kbHook, HHOOK& mouseHook) noexcept;
    void HookThreadLoop() noexcept;
    void HandleRawInputMessage(HRAWINPUT hRawInput) noexcept;

    [[nodiscard]] bool ProcessHookKeyEvent(
        uint16_t rawVk,
        DWORD scanCode,
        DWORD flags,
        bool isDown,
        uint64_t nowTsc
    ) noexcept;
    [[nodiscard]] bool ProcessHookMouseButtonEvent(
        uint16_t vk,
        bool isDown,
        DWORD flags,
        uint64_t nowTsc
    ) noexcept;

    void RecordGameInputCorrelation(
        const uint8_t deviceId[32],
        uint8_t deviceKind,
        uint16_t vk,
        bool isDown,
        uint64_t nowTsc
    ) noexcept;
    void ResolveAliasVidPidFromGi(const uint8_t giId[32], uint16_t& outVid, uint16_t& outPid) const noexcept;
    [[nodiscard]] CorrelatedDevice GetCorrelatedDevice(
        uint8_t deviceKind,
        uint16_t vk,
        uint64_t nowTsc,
        uint64_t windowTsc
    ) const noexcept;
    [[nodiscard]] bool MatchesCorrelatedDevice(
        const mask::core::RemapRuleConfig& rule,
        const CorrelatedDevice& corr
    ) const noexcept;
    [[nodiscard]] bool FindMatchingRule(
        uint16_t vk,
        uint8_t deviceKind,
        uint8_t activeLayer,
        bool unlocked,
        const CorrelatedDevice& corr,
        mask::core::RemapRuleConfig& outWinner
    ) const noexcept;
    [[nodiscard]] static bool MatchesUnlockPolicy(uint8_t policyRaw, bool unlocked) noexcept;
    [[nodiscard]] static bool SameDevice(const ActiveRemapHold& h, const uint8_t deviceId[32]) noexcept;
    [[nodiscard]] bool IsPointInMonitor(const POINT& pt, int8_t monitorIndex) const noexcept;
    [[nodiscard]] bool FindExistingHold(
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        ActiveHoldInfo& outInfo
    ) const noexcept;
    void RecordActiveHold(
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint16_t emittedTargetVk,
        uint8_t actionKind,
        uint8_t prevLayer,
        bool wasSuppressed
    ) noexcept;
    bool HandleKeyRelease(
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint64_t nowTsc,
        ActiveHoldInfo* outReleasedInfo = nullptr
    ) noexcept;
    void ExecuteRuleOnDown(
        const mask::core::RemapRuleConfig& rule,
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint8_t currentLayer,
        bool currentlyUnlocked,
        uint64_t nowTsc,
        bool wasSuppressed
    ) noexcept;

    mask::core::EngineSharedState& sharedState_;
    std::atomic<bool>              started_{false};
    std::atomic<uint64_t>          echoWindowTsc_{150'000'000ULL};

    mutable SRWLOCK ruleLock_{};
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules_{};

    mutable SRWLOCK monitorLock_{};
    std::array<MonitorRectInfo, kMaxMonitors> monitors_{};
    int monitorCount_{0};

    mutable SRWLOCK holdLock_{};
    std::array<ActiveRemapHold, kMaxActiveHolds> activeHolds_{};

    mutable SRWLOCK rawLock_{};
    std::array<RawHandleEntry, kMaxRawHandles>                    rawHandleMap_{};
    size_t                                                        rawHandleCount_{0};
    std::array<mask::input::PhysicalDeviceInfo, kMaxRawInventory> rawDevices_{};
    int                                                           rawDeviceCount_{0};

    mutable SRWLOCK corrLock_{};
    RawTransitionRecord                  mostRecentKbRaw_{};
    RawTransitionRecord                  mostRecentMouseRaw_{};
    std::array<RawTransitionRecord, 256> perVkRaw_{};
    GiTransitionRecord                   mostRecentGiKb_{};
    GiTransitionRecord                   mostRecentGiMouse_{};
    std::array<GiTransitionRecord, 256>  perVkGi_{};

    mutable SRWLOCK    hookLock_{};
    HANDLE             hookThreadHandle_{nullptr};
    HANDLE             hookReadyEvent_{nullptr};
    std::atomic<DWORD> hookThreadId_{0};
    std::atomic<bool>  desiredActive_{false};
    std::atomic<bool>  wantKeyboard_{false};
    std::atomic<bool>  wantMouse_{false};
    std::atomic<bool>  kbHookActive_{false};
    std::atomic<bool>  mouseHookActive_{false};
    std::atomic<bool>  rawRefreshQueued_{false};
};

} // namespace mask::remap
