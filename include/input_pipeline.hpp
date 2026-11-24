#pragma once

#include "core_contracts.hpp"
#include "hidmaestro_bridge.hpp"

#include <windows.h>
#include <GameInput.h>
#include <intrin.h>
#include <cstdint>
#include <atomic>
#include <array>

namespace mask::input {

inline constexpr UINT WM_APP_KEY_CAPTURED = WM_APP + 42;
inline constexpr size_t kMaxPhysicalGameInputDevices = 16;

// Exact VID/PID of our two default HIDMaestro virtual controllers so we can filter their readings out
// at the hardware device level (preventing any synthetic macro output from re-triggering inputs).
//   - logitech-g502-hero : VID 0x046D, PID 0xC08B
//   - keychron-q6        : VID 0x3434, PID 0x0160 (also guard legacy 0x0161)
inline constexpr uint16_t kHidMaestroMouseVid       = 0x046DU;
inline constexpr uint16_t kHidMaestroMousePid       = 0xC08BU;
inline constexpr uint16_t kHidMaestroKeyboardVid    = 0x3434U;
inline constexpr uint16_t kHidMaestroKeyboardPid    = 0x0160U;
inline constexpr uint16_t kHidMaestroKeyboardPidAlt = 0x0161U;

/**
 * @brief Snapshot of an observed physical GameInput keyboard/mouse device for UI display and remapping.
 */
struct PhysicalDeviceInfo {
    uint8_t  deviceIdBytes[32]{};
    uint16_t vendorId{0};
    uint16_t productId{0};
    bool     isKeyboard{false};
    bool     isMouse{false};
    wchar_t  displayName[40]{};
};

static_assert(sizeof(APP_LOCAL_DEVICE_ID::value) == 32, "APP_LOCAL_DEVICE_ID::value must be 32 bytes.");

/**
 * @brief Per-device GameInput state slot so multiple physical keyboards/mice never clobber
 * each other's held keys or button masks.
 */
struct PerDeviceInputState {
    APP_LOCAL_DEVICE_ID deviceId{};
    uint64_t            lastKbTimestamp{0};
    uint64_t            lastMouseTimestamp{0};
    uint64_t            kbBits[4]{0, 0, 0, 0};
    uint32_t            mouseButtons{0};
    uint64_t            lastTouchSeq{0};
    uint16_t            vendorId{0};
    uint16_t            productId{0};
    bool                isKeyboard{false};
    bool                isMouse{false};
    bool                hasMetadata{false};
    bool                active{false};
    bool                hasDeviceId{false};
    bool                hasKbReading{false};
    bool                hasMouseReading{false};
    wchar_t             displayName[40]{};
};

/**
 * @brief Global Context for Microsoft GameInput hardware reading callbacks and UI notifications.
 * Opt-in low-level hook remapping lives separately in `RemapEngine`.
 */
struct GameInputGlobalContext {
    using PerDeviceTransitionCallback = void(*)(
        const uint8_t deviceId[32],
        uint8_t deviceKind,
        uint16_t vk,
        bool isDown,
        uint64_t nowTsc
    ) noexcept;

    inline static mask::core::EngineSharedState* sharedState{nullptr};
    inline static std::atomic<IGameInput*>       gameInputInstance{nullptr};
    inline static std::atomic<GameInputCallbackToken> readingToken{0};
    inline static std::atomic<HWND>              uiWindowHandle{nullptr};
    inline static std::atomic<uint64_t>          echoSuppressionWindowTsc{6'000'000ULL}; // ~2ms secondary echo guard
    inline static std::atomic<uint32_t>          activePhysicalDeviceCount{0};
    inline static std::atomic<PerDeviceTransitionCallback> perDeviceCallback{nullptr};

    // Pre-allocated multi-device GameInput state table (zero heap allocation). Readings arrive both from the
    // GameInput callback thread and from the polling thread, so access is serialised by readingLock.
    inline static std::array<PerDeviceInputState, kMaxPhysicalGameInputDevices> deviceStates{};
    inline static std::array<uint64_t, 4>        prevCombinedKbBits{};
    inline static uint32_t                       prevCombinedMouseButtonsMask{0};
    inline static uint64_t                       deviceTouchCounter{0};
    inline static std::atomic_flag               readingLock{};

    // Optional dynamically registered virtual device VID/PID pairs ((uint32_t(vid) << 16) | pid)
    inline static std::array<std::atomic<uint32_t>, 16> registeredVirtualVidPids{};

    // Auto-reset events, created once before any thread starts and closed after they have all stopped.
    inline static HANDLE                         engineWakeEvent{nullptr};
    inline static HANDLE                         watchdogWakeEvent{nullptr};
    inline static HANDLE                         outputWakeEvent{nullptr};
};

void RegisterVirtualDeviceVidPid(uint16_t vendorId, uint16_t productId) noexcept;

void WakeEngine() noexcept;
void WakeWatchdog() noexcept;
void WakeOutput() noexcept;

[[nodiscard]] bool IsSyntheticVirtualDeviceInfo(const GameInputDeviceInfo* info) noexcept;
[[nodiscard]] bool IsSyntheticVirtualDevice(IGameInputDevice* device) noexcept;

void UpdatePhysicalAndEnqueueTransition(
    mask::core::EngineSharedState* state,
    uint16_t vk,
    bool isDown,
    uint64_t nowTsc,
    bool isMouse
) noexcept;

[[nodiscard]] PerDeviceInputState* FindOrAllocatePhysicalDeviceSlot(
    bool hasDeviceId,
    const APP_LOCAL_DEVICE_ID& deviceId
) noexcept;

[[nodiscard]] int CopyPhysicalDeviceInventory(
    std::array<PhysicalDeviceInfo, kMaxPhysicalGameInputDevices>& outList
) noexcept;

void PopulatePhysicalDeviceMetadata(
    PerDeviceInputState& slot,
    uint16_t vendorId,
    uint16_t productId,
    bool isKeyboard,
    bool isMouse,
    const char* utf8DisplayName
) noexcept;

void ProcessGameInputReading(IGameInputReading* reading) noexcept;

void CALLBACK OnGameInputReadingCallback(
    _In_ GameInputCallbackToken callbackToken,
    _In_ void* context,
    _In_ IGameInputReading* reading,
    _In_ bool hasOverrunOccurred
) noexcept;

namespace detail {

[[nodiscard]] constexpr std::array<uint16_t, 256> BuildVkToHidAndModTable() noexcept {
    std::array<uint16_t, 256> table{};
    for (uint16_t vk = 0; vk < 256; ++vk) {
        const uint8_t modBit = mask::bridge::hm::VkToHMModifierBit(vk);
        const uint8_t hidKey = mask::bridge::hm::VkToHMKey(vk);
        table[vk] = static_cast<uint16_t>((static_cast<uint16_t>(modBit) << 8) | static_cast<uint16_t>(hidKey));
    }
    return table;
}

} // namespace detail

/**
 * @brief Pure HIDMaestro v2.1.0 Hardware Input Dispatcher (`SendInput` / `mouse_event` 100% removed).
 * By default, enqueues chronological `OutputBatchItem` frames into `EngineSharedState::outputBatchQueue`
 * for the dedicated MMCSS output thread (`HMBridge_SubmitBatch`), eliminating driver/CoreCLR stalls on
 * the real-time engine thread. Synchronous direct mode remains selectable via `SetBatchQueueMode(false)`.
 */
class alignas(mask::core::kCacheLineSize) InputDispatcher {
public:
    explicit InputDispatcher(mask::core::EngineSharedState& sharedState, bool batchQueueMode = true) noexcept;
    ~InputDispatcher() noexcept;

    InputDispatcher(const InputDispatcher&) = delete;
    InputDispatcher& operator=(const InputDispatcher&) = delete;

    [[nodiscard]] static constexpr bool IsWheelVirtualKey(uint16_t vk) noexcept {
        return vk == mask::core::kVkWheelUp || vk == mask::core::kVkWheelDown;
    }

    [[nodiscard]] static constexpr bool IsMouseVirtualKey(uint16_t vk) noexcept {
        return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
               vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || IsWheelVirtualKey(vk);
    }

    [[nodiscard]] static constexpr uint16_t NormalizeEchoVk(uint16_t vk) noexcept {
        const uint8_t b = static_cast<uint8_t>(vk & 0xFFU);
        if (b == VK_SHIFT)   return VK_LSHIFT;
        if (b == VK_CONTROL) return VK_LCONTROL;
        if (b == VK_MENU)    return VK_LMENU;
        return b;
    }

    static void StampVirtualEchoToken(
        mask::core::EngineSharedState& state,
        uint16_t vk,
        bool isDown
    ) noexcept;

    static void ClearVirtualEchoTokens(mask::core::EngineSharedState& state) noexcept;

    void SetBatchQueueMode(bool enabled) noexcept {
        batchQueueMode_ = enabled;
    }

    [[nodiscard]] bool IsBatchQueueMode() const noexcept {
        return batchQueueMode_;
    }

    void SetAutoWake(bool enabled) noexcept {
        autoWake_ = enabled;
    }

    [[nodiscard]] bool GetAutoWake() const noexcept {
        return autoWake_;
    }

    [[nodiscard]] bool EnqueueMouseItem(
        uint32_t buttons,
        int32_t dx,
        int32_t dy,
        int32_t wheel,
        int32_t pan,
        uint64_t benchToken = 0
    ) noexcept;

    [[nodiscard]] bool EnqueueKeyboardItem(
        uint8_t mods,
        const uint8_t* keys,
        int32_t keyCount,
        uint64_t benchToken = 0
    ) noexcept;

    void FlushBatchWake() noexcept;

    bool DispatchKeyTransition(uint16_t vk, bool isDown) noexcept;
    bool DispatchMouseButtonTransition(uint16_t buttonVk, bool isDown, int32_t dx = 0, int32_t dy = 0) noexcept;
    bool DispatchSeamlessSwap(uint16_t releaseVk, uint16_t pressVk) noexcept;
    bool DispatchMouseMoveUncoalesced(int32_t dx, int32_t dy) noexcept;
    bool DispatchScrollWheel(int32_t wheelDelta, int32_t panDelta = 0) noexcept;
    bool DispatchAbsoluteCursorMove(int32_t screenX, int32_t screenY) noexcept;

    [[nodiscard]] uint64_t GetTotalDispatched() const noexcept {
        return totalDispatched_;
    }

private:
    static constexpr std::array<uint16_t, 256> kVkToHidAndMod = detail::BuildVkToHidAndModTable();

    void StampSyntheticWrite() noexcept;
    [[nodiscard]] bool SubmitMouseReport(uint32_t buttons, int32_t dx, int32_t dy, int32_t wheel, int32_t pan) noexcept;
    bool SubmitKeyboardReport() noexcept;
    bool Record(bool ok) noexcept;

    mask::core::EngineSharedState& sharedState_;
    mask::bridge::HIDMaestroHost&  hmHost_;
    uint32_t                       failureStreak_{0};
    uint32_t                       enqueuedSinceWake_{0};
    uint64_t                       totalDispatched_{0};
    bool                           lastKbSubmitOk_{false};
    bool                           lastMouseSubmitOk_{false};
    bool                           batchQueueMode_{true};
    bool                           autoWake_{false};
};

} // namespace mask::input
