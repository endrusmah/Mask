#pragma once

#include "core_contracts.hpp"

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <atomic>

namespace mask::bridge {

// HostFXR / CoreCLR Hosting ABI Definitions (matching .NET 10 hostfxr.h & coreclr_delegates.h)
enum class HostfxrDelegateType : int32_t {
    ComActivation = 0,
    LoadInMemoryAssembly = 1,
    WinrtActivation = 2,
    ComRegister = 3,
    ComUnregister = 4,
    LoadAssemblyAndGetFunctionPointer = 5,
    GetFunctionPointer = 6,
    LoadAssembly = 7,
    LoadAssemblyBytes = 8
};

struct HostfxrInitializeParameters {
    size_t         size;
    const wchar_t* hostPath;
    const wchar_t* dotnetRoot;
};

using hostfxr_handle = void*;

using hostfxr_initialize_for_runtime_config_fn = int32_t(__cdecl*)(
    const wchar_t* runtimeConfigPath,
    const HostfxrInitializeParameters* parameters,
    hostfxr_handle* hostContextHandle
);

using hostfxr_get_runtime_delegate_fn = int32_t(__cdecl*)(
    const hostfxr_handle hostContextHandle,
    HostfxrDelegateType type,
    void** delegateOut
);

using hostfxr_close_fn = int32_t(__cdecl*)(const hostfxr_handle hostContextHandle);

using load_assembly_and_get_function_pointer_fn = int(__stdcall*)(
    const wchar_t* assemblyPath,
    const wchar_t* typeName,
    const wchar_t* methodName,
    const wchar_t* delegateTypeName,
    void* reserved,
    void** delegateOut
);

inline const wchar_t* const kUnmanagedCallersOnlyMethod = reinterpret_cast<const wchar_t*>(static_cast<intptr_t>(-1));

// UnmanagedCallersOnly Entry Point Signatures in Mask.HIDMaestroBridge.dll
using HMBridge_Initialize_Fn = int(__stdcall*)(
    const wchar_t* mouseProfileId,
    const wchar_t* keyboardProfileId,
    wchar_t* statusOutBuffer,
    int statusOutBufferChars
);

using HMBridge_SubmitMouse_Fn = int(__stdcall*)(
    uint32_t buttonsMask,
    int32_t dx,
    int32_t dy,
    int32_t wheel,
    int32_t pan
);

using HMBridge_SubmitKeyboard_Fn = int(__stdcall*)(
    uint8_t modifiersMask,
    const uint8_t* hidKeyCodes,
    int32_t keyCount
);

using HMBridge_SubmitBatch_Fn = int(__stdcall*)(
    const mask::core::OutputBatchItem* items,
    int32_t count,
    int32_t dryRun
);

using HMBridge_ReleaseAllNeutral_Fn = int(__stdcall*)();

using HMBridge_EmergencyTeardown_Fn = int(__stdcall*)(int32_t emergencyScopeEverything);

using HMBridge_QueryTelemetry_Fn = void(__stdcall*)(
    int32_t* outInitialized,
    int32_t* outElevated,
    int32_t* outDriverInstalled,
    int32_t* outMouseTransportIdx,
    int32_t* outKbTransportIdx,
    int32_t* outDeliversEveryReport,
    int32_t* outReleasesOnExit,
    int32_t* outKeyboardLeds
);

using HMBridge_GetActiveHardwareIds_Fn = void(__stdcall*)(
    uint16_t* outMouseVid,
    uint16_t* outMousePid,
    uint16_t* outKbVid,
    uint16_t* outKbPid
);

/**
 * @brief HIDMaestro v2.1.0 HMKey (USB HID Usage Page 0x07) & HMKeyModifiers definitions.
 */
namespace hm {

enum HMKeyModifiers : uint8_t {
    Mod_None         = 0,
    Mod_LeftControl  = 1 << 0,
    Mod_LeftShift    = 1 << 1,
    Mod_LeftAlt      = 1 << 2,
    Mod_LeftGui      = 1 << 3,
    Mod_RightControl = 1 << 4,
    Mod_RightShift   = 1 << 5,
    Mod_RightAlt     = 1 << 6,
    Mod_RightGui     = 1 << 7
};

enum HMMouseButtons : uint32_t {
    MouseBtn_None    = 0,
    MouseBtn_Left    = 1U << 0,
    MouseBtn_Right   = 1U << 1,
    MouseBtn_Middle  = 1U << 2,
    MouseBtn_Back    = 1U << 3,
    MouseBtn_Forward = 1U << 4,
    MouseBtn_Button6 = 1U << 5,
    MouseBtn_Button7 = 1U << 6,
    MouseBtn_Button8 = 1U << 7
};

/**
 * @brief Converts a Win32 Virtual-Key code (0..255) to a HIDMaestro v2.1.0 `HMKey` (USB HID Usage Page 0x07 byte).
 */
[[nodiscard]] constexpr uint8_t VkToHMKey(uint16_t vk) noexcept {
    const uint8_t c = static_cast<uint8_t>(vk & 0xFFU);
    if (c >= 'A' && c <= 'Z') {
        return static_cast<uint8_t>(4 + (c - 'A')); // HMKey.A = 4 .. HMKey.Z = 29
    }
    if (c >= '1' && c <= '9') {
        return static_cast<uint8_t>(30 + (c - '1')); // HMKey.D1 = 30 .. HMKey.D9 = 38
    }
    if (c == '0') {
        return 39; // HMKey.D0 = 39
    }
    if (c >= VK_F1 && c <= VK_F12) {
        return static_cast<uint8_t>(58 + (c - VK_F1)); // HMKey.F1 = 58 .. HMKey.F12 = 69
    }
    if (c >= VK_F13 && c <= VK_F24) {
        return static_cast<uint8_t>(104 + (c - VK_F13)); // HMKey.F13 = 104 .. HMKey.F24 = 115
    }

    switch (c) {
        case VK_RETURN:     return 40;  // Enter
        case VK_ESCAPE:     return 41;  // Escape
        case VK_BACK:       return 42;  // Backspace
        case VK_TAB:        return 43;  // Tab
        case VK_SPACE:      return 44;  // Space
        case VK_OEM_MINUS:  return 45;  // Minus
        case VK_OEM_PLUS:   return 46;  // Equals
        case VK_OEM_4:      return 47;  // LeftBracket
        case VK_OEM_6:      return 48;  // RightBracket
        case VK_OEM_5:      return 49;  // Backslash
        case VK_OEM_1:      return 51;  // Semicolon
        case VK_OEM_7:      return 52;  // Apostrophe
        case VK_OEM_3:      return 53;  // Grave
        case VK_OEM_COMMA:  return 54;  // Comma
        case VK_OEM_PERIOD: return 55;  // Period
        case VK_OEM_2:      return 56;  // Slash
        case VK_CAPITAL:    return 57;  // CapsLock
        case VK_SNAPSHOT:   return 70;  // PrintScreen
        case VK_SCROLL:     return 71;  // ScrollLock
        case VK_PAUSE:      return 72;  // Pause
        case VK_INSERT:     return 73;  // Insert
        case VK_HOME:       return 74;  // Home
        case VK_PRIOR:      return 75;  // PageUp
        case VK_DELETE:     return 76;  // Delete
        case VK_END:        return 77;  // End
        case VK_NEXT:       return 78;  // PageDown
        case VK_RIGHT:      return 79;  // Right
        case VK_LEFT:       return 80;  // Left
        case VK_DOWN:       return 81;  // Down
        case VK_UP:         return 82;  // Up
        case VK_NUMLOCK:    return 83;  // NumLock
        case VK_DIVIDE:     return 84;  // KeypadDivide
        case VK_MULTIPLY:   return 85;  // KeypadMultiply
        case VK_SUBTRACT:   return 86;  // KeypadMinus
        case VK_ADD:        return 87;  // KeypadPlus
        case VK_NUMPAD1:    return 89;  // Keypad1
        case VK_NUMPAD2:    return 90;  // Keypad2
        case VK_NUMPAD3:    return 91;  // Keypad3
        case VK_NUMPAD4:    return 92;  // Keypad4
        case VK_NUMPAD5:    return 93;  // Keypad5
        case VK_NUMPAD6:    return 94;  // Keypad6
        case VK_NUMPAD7:    return 95;  // Keypad7
        case VK_NUMPAD8:    return 96;  // Keypad8
        case VK_NUMPAD9:    return 97;  // Keypad9
        case VK_NUMPAD0:    return 98;  // Keypad0
        case VK_DECIMAL:    return 99;  // KeypadPeriod
        case VK_APPS:       return 101; // Application
        case VK_VOLUME_MUTE:return 127; // Mute
        case VK_VOLUME_UP:  return 128; // VolumeUp
        case VK_VOLUME_DOWN:return 129; // VolumeDown
        case VK_CONTROL:
        case VK_LCONTROL:   return 224; // LeftControl
        case VK_SHIFT:
        case VK_LSHIFT:     return 225; // LeftShift
        case VK_MENU:
        case VK_LMENU:      return 226; // LeftAlt
        case VK_LWIN:       return 227; // LeftGui
        case VK_RCONTROL:   return 228; // RightControl
        case VK_RSHIFT:     return 229; // RightShift
        case VK_RMENU:      return 230; // RightAlt
        case VK_RWIN:       return 231; // RightGui
        default:            return 0;   // HMKey.None
    }
}

/**
 * @brief Returns the `HMKeyModifiers` bit if `vk` is a modifier key, or 0 if it is a regular key.
 */
[[nodiscard]] constexpr uint8_t VkToHMModifierBit(uint16_t vk) noexcept {
    switch (vk & 0xFFU) {
        case VK_CONTROL:
        case VK_LCONTROL: return Mod_LeftControl;
        case VK_SHIFT:
        case VK_LSHIFT:   return Mod_LeftShift;
        case VK_MENU:
        case VK_LMENU:    return Mod_LeftAlt;
        case VK_LWIN:     return Mod_LeftGui;
        case VK_RCONTROL: return Mod_RightControl;
        case VK_RSHIFT:   return Mod_RightShift;
        case VK_RMENU:    return Mod_RightAlt;
        case VK_RWIN:     return Mod_RightGui;
        default:          return Mod_None;
    }
}

/**
 * @brief Returns the `HMMouseButtons` bitmask for a mouse Virtual-Key code.
 */
[[nodiscard]] constexpr uint32_t VkToHMMouseButtonBit(uint16_t vk) noexcept {
    switch (vk & 0xFFU) {
        case VK_LBUTTON:  return MouseBtn_Left;
        case VK_RBUTTON:  return MouseBtn_Right;
        case VK_MBUTTON:  return MouseBtn_Middle;
        case VK_XBUTTON1: return MouseBtn_Back;
        case VK_XBUTTON2: return MouseBtn_Forward;
        default:          return MouseBtn_None;
    }
}

} // namespace hm

/**
 * @brief Native C++23 HostFXR Loader & Function-Pointer Dispatcher for `HIDMaestro.Core.dll` v2.1.0.
 * Loads CoreCLR in-process once at startup, resolves direct `UnmanagedCallersOnly` function pointers,
 * and provides lock-free / zero-allocation hardware mouse, keyboard & batched submission methods.
 */
class HIDMaestroHost {
public:
    static HIDMaestroHost& Instance() noexcept;

    bool LoadAndInitialize(
        const wchar_t* mouseProfileId = L"logitech-g502-hero",
        const wchar_t* keyboardProfileId = L"keychron-q6"
    ) noexcept;

    [[nodiscard]] __forceinline bool SubmitMouseHardware(
        uint32_t buttonsMask,
        int32_t dx,
        int32_t dy,
        int32_t wheel = 0,
        int32_t pan = 0
    ) noexcept {
        const HMBridge_SubmitMouse_Fn fn = fnSubmitMouse_.load(std::memory_order_acquire);
        if (fn == nullptr) [[unlikely]] {
            return false;
        }
        return fn(buttonsMask, dx, dy, wheel, pan) == 0;
    }

    [[nodiscard]] __forceinline bool SubmitKeyboardHardware(
        uint8_t modifiersMask,
        const uint8_t* hidKeyCodes,
        int32_t keyCount
    ) noexcept {
        const HMBridge_SubmitKeyboard_Fn fn = fnSubmitKeyboard_.load(std::memory_order_acquire);
        if (fn == nullptr) [[unlikely]] {
            return false;
        }
        return fn(modifiersMask, hidKeyCodes, keyCount) == 0;
    }

    [[nodiscard]] __forceinline bool SubmitBatchHardware(
        const mask::core::OutputBatchItem* items,
        int32_t count,
        bool dryRun = false
    ) noexcept {
        if (count <= 0) {
            return count == 0;
        }
        if (items == nullptr) [[unlikely]] {
            return false;
        }
        const HMBridge_SubmitBatch_Fn fn = fnSubmitBatch_.load(std::memory_order_acquire);
        if (fn == nullptr) [[unlikely]] {
            return false;
        }
        return fn(items, count, dryRun ? 1 : 0) == 0;
    }

    void ReleaseAllNeutralHardware() noexcept;
    void EmergencyTeardownHardware(bool sweepEverything) noexcept;

    void QueryDriverTelemetry(
        bool& outControllersLive,
        bool& outProcessElevated,
        bool& outDriverInstalled,
        int32_t& outMouseTransportIdx,
        int32_t& outKbTransportIdx,
        bool& outDeliversEveryReport,
        bool& outReleasesOnExit,
        uint8_t& outKeyboardLeds
    ) const noexcept;

    [[nodiscard]] const wchar_t* GetLastStatusMessage() const noexcept {
        return lastStatusMessage_;
    }

    [[nodiscard]] const wchar_t* GetActiveMouseProfileId() const noexcept {
        return activeMouseProfileId_;
    }

    [[nodiscard]] const wchar_t* GetActiveKeyboardProfileId() const noexcept {
        return activeKeyboardProfileId_;
    }

    void GetActiveVirtualHardwareIds(
        uint16_t& outMouseVid,
        uint16_t& outMousePid,
        uint16_t& outKbVid,
        uint16_t& outKbPid
    ) const noexcept;

    [[nodiscard]] bool AreControllersLive() const noexcept {
        return controllersLive_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsHostLoaded() const noexcept {
        return hostLoaded_.load(std::memory_order_acquire);
    }

private:
    HIDMaestroHost() noexcept = default;
    ~HIDMaestroHost() noexcept = default;

    [[nodiscard]] static uint64_t ParseVersionKey(const wchar_t* text) noexcept;
    [[nodiscard]] static bool FindNewestHostfxrUnder(const wchar_t* dotnetRoot, wchar_t* outPath, size_t maxChars) noexcept;
    bool LocateHostfxrDll(wchar_t* outPath, size_t maxChars) noexcept;
    bool LoadDotNetHostAndDelegates() noexcept;

    SRWLOCK                      lifecycleLock_ = SRWLOCK_INIT;
    HMODULE                      hHostfxr_{nullptr};
    std::atomic<bool>            hostLoaded_{false};
    std::atomic<bool>            controllersLive_{false};
    std::atomic<uint16_t>        activeMouseVid_{0};
    std::atomic<uint16_t>        activeMousePid_{0};
    std::atomic<uint16_t>        activeKeyboardVid_{0};
    std::atomic<uint16_t>        activeKeyboardPid_{0};
    wchar_t                      activeMouseProfileId_[64]{L"logitech-g502-hero"};
    wchar_t                      activeKeyboardProfileId_[64]{L"keychron-q6"};
    wchar_t                      lastStatusMessage_[256]{L"HIDMaestro Host Uninitialized"};

    // Published by whichever thread loads the host; read by the engine, output, watchdog, and UI threads.
    std::atomic<HMBridge_Initialize_Fn>           fnInitialize_{nullptr};
    std::atomic<HMBridge_SubmitMouse_Fn>          fnSubmitMouse_{nullptr};
    std::atomic<HMBridge_SubmitKeyboard_Fn>       fnSubmitKeyboard_{nullptr};
    std::atomic<HMBridge_SubmitBatch_Fn>          fnSubmitBatch_{nullptr};
    std::atomic<HMBridge_ReleaseAllNeutral_Fn>    fnReleaseAllNeutral_{nullptr};
    std::atomic<HMBridge_EmergencyTeardown_Fn>    fnEmergencyTeardown_{nullptr};
    std::atomic<HMBridge_QueryTelemetry_Fn>       fnQueryTelemetry_{nullptr};
    std::atomic<HMBridge_GetActiveHardwareIds_Fn> fnGetActiveHardwareIds_{nullptr};
};

} // namespace mask::bridge
