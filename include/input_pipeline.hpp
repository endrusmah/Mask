#pragma once

#include "core_contracts.hpp"
#include "hidmaestro_bridge.hpp"

#include <windows.h>
#include <GameInput.h>
#include <intrin.h>
#include <immintrin.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <algorithm>
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
 * Zero Win32 hooks (`SetWindowsHookEx` / `WH_KEYBOARD_LL` / `WH_MOUSE_LL` are 100% removed).
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
};

inline void RegisterVirtualDeviceVidPid(uint16_t vendorId, uint16_t productId) noexcept {
    if (vendorId == 0U && productId == 0U) {
        return;
    }
    const uint32_t packed = (static_cast<uint32_t>(vendorId) << 16) | static_cast<uint32_t>(productId);
    for (auto& slot : GameInputGlobalContext::registeredVirtualVidPids) {
        const uint32_t cur = slot.load(std::memory_order_relaxed);
        if (cur == packed) {
            return;
        }
        if (cur == 0U) {
            uint32_t expected = 0U;
            if (slot.compare_exchange_strong(expected, packed, std::memory_order_release, std::memory_order_relaxed) ||
                expected == packed) {
                return;
            }
        }
    }
}

/**
 * @brief Wakes the engine thread immediately (new trigger event, config change, pause, flush, shutdown),
 * so it never has to poll while idle.
 */
inline void WakeEngine() noexcept {
    if (const HANDLE ev = GameInputGlobalContext::engineWakeEvent) {
        SetEvent(ev);
    }
}

inline void WakeWatchdog() noexcept {
    if (const HANDLE ev = GameInputGlobalContext::watchdogWakeEvent) {
        SetEvent(ev);
    }
}

/**
 * @brief Checks if a GameInputDeviceInfo describes one of our own HIDMaestro virtual controllers.
 * Filtering by `GameInputDeviceInfo` (`GameInputFamilyVirtual`, default & active/registered VID/PIDs)
 * guarantees that synthetic macro outputs never interfere with or re-trigger physical user inputs.
 */
[[nodiscard]] inline bool IsSyntheticVirtualDeviceInfo(const GameInputDeviceInfo* info) noexcept {
    if (info == nullptr) {
        return false;
    }
    if (info->deviceFamily == GameInputFamilyVirtual) {
        return true;
    }
    const uint16_t vid = info->vendorId;
    const uint16_t pid = info->productId;
    if ((vid == kHidMaestroMouseVid && pid == kHidMaestroMousePid) ||
        (vid == kHidMaestroKeyboardVid && (pid == kHidMaestroKeyboardPid || pid == kHidMaestroKeyboardPidAlt))) {
        return true;
    }

    uint16_t activeMouseVid = 0, activeMousePid = 0, activeKbVid = 0, activeKbPid = 0;
    mask::bridge::HIDMaestroHost::Instance().GetActiveVirtualHardwareIds(
        activeMouseVid, activeMousePid, activeKbVid, activeKbPid);
    if ((activeMouseVid != 0U && vid == activeMouseVid && pid == activeMousePid) ||
        (activeKbVid != 0U && vid == activeKbVid && pid == activeKbPid)) {
        return true;
    }

    const uint32_t packed = (static_cast<uint32_t>(vid) << 16) | static_cast<uint32_t>(pid);
    for (const auto& entry : GameInputGlobalContext::registeredVirtualVidPids) {
        const uint32_t reg = entry.load(std::memory_order_relaxed);
        if (reg == 0U) {
            break;
        }
        if (reg == packed) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a GameInput device is one of our own HIDMaestro virtual hardware controllers.
 */
[[nodiscard]] inline bool IsSyntheticVirtualDevice(IGameInputDevice* device) noexcept {
    if (device == nullptr) {
        return false;
    }
    return IsSyntheticVirtualDeviceInfo(device->GetDeviceInfo());
}

/**
 * @brief Normalizes specific left/right modifier virtual keys so triggers bound to either generic
 * (`VK_SHIFT`, `VK_CONTROL`, `VK_MENU`) or side-specific (`VK_LSHIFT`, etc.) always match reliably.
 */
inline void UpdatePhysicalAndEnqueueTransition(
    mask::core::EngineSharedState* state,
    uint16_t vk,
    bool isDown,
    uint64_t nowTsc,
    bool isMouse
) noexcept {
    const uint8_t boundedVk = static_cast<uint8_t>(vk & 0xFFU);
    if (boundedVk == 0U) {
        return;
    }

    // Secondary timestamp + syntheticHeldState guard in case an aggregate reading includes virtual state
    const uint64_t lastSynthTsc = state->lastSyntheticHardwareWriteTsc.load(std::memory_order_relaxed);
    const uint64_t echoWin = GameInputGlobalContext::echoSuppressionWindowTsc.load(std::memory_order_relaxed);
    const bool withinEchoWindow = (nowTsc >= lastSynthTsc) && ((nowTsc - lastSynthTsc) <= echoWin);
    const uint8_t synthState = state->syntheticHeldState[boundedVk].load(std::memory_order_relaxed);
    const uint8_t newState = isDown ? 1U : 0U;

    if (withinEchoWindow && synthState == newState && boundedVk != VK_ESCAPE) {
        return;
    }

    const uint8_t prevState = state->physicalKeyState[boundedVk].exchange(newState, std::memory_order_acq_rel);

    // Also mirror Left/Right modifiers into generic VK_SHIFT / VK_CONTROL / VK_MENU
    if (boundedVk == VK_LSHIFT || boundedVk == VK_RSHIFT) {
        const uint8_t l = state->physicalKeyState[VK_LSHIFT].load(std::memory_order_relaxed);
        const uint8_t r = state->physicalKeyState[VK_RSHIFT].load(std::memory_order_relaxed);
        state->physicalKeyState[VK_SHIFT].store((l | r) ? 1U : 0U, std::memory_order_release);
    } else if (boundedVk == VK_LCONTROL || boundedVk == VK_RCONTROL) {
        const uint8_t l = state->physicalKeyState[VK_LCONTROL].load(std::memory_order_relaxed);
        const uint8_t r = state->physicalKeyState[VK_RCONTROL].load(std::memory_order_relaxed);
        state->physicalKeyState[VK_CONTROL].store((l | r) ? 1U : 0U, std::memory_order_release);
    } else if (boundedVk == VK_LMENU || boundedVk == VK_RMENU) {
        const uint8_t l = state->physicalKeyState[VK_LMENU].load(std::memory_order_relaxed);
        const uint8_t r = state->physicalKeyState[VK_RMENU].load(std::memory_order_relaxed);
        state->physicalKeyState[VK_MENU].store((l | r) ? 1U : 0U, std::memory_order_release);
    }

    // Check physical emergency panic chord: L-Ctrl + R-Ctrl + Esc
    if (isDown &&
        state->panicChordEnabled.load(std::memory_order_relaxed) &&
        (boundedVk == VK_ESCAPE || boundedVk == VK_LCONTROL || boundedVk == VK_RCONTROL || boundedVk == VK_CONTROL)) [[unlikely]] {
        if (state->physicalKeyState[VK_LCONTROL].load(std::memory_order_relaxed) != 0U &&
            state->physicalKeyState[VK_RCONTROL].load(std::memory_order_relaxed) != 0U &&
            state->physicalKeyState[VK_ESCAPE].load(std::memory_order_relaxed) != 0U) {
            // The watchdog thread releases every output and pauses automation; the app keeps running.
            state->emergencyPanicTriggered.store(true, std::memory_order_release);
            WakeWatchdog();
            WakeEngine();
            return;
        }
    }

    // Interactive Click-to-Bind capture check
    if (isDown && state->captureBindTarget.load(std::memory_order_relaxed) != 0U) [[unlikely]] {
        state->capturedVk.store(boundedVk, std::memory_order_release);
        const HWND uiHwnd = GameInputGlobalContext::uiWindowHandle.load(std::memory_order_relaxed);
        if (uiHwnd != nullptr) {
            PostMessageW(uiHwnd, WM_APP_KEY_CAPTURED, static_cast<WPARAM>(boundedVk), isMouse ? 1 : 0);
        }
        return;
    }

    if (prevState != newState) {
        const mask::core::InputTriggerEvent ev{
            nowTsc,
            boundedVk,
            isDown,
            {0, 0, 0, 0, 0}
        };
        if (state->inputEventQueue.try_push(ev)) {
            WakeEngine();
        }
    }
}

/**
 * @brief Finds or allocates a `PerDeviceInputState` slot keyed by `APP_LOCAL_DEVICE_ID`.
 * Caller must hold `GameInputGlobalContext::readingLock`.
 */
[[nodiscard]] inline PerDeviceInputState* FindOrAllocatePhysicalDeviceSlot(
    bool hasDeviceId,
    const APP_LOCAL_DEVICE_ID& deviceId
) noexcept {
    auto& table = GameInputGlobalContext::deviceStates;
    const uint64_t touch = ++GameInputGlobalContext::deviceTouchCounter;

    size_t freeIdx = kMaxPhysicalGameInputDevices;
    for (size_t i = 0; i < kMaxPhysicalGameInputDevices; ++i) {
        auto& entry = table[i];
        if (entry.active) {
            if (hasDeviceId) {
                if (entry.hasDeviceId && std::memcmp(&entry.deviceId, &deviceId, sizeof(APP_LOCAL_DEVICE_ID)) == 0) {
                    entry.lastTouchSeq = touch;
                    return &entry;
                }
            } else if (!entry.hasDeviceId) {
                entry.lastTouchSeq = touch;
                return &entry;
            }
        } else if (freeIdx == kMaxPhysicalGameInputDevices) {
            freeIdx = i;
        }
    }

    if (freeIdx == kMaxPhysicalGameInputDevices) {
        // Table full: evict the least recently seen idle slot (or oldest overall if all have active keys)
        size_t bestIdleIdx = kMaxPhysicalGameInputDevices;
        size_t bestAnyIdx = 0;
        for (size_t i = 0; i < kMaxPhysicalGameInputDevices; ++i) {
            const auto& entry = table[i];
            if (entry.lastTouchSeq < table[bestAnyIdx].lastTouchSeq) {
                bestAnyIdx = i;
            }
            const bool idle = (entry.kbBits[0] | entry.kbBits[1] | entry.kbBits[2] | entry.kbBits[3]) == 0ULL &&
                              entry.mouseButtons == 0U;
            if (idle && (bestIdleIdx == kMaxPhysicalGameInputDevices ||
                         entry.lastTouchSeq < table[bestIdleIdx].lastTouchSeq)) {
                bestIdleIdx = i;
            }
        }
        freeIdx = (bestIdleIdx != kMaxPhysicalGameInputDevices) ? bestIdleIdx : bestAnyIdx;
    }

    auto& slot = table[freeIdx];
    slot = PerDeviceInputState{};
    slot.deviceId = deviceId;
    slot.lastTouchSeq = touch;
    slot.active = true;
    slot.hasDeviceId = hasDeviceId;

    uint32_t activeCount = 0;
    for (const auto& entry : table) {
        if (entry.active) {
            ++activeCount;
        }
    }
    GameInputGlobalContext::activePhysicalDeviceCount.store(activeCount, std::memory_order_release);
    return &slot;
}

/**
 * @brief Thread-safe snapshot copy of all observed physical GameInput devices.
 * @return Number of populated entries in `outList`.
 */
[[nodiscard]] inline int CopyPhysicalDeviceInventory(
    std::array<PhysicalDeviceInfo, kMaxPhysicalGameInputDevices>& outList
) noexcept {
    while (GameInputGlobalContext::readingLock.test_and_set(std::memory_order_acquire)) {
        _mm_pause();
    }
    struct InventoryUnlock {
        ~InventoryUnlock() noexcept { GameInputGlobalContext::readingLock.clear(std::memory_order_release); }
    } unlock;

    int count = 0;
    for (size_t i = 0; i < kMaxPhysicalGameInputDevices; ++i) {
        const auto& entry = GameInputGlobalContext::deviceStates[i];
        if (!entry.active) {
            continue;
        }
        auto& dst = outList[static_cast<size_t>(count)];
        dst = PhysicalDeviceInfo{};
        std::memcpy(dst.deviceIdBytes, entry.deviceId.value, sizeof(dst.deviceIdBytes));
        dst.vendorId   = entry.vendorId;
        dst.productId  = entry.productId;
        dst.isKeyboard = entry.isKeyboard;
        dst.isMouse    = entry.isMouse;
        std::wmemcpy(dst.displayName, entry.displayName, std::size(dst.displayName));
        dst.displayName[std::size(dst.displayName) - 1] = L'\0';
        ++count;
    }
    for (size_t i = static_cast<size_t>(count); i < kMaxPhysicalGameInputDevices; ++i) {
        outList[i] = PhysicalDeviceInfo{};
    }
    return count;
}

inline void PopulatePhysicalDeviceMetadata(
    PerDeviceInputState& slot,
    uint16_t vendorId,
    uint16_t productId,
    bool isKeyboard,
    bool isMouse,
    const char* utf8DisplayName
) noexcept {
    slot.vendorId   = vendorId;
    slot.productId  = productId;
    slot.isKeyboard = slot.isKeyboard || isKeyboard;
    slot.isMouse    = slot.isMouse || isMouse;

    bool haveName = false;
    if (utf8DisplayName != nullptr && utf8DisplayName[0] != '\0') {
        wchar_t wideTmp[128]{};
        const int wlen = MultiByteToWideChar(
            CP_UTF8, 0, utf8DisplayName, -1, wideTmp, static_cast<int>(std::size(wideTmp)));
        if (wlen > 1) {
            const size_t maxChars = std::size(slot.displayName) - 1;
            const size_t copyLen = std::min<size_t>(static_cast<size_t>(wlen - 1), maxChars);
            std::wmemcpy(slot.displayName, wideTmp, copyLen);
            slot.displayName[copyLen] = L'\0';
            haveName = true;
        }
    }

    if (!haveName) {
        const wchar_t* prefix = L"HID Device";
        if (slot.isKeyboard && !slot.isMouse) {
            prefix = L"HID Keyboard";
        } else if (slot.isMouse && !slot.isKeyboard) {
            prefix = L"HID Mouse";
        } else if (slot.isKeyboard && slot.isMouse) {
            prefix = L"HID Keyboard/Mouse";
        }
        std::swprintf(
            slot.displayName,
            std::size(slot.displayName),
            L"%ls (%04X:%04X)",
            prefix,
            static_cast<unsigned>(slot.vendorId),
            static_cast<unsigned>(slot.productId));
    }

    slot.hasMetadata = true;
}

/**
 * @brief Processes a single `IGameInputReading` for Keyboard and Mouse state transitions across
 * up to 16 concurrent physical devices without cross-device clobbering.
 */
inline void ProcessGameInputReading(IGameInputReading* reading) noexcept {
    if (reading == nullptr) {
        return;
    }
    auto* state = GameInputGlobalContext::sharedState;
    if (state == nullptr) {
        return;
    }

    // Check if the reading came from one of our own HIDMaestro virtual controllers,
    // copying deviceId and metadata before releasing the COM pointer so zero COM refs are ever held.
    IGameInputDevice* device = nullptr;
    reading->GetDevice(&device);
    bool isVirtual = false;
    bool hasDeviceId = false;
    APP_LOCAL_DEVICE_ID deviceId{};
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    bool infoIsKeyboard = false;
    bool infoIsMouse = false;
    char rawDisplayName[128]{};
    if (device != nullptr) {
        const GameInputDeviceInfo* info = device->GetDeviceInfo();
        if (info != nullptr) {
            isVirtual = IsSyntheticVirtualDeviceInfo(info);
            if (!isVirtual) {
                deviceId = info->deviceId;
                hasDeviceId = true;
                vendorId = info->vendorId;
                productId = info->productId;
                infoIsKeyboard = ((info->supportedInput & GameInputKindKeyboard) != 0) || (info->keyboardInfo != nullptr);
                infoIsMouse    = ((info->supportedInput & GameInputKindMouse) != 0)    || (info->mouseInfo != nullptr);
                if (info->displayName != nullptr && info->displayName->data != nullptr && info->displayName->data[0] != '\0') {
                    const size_t maxCopy = sizeof(rawDisplayName) - 1;
                    size_t limit = info->displayName->sizeInBytes;
                    if (limit == 0U || limit > maxCopy) {
                        limit = maxCopy;
                    }
                    size_t i = 0;
                    for (; i < limit && info->displayName->data[i] != '\0'; ++i) {
                        rawDisplayName[i] = info->displayName->data[i];
                    }
                    rawDisplayName[i] = '\0';
                }
            }
        }
        device->Release();
        device = nullptr;
    }
    if (isVirtual) {
        return;
    }

    struct PendingPerDeviceEvent {
        uint8_t  deviceKind{0};
        uint16_t vk{0};
        bool     isDown{false};
    };
    const auto perDevCb = GameInputGlobalContext::perDeviceCallback.load(std::memory_order_acquire);
    PendingPerDeviceEvent pendingDevEvents[40]{};
    size_t pendingDevCount = 0;
    uint8_t devIdBytesCopy[32]{};
    uint64_t nowTsc = 0;

    {
        // Short critical section: the callback thread and the polling thread both update per-device state.
        while (GameInputGlobalContext::readingLock.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
        struct ReadingUnlock {
            ~ReadingUnlock() noexcept { GameInputGlobalContext::readingLock.clear(std::memory_order_release); }
        } readingUnlock;

        PerDeviceInputState* slot = FindOrAllocatePhysicalDeviceSlot(hasDeviceId, deviceId);
        const uint64_t readingTs = reading->GetTimestamp();

        unsigned int aux = 0;
        nowTsc = __rdtscp(&aux);
        const GameInputKind kind = reading->GetInputKind();

        const bool readingIsKb    = (kind & GameInputKindKeyboard) != 0;
        const bool readingIsMouse = (kind & GameInputKindMouse) != 0;
        if (!slot->hasMetadata) {
            PopulatePhysicalDeviceMetadata(
                *slot,
                vendorId,
                productId,
                infoIsKeyboard || readingIsKb,
                infoIsMouse || readingIsMouse,
                rawDisplayName);
        } else {
            if (readingIsKb) {
                slot->isKeyboard = true;
            }
            if (readingIsMouse) {
                slot->isMouse = true;
            }
        }

        if (perDevCb != nullptr) {
            std::memcpy(devIdBytesCopy, slot->deviceId.value, sizeof(devIdBytesCopy));
        }

        // 1. Process Keyboard Reading
        if (readingIsKb) {
            if (!slot->hasKbReading || readingTs != slot->lastKbTimestamp) {
                slot->hasKbReading = true;
                slot->lastKbTimestamp = readingTs;

                std::array<GameInputKeyState, 32> keyStates{};
                const uint32_t rawCount = reading->GetKeyState(static_cast<uint32_t>(keyStates.size()), keyStates.data());
                const uint32_t count = (rawCount <= static_cast<uint32_t>(keyStates.size()))
                    ? rawCount : static_cast<uint32_t>(keyStates.size());

                uint64_t deviceBits[4]{0, 0, 0, 0};
                for (uint32_t i = 0; i < count; ++i) {
                    uint8_t vk = keyStates[i].virtualKey;
                    const uint32_t sc = keyStates[i].scanCode;
                    // Refine generic modifier VKs using hardware scan codes when provided
                    if (vk == VK_SHIFT) {
                        vk = (sc == 0x36U) ? VK_RSHIFT : VK_LSHIFT;
                    } else if (vk == VK_CONTROL) {
                        vk = (sc > 0xFFU) ? VK_RCONTROL : VK_LCONTROL;
                    } else if (vk == VK_MENU) {
                        vk = (sc > 0xFFU) ? VK_RMENU : VK_LMENU;
                    }
                    // Skip 0 and mouse VKs in the keyboard bitmask
                    if (vk != 0U &&
                        vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_MBUTTON &&
                        vk != VK_XBUTTON1 && vk != VK_XBUTTON2) {
                        deviceBits[vk >> 6] |= (1ULL << (vk & 63U));
                    }
                }

                if (perDevCb != nullptr) {
                    for (size_t w = 0; w < 4; ++w) {
                        uint64_t devDiff = deviceBits[w] ^ slot->kbBits[w];
                        while (devDiff != 0ULL) {
                            const uint64_t bitIdx = _tzcnt_u64(devDiff);
                            devDiff &= (devDiff - 1ULL);
                            const uint16_t vk = static_cast<uint16_t>((w << 6) | bitIdx);
                            if (vk != 0U && pendingDevCount < std::size(pendingDevEvents)) {
                                const bool isDown = ((deviceBits[w] >> bitIdx) & 1ULL) != 0ULL;
                                pendingDevEvents[pendingDevCount++] = PendingPerDeviceEvent{1U, vk, isDown};
                            }
                        }
                    }
                }

                for (size_t w = 0; w < 4; ++w) {
                    slot->kbBits[w] = deviceBits[w];
                }

                uint64_t combinedBits[4]{0, 0, 0, 0};
                for (size_t d = 0; d < kMaxPhysicalGameInputDevices; ++d) {
                    const auto& entry = GameInputGlobalContext::deviceStates[d];
                    if (!entry.active) {
                        continue;
                    }
                    combinedBits[0] |= entry.kbBits[0];
                    combinedBits[1] |= entry.kbBits[1];
                    combinedBits[2] |= entry.kbBits[2];
                    combinedBits[3] |= entry.kbBits[3];
                }

                // Diff against previous combined keyboard state using 4 XORs + _tzcnt_u64
                for (size_t w = 0; w < 4; ++w) {
                    uint64_t diff = combinedBits[w] ^ GameInputGlobalContext::prevCombinedKbBits[w];
                    if (diff == 0ULL) {
                        continue;
                    }
                    GameInputGlobalContext::prevCombinedKbBits[w] = combinedBits[w];
                    while (diff != 0ULL) {
                        const uint64_t bitIdx = _tzcnt_u64(diff);
                        diff &= (diff - 1ULL);
                        const uint16_t vk = static_cast<uint16_t>((w << 6) | bitIdx);
                        if (vk != 0U) {
                            const bool isDown = ((combinedBits[w] >> bitIdx) & 1ULL) != 0ULL;
                            UpdatePhysicalAndEnqueueTransition(state, vk, isDown, nowTsc, false);
                        }
                    }
                }
            }
        }

        // 2. Process Mouse Reading
        if (readingIsMouse) {
            if (!slot->hasMouseReading || readingTs != slot->lastMouseTimestamp) {
                GameInputMouseState ms{};
                if (reading->GetMouseState(&ms)) {
                    slot->hasMouseReading = true;
                    slot->lastMouseTimestamp = readingTs;
                    const uint32_t newDevButtons = static_cast<uint32_t>(ms.buttons);
                    const uint32_t devDiffMask = newDevButtons ^ slot->mouseButtons;
                    slot->mouseButtons = newDevButtons;

                    struct MouseBtnMap {
                        uint32_t flag;
                        uint16_t vk;
                    };
                    constexpr MouseBtnMap kMap[5] = {
                        { static_cast<uint32_t>(GameInputMouseLeftButton),   VK_LBUTTON  },
                        { static_cast<uint32_t>(GameInputMouseRightButton),  VK_RBUTTON  },
                        { static_cast<uint32_t>(GameInputMouseMiddleButton), VK_MBUTTON  },
                        { static_cast<uint32_t>(GameInputMouseButton4),      VK_XBUTTON1 },
                        { static_cast<uint32_t>(GameInputMouseButton5),      VK_XBUTTON2 }
                    };

                    if (perDevCb != nullptr && devDiffMask != 0U) {
                        for (const auto& m : kMap) {
                            if ((devDiffMask & m.flag) != 0U && pendingDevCount < std::size(pendingDevEvents)) {
                                const bool isDown = (newDevButtons & m.flag) != 0U;
                                pendingDevEvents[pendingDevCount++] = PendingPerDeviceEvent{2U, m.vk, isDown};
                            }
                        }
                    }

                    uint32_t combinedMask = 0U;
                    for (size_t d = 0; d < kMaxPhysicalGameInputDevices; ++d) {
                        const auto& entry = GameInputGlobalContext::deviceStates[d];
                        if (entry.active) {
                            combinedMask |= entry.mouseButtons;
                        }
                    }

                    const uint32_t prevMask = GameInputGlobalContext::prevCombinedMouseButtonsMask;
                    const uint32_t diffMask = combinedMask ^ prevMask;

                    if (diffMask != 0U) {
                        GameInputGlobalContext::prevCombinedMouseButtonsMask = combinedMask;
                        for (const auto& m : kMap) {
                            if ((diffMask & m.flag) != 0U) {
                                const bool isDown = (combinedMask & m.flag) != 0U;
                                UpdatePhysicalAndEnqueueTransition(state, m.vk, isDown, nowTsc, true);
                            }
                        }
                    }
                }
            }
        }
    }

    // Invoke per-device transition callback outside readingLock to avoid any re-entrancy/deadlock.
    if (perDevCb != nullptr && pendingDevCount > 0U) {
        for (size_t i = 0; i < pendingDevCount; ++i) {
            perDevCb(
                devIdBytesCopy,
                pendingDevEvents[i].deviceKind,
                pendingDevEvents[i].vk,
                pendingDevEvents[i].isDown,
                nowTsc);
        }
    }
}

/**
 * @brief Microsoft GameInput Reading Callback (`RegisterReadingCallback`).
 * Invoked immediately by the GameInput runtime whenever any keyboard or mouse state changes.
 */
inline void CALLBACK OnGameInputReadingCallback(
    _In_ GameInputCallbackToken /*callbackToken*/,
    _In_ void* /*context*/,
    _In_ IGameInputReading* reading,
    _In_ bool /*hasOverrunOccurred*/
) noexcept {
    ProcessGameInputReading(reading);
}

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
 * Dispatches all keyboard transitions via `HMController.SubmitKeyboard(HMKeyModifiers, ReadOnlySpan<HMKey>)`
 * and all mouse button/movement transitions via `HMController.SubmitMouse(in HMMouseState)`.
 *
 * Both devices are driven by full state reports, so several changes can be folded into a single report:
 * a button press together with its pointer movement, or "release key A + press key B" with no gap between.
 */
class alignas(mask::core::kCacheLineSize) InputDispatcher {
public:
    explicit InputDispatcher(mask::core::EngineSharedState& sharedState) noexcept
        : sharedState_(sharedState)
        , hmHost_(mask::bridge::HIDMaestroHost::Instance()) {}

    ~InputDispatcher() noexcept = default;

    InputDispatcher(const InputDispatcher&) = delete;
    InputDispatcher& operator=(const InputDispatcher&) = delete;

    [[nodiscard]] static constexpr bool IsWheelVirtualKey(uint16_t vk) noexcept {
        return vk == mask::core::kVkWheelUp || vk == mask::core::kVkWheelDown;
    }

    [[nodiscard]] static constexpr bool IsMouseVirtualKey(uint16_t vk) noexcept {
        return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
               vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || IsWheelVirtualKey(vk);
    }

    /**
     * @brief Presses or releases one key or mouse button.
     */
    bool DispatchKeyTransition(uint16_t vk, bool isDown) noexcept {
        if (IsMouseVirtualKey(vk)) {
            return DispatchMouseButtonTransition(vk, isDown);
        }
        const uint8_t boundedVk = static_cast<uint8_t>(vk & 0xFFU);
        if (boundedVk == 0U) {
            return false;
        }

        const uint8_t newVal = isDown ? 1U : 0U;
        const uint8_t prevVal = sharedState_.syntheticHeldState[boundedVk].load(std::memory_order_relaxed);
        if (prevVal == newVal && lastKbSubmitOk_) {
            if (newVal == 0U) {
                activeKeyBits_[boundedVk >> 6] &= ~(1ULL << (boundedVk & 63U));
            }
            return true;
        }

        // Update syntheticHeldState and the echo timestamp BEFORE calling HIDMaestro
        StampSyntheticWrite();
        sharedState_.syntheticHeldState[boundedVk].store(newVal, std::memory_order_release);
        if (newVal != 0U) {
            activeKeyBits_[boundedVk >> 6] |= (1ULL << (boundedVk & 63U));
        } else {
            activeKeyBits_[boundedVk >> 6] &= ~(1ULL << (boundedVk & 63U));
        }
        return SubmitKeyboardReport();
    }

    /**
     * @brief Presses or releases one mouse button (or fires an instantaneous wheel tick for kVkWheelUp /
     * kVkWheelDown). An optional relative movement rides in the same report, so "click and move" costs
     * one driver call instead of two.
     */
    bool DispatchMouseButtonTransition(uint16_t buttonVk, bool isDown, int32_t dx = 0, int32_t dy = 0) noexcept {
        if (IsWheelVirtualKey(buttonVk)) {
            const uint32_t currentButtons = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
            if (isDown) {
                StampSyntheticWrite();
                const int32_t wheelDelta = (buttonVk == mask::core::kVkWheelUp) ? 1 : -1;
                const bool ok = Record(hmHost_.SubmitMouseHardware(currentButtons, dx, dy, wheelDelta, 0));
                lastMouseSubmitOk_ = ok;
                return ok;
            }
            if (dx != 0 || dy != 0) {
                StampSyntheticWrite();
                const bool ok = Record(hmHost_.SubmitMouseHardware(currentButtons, dx, dy, 0, 0));
                lastMouseSubmitOk_ = ok;
                return ok;
            }
            return true;
        }

        const uint8_t trackedVk = static_cast<uint8_t>(buttonVk & 0xFFU);
        const uint32_t btnBit = mask::bridge::hm::VkToHMMouseButtonBit(trackedVk);
        if (btnBit == 0U) {
            return false;
        }

        const uint32_t prevMask = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
        uint32_t newMask = prevMask;
        if (isDown) {
            newMask |= btnBit;
        } else {
            newMask &= ~btnBit;
        }

        if (newMask == prevMask && dx == 0 && dy == 0 && lastMouseSubmitOk_) {
            return true;
        }

        StampSyntheticWrite();
        sharedState_.syntheticHeldState[trackedVk].store(isDown ? 1U : 0U, std::memory_order_release);
        sharedState_.activeMouseButtonMask.store(newMask, std::memory_order_release);

        const bool ok = Record(hmHost_.SubmitMouseHardware(newMask, dx, dy, 0, 0));
        lastMouseSubmitOk_ = ok;
        return ok;
    }

    /**
     * @brief Releases `releaseVk` and presses `pressVk` with no time in between.
     * When both live on the same device this is literally one report, so there is never a moment where
     * neither (or, for snap-tap, both) is down. Either argument may be 0 to skip that half.
     */
    bool DispatchSeamlessSwap(uint16_t releaseVk, uint16_t pressVk) noexcept {
        const uint8_t relVk = static_cast<uint8_t>(releaseVk & 0xFFU);
        const uint8_t prsVk = static_cast<uint8_t>(pressVk & 0xFFU);
        if (relVk == prsVk) {
            return IsWheelVirtualKey(prsVk) ? DispatchMouseButtonTransition(prsVk, true) : true;
        }
        if (relVk == 0U) {
            return DispatchKeyTransition(prsVk, true);
        }
        if (prsVk == 0U) {
            return DispatchKeyTransition(relVk, false);
        }

        const bool relMouse = IsMouseVirtualKey(relVk);
        const bool prsMouse = IsMouseVirtualKey(prsVk);

        if (relMouse && prsMouse) {
            const bool relWheel = IsWheelVirtualKey(relVk);
            const bool prsWheel = IsWheelVirtualKey(prsVk);
            StampSyntheticWrite();
            uint32_t newMask = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
            if (!relWheel) {
                sharedState_.syntheticHeldState[relVk].store(0U, std::memory_order_release);
                newMask &= ~mask::bridge::hm::VkToHMMouseButtonBit(relVk);
            }
            int32_t wheelDelta = 0;
            if (!prsWheel) {
                sharedState_.syntheticHeldState[prsVk].store(1U, std::memory_order_release);
                newMask |= mask::bridge::hm::VkToHMMouseButtonBit(prsVk);
            } else {
                wheelDelta = (prsVk == mask::core::kVkWheelUp) ? 1 : -1;
            }
            sharedState_.activeMouseButtonMask.store(newMask, std::memory_order_release);
            const bool ok = Record(hmHost_.SubmitMouseHardware(newMask, 0, 0, wheelDelta, 0));
            lastMouseSubmitOk_ = ok;
            return ok;
        }
        if (!relMouse && !prsMouse) {
            StampSyntheticWrite();
            sharedState_.syntheticHeldState[relVk].store(0U, std::memory_order_release);
            sharedState_.syntheticHeldState[prsVk].store(1U, std::memory_order_release);
            activeKeyBits_[relVk >> 6] &= ~(1ULL << (relVk & 63U));
            activeKeyBits_[prsVk >> 6] |= (1ULL << (prsVk & 63U));
            return SubmitKeyboardReport();
        }
        // Different devices: two reports, issued back to back (press first so there is no gap).
        const bool pressed  = DispatchKeyTransition(prsVk, true);
        const bool released = DispatchKeyTransition(relVk, false);
        return pressed && released;
    }

    /**
     * @brief Dispatches relative hardware mouse movement while preserving currently held mouse buttons.
     */
    bool DispatchMouseMoveUncoalesced(int32_t dx, int32_t dy) noexcept {
        if (dx == 0 && dy == 0) {
            return true;
        }
        const uint32_t currentButtons = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
        const bool ok = Record(hmHost_.SubmitMouseHardware(currentButtons, dx, dy, 0, 0));
        lastMouseSubmitOk_ = ok;
        return ok;
    }

    /**
     * @brief Dispatches vertical/horizontal hardware scroll wheel deltas while preserving held mouse buttons.
     */
    bool DispatchScrollWheel(int32_t wheelDelta, int32_t panDelta = 0) noexcept {
        if (wheelDelta == 0 && panDelta == 0) {
            return true;
        }
        StampSyntheticWrite();
        const uint32_t currentButtons = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
        const bool ok = Record(hmHost_.SubmitMouseHardware(currentButtons, 0, 0, wheelDelta, panDelta));
        lastMouseSubmitOk_ = ok;
        return ok;
    }

    /**
     * @brief Moves the cursor accurately to virtual-desktop screen coordinates `(screenX, screenY)`
     * across single or multi-monitor layouts and emits a zero-delta HID sync report when controllers are live.
     */
    bool DispatchAbsoluteCursorMove(int32_t screenX, int32_t screenY) noexcept {
        const int32_t vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int32_t vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int32_t vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int32_t vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        int32_t targetX = screenX;
        int32_t targetY = screenY;
        if (vw > 0 && vh > 0) {
            targetX = std::clamp<int32_t>(targetX, vx, vx + vw - 1);
            targetY = std::clamp<int32_t>(targetY, vy, vy + vh - 1);
        }

        const BOOL moved = SetCursorPos(targetX, targetY);
        const uint32_t currentButtons = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
        if (hmHost_.AreControllersLive()) {
            StampSyntheticWrite();
            const bool hidOk = Record(hmHost_.SubmitMouseHardware(currentButtons, 0, 0, 0, 0));
            lastMouseSubmitOk_ = hidOk;
            return (moved != FALSE) && hidOk;
        }
        if (moved != FALSE) {
            ++totalDispatched_;
            return true;
        }
        return false;
    }

    [[nodiscard]] uint64_t GetTotalDispatched() const noexcept {
        return totalDispatched_;
    }

private:
    static constexpr std::array<uint16_t, 256> kVkToHidAndMod = detail::BuildVkToHidAndModTable();

    __forceinline void StampSyntheticWrite() noexcept {
        unsigned int aux = 0;
        sharedState_.lastSyntheticHardwareWriteTsc.store(__rdtscp(&aux), std::memory_order_release);
    }

    /**
     * @brief Builds the complete keyboard report (modifier bitmask + up to 6 keys) in O(k) via
     * `activeKeyBits_` + `kVkToHidAndMod`, verifying against `syntheticHeldState` so Watchdog
     * releases from another thread are always honoured immediately.
     */
    bool SubmitKeyboardReport() noexcept {
        uint8_t modifiersMask = 0;
        std::array<uint8_t, 6> activeHidKeys{};
        int32_t keyCount = 0;

        for (size_t w = 0; w < 4; ++w) {
            uint64_t bits = activeKeyBits_[w];
            while (bits != 0ULL) {
                const uint64_t bitIdx = _tzcnt_u64(bits);
                bits &= (bits - 1ULL);
                const uint16_t vk = static_cast<uint16_t>((w << 6) | bitIdx);
                if (sharedState_.syntheticHeldState[vk].load(std::memory_order_relaxed) == 0U) {
                    activeKeyBits_[w] &= ~(1ULL << bitIdx);
                    continue;
                }
                const uint16_t packed = kVkToHidAndMod[vk];
                const uint8_t modBit = static_cast<uint8_t>(packed >> 8);
                if (modBit != 0U) {
                    modifiersMask |= modBit;
                } else {
                    const uint8_t hidKey = static_cast<uint8_t>(packed & 0xFFU);
                    if (hidKey != 0U && keyCount < 6) {
                        activeHidKeys[static_cast<size_t>(keyCount)] = hidKey;
                        ++keyCount;
                    }
                }
            }
        }

        sharedState_.activeKeyModifierMask.store(modifiersMask, std::memory_order_release);
        const bool ok = Record(hmHost_.SubmitKeyboardHardware(modifiersMask, activeHidKeys.data(), keyCount));
        lastKbSubmitOk_ = ok;
        return ok;
    }

    /**
     * @brief Tracks consecutive failed submissions so the UI can tell the user (and try to reconnect)
     * instead of macros silently doing nothing.
     */
    __forceinline bool Record(bool ok) noexcept {
        if (ok) [[likely]] {
            ++totalDispatched_;
            if (failureStreak_ != 0U) {
                failureStreak_ = 0U;
                sharedState_.outputFailureStreak.store(0U, std::memory_order_relaxed);
            }
        } else if (failureStreak_ < 1'000'000U) {
            ++failureStreak_;
            sharedState_.outputFailureStreak.store(failureStreak_, std::memory_order_relaxed);
        }
        return ok;
    }

    mask::core::EngineSharedState& sharedState_;
    mask::bridge::HIDMaestroHost&  hmHost_;
    std::array<uint64_t, 4>        activeKeyBits_{};
    uint32_t                       failureStreak_{0};
    uint64_t                       totalDispatched_{0};
    bool                           lastKbSubmitOk_{false};
    bool                           lastMouseSubmitOk_{false};
};

} // namespace mask::input
