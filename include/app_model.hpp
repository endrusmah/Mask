#pragma once

// Application model for the Mask suite: Autoclickers, Complex Sequences (128 steps),
// Color Triggers, Multi-Device/Monitor Remap Rules, Persistent AppSettings,
// Multi-Bank storage, and plain-language descriptions. Owned by the UI thread.

#include "core_contracts.hpp"
#include "ui_actions.hpp"
#include "app_settings.hpp"
#include "input_pipeline.hpp"
#include "pixel_trigger_engine.hpp"
#include "complex_sequence_engine.hpp"
#include "remap_engine.hpp"
#include "ui_anim.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace mask::app {

using mask::core::MacroConfig;
using mask::core::MacroMode;
using mask::core::ComplexSequenceConfig;
using mask::core::ComplexStep;
using mask::core::ComplexStepKind;
using mask::core::ComplexConditionKind;
using mask::core::ColorTriggerConfig;
using mask::core::ColorTriggerMode;
using mask::core::ColorTriggerActionKind;
using mask::core::ColorTriggerMatchRule;
using mask::core::GetTriggerMatchRule;
using mask::core::SetTriggerMatchRule;
using mask::core::RemapRuleConfig;
using mask::core::RemapLayerAction;
using mask::core::RemapUnlockPolicy;

inline constexpr int kSlotCount         = static_cast<int>(mask::core::kMaxMacroSlots);
inline constexpr int kComplexSeqCount   = static_cast<int>(mask::core::kMaxComplexSequences);
inline constexpr int kComplexStepMax    = static_cast<int>(mask::core::kMaxComplexSteps);
inline constexpr int kColorTriggerCount = static_cast<int>(mask::core::kMaxColorTriggers);
inline constexpr int kRemapRuleCount    = static_cast<int>(mask::core::kMaxRemapRules);
inline constexpr int kRemapLayerCount   = static_cast<int>(mask::core::kMaxRemapLayers);
inline constexpr int kModeCount         = 3; // Hold (0), Toggle (1), Burst (2)

/// Unified sidebar item classification.
enum class ItemKind : int {
    Autoclicker = 0,
    Sequence    = 1,
    Trigger     = 2,
    Remap       = 3
};

[[nodiscard]] constexpr size_t AnimSlotFor(ItemKind kind, int index) noexcept {
    const size_t idx = static_cast<size_t>(index >= 0 ? index : 0);
    switch (kind) {
        case ItemKind::Autoclicker: return mask::ui::UiAnimator::kAutoclickerOffset + idx;
        case ItemKind::Sequence:    return mask::ui::UiAnimator::kSequenceOffset    + idx;
        case ItemKind::Trigger:     return mask::ui::UiAnimator::kTriggerOffset     + idx;
        case ItemKind::Remap:       return mask::ui::UiAnimator::kRemapOffset       + idx;
        default:                    return 0U;
    }
}

[[nodiscard]] constexpr bool DecodeAnimSlot(int animSlot, ItemKind& outKind, int& outIndex) noexcept {
    if (animSlot < 0 || static_cast<size_t>(animSlot) >= mask::ui::UiAnimator::kSlotAnimTotal) {
        return false;
    }
    const size_t s = static_cast<size_t>(animSlot);
    if (s < mask::ui::UiAnimator::kSequenceOffset) {
        outKind = ItemKind::Autoclicker;
        outIndex = static_cast<int>(s - mask::ui::UiAnimator::kAutoclickerOffset);
        return true;
    }
    if (s < mask::ui::UiAnimator::kTriggerOffset) {
        outKind = ItemKind::Sequence;
        outIndex = static_cast<int>(s - mask::ui::UiAnimator::kSequenceOffset);
        return true;
    }
    if (s < mask::ui::UiAnimator::kRemapOffset) {
        outKind = ItemKind::Trigger;
        outIndex = static_cast<int>(s - mask::ui::UiAnimator::kTriggerOffset);
        return true;
    }
    outKind = ItemKind::Remap;
    outIndex = static_cast<int>(s - mask::ui::UiAnimator::kRemapOffset);
    return true;
}

[[nodiscard]] constexpr int EncodeDeleteId(ItemKind kind, int index) noexcept {
    return (static_cast<int>(kind) << 8) | (index & 0xFF);
}

[[nodiscard]] constexpr ItemKind DecodeDeleteKind(int deleteId) noexcept {
    return static_cast<ItemKind>((deleteId >> 8) & 0xFF);
}

[[nodiscard]] constexpr int DecodeDeleteIndex(int deleteId) noexcept {
    return deleteId & 0xFF;
}

// =====================================================================================
//  Names & Step / Remap / Trigger Descriptions
// =====================================================================================

struct KeyName {
    wchar_t text[24]{};
};

[[nodiscard]] KeyName NameOf(uint16_t vk) noexcept;
[[nodiscard]] const wchar_t* ModeName(int mode) noexcept;
[[nodiscard]] const wchar_t* ModeDescription(int mode) noexcept;
[[nodiscard]] bool IsTimedMode(int mode) noexcept;

[[nodiscard]] const wchar_t* StepKindName(ComplexStepKind kind) noexcept;
[[nodiscard]] const wchar_t* ConditionKindName(ComplexConditionKind cond) noexcept;
[[nodiscard]] int CountStepKind(const ComplexSequenceConfig& seq, ComplexStepKind target) noexcept;
[[nodiscard]] int FindMatchingEndIf(const ComplexSequenceConfig& seq, int ifIndex) noexcept;
[[nodiscard]] bool HasElseInIfBlock(const ComplexSequenceConfig& seq, int ifIndex) noexcept;
[[nodiscard]] const wchar_t* SeqTriggerModeName(uint8_t mode) noexcept;
[[nodiscard]] const wchar_t* LayerName(uint8_t layer) noexcept;
void DescribeComplexStep(const ComplexStep& st, wchar_t* buf, size_t n) noexcept;
void DescribeComplexSequence(const ComplexSequenceConfig& q, wchar_t* buf, size_t n) noexcept;

[[nodiscard]] const wchar_t* RemapActionName(RemapLayerAction action) noexcept;
[[nodiscard]] const wchar_t* UnlockPolicyName(RemapUnlockPolicy pol) noexcept;
void DescribeRemapRule(const RemapRuleConfig& r, wchar_t* buf, size_t n) noexcept;

[[nodiscard]] const wchar_t* TriggerModeName(ColorTriggerMode mode) noexcept;
[[nodiscard]] const wchar_t* TriggerMatchRuleName(ColorTriggerMatchRule rule) noexcept;
[[nodiscard]] const wchar_t* ToleranceWord(uint8_t tol) noexcept;
void DescribeColorTrigger(const ColorTriggerConfig& t, wchar_t* buf, size_t n) noexcept;

// =====================================================================================
//  Number formatting & unrestricted timing maths
// =====================================================================================

void FormatNumber(double value, int maxDecimals, wchar_t* buf, size_t n) noexcept;
void FormatMs(double ms, wchar_t* buf, size_t n) noexcept;
void FormatDuration(double ms, wchar_t* buf, size_t n) noexcept;

[[nodiscard]] inline double DownMs(const MacroConfig& c) noexcept {
    return static_cast<double>(c.dwellDurationNs) / 1e6;
}

[[nodiscard]] inline double UpMs(const MacroConfig& c) noexcept {
    return static_cast<double>(c.recoveryDurationNs) / 1e6;
}

[[nodiscard]] inline double PeriodMs(const MacroConfig& c) noexcept {
    return DownMs(c) + UpMs(c);
}

[[nodiscard]] double RatePerSecond(const MacroConfig& c) noexcept;

[[nodiscard]] constexpr bool IsFullDuty(const MacroConfig&) noexcept {
    return false;
}

[[nodiscard]] double DutyPercent(const MacroConfig& c) noexcept;
[[nodiscard]] uint64_t MsToNs(double ms) noexcept;
void SetPeriodAndDuty(MacroConfig& c, double periodMs, double dutyFraction) noexcept;
void SetRate(MacroConfig& c, double perSecond) noexcept;
void SetDuty(MacroConfig& c, double percent) noexcept;
void FormatDuty(const MacroConfig& c, wchar_t* buf, size_t n) noexcept;
void FormatRate(double perSecond, wchar_t* buf, size_t n) noexcept;

[[nodiscard]] bool IsAutoclickerTimingRisky(const MacroConfig& c) noexcept;

void DescribeRate(double perSecond, wchar_t* buf, size_t n) noexcept;
void DescribeAutoclicker(const MacroConfig& c, wchar_t* buf, size_t n) noexcept;

// =====================================================================================
//  Editable fields
// =====================================================================================

enum class Field : int {
    None = 0,
    Name = 1,
    Rate = 2,
    Duty = 3,
    Down = 4,
    Up = 5,
    Repeat = 6,
    MoveX = 7,
    MoveY = 8,
    SeqName = 9,
    SeqStepDuration = 10,
    SeqStepX = 11,
    SeqStepY = 12,
    SeqStepParam = 13,
    SeqStepR = 14,
    SeqStepG = 15,
    SeqStepB = 24,
    SeqStepTol = 25,
    SeqStepText = 26,
    RemapName = 27,
    SettingsAutosaveMs = 28,
    TriggerName     = 40,
    TriggerDelay    = 41,
    TriggerHold     = 42,
    TriggerCooldown = 43,
    TriggerLinked   = 44
};

[[nodiscard]] constexpr bool IsDownTime(Field f) noexcept { return f == Field::Down; }
[[nodiscard]] constexpr bool IsUpTime(Field f) noexcept { return f == Field::Up; }
[[nodiscard]] constexpr bool IsTimeField(Field f) noexcept { return IsDownTime(f) || IsUpTime(f); }

[[nodiscard]] constexpr bool IsTextField(Field f) noexcept {
    return f == Field::Name || f == Field::SeqName || f == Field::RemapName ||
           f == Field::SeqStepText || f == Field::TriggerName;
}

[[nodiscard]] constexpr bool IsTriggerField(Field f) noexcept {
    return f == Field::TriggerDelay || f == Field::TriggerHold ||
           f == Field::TriggerCooldown || f == Field::TriggerLinked;
}

[[nodiscard]] constexpr bool IsAutoclickerField(Field f) noexcept {
    const int v = static_cast<int>(f);
    return v >= 1 && v <= 8;
}

[[nodiscard]] constexpr bool IsSeqStepField(Field f) noexcept {
    return f == Field::SeqStepDuration || f == Field::SeqStepX || f == Field::SeqStepY ||
           f == Field::SeqStepParam || f == Field::SeqStepR || f == Field::SeqStepG ||
           f == Field::SeqStepB || f == Field::SeqStepTol || f == Field::SeqStepText;
}

[[nodiscard]] constexpr bool AllowsDecimal(Field f) noexcept {
    return IsTimeField(f) || f == Field::Rate || f == Field::Duty;
}

[[nodiscard]] constexpr bool AllowsNegative(Field f) noexcept {
    return f == Field::MoveX || f == Field::MoveY || f == Field::SeqStepX || f == Field::SeqStepY || f == Field::SeqStepParam;
}

[[nodiscard]] double FieldGet(const MacroConfig& s, Field f) noexcept;
void FieldSet(MacroConfig& s, Field f, double v) noexcept;
void FormatFieldNumber(const MacroConfig& s, Field f, wchar_t* buf, size_t n) noexcept;
[[nodiscard]] const wchar_t* FieldSuffix(Field f) noexcept;
void FieldStep(MacroConfig& s, Field f, int direction, bool coarse, bool fine) noexcept;

// =====================================================================================
//  Output (virtual devices) state as the UI sees it
// =====================================================================================

enum class OutputState : int {
    Starting = 0,
    Ready,
    NeedsAdmin,
    Unavailable,
    NotResponding
};

// =====================================================================================
//  Profile storage helpers
// =====================================================================================

namespace storage {

inline constexpr uint32_t kMagic        = 0x4D41534BU; // 'MASK'
inline constexpr uint32_t kVersionV1    = 1U;
inline constexpr uint32_t kVersionV2    = 2U;
inline constexpr uint32_t kVersion      = 3U;
inline constexpr size_t   kHeaderBytes  = 8;
inline constexpr size_t   kPayloadBytes = sizeof(MacroConfig) * mask::core::kMaxMacroSlots;

[[nodiscard]] bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept;
[[nodiscard]] bool ReadProfileFile(const wchar_t* path,
                                   std::array<MacroConfig, mask::core::kMaxMacroSlots>& out,
                                   uint32_t* outFileVersion = nullptr) noexcept;
void Sanitize(MacroConfig& s, int index) noexcept;
void SanitizeTrigger(ColorTriggerConfig& c, int index) noexcept;
[[nodiscard]] bool RunProfileMigrationSelfTest() noexcept;

} // namespace storage

// =====================================================================================
//  UI model
// =====================================================================================

enum class SaveState : int { Saved = 0, Pending, Failed };

struct SidebarEntry {
    ItemKind kind{ItemKind::Autoclicker};
    int      index{0};
};

class AppModel {
public:
    explicit AppModel(mask::core::EngineSharedState& sharedState) noexcept;
    ~AppModel() noexcept = default;

    AppModel(const AppModel&) = delete;
    AppModel& operator=(const AppModel&) = delete;

    [[nodiscard]] mask::core::EngineSharedState& SharedState() noexcept { return sharedState_; }
    [[nodiscard]] const mask::core::EngineSharedState& SharedState() const noexcept { return sharedState_; }

    // Persistent app settings
    mask::settings::AppSettings settings{};

    // Core data banks
    std::array<MacroConfig, mask::core::kMaxMacroSlots>                  slots{};
    std::array<ComplexSequenceConfig, mask::core::kMaxComplexSequences>  sequences{};
    std::array<ColorTriggerConfig, mask::core::kMaxColorTriggers>        triggers{};
    std::array<RemapRuleConfig, mask::core::kMaxRemapRules>              remaps{};

    // Unified selection & navigation state
    ItemKind  selectedKind{ItemKind::Autoclicker};
    int       selectedIndex{0};
    int       selectedSlot{0}; // mirrors selectedIndex when selectedKind == ItemKind::Autoclicker
    int       sidebarFilter{0}; // 0=All, 1=Auto, 2=Seq, 3=Trig, 4=Map
    float     listScroll{0.0f};
    bool      showNewPicker{false};
    bool      showSettings{false};
    int       settingsTab{0}; // 0=General, 1=Appearance, 2=Performance, 3=Hotkeys, 4=Hardware & Profiles
    int       selectedSeqStep{0};
    float     seqStepScroll{0.0f};
    bool      pendingDeleteConfirm{false};
    bool      pendingResetConfirm{false};
    bool      seqColorPickActive{false};
    bool      triggerColorPickActive{false};
    UiAction  draggingTriggerSlider{};
    bool      showStepKindMenu{false};
    mask::triggers::ColorTriggerPreviewSnapshot eyedropperPreview{};

    // Cached physical GameInput devices & connected monitors
    std::array<mask::input::PhysicalDeviceInfo, 16> physicalDevices{};
    int                                             physicalDeviceCount{0};
    std::array<mask::remap::MonitorRectInfo, 8>     monitors{};
    int                                             monitorCount{0};

    // Interaction state
    UiAction  hoveredAction{};
    Field     editField{Field::None};
    wchar_t   editBuffer[128]{};
    bool      draggingDuty{false};
    ULONGLONG suppressClickUntil{0};
    ULONGLONG captureStartTick{0};

    // Toast
    wchar_t   toast[200]{};
    ULONGLONG toastTick{0};
    UiAction  toastAction{};
    wchar_t   toastActionLabel[24]{};

    // Undo for Delete (supports all 4 kinds)
    bool                  hasUndo{false};
    ItemKind              undoKind{ItemKind::Autoclicker};
    int                   undoSlot{0};
    MacroConfig           undoConfig{};
    ComplexSequenceConfig undoSeq{};
    ColorTriggerConfig    undoTrigger{};
    RemapRuleConfig       undoRemap{};

    // Engine sync + autosave
    uint64_t  pendingSyncMask{0};
    SaveState saveState{SaveState::Saved};
    ULONGLONG lastChangeTick{0};
    wchar_t   dataDir[MAX_PATH]{};
    wchar_t   fallbackDir[MAX_PATH]{};
    wchar_t   profilePath[MAX_PATH]{};
    wchar_t   fallbackPath[MAX_PATH]{};

    // Virtual device status & active hardware identity
    OutputState outputState{OutputState::Starting};
    wchar_t     outputMessage[256]{};
    wchar_t     mouseProfileId[64]{L"logitech-g502-hero"};
    wchar_t     keyboardProfileId[64]{L"keychron-q6"};
    int32_t     mouseTransportIdx{-1};
    int32_t     kbTransportIdx{-1};
    bool        driverInstalled{false};
    bool        processElevated{false};
    bool        deliversEveryReport{false};
    bool        releasesOnExit{false};

    // ---- Device & Monitor Inventory -----------------------------------------------
    void RefreshDevicesAndMonitors(const mask::remap::RemapEngine* remap = nullptr) noexcept;

    // ---- Slot presence & selection helpers ----------------------------------------
    [[nodiscard]] bool IsAutoclickerInUse(int i) const noexcept;
    [[nodiscard]] bool IsSeqInUse(int i) const noexcept;
    [[nodiscard]] bool IsTriggerInUse(int i) const noexcept;
    [[nodiscard]] bool IsRemapInUse(int i) const noexcept;
    [[nodiscard]] bool IsItemInUse(ItemKind kind, int idx) const noexcept;
    [[nodiscard]] bool HasSelection() const noexcept;

    [[nodiscard]] MacroConfig& SelectedAutoclicker() noexcept;
    [[nodiscard]] const MacroConfig& SelectedAutoclicker() const noexcept;
    [[nodiscard]] ComplexSequenceConfig& SelectedSeq() noexcept;
    [[nodiscard]] const ComplexSequenceConfig& SelectedSeq() const noexcept;
    [[nodiscard]] ColorTriggerConfig& SelectedTrigger() noexcept;
    [[nodiscard]] const ColorTriggerConfig& SelectedTrigger() const noexcept;
    [[nodiscard]] RemapRuleConfig& SelectedRemap() noexcept;
    [[nodiscard]] const RemapRuleConfig& SelectedRemap() const noexcept;

    [[nodiscard]] bool AnyRemapEnabled() const noexcept;

    [[nodiscard]] int FindFirstFreeSlot() const noexcept;
    [[nodiscard]] int FindFirstFreeSeq() const noexcept;
    [[nodiscard]] int FindFirstFreeTrigger() const noexcept;
    [[nodiscard]] int FindFirstFreeRemap() const noexcept;

    [[nodiscard]] int BuildSidebarList(std::array<SidebarEntry, 128>& out) const noexcept;
    [[nodiscard]] int ListedCount() const noexcept;
    void EnsureValidSelection() noexcept;

    // ---- Reset & creation helpers -------------------------------------------------
    void ResetSlot(int i) noexcept;
    void ResetSequence(int i) noexcept;
    void ResetTrigger(int i) noexcept;
    void ResetRemap(int i) noexcept;

    [[nodiscard]] int CreateNewAutoclicker(
        MacroMode mode = MacroMode::HoldToLoop,
        uint32_t repeatCount = 10U) noexcept;
    [[nodiscard]] int CreateNewSequence() noexcept;
    [[nodiscard]] int CreateNewTrigger() noexcept;
    [[nodiscard]] int CreateNewRemap() noexcept;

    void InitializeDefaults() noexcept;

    // ---- Engine sync --------------------------------------------------------------
    void PublishWatchedKeys() noexcept;
    void MarkDirty() noexcept;
    void SyncSlotToEngine(int slotIndex, bool markChanged = true) noexcept;
    void FlushPendingSync() noexcept;
    void SyncAllSlotsToEngine() noexcept;
    void ApplySettings() noexcept;

    // ---- Toast --------------------------------------------------------------------
    void SetToast(const wchar_t* message, UiAction action = UiAction{}, const wchar_t* actionLabel = L"") noexcept;
    void ClearToast() noexcept;

    // ---- Multi-bank & JSON persistence --------------------------------------------
    void ResolveProfilePaths() noexcept;
    void BuildBankFilePath(wchar_t* out, size_t cap, const wchar_t* dir, const wchar_t* stem, uint8_t bank) const noexcept;
    void UpdateActiveBankPaths() noexcept;
    [[nodiscard]] bool SaveSettingsOnly() noexcept;
    bool LoadSettingsFromDisk() noexcept;
    bool SaveToDisk() noexcept;
    int LoadFromDisk() noexcept;

    [[nodiscard]] bool ExportBankToJson(const wchar_t* path) const noexcept;
    [[nodiscard]] bool ImportBankFromJson(const wchar_t* path, bool& outHasImportedCommands) noexcept;

private:
    mask::core::EngineSharedState& sharedState_;
};

using UiState = AppModel;

} // namespace mask::app
