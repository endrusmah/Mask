#include "input_pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace mask::input {

namespace {

[[nodiscard]] inline uint32_t FirstSetBitIndex64(uint64_t mask) noexcept {
    unsigned long idx = 0;
    return _BitScanForward64(&idx, mask) ? static_cast<uint32_t>(idx) : 64U;
}

} // namespace

void RegisterVirtualDeviceVidPid(uint16_t vendorId, uint16_t productId) noexcept {
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

void WakeEngine() noexcept {
    if (const HANDLE ev = GameInputGlobalContext::engineWakeEvent) {
        SetEvent(ev);
    }
}

void WakeWatchdog() noexcept {
    if (const HANDLE ev = GameInputGlobalContext::watchdogWakeEvent) {
        SetEvent(ev);
    }
}

void WakeOutput() noexcept {
    if (const HANDLE ev = GameInputGlobalContext::outputWakeEvent) {
        SetEvent(ev);
    }
}

bool IsSyntheticVirtualDeviceInfo(const GameInputDeviceInfo* info) noexcept {
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

bool IsSyntheticVirtualDevice(IGameInputDevice* device) noexcept {
    if (device == nullptr) {
        return false;
    }
    return IsSyntheticVirtualDeviceInfo(device->GetDeviceInfo());
}

void UpdatePhysicalAndEnqueueTransition(
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

PerDeviceInputState* FindOrAllocatePhysicalDeviceSlot(
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

int CopyPhysicalDeviceInventory(
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

void PopulatePhysicalDeviceMetadata(
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
            const size_t copyLen = (std::min<size_t>)(static_cast<size_t>(wlen - 1), maxChars);
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

void ProcessGameInputReading(IGameInputReading* reading) noexcept {
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
                            const uint32_t bitIdx = FirstSetBitIndex64(devDiff);
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

                // Diff against previous combined keyboard state using 4 XORs + _BitScanForward64
                for (size_t w = 0; w < 4; ++w) {
                    uint64_t diff = combinedBits[w] ^ GameInputGlobalContext::prevCombinedKbBits[w];
                    if (diff == 0ULL) {
                        continue;
                    }
                    GameInputGlobalContext::prevCombinedKbBits[w] = combinedBits[w];
                    while (diff != 0ULL) {
                        const uint32_t bitIdx = FirstSetBitIndex64(diff);
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

void CALLBACK OnGameInputReadingCallback(
    _In_ GameInputCallbackToken /*callbackToken*/,
    _In_ void* /*context*/,
    _In_ IGameInputReading* reading,
    _In_ bool /*hasOverrunOccurred*/
) noexcept {
    ProcessGameInputReading(reading);
}

// =====================================================================================
//  InputDispatcher
// =====================================================================================

InputDispatcher::InputDispatcher(mask::core::EngineSharedState& sharedState, bool batchQueueMode) noexcept
    : sharedState_(sharedState)
    , hmHost_(mask::bridge::HIDMaestroHost::Instance())
    , batchQueueMode_(batchQueueMode) {}

InputDispatcher::~InputDispatcher() noexcept {
    FlushBatchWake();
}

void InputDispatcher::StampVirtualEchoToken(
    mask::core::EngineSharedState& state,
    uint16_t vk,
    bool isDown
) noexcept {
    if (vk == 0U || IsWheelVirtualKey(vk)) {
        return;
    }
    const uint16_t normVk = NormalizeEchoVk(vk);
    if (normVk == 0U || normVk >= 256U) {
        return;
    }
    unsigned int aux = 0;
    const uint64_t nowTsc = __rdtscp(&aux);
    if (isDown) {
        state.virtualEchoDownTsc[normVk].store(nowTsc, std::memory_order_release);
        state.virtualEchoDownCount[normVk].fetch_add(1U, std::memory_order_acq_rel);
    } else {
        state.virtualEchoUpTsc[normVk].store(nowTsc, std::memory_order_release);
        state.virtualEchoUpCount[normVk].fetch_add(1U, std::memory_order_acq_rel);
    }
}

void InputDispatcher::ClearVirtualEchoTokens(mask::core::EngineSharedState& state) noexcept {
    for (size_t i = 0; i < 256; ++i) {
        state.virtualEchoDownCount[i].store(0U, std::memory_order_release);
        state.virtualEchoUpCount[i].store(0U, std::memory_order_release);
        state.virtualEchoDownTsc[i].store(0ULL, std::memory_order_release);
        state.virtualEchoUpTsc[i].store(0ULL, std::memory_order_release);
    }
}

bool InputDispatcher::EnqueueMouseItem(
    uint32_t buttons,
    int32_t dx,
    int32_t dy,
    int32_t wheel,
    int32_t pan,
    uint64_t benchToken
) noexcept {
    unsigned int aux = 0;
    mask::core::OutputBatchItem item{};
    item.kind        = static_cast<uint8_t>(mask::core::OutputBatchKind::Mouse);
    item.buttonsMask = buttons;
    item.dx          = dx;
    item.dy          = dy;
    item.wheel       = wheel;
    item.pan         = pan;
    item.enqueueTsc  = __rdtscp(&aux);
    item.benchToken  = benchToken;

    const bool ok = sharedState_.outputBatchQueue.try_push(item);
    if (ok) {
        ++enqueuedSinceWake_;
        if (autoWake_) {
            FlushBatchWake();
        }
    } else {
        WakeOutput();
    }
    return Record(ok);
}

bool InputDispatcher::EnqueueKeyboardItem(
    uint8_t mods,
    const uint8_t* keys,
    int32_t keyCount,
    uint64_t benchToken
) noexcept {
    unsigned int aux = 0;
    mask::core::OutputBatchItem item{};
    item.kind          = static_cast<uint8_t>(mask::core::OutputBatchKind::Keyboard);
    item.modifiersMask = mods;
    const int32_t clamped = (keys != nullptr) ? std::clamp<int32_t>(keyCount, 0, 6) : 0;
    item.keyCount      = static_cast<uint8_t>(clamped);
    for (int32_t i = 0; i < clamped; ++i) {
        item.hidKeyCodes[i] = keys[i];
    }
    item.enqueueTsc    = __rdtscp(&aux);
    item.benchToken    = benchToken;

    const bool ok = sharedState_.outputBatchQueue.try_push(item);
    if (ok) {
        ++enqueuedSinceWake_;
        if (autoWake_) {
            FlushBatchWake();
        }
    } else {
        WakeOutput();
    }
    return Record(ok);
}

void InputDispatcher::FlushBatchWake() noexcept {
    if (enqueuedSinceWake_ > 0U) {
        enqueuedSinceWake_ = 0U;
        WakeOutput();
    }
}

bool InputDispatcher::DispatchKeyTransition(uint16_t vk, bool isDown) noexcept {
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
            sharedState_.syntheticKeyBits[boundedVk >> 6].fetch_and(~(1ULL << (boundedVk & 63U)), std::memory_order_acq_rel);
        }
        return true;
    }

    // Update echo token, syntheticHeldState, and echo timestamp BEFORE queuing/submitting to HIDMaestro
    StampVirtualEchoToken(sharedState_, boundedVk, isDown);
    StampSyntheticWrite();
    sharedState_.syntheticHeldState[boundedVk].store(newVal, std::memory_order_release);
    if (newVal != 0U) {
        sharedState_.syntheticKeyBits[boundedVk >> 6].fetch_or(1ULL << (boundedVk & 63U), std::memory_order_acq_rel);
    } else {
        sharedState_.syntheticKeyBits[boundedVk >> 6].fetch_and(~(1ULL << (boundedVk & 63U)), std::memory_order_acq_rel);
    }
    return SubmitKeyboardReport();
}

bool InputDispatcher::DispatchMouseButtonTransition(uint16_t buttonVk, bool isDown, int32_t dx, int32_t dy) noexcept {
    if (IsWheelVirtualKey(buttonVk)) {
        const uint32_t currentButtons = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
        if (isDown) {
            StampSyntheticWrite();
            const int32_t wheelDelta = (buttonVk == mask::core::kVkWheelUp) ? 1 : -1;
            const bool ok = SubmitMouseReport(currentButtons, dx, dy, wheelDelta, 0);
            lastMouseSubmitOk_ = ok;
            return ok;
        }
        if (dx != 0 || dy != 0) {
            StampSyntheticWrite();
            const bool ok = SubmitMouseReport(currentButtons, dx, dy, 0, 0);
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

    const uint32_t prevMask = sharedState_.activeMouseButtonMask.load(std::memory_order_acquire);
    const bool alreadySame = isDown ? ((prevMask & btnBit) != 0U) : ((prevMask & btnBit) == 0U);
    if (alreadySame && dx == 0 && dy == 0 && lastMouseSubmitOk_) {
        return true;
    }

    if (!alreadySame) {
        StampVirtualEchoToken(sharedState_, trackedVk, isDown);
    }
    StampSyntheticWrite();
    sharedState_.syntheticHeldState[trackedVk].store(isDown ? 1U : 0U, std::memory_order_release);
    const uint32_t newMask = isDown
        ? (sharedState_.activeMouseButtonMask.fetch_or(btnBit, std::memory_order_acq_rel) | btnBit)
        : (sharedState_.activeMouseButtonMask.fetch_and(~btnBit, std::memory_order_acq_rel) & ~btnBit);

    const bool ok = SubmitMouseReport(newMask, dx, dy, 0, 0);
    lastMouseSubmitOk_ = ok;
    return ok;
}

bool InputDispatcher::DispatchSeamlessSwap(uint16_t releaseVk, uint16_t pressVk) noexcept {
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
        if (!relWheel) {
            StampVirtualEchoToken(sharedState_, relVk, false);
        }
        if (!prsWheel) {
            StampVirtualEchoToken(sharedState_, prsVk, true);
        }
        StampSyntheticWrite();
        uint32_t clearBits = 0U;
        uint32_t setBits = 0U;
        if (!relWheel) {
            sharedState_.syntheticHeldState[relVk].store(0U, std::memory_order_release);
            clearBits = mask::bridge::hm::VkToHMMouseButtonBit(relVk);
        }
        int32_t wheelDelta = 0;
        if (!prsWheel) {
            sharedState_.syntheticHeldState[prsVk].store(1U, std::memory_order_release);
            setBits = mask::bridge::hm::VkToHMMouseButtonBit(prsVk);
        } else {
            wheelDelta = (prsVk == mask::core::kVkWheelUp) ? 1 : -1;
        }
        uint32_t newMask = sharedState_.activeMouseButtonMask.load(std::memory_order_acquire);
        if (clearBits != 0U) {
            newMask = sharedState_.activeMouseButtonMask.fetch_and(~clearBits, std::memory_order_acq_rel) & ~clearBits;
        }
        if (setBits != 0U) {
            newMask = sharedState_.activeMouseButtonMask.fetch_or(setBits, std::memory_order_acq_rel) | setBits;
        }
        const bool ok = SubmitMouseReport(newMask, 0, 0, wheelDelta, 0);
        lastMouseSubmitOk_ = ok;
        return ok;
    }
    if (!relMouse && !prsMouse) {
        StampVirtualEchoToken(sharedState_, relVk, false);
        StampVirtualEchoToken(sharedState_, prsVk, true);
        StampSyntheticWrite();
        sharedState_.syntheticHeldState[relVk].store(0U, std::memory_order_release);
        sharedState_.syntheticHeldState[prsVk].store(1U, std::memory_order_release);
        sharedState_.syntheticKeyBits[relVk >> 6].fetch_and(~(1ULL << (relVk & 63U)), std::memory_order_acq_rel);
        sharedState_.syntheticKeyBits[prsVk >> 6].fetch_or(1ULL << (prsVk & 63U), std::memory_order_acq_rel);
        return SubmitKeyboardReport();
    }
    // Different devices: two reports, issued back to back (press first so there is no gap).
    const bool pressed  = DispatchKeyTransition(prsVk, true);
    const bool released = DispatchKeyTransition(relVk, false);
    return pressed && released;
}

bool InputDispatcher::DispatchMouseMoveUncoalesced(int32_t dx, int32_t dy) noexcept {
    if (dx == 0 && dy == 0) {
        return true;
    }
    const uint32_t currentButtons = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
    const bool ok = SubmitMouseReport(currentButtons, dx, dy, 0, 0);
    lastMouseSubmitOk_ = ok;
    return ok;
}

bool InputDispatcher::DispatchScrollWheel(int32_t wheelDelta, int32_t panDelta) noexcept {
    if (wheelDelta == 0 && panDelta == 0) {
        return true;
    }
    StampSyntheticWrite();
    const uint32_t currentButtons = sharedState_.activeMouseButtonMask.load(std::memory_order_relaxed);
    const bool ok = SubmitMouseReport(currentButtons, 0, 0, wheelDelta, panDelta);
    lastMouseSubmitOk_ = ok;
    return ok;
}

bool InputDispatcher::DispatchAbsoluteCursorMove(int32_t screenX, int32_t screenY) noexcept {
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
    if (hmHost_.AreControllersLive() || sharedState_.benchDryRunOutput.load(std::memory_order_relaxed)) {
        StampSyntheticWrite();
        const bool hidOk = SubmitMouseReport(currentButtons, 0, 0, 0, 0);
        lastMouseSubmitOk_ = hidOk;
        return (moved != FALSE) && hidOk;
    }
    if (moved != FALSE) {
        ++totalDispatched_;
        return true;
    }
    return false;
}

void InputDispatcher::StampSyntheticWrite() noexcept {
    unsigned int aux = 0;
    sharedState_.lastSyntheticHardwareWriteTsc.store(__rdtscp(&aux), std::memory_order_release);
}

bool InputDispatcher::SubmitMouseReport(
    uint32_t buttons,
    int32_t dx,
    int32_t dy,
    int32_t wheel,
    int32_t pan
) noexcept {
    if (batchQueueMode_) {
        return EnqueueMouseItem(buttons, dx, dy, wheel, pan, 0ULL);
    }
    return Record(hmHost_.SubmitMouseHardware(buttons, dx, dy, wheel, pan));
}

bool InputDispatcher::SubmitKeyboardReport() noexcept {
    uint8_t modifiersMask = 0;
    std::array<uint8_t, 6> activeHidKeys{};
    int32_t keyCount = 0;

    for (size_t w = 0; w < 4; ++w) {
        uint64_t bits = sharedState_.syntheticKeyBits[w].load(std::memory_order_acquire);
        while (bits != 0ULL) {
            const uint32_t bitIdx = FirstSetBitIndex64(bits);
            bits &= (bits - 1ULL);
            const uint16_t vk = static_cast<uint16_t>((w << 6) | bitIdx);
            if (sharedState_.syntheticHeldState[vk].load(std::memory_order_acquire) == 0U) {
                sharedState_.syntheticKeyBits[w].fetch_and(~(1ULL << bitIdx), std::memory_order_acq_rel);
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
    const bool ok = batchQueueMode_
        ? EnqueueKeyboardItem(modifiersMask, activeHidKeys.data(), keyCount, 0ULL)
        : Record(hmHost_.SubmitKeyboardHardware(modifiersMask, activeHidKeys.data(), keyCount));
    lastKbSubmitOk_ = ok;
    return ok;
}

bool InputDispatcher::Record(bool ok) noexcept {
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

} // namespace mask::input
