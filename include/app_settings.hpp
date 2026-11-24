#pragma once

// Persistent application settings, Windows Run-key startup registration, and synchronization
// of user preferences into mask::core::EngineSharedState.

#include "core_contracts.hpp"

#include <cstdint>
#include <type_traits>

namespace mask::settings {

inline constexpr uint32_t kSettingsMagic     = 0x4D534554U; // 'MSET'
inline constexpr uint32_t kSettingsVersionV1 = 1U;
inline constexpr uint32_t kSettingsVersion   = 2U;
inline constexpr uint8_t  kProfileBankCount  = 3U;          // 0..2 = Profile 1..3

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
    CosmicCyan   = 0,
    DeepSapphire = 1,
    IceStarlight = 2,
    NebulaViolet = 3,
    AuroraTeal   = 4
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
    uint8_t  accentPreset{static_cast<uint8_t>(AccentPreset::CosmicCyan)};
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
    uint16_t pauseHotkeyVk{mask::core::kVkDefaultPause};
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

/**
 * @brief Clamps and normalizes every field of AppSettings so corrupt or hand-edited files
 *        cannot place the UI or real-time engine into an invalid state.
 */
void SanitizeSettings(AppSettings& s) noexcept;

/**
 * @brief Crash-safe atomic settings save: writes sanitized AppSettings to "<path>.tmp",
 *        flushes to disk, then swaps into "<path>" preserving the previous version as "<path>.bak".
 */
[[nodiscard]] bool SaveSettings(const wchar_t* path, const AppSettings& settings) noexcept;

/**
 * @brief Loads AppSettings (v1 or v2, migrating v1 -> v2) from "<path>", falling back to "<path>.bak"
 *        if the primary file is missing or corrupt. Leaves `out` unmodified on failure.
 */
[[nodiscard]] bool LoadSettings(const wchar_t* path, AppSettings& out) noexcept;

/**
 * @brief Registers or removes MaskEngine in HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
 *        When enabling with `startMinimized == true`, appends `--minimized` to the command line.
 */
[[nodiscard]] bool SetStartWithWindows(bool enable, bool startMinimized = false) noexcept;

/**
 * @brief Returns true if MaskEngine currently has a non-empty Run entry for the current user.
 */
[[nodiscard]] bool QueryStartWithWindows() noexcept;

/**
 * @brief Applies runtime-relevant preferences from AppSettings to EngineSharedState atomically.
 */
void ApplySettingsToSharedState(const AppSettings& s, mask::core::EngineSharedState& shared) noexcept;

/**
 * @brief Verifies v1 -> v2 settings.bin binary migration (and accentPreset clamping) using %TEMP%.
 */
[[nodiscard]] bool RunSettingsMigrationSelfTest() noexcept;

} // namespace mask::settings
