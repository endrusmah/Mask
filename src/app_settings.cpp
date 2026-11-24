#include "app_settings.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace mask::settings {

namespace {

struct AppSettingsV1 {
    uint32_t magic{kSettingsMagic};
    uint32_t version{kSettingsVersionV1};

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

    uint8_t  animationQuality{static_cast<uint8_t>(AnimationQuality::Full)};
    uint8_t  accentPreset{static_cast<uint8_t>(AccentPreset::CosmicCyan)};
    bool     compactSidebar{false};
    bool     showSignalView{true};
    bool     showLiveKeyIndicators{true};

    bool     lowResourceWhenUnfocused{true};
    bool     trimMemoryWhenMinimized{true};
    uint8_t  powerMode{static_cast<uint8_t>(EnginePowerMode::Balanced)};
    uint32_t armedPollHz{1000};
    uint8_t  spinPolicy{0};

    uint16_t pauseHotkeyVk{VK_F8};
    uint16_t stopAllHotkeyVk{0};
    bool     panicChordEnabled{true};
    bool     stopOnSessionLock{true};
    bool     suppressWhileTypingInApp{true};
    bool     soundFeedback{false};

    uint8_t  activeProfileBank{0};
    wchar_t  mouseProfileId[64]{L"logitech-g502-hero"};
    wchar_t  keyboardProfileId[64]{L"keychron-q6"};
};

static_assert(std::is_trivially_copyable_v<AppSettingsV1>, "AppSettingsV1 must remain trivially copyable.");
static_assert(sizeof(AppSettingsV1) == sizeof(AppSettings), "AppSettingsV1 layout must match AppSettings.");

[[nodiscard]] bool SanitizeBool(const bool& value) noexcept {
    uint8_t raw = 0;
    std::memcpy(&raw, &value, 1);
    return raw != 0;
}

[[nodiscard]] uint32_t SnapArmedPollHz(uint32_t hz) noexcept {
    if (hz <= 375U) {
        return 250U;
    }
    if (hz <= 750U) {
        return 500U;
    }
    return 1000U;
}

[[nodiscard]] AppSettings MigrateV1ToV2(const AppSettingsV1& v1) noexcept {
    AppSettings out{};
    std::memcpy(&out, &v1, sizeof(AppSettings));
    out.magic   = kSettingsMagic;
    out.version = kSettingsVersion;
    if (out.accentPreset > static_cast<uint8_t>(AccentPreset::AuroraTeal)) {
        out.accentPreset = static_cast<uint8_t>(AccentPreset::CosmicCyan);
    }
    return out;
}

[[nodiscard]] bool ReadSettingsFileRaw(const wchar_t* path, AppSettings& out) noexcept {
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
    const bool sizeOk = GetFileSizeEx(h, &fileSize) != FALSE;

    bool ok = false;
    if (sizeOk && fileSize.QuadPart == static_cast<LONGLONG>(sizeof(AppSettings))) {
        AppSettings loaded{};
        DWORD bytesRead = 0;
        if (ReadFile(h, &loaded, static_cast<DWORD>(sizeof(AppSettings)), &bytesRead, nullptr) != FALSE &&
            bytesRead == static_cast<DWORD>(sizeof(AppSettings)) &&
            loaded.magic == kSettingsMagic) {
            if (loaded.version == kSettingsVersion) {
                out = loaded;
                ok  = true;
            } else if (loaded.version == kSettingsVersionV1) {
                AppSettingsV1 v1{};
                std::memcpy(&v1, &loaded, sizeof(AppSettingsV1));
                out = MigrateV1ToV2(v1);
                ok  = true;
            }
        }
    } else if (sizeOk && fileSize.QuadPart == static_cast<LONGLONG>(sizeof(AppSettingsV1))) {
        AppSettingsV1 v1{};
        DWORD bytesRead = 0;
        if (ReadFile(h, &v1, static_cast<DWORD>(sizeof(AppSettingsV1)), &bytesRead, nullptr) != FALSE &&
            bytesRead == static_cast<DWORD>(sizeof(AppSettingsV1)) &&
            v1.magic == kSettingsMagic &&
            v1.version == kSettingsVersionV1) {
            out = MigrateV1ToV2(v1);
            ok  = true;
        }
    }

    CloseHandle(h);
    return ok;
}

[[nodiscard]] bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept {
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

void CleanupTempFileTriplet(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return;
    }
    DeleteFileW(path);
    wchar_t aux[1040]{};
    if (std::swprintf(aux, std::size(aux), L"%ls.bak", path) >= 0) {
        DeleteFileW(aux);
    }
    if (std::swprintf(aux, std::size(aux), L"%ls.tmp", path) >= 0) {
        DeleteFileW(aux);
    }
}

[[nodiscard]] bool BuildUniqueTempFilePath(const wchar_t* tag, wchar_t* outPath, size_t outCap) noexcept {
    static std::atomic<uint32_t> s_seq{0};
    wchar_t tempDir[MAX_PATH]{};
    const DWORD len = GetTempPathW(static_cast<DWORD>(std::size(tempDir)), tempDir);
    if (len == 0U || len >= static_cast<DWORD>(std::size(tempDir))) {
        return false;
    }
    const uint32_t seq = s_seq.fetch_add(1U, std::memory_order_relaxed);
    return std::swprintf(
        outPath,
        outCap,
        L"%ls%ls_%lu_%u.bin",
        tempDir,
        tag,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned>(seq)) > 0;
}

} // namespace

void SanitizeSettings(AppSettings& s) noexcept {
    s.magic   = kSettingsMagic;
    s.version = kSettingsVersion;

    // General
    s.startWithWindows     = SanitizeBool(s.startWithWindows);
    s.startMinimized       = SanitizeBool(s.startMinimized);
    s.minimizeToTray       = SanitizeBool(s.minimizeToTray);
    s.alwaysOnTop          = SanitizeBool(s.alwaysOnTop);
    s.rememberWindowBounds = SanitizeBool(s.rememberWindowBounds);
    s.confirmDelete        = SanitizeBool(s.confirmDelete);
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
    if (s.accentPreset > static_cast<uint8_t>(AccentPreset::AuroraTeal)) {
        s.accentPreset = static_cast<uint8_t>(AccentPreset::CosmicCyan);
    }
    s.compactSidebar        = SanitizeBool(s.compactSidebar);
    s.showSignalView        = SanitizeBool(s.showSignalView);
    s.showLiveKeyIndicators = SanitizeBool(s.showLiveKeyIndicators);

    // Performance & Background
    s.lowResourceWhenUnfocused = SanitizeBool(s.lowResourceWhenUnfocused);
    s.trimMemoryWhenMinimized  = SanitizeBool(s.trimMemoryWhenMinimized);
    if (s.powerMode > static_cast<uint8_t>(EnginePowerMode::Eco)) {
        s.powerMode = static_cast<uint8_t>(EnginePowerMode::Balanced);
    }
    s.armedPollHz = SnapArmedPollHz(s.armedPollHz);
    if (s.spinPolicy > 2U) {
        s.spinPolicy = 0U;
    }

    // Hotkeys & Safety
    s.pauseHotkeyVk            = static_cast<uint16_t>(s.pauseHotkeyVk & 0xFFU);
    s.stopAllHotkeyVk          = static_cast<uint16_t>(s.stopAllHotkeyVk & 0xFFU);
    s.panicChordEnabled        = SanitizeBool(s.panicChordEnabled);
    s.stopOnSessionLock        = SanitizeBool(s.stopOnSessionLock);
    s.suppressWhileTypingInApp = SanitizeBool(s.suppressWhileTypingInApp);
    s.soundFeedback            = SanitizeBool(s.soundFeedback);

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

bool SaveSettings(const wchar_t* path, const AppSettings& settings) noexcept {
    AppSettings clean = settings;
    SanitizeSettings(clean);
    return WriteFileAtomic(path, &clean, sizeof(AppSettings));
}

bool LoadSettings(const wchar_t* path, AppSettings& out) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }

    AppSettings loaded{};
    bool ok = ReadSettingsFileRaw(path, loaded);
    if (!ok) {
        wchar_t bak[1040]{};
        if (std::swprintf(bak, std::size(bak), L"%ls.bak", path) >= 0) {
            ok = ReadSettingsFileRaw(bak, loaded);
        }
    }
    if (!ok) {
        return false;
    }

    SanitizeSettings(loaded);
    out = loaded;
    return true;
}

bool SetStartWithWindows(bool enable, bool startMinimized) noexcept {
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

bool QueryStartWithWindows() noexcept {
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

void ApplySettingsToSharedState(const AppSettings& s, mask::core::EngineSharedState& shared) noexcept {
    const uint32_t hz = SnapArmedPollHz(s.armedPollHz);
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

bool RunSettingsMigrationSelfTest() noexcept {
    wchar_t path1[1040]{};
    wchar_t path2[1040]{};
    if (!BuildUniqueTempFilePath(L"mask_set_v1_clamp", path1, std::size(path1)) ||
        !BuildUniqueTempFilePath(L"mask_set_v1_valid", path2, std::size(path2))) {
        return false;
    }

    struct CleanupGuard {
        const wchar_t* p1;
        const wchar_t* p2;
        ~CleanupGuard() noexcept {
            CleanupTempFileTriplet(p1);
            CleanupTempFileTriplet(p2);
        }
    } cleanup{path1, path2};

    // Case 1: v1 file with out-of-range accentPreset -> clamps to CosmicCyan (0), upgrades version -> 2
    {
        AppSettingsV1 v1{};
        v1.magic                    = kSettingsMagic;
        v1.version                  = kSettingsVersionV1;
        v1.accentPreset             = 42U;
        v1.animationQuality         = static_cast<uint8_t>(AnimationQuality::Reduced);
        v1.powerMode                = static_cast<uint8_t>(EnginePowerMode::Eco);
        v1.armedPollHz              = 500U;
        v1.pauseHotkeyVk            = VK_F9;
        v1.stopAllHotkeyVk          = VK_F10;
        v1.activeProfileBank        = 2U;
        v1.windowW                  = 1440;
        v1.windowH                  = 900;
        wcscpy_s(v1.mouseProfileId, L"razer-viper-v2");
        wcscpy_s(v1.keyboardProfileId, L"wooting-60he");

        if (!WriteFileAtomic(path1, &v1, sizeof(AppSettingsV1))) {
            return false;
        }

        AppSettings loaded{};
        if (!LoadSettings(path1, loaded)) {
            return false;
        }
        if (loaded.magic != kSettingsMagic ||
            loaded.version != kSettingsVersion ||
            loaded.accentPreset != static_cast<uint8_t>(AccentPreset::CosmicCyan) ||
            loaded.animationQuality != static_cast<uint8_t>(AnimationQuality::Reduced) ||
            loaded.powerMode != static_cast<uint8_t>(EnginePowerMode::Eco) ||
            loaded.armedPollHz != 500U ||
            loaded.pauseHotkeyVk != VK_F9 ||
            loaded.stopAllHotkeyVk != VK_F10 ||
            loaded.activeProfileBank != 2U ||
            loaded.windowW != 1440 ||
            loaded.windowH != 900 ||
            std::wcscmp(loaded.mouseProfileId, L"razer-viper-v2") != 0 ||
            std::wcscmp(loaded.keyboardProfileId, L"wooting-60he") != 0) {
            return false;
        }

        // Save as v2 and verify round-trip reload
        if (!SaveSettings(path1, loaded)) {
            return false;
        }
        AppSettings v2Reloaded{};
        if (!LoadSettings(path1, v2Reloaded) ||
            v2Reloaded.version != kSettingsVersion ||
            v2Reloaded.pauseHotkeyVk != VK_F9 ||
            v2Reloaded.activeProfileBank != 2U) {
            return false;
        }
    }

    // Case 2: v1 file with valid accentPreset preserved (NebulaViolet = 3)
    {
        AppSettingsV1 v1{};
        v1.magic        = kSettingsMagic;
        v1.version      = kSettingsVersionV1;
        v1.accentPreset = static_cast<uint8_t>(AccentPreset::NebulaViolet);
        v1.compactSidebar = true;

        if (!WriteFileAtomic(path2, &v1, sizeof(AppSettingsV1))) {
            return false;
        }

        AppSettings loaded{};
        if (!LoadSettings(path2, loaded) ||
            loaded.version != kSettingsVersion ||
            loaded.accentPreset != static_cast<uint8_t>(AccentPreset::NebulaViolet) ||
            !loaded.compactSidebar) {
            return false;
        }
    }

    return true;
}

} // namespace mask::settings
