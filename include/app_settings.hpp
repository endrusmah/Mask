#pragma once

// Persistent application settings, Windows Run-key startup registration, and synchronization
// of user preferences into mask::core::EngineSharedState.

#include "core_contracts.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <type_traits>

namespace mask::settings {

inline constexpr uint32_t kSettingsMagic    = 0x4D534554U; // 'MSET'
inline constexpr uint32_t kSettingsVersion  = 1U;
inline constexpr uint8_t  kProfileBankCount = 3U;          // 0..2 = Profile 1..3

inline constexpr wchar_t  kRunRegistrySubKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t  kRunRegistryValue[]  = L"MaskEngine";

enum class AnimationQuality : uint8_t {
    Full    = 0,
    Reduced = 1,
    Off     = 2
};

enum class EnginePowerMode : uint8_t {
    Balanced       = 0,
    MaxPerformance = 1,
    Eco            = 2
};

enum class AccentPreset : uint8_t {
    Amber   = 0,
    Cyan    = 1,
    Emerald = 2,
    Violet  = 3,
    Crimson = 4
};

struct AppSettings {
    uint32_t magic{kSettingsMagic};
    uint32_t version{kSettingsVersion};

    // General
    bool     startWithWindows{false};
    bool     startMinimized{false};
    bool     minimizeToTray{true};
    bool     alwaysOnTop{false};
    bool     rememberWindowBounds{true};
    bool     confirmDelete{false};
    uint32_t autosaveDelayMs{1200};
    int32_t  windowX{-1};
    int32_t  windowY{-1};
    int32_t  windowW{1160};
    int32_t  windowH{760};

    // Appearance & Animations
    uint8_t  animationQuality{static_cast<uint8_t>(AnimationQuality::Full)};
    uint8_t  accentPreset{static_cast<uint8_t>(AccentPreset::Amber)};
    bool     compactSidebar{false};
    bool     showSignalView{true};
    bool     showLiveKeyIndicators{true};

    // Performance & Background
    bool     lowResourceWhenUnfocused{true};
    bool     trimMemoryWhenMinimized{true};
    uint8_t  powerMode{static_cast<uint8_t>(EnginePowerMode::Balanced)};
    uint32_t armedPollHz{1000};       // 250 / 500 / 1000
    uint8_t  spinPolicy{0};           // 0=Auto, 1=MaxPrecision, 2=Eco

    // Hotkeys & Safety
    uint16_t pauseHotkeyVk{VK_F8};
    uint16_t stopAllHotkeyVk{0};
    bool     panicChordEnabled{true};
    bool     stopOnSessionLock{true};
    bool     suppressWhileTypingInApp{true};
    bool     soundFeedback{false};

    // Hardware & Profiles
    uint8_t  activeProfileBank{0};    // 0..2 = Profile 1..3
    wchar_t  mouseProfileId[64]{L"logitech-g502-hero"};
    wchar_t  keyboardProfileId[64]{L"keychron-q6"};
};

static_assert(std::is_trivially_copyable_v<AppSettings>, "AppSettings must remain trivially copyable.");

namespace detail {

[[nodiscard]] inline bool SanitizeBool(const bool& value) noexcept {
    uint8_t raw = 0;
    std::memcpy(&raw, &value, 1);
    return raw != 0;
}

[[nodiscard]] inline uint32_t SnapArmedPollHz(uint32_t hz) noexcept {
    if (hz <= 375U) {
        return 250U;
    }
    if (hz <= 750U) {
        return 500U;
    }
    return 1000U;
}

[[nodiscard]] inline bool ReadSettingsFileRaw(const wchar_t* path, AppSettings& out) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const HANDLE h = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    bool ok = GetFileSizeEx(h, &fileSize) != FALSE &&
              fileSize.QuadPart == static_cast<LONGLONG>(sizeof(AppSettings));

    AppSettings loaded{};
    if (ok) {
        DWORD bytesRead = 0;
        ok = ReadFile(h, &loaded, static_cast<DWORD>(sizeof(AppSettings)), &bytesRead, nullptr) != FALSE &&
             bytesRead == static_cast<DWORD>(sizeof(AppSettings)) &&
             loaded.magic == kSettingsMagic &&
             loaded.version == kSettingsVersion;
    }

    CloseHandle(h);
    if (ok) {
        out = loaded;
    }
    return ok;
}

[[nodiscard]] inline bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept {
    if (path == nullptr || path[0] == L'\0' || data == nullptr || size == 0U) {
        return false;
    }

    wchar_t tmp[1040]{};
    wchar_t bak[1040]{};
    if (std::swprintf(tmp, std::size(tmp), L"%ls.tmp", path) < 0 ||
        std::swprintf(bak, std::size(bak), L"%ls.bak", path) < 0) {
        return false;
    }

    // Best-effort creation of the immediate parent directory if it does not yet exist.
    wchar_t dir[1040]{};
    if (wcscpy_s(dir, path) == 0) {
        if (wchar_t* slash = std::wcsrchr(dir, L'\\')) {
            *slash = L'\0';
            if (dir[0] != L'\0') {
                CreateDirectoryW(dir, nullptr);
            }
        }
    }

    const HANDLE h = CreateFileW(
        tmp,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
                    written == static_cast<DWORD>(size) &&
                    FlushFileBuffers(h) != FALSE;
    CloseHandle(h);

    if (!ok) {
        DeleteFileW(tmp);
        return false;
    }

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
        ReplaceFileW(path, tmp, bak, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE) {
        return true;
    }
    if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
        return true;
    }

    DeleteFileW(tmp);
    return false;
}

} // namespace detail

/**
 * @brief Clamps and normalizes every field of AppSettings so corrupt or hand-edited files
 *        cannot place the UI or real-time engine into an invalid state.
 */
inline void SanitizeSettings(AppSettings& s) noexcept {
    s.magic   = kSettingsMagic;
    s.version = kSettingsVersion;

    // General
    s.startWithWindows     = detail::SanitizeBool(s.startWithWindows);
    s.startMinimized       = detail::SanitizeBool(s.startMinimized);
    s.minimizeToTray       = detail::SanitizeBool(s.minimizeToTray);
    s.alwaysOnTop          = detail::SanitizeBool(s.alwaysOnTop);
    s.rememberWindowBounds = detail::SanitizeBool(s.rememberWindowBounds);
    s.confirmDelete        = detail::SanitizeBool(s.confirmDelete);
    s.autosaveDelayMs      = std::clamp(s.autosaveDelayMs, 200U, 30'000U);
    if (s.windowX != -1) {
        s.windowX = std::clamp(s.windowX, -16384, 16384);
    }
    if (s.windowY != -1) {
        s.windowY = std::clamp(s.windowY, -16384, 16384);
    }
    s.windowW = std::clamp(s.windowW, 640, 7680);
    s.windowH = std::clamp(s.windowH, 480, 4320);

    // Appearance & Animations
    if (s.animationQuality > static_cast<uint8_t>(AnimationQuality::Off)) {
        s.animationQuality = static_cast<uint8_t>(AnimationQuality::Full);
    }
    if (s.accentPreset > static_cast<uint8_t>(AccentPreset::Crimson)) {
        s.accentPreset = static_cast<uint8_t>(AccentPreset::Amber);
    }
    s.compactSidebar        = detail::SanitizeBool(s.compactSidebar);
    s.showSignalView        = detail::SanitizeBool(s.showSignalView);
    s.showLiveKeyIndicators = detail::SanitizeBool(s.showLiveKeyIndicators);

    // Performance & Background
    s.lowResourceWhenUnfocused = detail::SanitizeBool(s.lowResourceWhenUnfocused);
    s.trimMemoryWhenMinimized  = detail::SanitizeBool(s.trimMemoryWhenMinimized);
    if (s.powerMode > static_cast<uint8_t>(EnginePowerMode::Eco)) {
        s.powerMode = static_cast<uint8_t>(EnginePowerMode::Balanced);
    }
    s.armedPollHz = detail::SnapArmedPollHz(s.armedPollHz);
    if (s.spinPolicy > 2U) {
        s.spinPolicy = 0U;
    }

    // Hotkeys & Safety
    s.pauseHotkeyVk            = static_cast<uint16_t>(s.pauseHotkeyVk & 0xFFU);
    s.stopAllHotkeyVk          = static_cast<uint16_t>(s.stopAllHotkeyVk & 0xFFU);
    s.panicChordEnabled        = detail::SanitizeBool(s.panicChordEnabled);
    s.stopOnSessionLock        = detail::SanitizeBool(s.stopOnSessionLock);
    s.suppressWhileTypingInApp = detail::SanitizeBool(s.suppressWhileTypingInApp);
    s.soundFeedback            = detail::SanitizeBool(s.soundFeedback);

    // Hardware & Profiles
    if (s.activeProfileBank >= kProfileBankCount) {
        s.activeProfileBank = 0U;
    }
    s.mouseProfileId[std::size(s.mouseProfileId) - 1] = L'\0';
    if (s.mouseProfileId[0] == L'\0') {
        wcscpy_s(s.mouseProfileId, L"logitech-g502-hero");
    }
    s.keyboardProfileId[std::size(s.keyboardProfileId) - 1] = L'\0';
    if (s.keyboardProfileId[0] == L'\0') {
        wcscpy_s(s.keyboardProfileId, L"keychron-q6");
    }
}

/**
 * @brief Crash-safe atomic settings save: writes sanitized AppSettings to "<path>.tmp",
 *        flushes to disk, then swaps into "<path>" preserving the previous version as "<path>.bak".
 */
[[nodiscard]] inline bool SaveSettings(const wchar_t* path, const AppSettings& settings) noexcept {
    AppSettings clean = settings;
    SanitizeSettings(clean);
    return detail::WriteFileAtomic(path, &clean, sizeof(AppSettings));
}

/**
 * @brief Loads AppSettings from "<path>", automatically falling back to "<path>.bak" if the
 *        primary file is missing or corrupt. Leaves `out` unmodified on failure.
 */
[[nodiscard]] inline bool LoadSettings(const wchar_t* path, AppSettings& out) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }

    AppSettings loaded{};
    bool ok = detail::ReadSettingsFileRaw(path, loaded);
    if (!ok) {
        wchar_t bak[1040]{};
        if (std::swprintf(bak, std::size(bak), L"%ls.bak", path) >= 0) {
            ok = detail::ReadSettingsFileRaw(bak, loaded);
        }
    }
    if (!ok) {
        return false;
    }

    SanitizeSettings(loaded);
    out = loaded;
    return true;
}

/**
 * @brief Registers or removes MaskEngine in HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
 *        When enabling with `startMinimized == true`, appends `--minimized` to the command line.
 */
[[nodiscard]] inline bool SetStartWithWindows(bool enable, bool startMinimized = false) noexcept {
    HKEY hKey = nullptr;
    const LONG openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kRunRegistrySubKey,
        0,
        KEY_SET_VALUE,
        &hKey
    );
    if (openStatus != ERROR_SUCCESS || hKey == nullptr) {
        return false;
    }

    bool ok = false;
    if (!enable) {
        const LONG delStatus = RegDeleteValueW(hKey, kRunRegistryValue);
        ok = (delStatus == ERROR_SUCCESS || delStatus == ERROR_FILE_NOT_FOUND);
    } else {
        wchar_t exePath[1024]{};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (len > 0U && len < static_cast<DWORD>(std::size(exePath))) {
            wchar_t cmdLine[1088]{};
            const int written = startMinimized
                ? std::swprintf(cmdLine, std::size(cmdLine), L"\"%ls\" --minimized", exePath)
                : std::swprintf(cmdLine, std::size(cmdLine), L"\"%ls\"", exePath);
            if (written > 0) {
                const DWORD byteCount = static_cast<DWORD>((std::wcslen(cmdLine) + 1U) * sizeof(wchar_t));
                const LONG setStatus = RegSetValueExW(
                    hKey,
                    kRunRegistryValue,
                    0,
                    REG_SZ,
                    reinterpret_cast<const BYTE*>(cmdLine),
                    byteCount
                );
                ok = (setStatus == ERROR_SUCCESS);
            }
        }
    }

    RegCloseKey(hKey);
    return ok;
}

/**
 * @brief Returns true if MaskEngine currently has a non-empty Run entry for the current user.
 */
[[nodiscard]] inline bool QueryStartWithWindows() noexcept {
    HKEY hKey = nullptr;
    const LONG openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kRunRegistrySubKey,
        0,
        KEY_QUERY_VALUE,
        &hKey
    );
    if (openStatus != ERROR_SUCCESS || hKey == nullptr) {
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    const LONG queryStatus = RegQueryValueExW(
        hKey,
        kRunRegistryValue,
        nullptr,
        &type,
        nullptr,
        &bytes
    );
    RegCloseKey(hKey);

    return queryStatus == ERROR_SUCCESS &&
           (type == REG_SZ || type == REG_EXPAND_SZ) &&
           bytes > sizeof(wchar_t);
}

/**
 * @brief Applies runtime-relevant preferences from AppSettings to EngineSharedState atomically.
 */
inline void ApplySettingsToSharedState(const AppSettings& s, mask::core::EngineSharedState& shared) noexcept {
    const uint32_t hz = detail::SnapArmedPollHz(s.armedPollHz);
    uint32_t pollIntervalUs = 1'000'000U / hz; // 1000Hz->1000us, 500Hz->2000us, 250Hz->4000us
    uint32_t effectiveSpin = (s.spinPolicy <= 2U) ? static_cast<uint32_t>(s.spinPolicy) : 0U;

    const auto power = static_cast<EnginePowerMode>(s.powerMode);
    if (power == EnginePowerMode::MaxPerformance) {
        if (effectiveSpin == 0U) {
            effectiveSpin = 1U; // MaxPrecision
        }
        pollIntervalUs = std::min(pollIntervalUs, 1000U);
    } else if (power == EnginePowerMode::Eco) {
        if (effectiveSpin == 0U) {
            effectiveSpin = 2U; // Eco
        }
        pollIntervalUs = std::max(pollIntervalUs, 2000U);
    }

    shared.globalPauseVk.store(static_cast<uint16_t>(s.pauseHotkeyVk & 0xFFU), std::memory_order_release);
    shared.globalStopAllVk.store(static_cast<uint16_t>(s.stopAllHotkeyVk & 0xFFU), std::memory_order_release);
    shared.armedPollIntervalUs.store(pollIntervalUs, std::memory_order_release);
    shared.spinPolicy.store(effectiveSpin, std::memory_order_release);
    shared.panicChordEnabled.store(s.panicChordEnabled, std::memory_order_release);
    shared.soundFeedbackEnabled.store(s.soundFeedback, std::memory_order_release);

    if (!s.lowResourceWhenUnfocused) {
        shared.lowPowerUiMode.store(false, std::memory_order_release);
    }
}

} // namespace mask::settings
