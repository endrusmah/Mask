#pragma once

// Application model for the Mask suite: Macros, Complex Sequences (128 steps),
// Color Triggers, Multi-Device/Monitor Remap Rules, Persistent AppSettings,
// Multi-Bank storage, and plain-language descriptions. Owned by the UI thread.

#include "core_contracts.hpp"
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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace mask::app {

using mask::core::MacroConfig;
using mask::core::MacroMode;
using mask::core::ComplexSequenceConfig;
using mask::core::ComplexStep;
using mask::core::ComplexStepKind;
using mask::core::ColorTriggerConfig;
using mask::core::ColorTriggerMode;
using mask::core::ColorTriggerActionKind;
using mask::core::RemapRuleConfig;
using mask::core::RemapLayerAction;
using mask::core::RemapUnlockPolicy;

// Static pre-allocated global engine state (aligned to 64-byte cache lines, zero heap allocation on hot path)
inline mask::core::EngineSharedState g_SharedState{};

inline constexpr int    kSlotCount          = static_cast<int>(mask::core::kMaxMacroSlots);
inline constexpr int    kStepCount          = static_cast<int>(mask::core::kMaxSequenceSteps);
inline constexpr int    kComplexSeqCount    = static_cast<int>(mask::core::kMaxComplexSequences);
inline constexpr int    kComplexStepMax     = static_cast<int>(mask::core::kMaxComplexSteps);
inline constexpr int    kColorTriggerCount  = static_cast<int>(mask::core::kMaxColorTriggers);
inline constexpr int    kRemapRuleCount     = static_cast<int>(mask::core::kMaxRemapRules);
inline constexpr int    kRemapLayerCount    = static_cast<int>(mask::core::kMaxRemapLayers);
inline constexpr int    kModeCount          = 5;
inline constexpr double kMinPhaseMs         = 0.05;      // 50 us engine floor
inline constexpr double kMaxPhaseMs         = 60'000.0;  // 60 s
inline constexpr double kMaxStepPhaseMs     = 10'000.0;  // 10 s per sequence step

// Unified sidebar item classification
enum class ItemKind : int {
    Macro    = 0,
    Sequence = 1,
    Trigger  = 2,
    Remap    = 3
};

[[nodiscard]] constexpr size_t AnimSlotFor(ItemKind kind, int index) noexcept {
    switch (kind) {
        case ItemKind::Macro:    return mask::ui::UiAnimator::kMacroOffset    + static_cast<size_t>(index >= 0 ? index : 0);
        case ItemKind::Sequence: return mask::ui::UiAnimator::kSequenceOffset + static_cast<size_t>(index >= 0 ? index : 0);
        case ItemKind::Trigger:  return mask::ui::UiAnimator::kTriggerOffset  + static_cast<size_t>(index >= 0 ? index : 0);
        case ItemKind::Remap:    return mask::ui::UiAnimator::kRemapOffset    + static_cast<size_t>(index >= 0 ? index : 0);
        default:                 return 0U;
    }
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

/**
 * @brief Human-readable name for a virtual-key / mouse-button code. Returned by value so several names can
 * be used inside one formatting call.
 */
[[nodiscard]] inline KeyName NameOf(uint16_t vk) noexcept {
    KeyName out{};
    const wchar_t* fixed = nullptr;
    switch (vk & 0xFFU) {
        case 0:                         fixed = L"Not set"; break;
        case mask::core::kVkWheelUp:    fixed = L"Scroll up"; break;
        case mask::core::kVkWheelDown:  fixed = L"Scroll down"; break;
        case VK_LBUTTON:                fixed = L"Left click"; break;
        case VK_RBUTTON:                fixed = L"Right click"; break;
        case VK_MBUTTON:                fixed = L"Middle click"; break;
        case VK_XBUTTON1:               fixed = L"Mouse 4"; break;
        case VK_XBUTTON2:               fixed = L"Mouse 5"; break;
        case VK_BACK:                   fixed = L"Backspace"; break;
        case VK_TAB:                    fixed = L"Tab"; break;
        case VK_RETURN:                 fixed = L"Enter"; break;
        case VK_SHIFT:                  fixed = L"Shift"; break;
        case VK_CONTROL:                fixed = L"Ctrl"; break;
        case VK_MENU:                   fixed = L"Alt"; break;
        case VK_PAUSE:                  fixed = L"Pause"; break;
        case VK_CAPITAL:                fixed = L"Caps Lock"; break;
        case VK_ESCAPE:                 fixed = L"Esc"; break;
        case VK_SPACE:                  fixed = L"Space"; break;
        case VK_PRIOR:                  fixed = L"Page Up"; break;
        case VK_NEXT:                   fixed = L"Page Down"; break;
        case VK_END:                    fixed = L"End"; break;
        case VK_HOME:                   fixed = L"Home"; break;
        case VK_LEFT:                   fixed = L"Left arrow"; break;
        case VK_UP:                     fixed = L"Up arrow"; break;
        case VK_RIGHT:                  fixed = L"Right arrow"; break;
        case VK_DOWN:                   fixed = L"Down arrow"; break;
        case VK_SNAPSHOT:               fixed = L"Print Screen"; break;
        case VK_INSERT:                 fixed = L"Insert"; break;
        case VK_DELETE:                 fixed = L"Delete"; break;
        case VK_LWIN:                   fixed = L"Left Win"; break;
        case VK_RWIN:                   fixed = L"Right Win"; break;
        case VK_APPS:                   fixed = L"Menu"; break;
        case VK_MULTIPLY:               fixed = L"Num *"; break;
        case VK_ADD:                    fixed = L"Num +"; break;
        case VK_SUBTRACT:               fixed = L"Num -"; break;
        case VK_DECIMAL:                fixed = L"Num ."; break;
        case VK_DIVIDE:                 fixed = L"Num /"; break;
        case VK_NUMLOCK:                fixed = L"Num Lock"; break;
        case VK_SCROLL:                 fixed = L"Scroll Lock"; break;
        case VK_LSHIFT:                 fixed = L"Left Shift"; break;
        case VK_RSHIFT:                 fixed = L"Right Shift"; break;
        case VK_LCONTROL:               fixed = L"Left Ctrl"; break;
        case VK_RCONTROL:               fixed = L"Right Ctrl"; break;
        case VK_LMENU:                  fixed = L"Left Alt"; break;
        case VK_RMENU:                  fixed = L"Right Alt"; break;
        case VK_OEM_1:                  fixed = L";"; break;
        case VK_OEM_PLUS:               fixed = L"="; break;
        case VK_OEM_COMMA:              fixed = L","; break;
        case VK_OEM_MINUS:              fixed = L"-"; break;
        case VK_OEM_PERIOD:             fixed = L"."; break;
        case VK_OEM_2:                  fixed = L"/"; break;
        case VK_OEM_3:                  fixed = L"`"; break;
        case VK_OEM_4:                  fixed = L"["; break;
        case VK_OEM_5:                  fixed = L"\\"; break;
        case VK_OEM_6:                  fixed = L"]"; break;
        case VK_OEM_7:                  fixed = L"'"; break;
        default: break;
    }
    if (fixed != nullptr) {
        wcscpy_s(out.text, fixed);
        return out;
    }
    const unsigned c = vk & 0xFFU;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
        out.text[0] = static_cast<wchar_t>(c);
    } else if (c >= VK_F1 && c <= VK_F24) {
        std::swprintf(out.text, std::size(out.text), L"F%u", c - VK_F1 + 1U);
    } else if (c >= VK_NUMPAD0 && c <= VK_NUMPAD9) {
        std::swprintf(out.text, std::size(out.text), L"Num %u", c - VK_NUMPAD0);
    } else {
        std::swprintf(out.text, std::size(out.text), L"Key 0x%02X", c);
    }
    return out;
}

[[nodiscard]] inline const wchar_t* ModeName(int mode) noexcept {
    switch (static_cast<MacroMode>(mode)) {
        case MacroMode::HoldToLoop:         return L"Hold";
        case MacroMode::InfiniteToggleLoop: return L"Toggle";
        case MacroMode::RepeatNTimes:       return L"Burst";
        case MacroMode::Sequence:           return L"Sequence";
        case MacroMode::SocdPair:           return L"Snap-tap";
        default:                            return L"Hold";
    }
}

[[nodiscard]] inline const wchar_t* ModeDescription(int mode) noexcept {
    switch (static_cast<MacroMode>(mode)) {
        case MacroMode::HoldToLoop:         return L"Runs for as long as the trigger is held down.";
        case MacroMode::InfiniteToggleLoop: return L"Press the trigger to start. Press it again to stop.";
        case MacroMode::RepeatNTimes:       return L"Press the trigger once to run a set number of times.";
        case MacroMode::Sequence:           return L"Press the trigger to play the steps in a loop. Press it again to stop.";
        case MacroMode::SocdPair:           return L"When both keys are down, only the one pressed last counts.";
        default:                            return L"";
    }
}

[[nodiscard]] inline bool IsTimedMode(int mode) noexcept {
    const auto m = static_cast<MacroMode>(mode);
    return m == MacroMode::HoldToLoop || m == MacroMode::InfiniteToggleLoop || m == MacroMode::RepeatNTimes;
}

[[nodiscard]] inline const wchar_t* StepKindName(ComplexStepKind kind) noexcept {
    switch (kind) {
        case ComplexStepKind::KeyClick:      return L"Click key";
        case ComplexStepKind::KeyDown:       return L"Hold key";
        case ComplexStepKind::KeyUp:         return L"Release key";
        case ComplexStepKind::MouseMoveRel:  return L"Move mouse";
        case ComplexStepKind::MouseMoveAbs:  return L"Move to spot";
        case ComplexStepKind::MouseScroll:   return L"Scroll wheel";
        case ComplexStepKind::WaitMs:        return L"Wait";
        case ComplexStepKind::WaitForColor:  return L"Wait for color";
        case ComplexStepKind::LoopStart:     return L"Start loop";
        case ComplexStepKind::LoopEnd:       return L"End loop";
        case ComplexStepKind::LaunchProgram: return L"Open app";
        case ComplexStepKind::RunCommand:    return L"Run command";
        case ComplexStepKind::SetLayer:      return L"Switch layer";
        case ComplexStepKind::ToggleMacro:   return L"Toggle macro";
        default:                             return L"Step";
    }
}

[[nodiscard]] inline const wchar_t* SeqTriggerModeName(uint8_t mode) noexcept {
    switch (mode) {
        case 0:  return L"Once";
        case 1:  return L"Toggle";
        case 2:  return L"Hold";
        default: return L"Once";
    }
}

[[nodiscard]] inline const wchar_t* LayerName(uint8_t layer) noexcept {
    switch (layer & 3U) {
        case 0:  return L"Base";
        case 1:  return L"L1";
        case 2:  return L"L2";
        case 3:  return L"L3";
        default: return L"Base";
    }
}

inline void DescribeComplexStep(const ComplexStep& st, wchar_t* buf, size_t n) noexcept {
    const auto kind = static_cast<ComplexStepKind>(st.kind);
    switch (kind) {
        case ComplexStepKind::KeyClick:
            std::swprintf(buf, n, L"Click %ls (%u ms)", NameOf(st.vk).text, static_cast<unsigned>(st.durationMs));
            break;
        case ComplexStepKind::KeyDown:
            std::swprintf(buf, n, L"Hold %ls down", NameOf(st.vk).text);
            break;
        case ComplexStepKind::KeyUp:
            std::swprintf(buf, n, L"Release %ls", NameOf(st.vk).text);
            break;
        case ComplexStepKind::MouseMoveRel:
            std::swprintf(buf, n, L"Move mouse (%+d, %+d)", static_cast<int>(st.x), static_cast<int>(st.y));
            break;
        case ComplexStepKind::MouseMoveAbs:
            std::swprintf(buf, n, L"Move cursor to (%d, %d)", static_cast<int>(st.x), static_cast<int>(st.y));
            break;
        case ComplexStepKind::MouseScroll: {
            const int32_t delta = static_cast<int32_t>(st.param != 0U ? static_cast<int32_t>(st.param) : st.y);
            const int32_t mag = delta >= 0 ? delta : -delta;
            std::swprintf(buf, n, L"Scroll %ls (%d)", delta >= 0 ? L"up" : L"down", static_cast<int>(mag));
            break;
        }
        case ComplexStepKind::WaitMs:
            std::swprintf(buf, n, L"Wait %u ms", static_cast<unsigned>(st.durationMs));
            break;
        case ComplexStepKind::WaitForColor:
            std::swprintf(buf, n, L"Wait for color at (%d, %d)", static_cast<int>(st.x), static_cast<int>(st.y));
            break;
        case ComplexStepKind::LoopStart:
            if (st.param == 0U) wcscpy_s(buf, n, L"Repeat forever");
            else if (st.param == 1U) wcscpy_s(buf, n, L"Repeat 1 time");
            else std::swprintf(buf, n, L"Repeat %u times", static_cast<unsigned>(st.param));
            break;
        case ComplexStepKind::LoopEnd:
            wcscpy_s(buf, n, L"End repeat block");
            break;
        case ComplexStepKind::LaunchProgram:
            if (st.text[0] != L'\0') std::swprintf(buf, n, L"Open %ls", st.text);
            else wcscpy_s(buf, n, L"Open an app");
            break;
        case ComplexStepKind::RunCommand:
            if (st.text[0] != L'\0') std::swprintf(buf, n, L"Run %ls", st.text);
            else wcscpy_s(buf, n, L"Run a command");
            break;
        case ComplexStepKind::SetLayer:
            std::swprintf(buf, n, L"Switch to layer %ls", LayerName(static_cast<uint8_t>(st.param)));
            break;
        case ComplexStepKind::ToggleMacro:
            std::swprintf(buf, n, L"Toggle macro #%u", static_cast<unsigned>((st.param % mask::core::kMaxMacroSlots) + 1U));
            break;
        default:
            wcscpy_s(buf, n, L"Step");
            break;
    }
}

inline void DescribeComplexSequence(const ComplexSequenceConfig& q, wchar_t* buf, size_t n) noexcept {
    const unsigned cnt = static_cast<unsigned>(q.stepCount);
    const wchar_t* noun = (cnt == 1U) ? L"step" : L"steps";
    if (q.triggerVk == 0U) {
        std::swprintf(buf, n, L"Choose a trigger key to run this %u-step sequence.", cnt);
        return;
    }
    const KeyName trig = NameOf(q.triggerVk);
    switch (q.triggerMode) {
        case 1:
            std::swprintf(buf, n, L"Press %ls to start or stop looping %u %ls.", trig.text, cnt, noun);
            break;
        case 2:
            std::swprintf(buf, n, L"Hold %ls to repeat %u %ls.", trig.text, cnt, noun);
            break;
        default:
            std::swprintf(buf, n, L"Press %ls to run %u %ls once.", trig.text, cnt, noun);
            break;
    }
}

[[nodiscard]] inline const wchar_t* RemapActionName(RemapLayerAction action) noexcept {
    switch (action) {
        case RemapLayerAction::RemapKey:          return L"Remap";
        case RemapLayerAction::BlockKey:          return L"Block";
        case RemapLayerAction::ShiftLayerHold:    return L"Hold layer";
        case RemapLayerAction::ToggleLayer:       return L"Toggle layer";
        case RemapLayerAction::ToggleUnlockState: return L"Lock";
        case RemapLayerAction::WarpToMonitor:     return L"Warp";
        default:                                  return L"Remap";
    }
}

[[nodiscard]] inline const wchar_t* UnlockPolicyName(RemapUnlockPolicy pol) noexcept {
    switch (pol) {
        case RemapUnlockPolicy::Always:       return L"Always";
        case RemapUnlockPolicy::WhenUnlocked: return L"When unlocked";
        case RemapUnlockPolicy::WhenLocked:   return L"When locked";
        default:                              return L"Always";
    }
}

inline void DescribeRemapRule(const RemapRuleConfig& r, wchar_t* buf, size_t n) noexcept {
    if (r.sourceVk == 0U) {
        wcscpy_s(buf, n, L"Choose a source key to finish setting up this remap rule.");
        return;
    }
    const KeyName src = NameOf(r.sourceVk);
    const auto actKind = static_cast<RemapLayerAction>(r.actionKind);
    switch (actKind) {
        case RemapLayerAction::RemapKey:
            std::swprintf(buf, n, L"%ls on %ls sends %ls (%ls%ls).",
                          src.text, r.anyDevice ? L"any device" : r.deviceLabel,
                          NameOf(r.targetVk).text, LayerName(r.layerIndex),
                          r.suppressOriginal ? L", original suppressed" : L"");
            break;
        case RemapLayerAction::BlockKey:
            std::swprintf(buf, n, L"Blocks %ls on %ls while on layer %ls.",
                          src.text, r.anyDevice ? L"any device" : r.deviceLabel, LayerName(r.layerIndex));
            break;
        case RemapLayerAction::ShiftLayerHold:
            std::swprintf(buf, n, L"Holding %ls temporarily shifts remap layer to %ls.",
                          src.text, LayerName(r.targetLayer));
            break;
        case RemapLayerAction::ToggleLayer:
            std::swprintf(buf, n, L"Pressing %ls toggles remap layer %ls.",
                          src.text, LayerName(r.targetLayer));
            break;
        case RemapLayerAction::ToggleUnlockState:
            std::swprintf(buf, n, L"Pressing %ls toggles global remap Lock / Unlock state.", src.text);
            break;
        case RemapLayerAction::WarpToMonitor:
            std::swprintf(buf, n, L"Pressing %ls warps cursor to Monitor %u center.",
                          src.text, static_cast<unsigned>((r.monitorIndex >= 0 ? r.monitorIndex : 0) + 1));
            break;
        default:
            buf[0] = L'\0';
            break;
    }
}

[[nodiscard]] inline const wchar_t* ToleranceWord(uint8_t tol) noexcept {
    if (tol <= 12U) return L"Very strict";
    if (tol <= 26U) return L"Strict";
    if (tol <= 48U) return L"Balanced";
    if (tol <= 85U) return L"Wide";
    return L"Very wide";
}

inline void DescribeColorTrigger(const ColorTriggerConfig& t, wchar_t* buf, size_t n) noexcept {
    wchar_t spotBuf[48]{};
    const unsigned monNum = static_cast<unsigned>(t.monitorIndex + 1U);
    if (t.sampleX < 0 || t.sampleY < 0) {
        std::swprintf(spotBuf, std::size(spotBuf), L"the center of Screen %u", monNum);
    } else {
        std::swprintf(spotBuf, std::size(spotBuf), L"Screen %u (%d, %d)",
                      monNum, static_cast<int>(t.sampleX), static_cast<int>(t.sampleY));
    }

    const wchar_t* modeStr = (t.mode == static_cast<uint8_t>(ColorTriggerMode::ColorChanged))
        ? L"for any color change" : L"for this color";

    wchar_t armBuf[48]{};
    if (t.armMode == 1U) {
        std::swprintf(armBuf, std::size(armBuf), L" while holding %ls", NameOf(t.armVk).text);
    } else if (t.armMode == 2U) {
        std::swprintf(armBuf, std::size(armBuf), L" when toggled with %ls", NameOf(t.armVk).text);
    }

    wchar_t actBuf[48]{};
    switch (static_cast<ColorTriggerActionKind>(t.actionKind)) {
        case ColorTriggerActionKind::ClickKey:
            std::swprintf(actBuf, std::size(actBuf), L"clicks %ls", NameOf(t.actionVk).text);
            break;
        case ColorTriggerActionKind::HoldKey:
            std::swprintf(actBuf, std::size(actBuf), L"holds %ls", NameOf(t.actionVk).text);
            break;
        case ColorTriggerActionKind::RunMacroSlot:
            std::swprintf(actBuf, std::size(actBuf), L"runs Macro #%u", static_cast<unsigned>(t.linkedIndex + 1U));
            break;
        case ColorTriggerActionKind::RunComplexSeq:
            std::swprintf(actBuf, std::size(actBuf), L"runs Sequence #%u", static_cast<unsigned>(t.linkedIndex + 1U));
            break;
        default:
            wcscpy_s(actBuf, L"runs an action");
            break;
    }
    std::swprintf(buf, n, L"Watches %ls %ls%ls, then %ls.", spotBuf, modeStr, armBuf, actBuf);
}

// =====================================================================================
//  Number formatting
// =====================================================================================

inline void FormatNumber(double value, int maxDecimals, wchar_t* buf, size_t n) noexcept {
    std::swprintf(buf, n, L"%.*f", maxDecimals, value);
    if (std::wcschr(buf, L'.') == nullptr) {
        return;
    }
    size_t len = std::wcslen(buf);
    while (len > 0 && buf[len - 1] == L'0') { buf[--len] = L'\0'; }
    if (len > 0 && buf[len - 1] == L'.') { buf[--len] = L'\0'; }
    if (std::wcscmp(buf, L"-0") == 0) { wcscpy_s(buf, n, L"0"); }
}

inline void FormatMs(double ms, wchar_t* buf, size_t n) noexcept {
    FormatNumber(ms, ms >= 100.0 ? 0 : 2, buf, n);
}

inline void FormatDuration(double ms, wchar_t* buf, size_t n) noexcept {
    wchar_t num[32]{};
    if (ms >= 1000.0) {
        FormatNumber(ms / 1000.0, 2, num, std::size(num));
        std::swprintf(buf, n, L"%ls s", num);
    } else {
        FormatMs(ms, num, std::size(num));
        std::swprintf(buf, n, L"%ls ms", num);
    }
}

// =====================================================================================
//  Timing maths: rate + duty cycle  <->  press time + release time
// =====================================================================================

[[nodiscard]] inline double DownMs(const MacroConfig& c) noexcept { return static_cast<double>(c.dwellDurationNs) / 1e6; }
[[nodiscard]] inline double UpMs(const MacroConfig& c) noexcept { return static_cast<double>(c.recoveryDurationNs) / 1e6; }
[[nodiscard]] inline double PeriodMs(const MacroConfig& c) noexcept { return DownMs(c) + UpMs(c); }
[[nodiscard]] inline double RatePerSecond(const MacroConfig& c) noexcept { return 1000.0 / std::max(PeriodMs(c), 1e-6); }
[[nodiscard]] inline bool   IsFullDuty(const MacroConfig& c) noexcept { return c.recoveryDurationNs == 0ULL; }

[[nodiscard]] inline double DutyPercent(const MacroConfig& c) noexcept {
    if (IsFullDuty(c)) {
        return 100.0;
    }
    return 100.0 * DownMs(c) / std::max(PeriodMs(c), 1e-6);
}

[[nodiscard]] inline uint64_t MsToNs(double ms) noexcept {
    return static_cast<uint64_t>(std::llround(std::max(ms, 0.0) * 1e6));
}

[[nodiscard]] inline double ClampDownMs(double ms, double maxMs = kMaxPhaseMs) noexcept {
    return std::clamp(ms, kMinPhaseMs, maxMs);
}

[[nodiscard]] inline double ClampUpMs(double ms, double maxMs = kMaxPhaseMs) noexcept {
    if (ms <= 0.0) {
        return 0.0;
    }
    return std::clamp(ms, kMinPhaseMs, maxMs);
}

inline void SetPeriodAndDuty(MacroConfig& c, double periodMs, double dutyFraction) noexcept {
    if (dutyFraction >= 1.0) {
        c.dwellDurationNs = MsToNs(ClampDownMs(periodMs));
        c.recoveryDurationNs = 0ULL;
        return;
    }
    periodMs = std::clamp(periodMs, 2.0 * kMinPhaseMs, 2.0 * kMaxPhaseMs);
    const double up = std::clamp(periodMs * (1.0 - std::max(dutyFraction, 0.0)), kMinPhaseMs, kMaxPhaseMs);
    const double down = ClampDownMs(periodMs - up);
    c.dwellDurationNs = MsToNs(down);
    c.recoveryDurationNs = MsToNs(up);
}

inline void SetRate(MacroConfig& c, double perSecond) noexcept {
    perSecond = std::clamp(perSecond, 0.01, 20'000.0);
    const double fraction = IsFullDuty(c) ? 1.0 : DownMs(c) / std::max(PeriodMs(c), 1e-6);
    SetPeriodAndDuty(c, 1000.0 / perSecond, fraction);
}

inline void SetDuty(MacroConfig& c, double percent) noexcept {
    percent = std::clamp(percent, 0.1, 100.0);
    SetPeriodAndDuty(c, PeriodMs(c), percent >= 99.9995 ? 1.0 : percent / 100.0);
}

inline void FormatDuty(const MacroConfig& c, wchar_t* buf, size_t n) noexcept {
    if (IsFullDuty(c)) {
        wcscpy_s(buf, n, L"100");
        return;
    }
    const double shown = std::min(DutyPercent(c), 99.9);
    FormatNumber(shown, shown < 10.0 ? 2 : 1, buf, n);
}

inline void FormatRate(double perSecond, wchar_t* buf, size_t n) noexcept {
    FormatNumber(perSecond, perSecond >= 100.0 ? 0 : (perSecond >= 10.0 ? 1 : 2), buf, n);
}

// =====================================================================================
//  Editable fields (all IDs fit in 0..63 so act::FieldFocus/Dec/Inc stay compact)
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
    StepDownBase = 16, // 16..23
    SeqStepB = 24,
    SeqStepTol = 25,
    SeqStepText = 26,
    RemapName = 27,
    SettingsAutosaveMs = 28,
    StepUpBase   = 32, // 32..39
    TriggerName     = 40,
    TriggerDelay    = 41,
    TriggerHold     = 42,
    TriggerCooldown = 43,
    TriggerLinked   = 44
};

[[nodiscard]] inline Field StepDownField(int s) noexcept { return static_cast<Field>(static_cast<int>(Field::StepDownBase) + s); }
[[nodiscard]] inline Field StepUpField(int s) noexcept { return static_cast<Field>(static_cast<int>(Field::StepUpBase) + s); }
[[nodiscard]] inline bool IsStepDown(Field f) noexcept { const int v = static_cast<int>(f); return v >= 16 && v < 16 + kStepCount; }
[[nodiscard]] inline bool IsStepUp(Field f) noexcept { const int v = static_cast<int>(f); return v >= 32 && v < 32 + kStepCount; }
[[nodiscard]] inline bool IsDownTime(Field f) noexcept { return f == Field::Down || IsStepDown(f); }
[[nodiscard]] inline bool IsUpTime(Field f) noexcept { return f == Field::Up || IsStepUp(f); }
[[nodiscard]] inline bool IsTimeField(Field f) noexcept { return IsDownTime(f) || IsUpTime(f); }
[[nodiscard]] inline bool IsTextField(Field f) noexcept {
    return f == Field::Name || f == Field::SeqName || f == Field::RemapName ||
           f == Field::SeqStepText || f == Field::TriggerName;
}
[[nodiscard]] inline bool IsTriggerField(Field f) noexcept {
    return f == Field::TriggerDelay || f == Field::TriggerHold ||
           f == Field::TriggerCooldown || f == Field::TriggerLinked;
}
[[nodiscard]] inline bool IsMacroField(Field f) noexcept {
    const int v = static_cast<int>(f);
    return (v >= 1 && v <= 8) || IsStepDown(f) || IsStepUp(f);
}
[[nodiscard]] inline bool IsSeqStepField(Field f) noexcept {
    return f == Field::SeqStepDuration || f == Field::SeqStepX || f == Field::SeqStepY ||
           f == Field::SeqStepParam || f == Field::SeqStepR || f == Field::SeqStepG ||
           f == Field::SeqStepB || f == Field::SeqStepTol || f == Field::SeqStepText;
}
[[nodiscard]] inline bool AllowsDecimal(Field f) noexcept { return IsTimeField(f) || f == Field::Rate || f == Field::Duty; }
[[nodiscard]] inline bool AllowsNegative(Field f) noexcept {
    return f == Field::MoveX || f == Field::MoveY || f == Field::SeqStepX || f == Field::SeqStepY || f == Field::SeqStepParam;
}

[[nodiscard]] inline double FieldGet(const MacroConfig& s, Field f) noexcept {
    switch (f) {
        case Field::Rate:   return RatePerSecond(s);
        case Field::Duty:   return DutyPercent(s);
        case Field::Down:   return DownMs(s);
        case Field::Up:     return UpMs(s);
        case Field::Repeat: return static_cast<double>(s.repeatCount);
        case Field::MoveX:  return static_cast<double>(s.mouseDeltaX);
        case Field::MoveY:  return static_cast<double>(s.mouseDeltaY);
        default: break;
    }
    if (IsStepDown(f)) return static_cast<double>(s.steps[static_cast<int>(f) - 16].dwellDurationNs) / 1e6;
    if (IsStepUp(f))   return static_cast<double>(s.steps[static_cast<int>(f) - 32].recoveryDurationNs) / 1e6;
    return 0.0;
}

inline void FieldSet(MacroConfig& s, Field f, double v) noexcept {
    switch (f) {
        case Field::Rate:   SetRate(s, v); return;
        case Field::Duty:   SetDuty(s, v); return;
        case Field::Down:   s.dwellDurationNs = MsToNs(ClampDownMs(v)); return;
        case Field::Up:     s.recoveryDurationNs = MsToNs(ClampUpMs(v)); return;
        case Field::Repeat: s.repeatCount = static_cast<uint32_t>(std::clamp<long long>(std::llround(v), 1, 9999)); return;
        case Field::MoveX:  s.mouseDeltaX = static_cast<int32_t>(std::clamp<long long>(std::llround(v), -500, 500)); return;
        case Field::MoveY:  s.mouseDeltaY = static_cast<int32_t>(std::clamp<long long>(std::llround(v), -500, 500)); return;
        default: break;
    }
    if (IsStepDown(f)) {
        s.steps[static_cast<int>(f) - 16].dwellDurationNs = MsToNs(ClampDownMs(v, kMaxStepPhaseMs));
    } else if (IsStepUp(f)) {
        s.steps[static_cast<int>(f) - 32].recoveryDurationNs = MsToNs(ClampUpMs(v, kMaxStepPhaseMs));
    }
}

inline void FormatFieldNumber(const MacroConfig& s, Field f, wchar_t* buf, size_t n) noexcept {
    if (f == Field::Duty) {
        FormatDuty(s, buf, n);
    } else if (f == Field::Rate) {
        FormatRate(RatePerSecond(s), buf, n);
    } else if (IsTimeField(f)) {
        FormatMs(FieldGet(s, f), buf, n);
    } else {
        std::swprintf(buf, n, L"%lld", static_cast<long long>(std::llround(FieldGet(s, f))));
    }
}

[[nodiscard]] inline const wchar_t* FieldSuffix(Field f) noexcept {
    if (IsTimeField(f) || f == Field::SeqStepDuration || f == Field::SettingsAutosaveMs ||
        f == Field::TriggerDelay || f == Field::TriggerHold || f == Field::TriggerCooldown) return L" ms";
    if (f == Field::Rate) return L" /s";
    if (f == Field::Duty) return L"%";
    if (f == Field::MoveX || f == Field::MoveY) return L" px";
    if (f == Field::Repeat) return L"\u00D7";
    return L"";
}

inline void FieldStep(MacroConfig& s, Field f, int direction, bool coarse, bool fine) noexcept {
    const double cur = FieldGet(s, f);
    const bool fractional = AllowsDecimal(f);
    double step = coarse ? 10.0 : 1.0;
    if (fine && fractional) {
        step = 0.1;
    }

    double next = cur + direction * step;
    if (fractional && step >= 1.0 && cur >= 1.0) {
        next = (direction > 0 ? std::floor(cur + 1e-9) : std::ceil(cur - 1e-9)) + direction * step;
    }

    if (f == Field::Duty) {
        if (direction > 0 && next > 99.9) next = 100.0;
        if (direction < 0 && IsFullDuty(s)) next = 100.0 - step;
        next = std::max(next, 0.1);
    } else if (IsUpTime(f)) {
        if (next < kMinPhaseMs) next = 0.0;
    } else if (IsDownTime(f)) {
        next = std::max(next, kMinPhaseMs);
    } else if (f == Field::Rate) {
        next = std::max(next, 0.1);
    }
    FieldSet(s, f, next);
}

// =====================================================================================
//  Plain-language macro descriptions
// =====================================================================================

inline void DescribeRate(double perSecond, wchar_t* buf, size_t n) noexcept {
    wchar_t num[32]{};
    if (perSecond < 0.995) {
        FormatNumber(1.0 / perSecond, 2, num, std::size(num));
        std::swprintf(buf, n, L"once every %ls s", num);
    } else if (std::fabs(perSecond - 1.0) < 0.005) {
        wcscpy_s(buf, n, L"once a second");
    } else {
        FormatRate(perSecond, num, std::size(num));
        std::swprintf(buf, n, L"%ls times a second", num);
    }
}

[[nodiscard]] inline double SequenceCycleMs(const MacroConfig& c) noexcept {
    double total = 0.0;
    const int count = std::clamp<int>(c.stepCount, 1, kStepCount);
    for (int i = 0; i < count; ++i) {
        total += static_cast<double>(c.steps[i].dwellDurationNs + c.steps[i].recoveryDurationNs) / 1e6;
    }
    return total;
}

inline void DescribeMacro(const MacroConfig& c, wchar_t* buf, size_t n) noexcept {
    const auto mode = static_cast<MacroMode>(c.mode);
    const KeyName trig = NameOf(c.triggerVk);
    const KeyName out = NameOf(c.outputVk);

    if (c.triggerVk == 0) {
        wcscpy_s(buf, n, mode == MacroMode::SocdPair ? L"Choose the two keys to finish setting this up."
                                                     : L"Choose a trigger to finish setting this up.");
        return;
    }
    if (mode == MacroMode::SocdPair) {
        std::swprintf(buf, n, L"Holding %ls and %ls together sends only the one pressed last.", trig.text, out.text);
        return;
    }
    if (mode == MacroMode::Sequence) {
        wchar_t cycle[32]{};
        FormatDuration(SequenceCycleMs(c), cycle, std::size(cycle));
        std::swprintf(buf, n, L"%ls starts and stops a %u-step sequence that repeats every %ls.", trig.text,
                      static_cast<unsigned>(std::clamp<int>(c.stepCount, 1, kStepCount)), cycle);
        return;
    }

    wchar_t rate[48]{};
    DescribeRate(RatePerSecond(c), rate, std::size(rate));
    const bool full = IsFullDuty(c);

    switch (mode) {
        case MacroMode::HoldToLoop:
            if (full) std::swprintf(buf, n, L"While %ls is held, %ls stays held down.", trig.text, out.text);
            else      std::swprintf(buf, n, L"While %ls is held, presses %ls %ls.", trig.text, out.text, rate);
            break;
        case MacroMode::InfiniteToggleLoop:
            if (full) std::swprintf(buf, n, L"%ls holds %ls down until you press it again.", trig.text, out.text);
            else      std::swprintf(buf, n, L"%ls starts and stops pressing %ls %ls.", trig.text, out.text, rate);
            break;
        case MacroMode::RepeatNTimes: {
            if (full) {
                wchar_t total[32]{};
                FormatDuration(DownMs(c) * static_cast<double>(c.repeatCount), total, std::size(total));
                std::swprintf(buf, n, L"%ls holds %ls down for %ls.", trig.text, out.text, total);
            } else {
                std::swprintf(buf, n, L"%ls presses %ls %u times, %ls.", trig.text, out.text,
                              static_cast<unsigned>(c.repeatCount), rate);
            }
            break;
        }
        default:
            buf[0] = L'\0';
            break;
    }

    const uint8_t hum = mask::core::HumanizePercent(c);
    if (hum > 0U && IsTimedMode(c.mode)) {
        const size_t len = std::wcslen(buf);
        if (len > 0 && buf[len - 1] == L'.' && len + 26 < n) {
            std::swprintf(buf + len - 1, n - (len - 1), L" (\u00B1%u%% variance).", static_cast<unsigned>(hum));
        } else if (len + 26 < n) {
            std::swprintf(buf + len, n - len, L" (\u00B1%u%% variance)", static_cast<unsigned>(hum));
        }
    }
}

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
inline constexpr uint32_t kVersion      = 2U;
inline constexpr size_t   kHeaderBytes  = 8;
inline constexpr size_t   kPayloadBytes = sizeof(MacroConfig) * mask::core::kMaxMacroSlots;

[[nodiscard]] inline bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept {
    wchar_t tmp[MAX_PATH + 16]{};
    wchar_t bak[MAX_PATH + 16]{};
    if (std::swprintf(tmp, std::size(tmp), L"%ls.tmp", path) < 0 ||
        std::swprintf(bak, std::size(bak), L"%ls.bak", path) < 0) {
        return false;
    }

    wchar_t dir[MAX_PATH + 16]{};
    if (wcscpy_s(dir, path) == 0) {
        if (wchar_t* slash = std::wcsrchr(dir, L'\\')) {
            *slash = L'\0';
            if (dir[0] != L'\0') {
                CreateDirectoryW(dir, nullptr);
            }
        }
    }

    const HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) && written == size &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp);
        return false;
    }

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
        ReplaceFileW(path, tmp, bak, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        return true;
    }
    if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(tmp);
    return false;
}

[[nodiscard]] inline bool ReadProfileFile(const wchar_t* path,
                                          std::array<MacroConfig, mask::core::kMaxMacroSlots>& out) noexcept {
    const HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint32_t header[2]{0, 0};
    DWORD got = 0;
    bool ok = ReadFile(h, header, sizeof(header), &got, nullptr) && got == sizeof(header) &&
              header[0] == kMagic && header[1] == kVersion;
    if (ok) {
        ok = ReadFile(h, static_cast<void*>(out.data()), static_cast<DWORD>(kPayloadBytes), &got, nullptr) &&
             got == kPayloadBytes;
    }
    CloseHandle(h);
    return ok;
}

inline void Sanitize(MacroConfig& s, int index) noexcept {
    auto rawBool = [](const bool& b) noexcept {
        uint8_t raw = 0;
        std::memcpy(&raw, &b, 1);
        return raw != 0;
    };
    s.slotIndex = static_cast<uint8_t>(index);
    s.enabled   = rawBool(s.enabled);
    s.name[std::size(s.name) - 1] = L'\0';
    if (s.name[0] == L'\0') {
        std::swprintf(s.name, std::size(s.name), L"Macro %d", index + 1);
    }
    s.mode        = static_cast<uint8_t>(std::min<int>(s.mode, kModeCount - 1));
    s.triggerVk   = static_cast<uint16_t>(s.triggerVk & 0xFFU);
    s.outputVk    = static_cast<uint16_t>(s.outputVk & 0xFFU);
    s.isMouse     = mask::input::InputDispatcher::IsMouseVirtualKey(s.outputVk);
    s.stepCount   = static_cast<uint8_t>(std::clamp<int>(s.stepCount, 1, kStepCount));
    s.repeatCount = std::clamp<uint32_t>(s.repeatCount, 1U, 9999U);
    s.mouseDeltaX = std::clamp<int32_t>(s.mouseDeltaX, -500, 500);
    s.mouseDeltaY = std::clamp<int32_t>(s.mouseDeltaY, -500, 500);
    s.dwellDurationNs    = MsToNs(ClampDownMs(static_cast<double>(std::min<uint64_t>(s.dwellDurationNs, 600'000'000'000ULL)) / 1e6));
    s.recoveryDurationNs = MsToNs(ClampUpMs(static_cast<double>(std::min<uint64_t>(s.recoveryDurationNs, 600'000'000'000ULL)) / 1e6));
    if (s.triggerVk == 0) {
        s.enabled = false;
    }
    s.reserved[0] = (s.reserved[0] != 0 || s.triggerVk != 0 || s.enabled) ? 1 : 0;
    s.reserved[1] = (s.reserved[1] <= 50) ? s.reserved[1] : 0;
    s.reserved[2] = 0;
    for (auto& step : s.steps) {
        step.outputVk = static_cast<uint16_t>(step.outputVk & 0xFFU);
        step.isMouse  = mask::input::InputDispatcher::IsMouseVirtualKey(step.outputVk);
        step.enabled  = true;
        step.dwellDurationNs    = MsToNs(ClampDownMs(static_cast<double>(std::min<uint64_t>(step.dwellDurationNs, 600'000'000'000ULL)) / 1e6, kMaxStepPhaseMs));
        step.recoveryDurationNs = MsToNs(ClampUpMs(static_cast<double>(std::min<uint64_t>(step.recoveryDurationNs, 600'000'000'000ULL)) / 1e6, kMaxStepPhaseMs));
    }
}

} // namespace storage

// =====================================================================================
//  UI model
// =====================================================================================

enum class SaveState : int { Saved = 0, Pending, Failed };

struct SidebarEntry {
    ItemKind kind{ItemKind::Macro};
    int      index{0};
};

struct AppModel {
    // Persistent app settings
    mask::settings::AppSettings settings{};

    // Core data banks
    std::array<MacroConfig, mask::core::kMaxMacroSlots>                  slots{};
    std::array<ComplexSequenceConfig, mask::core::kMaxComplexSequences>  sequences{};
    std::array<ColorTriggerConfig, mask::core::kMaxColorTriggers>        triggers{};
    std::array<RemapRuleConfig, mask::core::kMaxRemapRules>              remaps{};

    // Unified selection & navigation state
    ItemKind  selectedKind{ItemKind::Macro};
    int       selectedIndex{0};
    int       selectedSlot{0}; // mirrors selectedIndex when selectedKind == ItemKind::Macro
    int       sidebarFilter{0}; // 0=All, 1=Macros, 2=Sequences, 3=Triggers, 4=Remaps
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
    int       draggingTriggerSlider{0};
    bool      showStepKindMenu{false};
    mask::triggers::ColorTriggerPreviewSnapshot eyedropperPreview{};

    // Cached physical GameInput devices & connected monitors
    std::array<mask::input::PhysicalDeviceInfo, 16> physicalDevices{};
    int                                             physicalDeviceCount{0};
    std::array<mask::remap::MonitorRectInfo, 8>     monitors{};
    int                                             monitorCount{0};

    // Interaction state
    int       hoveredAction{0};
    Field     editField{Field::None};
    wchar_t   editBuffer[128]{};
    bool      draggingDuty{false};
    ULONGLONG suppressClickUntil{0};
    ULONGLONG captureStartTick{0};

    // Toast
    wchar_t   toast[200]{};
    ULONGLONG toastTick{0};
    int       toastAction{0};
    wchar_t   toastActionLabel[24]{};

    // Undo for Delete (supports all 4 kinds)
    bool                  hasUndo{false};
    ItemKind              undoKind{ItemKind::Macro};
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

    void RefreshDevicesAndMonitors(const mask::remap::RemapEngine* remapEngine = nullptr) noexcept {
        physicalDeviceCount = mask::input::CopyPhysicalDeviceInventory(physicalDevices);
        if (remapEngine != nullptr) {
            monitorCount = remapEngine->GetMonitors(monitors);
        }
        if (monitorCount <= 0) {
            std::array<mask::triggers::MonitorInfoEntry, mask::triggers::kMaxMonitors> tmp{};
            const int cnt = mask::triggers::PixelTriggerEngine::EnumerateMonitors(tmp);
            monitorCount = std::clamp(cnt, 0, 8);
            for (int i = 0; i < monitorCount; ++i) {
                monitors[i].left      = tmp[i].left;
                monitors[i].top       = tmp[i].top;
                monitors[i].right     = tmp[i].left + tmp[i].width;
                monitors[i].bottom    = tmp[i].top + tmp[i].height;
                monitors[i].dpi       = tmp[i].dpi;
                monitors[i].isPrimary = tmp[i].isPrimary;
                wcsncpy_s(monitors[i].name, tmp[i].name, _TRUNCATE);
            }
        }
    }

    // ---- Slot presence & selection helpers ----------------------------------------

    [[nodiscard]] bool IsMacroInUse(int i) const noexcept {
        return i >= 0 && i < kSlotCount && (slots[i].reserved[0] != 0 || slots[i].triggerVk != 0 || slots[i].enabled);
    }
    [[nodiscard]] bool IsInUse(int i) const noexcept { return IsMacroInUse(i); }
    [[nodiscard]] bool IsListed(int i) const noexcept { return IsMacroInUse(i); }

    [[nodiscard]] bool IsSeqInUse(int i) const noexcept {
        return i >= 0 && i < kComplexSeqCount && sequences[i].inUse;
    }
    [[nodiscard]] bool IsTriggerInUse(int i) const noexcept {
        return i >= 0 && i < kColorTriggerCount && triggers[i].inUse;
    }
    [[nodiscard]] bool IsRemapInUse(int i) const noexcept {
        return i >= 0 && i < kRemapRuleCount && remaps[i].inUse;
    }

    [[nodiscard]] bool IsItemInUse(ItemKind kind, int idx) const noexcept {
        switch (kind) {
            case ItemKind::Macro:    return IsMacroInUse(idx);
            case ItemKind::Sequence: return IsSeqInUse(idx);
            case ItemKind::Trigger:  return IsTriggerInUse(idx);
            case ItemKind::Remap:    return IsRemapInUse(idx);
            default:                 return false;
        }
    }

    [[nodiscard]] bool HasSelection() const noexcept {
        return IsItemInUse(selectedKind, selectedIndex);
    }

    [[nodiscard]] MacroConfig& Selected() noexcept {
        const int idx = (selectedKind == ItemKind::Macro && selectedIndex >= 0 && selectedIndex < kSlotCount)
            ? selectedIndex : std::clamp(selectedSlot, 0, kSlotCount - 1);
        return slots[idx];
    }

    [[nodiscard]] ComplexSequenceConfig& SelectedSeq() noexcept {
        return sequences[std::clamp(selectedIndex, 0, kComplexSeqCount - 1)];
    }

    [[nodiscard]] ColorTriggerConfig& SelectedTrigger() noexcept {
        return triggers[std::clamp(selectedIndex, 0, kColorTriggerCount - 1)];
    }

    [[nodiscard]] RemapRuleConfig& SelectedRemap() noexcept {
        return remaps[std::clamp(selectedIndex, 0, kRemapRuleCount - 1)];
    }

    [[nodiscard]] bool AnyRemapEnabled() const noexcept {
        for (const auto& r : remaps) {
            if (r.inUse && r.enabled) return true;
        }
        return false;
    }

    [[nodiscard]] int FindFirstFreeSlot() const noexcept {
        for (int i = 0; i < kSlotCount; ++i) {
            if (!IsMacroInUse(i)) return i;
        }
        return -1;
    }

    [[nodiscard]] int FindFirstFreeSeq() const noexcept {
        for (int i = 0; i < kComplexSeqCount; ++i) {
            if (!sequences[i].inUse) return i;
        }
        return -1;
    }

    [[nodiscard]] int FindFirstFreeTrigger() const noexcept {
        for (int i = 0; i < kColorTriggerCount; ++i) {
            if (!triggers[i].inUse) return i;
        }
        return -1;
    }

    [[nodiscard]] int FindFirstFreeRemap() const noexcept {
        for (int i = 0; i < kRemapRuleCount; ++i) {
            if (!remaps[i].inUse) return i;
        }
        return -1;
    }

    /// Builds the ordered list of visible sidebar items under the current filter.
    [[nodiscard]] int BuildSidebarList(std::array<SidebarEntry, 128>& out) const noexcept {
        int count = 0;
        if (sidebarFilter == 0 || sidebarFilter == 1) {
            for (int i = 0; i < kSlotCount; ++i) {
                if (IsMacroInUse(i)) out[count++] = SidebarEntry{ ItemKind::Macro, i };
            }
        }
        if (sidebarFilter == 0 || sidebarFilter == 2) {
            for (int i = 0; i < kComplexSeqCount; ++i) {
                if (IsSeqInUse(i)) out[count++] = SidebarEntry{ ItemKind::Sequence, i };
            }
        }
        if (sidebarFilter == 0 || sidebarFilter == 3) {
            for (int i = 0; i < kColorTriggerCount; ++i) {
                if (IsTriggerInUse(i)) out[count++] = SidebarEntry{ ItemKind::Trigger, i };
            }
        }
        if (sidebarFilter == 0 || sidebarFilter == 4) {
            for (int i = 0; i < kRemapRuleCount; ++i) {
                if (IsRemapInUse(i)) out[count++] = SidebarEntry{ ItemKind::Remap, i };
            }
        }
        return count;
    }

    [[nodiscard]] int ListedCount() const noexcept {
        std::array<SidebarEntry, 128> tmp{};
        return BuildSidebarList(tmp);
    }

    void EnsureValidSelection() noexcept {
        if (HasSelection()) {
            if (selectedKind == ItemKind::Macro) selectedSlot = selectedIndex;
            return;
        }
        std::array<SidebarEntry, 128> list{};
        const int n = BuildSidebarList(list);
        if (n > 0) {
            selectedKind  = list[0].kind;
            selectedIndex = list[0].index;
            if (selectedKind == ItemKind::Macro) selectedSlot = selectedIndex;
            return;
        }
        // Check across all items regardless of filter
        for (int i = 0; i < kSlotCount; ++i)       if (IsMacroInUse(i))   { selectedKind = ItemKind::Macro;    selectedIndex = i; selectedSlot = i; return; }
        for (int i = 0; i < kComplexSeqCount; ++i) if (IsSeqInUse(i))     { selectedKind = ItemKind::Sequence; selectedIndex = i; return; }
        for (int i = 0; i < kColorTriggerCount; ++i) if (IsTriggerInUse(i)) { selectedKind = ItemKind::Trigger; selectedIndex = i; return; }
        for (int i = 0; i < kRemapRuleCount; ++i)  if (IsRemapInUse(i))   { selectedKind = ItemKind::Remap;    selectedIndex = i; return; }
        selectedKind  = ItemKind::Macro;
        selectedIndex = 0;
        selectedSlot  = 0;
    }

    // ---- Reset helpers ------------------------------------------------------------

    void ResetSlot(int i) noexcept {
        auto& s = slots[i];
        s = MacroConfig{};
        s.slotIndex          = static_cast<uint8_t>(i);
        s.enabled            = false;
        s.triggerVk          = 0;
        s.outputVk           = VK_LBUTTON;
        s.isMouse            = true;
        s.mode               = static_cast<uint8_t>(MacroMode::HoldToLoop);
        s.dwellDurationNs    = 10'000'000ULL;
        s.recoveryDurationNs = 10'000'000ULL;
        s.repeatCount        = 10;
        s.stepCount          = 4;
        std::swprintf(s.name, std::size(s.name), L"Macro %d", i + 1);

        constexpr uint16_t kDefaultSeqKeys[8] = { '1', '2', '3', '4', '5', '6', '7', '8' };
        for (int step = 0; step < kStepCount; ++step) {
            s.steps[step].dwellDurationNs    = 15'000'000ULL;
            s.steps[step].recoveryDurationNs = 20'000'000ULL;
            s.steps[step].outputVk           = kDefaultSeqKeys[step];
            s.steps[step].isMouse            = false;
            s.steps[step].enabled            = true;
        }
    }

    void ResetSequence(int i) noexcept {
        auto& q = sequences[i];
        q = ComplexSequenceConfig{};
        q.slotIndex   = static_cast<uint8_t>(i);
        q.enabled     = false;
        q.inUse       = false;
        q.triggerMode = 0U;
        q.triggerVk   = 0U;
        q.stepCount   = 0U;
        q.humanizePct = 0U;
        std::swprintf(q.name, std::size(q.name), L"Sequence %d", i + 1);
    }

    void ResetTrigger(int i) noexcept {
        auto& t = triggers[i];
        t = ColorTriggerConfig{};
        t.slotIndex    = static_cast<uint8_t>(i);
        t.enabled      = false;
        t.inUse        = false;
        t.mode         = static_cast<uint8_t>(ColorTriggerMode::MatchColor);
        t.actionKind   = static_cast<uint8_t>(ColorTriggerActionKind::ClickKey);
        t.armMode      = 0U;
        t.r            = 255U;
        t.g            = 60U;
        t.b            = 60U;
        t.tolerance    = 28U;
        t.monitorIndex = 0U;
        t.sampleRadius = 2U;
        t.armVk        = 0U;
        t.actionVk     = VK_LBUTTON;
        t.sampleX      = -1;
        t.sampleY      = -1;
        t.delayMs      = 0U;
        t.holdMs       = 15U;
        t.cooldownMs   = 80U;
        t.linkedIndex  = 0U;
        std::swprintf(t.name, std::size(t.name), L"Color Trigger %d", i + 1);
    }

    void ResetRemap(int i) noexcept {
        auto& r = remaps[i];
        r = RemapRuleConfig{};
        r.slotIndex        = static_cast<uint8_t>(i);
        r.enabled          = false;
        r.inUse            = false;
        r.suppressOriginal = false;
        r.anyDevice        = true;
        r.deviceKind       = 0U;
        r.layerIndex       = 0U;
        r.unlockPolicy     = static_cast<uint8_t>(RemapUnlockPolicy::Always);
        r.actionKind       = static_cast<uint8_t>(RemapLayerAction::RemapKey);
        r.targetLayer      = 0U;
        r.monitorIndex     = -1;
        r.sourceVk         = 0U;
        r.targetVk         = 0U;
        wcscpy_s(r.deviceLabel, L"Any device");
        std::swprintf(r.name, std::size(r.name), L"Remap %d", i + 1);
    }

    void InitializeDefaults() noexcept {
        for (int i = 0; i < kSlotCount; ++i)         ResetSlot(i);
        for (int i = 0; i < kComplexSeqCount; ++i)   ResetSequence(i);
        for (int i = 0; i < kColorTriggerCount; ++i) ResetTrigger(i);
        for (int i = 0; i < kRemapRuleCount; ++i)    ResetRemap(i);

        auto& rapid = slots[0];
        rapid.triggerVk = VK_XBUTTON1;
        rapid.outputVk  = VK_LBUTTON;
        rapid.mode      = static_cast<uint8_t>(MacroMode::HoldToLoop);
        SetPeriodAndDuty(rapid, 10.0, 0.5);
        wcscpy_s(rapid.name, L"Rapid click");

        auto& toggle = slots[1];
        toggle.triggerVk = VK_F6;
        toggle.outputVk  = VK_LBUTTON;
        toggle.mode      = static_cast<uint8_t>(MacroMode::InfiniteToggleLoop);
        SetPeriodAndDuty(toggle, 10.0, 0.5);
        wcscpy_s(toggle.name, L"Auto-clicker");

        auto& hold = slots[2];
        hold.triggerVk = VK_F9;
        hold.outputVk  = 'W';
        hold.isMouse   = false;
        hold.mode      = static_cast<uint8_t>(MacroMode::InfiniteToggleLoop);
        SetPeriodAndDuty(hold, 10.0, 1.0);
        wcscpy_s(hold.name, L"Hold W");

        auto& seq = slots[3];
        seq.triggerVk = VK_F7;
        seq.outputVk  = '1';
        seq.isMouse   = false;
        seq.mode      = static_cast<uint8_t>(MacroMode::Sequence);
        seq.stepCount = 4;
        wcscpy_s(seq.name, L"Keys 1 to 4");

        auto& socd = slots[4];
        socd.triggerVk = 'A';
        socd.outputVk  = 'D';
        socd.isMouse   = false;
        socd.mode      = static_cast<uint8_t>(MacroMode::SocdPair);
        wcscpy_s(socd.name, L"A / D snap-tap");

        for (int i = 0; i < 5; ++i) {
            slots[i].reserved[0] = 1;
        }
        selectedKind  = ItemKind::Macro;
        selectedIndex = 0;
        selectedSlot  = 0;
    }

    // ---- Engine sync --------------------------------------------------------------

    void PublishWatchedKeys() noexcept {
        uint64_t bits[4]{0, 0, 0, 0};
        auto watch = [&bits](unsigned vk) noexcept {
            vk &= 0xFFU;
            if (vk != 0U) bits[vk >> 6] |= (1ULL << (vk & 63U));
        };

        if (settings.pauseHotkeyVk != 0U) {
            watch(settings.pauseHotkeyVk);
        }
        if (settings.stopAllHotkeyVk != 0U) {
            watch(settings.stopAllHotkeyVk);
        }
        if (settings.panicChordEnabled) {
            watch(VK_LCONTROL);
            watch(VK_RCONTROL);
            watch(VK_ESCAPE);
        }

        bool anyEnabled = false;
        for (const auto& s : slots) {
            if (!s.enabled || s.triggerVk == 0) continue;
            anyEnabled = true;
            watch(s.triggerVk);
            if (static_cast<MacroMode>(s.mode) == MacroMode::SocdPair) watch(s.outputVk);
        }
        for (const auto& q : sequences) {
            if (!q.inUse || !q.enabled || q.triggerVk == 0) continue;
            anyEnabled = true;
            watch(q.triggerVk);
        }
        for (const auto& t : triggers) {
            if (!t.inUse || !t.enabled) continue;
            anyEnabled = true;
            if (t.armVk != 0) watch(t.armVk);
        }
        for (const auto& r : remaps) {
            if (!r.inUse || !r.enabled || r.sourceVk == 0) continue;
            anyEnabled = true;
            watch(r.sourceVk);
        }

        for (int i = 0; i < 4; ++i) {
            g_SharedState.watchedVkBits[i].store(bits[i], std::memory_order_release);
        }
        g_SharedState.anyMacroEnabled.store(anyEnabled, std::memory_order_release);
    }

    void MarkDirty() noexcept {
        saveState = SaveState::Pending;
        lastChangeTick = GetTickCount64();
    }

    void SyncSlotToEngine(int slotIndex, bool markChanged = true) noexcept {
        if (slotIndex < 0 || slotIndex >= kSlotCount) {
            return;
        }
        auto& s = slots[slotIndex];
        s.slotIndex = static_cast<uint8_t>(slotIndex);
        s.name[std::size(s.name) - 1] = L'\0';
        s.isMouse = mask::input::InputDispatcher::IsMouseVirtualKey(s.outputVk);
        for (auto& step : s.steps) {
            step.isMouse = mask::input::InputDispatcher::IsMouseVirtualKey(step.outputVk);
        }
        pendingSyncMask |= (1ULL << slotIndex);
        FlushPendingSync();
        PublishWatchedKeys();
        if (markChanged) {
            MarkDirty();
        }
    }

    void FlushPendingSync() noexcept {
        for (int i = 0; i < kSlotCount && pendingSyncMask != 0ULL; ++i) {
            const uint64_t bit = (1ULL << i);
            if ((pendingSyncMask & bit) == 0ULL) continue;
            if (!g_SharedState.uiCommandQueue.try_push(slots[i])) {
                break;
            }
            pendingSyncMask &= ~bit;
        }
        mask::input::WakeEngine();
    }

    void SyncAllSlotsToEngine() noexcept {
        pendingSyncMask = ~0ULL;
        FlushPendingSync();
        PublishWatchedKeys();
    }

    void ApplySettings() noexcept {
        mask::settings::SanitizeSettings(settings);
        wcsncpy_s(mouseProfileId, settings.mouseProfileId, _TRUNCATE);
        wcsncpy_s(keyboardProfileId, settings.keyboardProfileId, _TRUNCATE);
        mask::settings::ApplySettingsToSharedState(settings, g_SharedState);
        PublishWatchedKeys();
    }

    // ---- Toast --------------------------------------------------------------------

    void SetToast(const wchar_t* message, int action = 0, const wchar_t* actionLabel = L"") noexcept {
        wcsncpy_s(toast, message, _TRUNCATE);
        wcsncpy_s(toastActionLabel, actionLabel, _TRUNCATE);
        toastAction = action;
        toastTick = GetTickCount64();
    }

    void ClearToast() noexcept {
        toast[0] = L'\0';
        toastAction = 0;
    }

    // ---- Multi-bank persistence ---------------------------------------------------

    void ResolveProfilePaths() noexcept {
        wchar_t exePath[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            if (wchar_t* slash = std::wcsrchr(exePath, L'\\')) {
                *(slash + 1) = L'\0';
            }
            wcscpy_s(dataDir, exePath);
        } else {
            wcscpy_s(dataDir, L".\\");
        }

        wchar_t localAppData[MAX_PATH]{};
        const DWORD envLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (envLen > 0 && envLen < MAX_PATH - 24) {
            std::swprintf(fallbackDir, std::size(fallbackDir), L"%ls\\Mask\\", localAppData);
        }
        UpdateActiveBankPaths();
    }

    void BuildBankFilePath(wchar_t* out, size_t cap, const wchar_t* dir, const wchar_t* stem, uint8_t bank) const noexcept {
        if (bank == 0U) {
            std::swprintf(out, cap, L"%ls%ls.bin", dir, stem);
        } else {
            std::swprintf(out, cap, L"%ls%ls_%u.bin", dir, stem, static_cast<unsigned>(bank + 1U));
        }
    }

    void UpdateActiveBankPaths() noexcept {
        const uint8_t bank = (settings.activeProfileBank < mask::settings::kProfileBankCount)
            ? settings.activeProfileBank : 0U;
        BuildBankFilePath(profilePath, std::size(profilePath), dataDir, L"profiles", bank);
        if (fallbackDir[0] != L'\0') {
            BuildBankFilePath(fallbackPath, std::size(fallbackPath), fallbackDir, L"profiles", bank);
        }
    }

    [[nodiscard]] bool SaveSettingsOnly() noexcept {
        wcsncpy_s(settings.mouseProfileId, mouseProfileId, _TRUNCATE);
        wcsncpy_s(settings.keyboardProfileId, keyboardProfileId, _TRUNCATE);
        wchar_t path[MAX_PATH]{};
        std::swprintf(path, std::size(path), L"%lssettings.bin", dataDir);
        bool ok = mask::settings::SaveSettings(path, settings);
        if (!ok && fallbackDir[0] != L'\0' && std::wcscmp(dataDir, fallbackDir) != 0) {
            std::swprintf(path, std::size(path), L"%lssettings.bin", fallbackDir);
            ok = mask::settings::SaveSettings(path, settings);
            if (ok) {
                wcscpy_s(dataDir, fallbackDir);
                UpdateActiveBankPaths();
            }
        }
        return ok;
    }

    bool LoadSettingsFromDisk() noexcept {
        wchar_t path[MAX_PATH]{};
        std::swprintf(path, std::size(path), L"%lssettings.bin", dataDir);
        bool ok = mask::settings::LoadSettings(path, settings);
        if (!ok && fallbackDir[0] != L'\0') {
            std::swprintf(path, std::size(path), L"%lssettings.bin", fallbackDir);
            ok = mask::settings::LoadSettings(path, settings);
            if (ok) {
                wcscpy_s(dataDir, fallbackDir);
            }
        }
        settings.startWithWindows = mask::settings::QueryStartWithWindows();
        ApplySettings();
        UpdateActiveBankPaths();
        return ok;
    }

    bool SaveToDisk() noexcept {
        UpdateActiveBankPaths();
        const uint8_t bank = (settings.activeProfileBank < mask::settings::kProfileBankCount)
            ? settings.activeProfileBank : 0U;

        // 1. Macros (profiles.bin / profiles_2.bin / profiles_3.bin)
        static uint8_t buffer[storage::kHeaderBytes + storage::kPayloadBytes];
        std::memcpy(buffer, &storage::kMagic, 4);
        std::memcpy(buffer + 4, &storage::kVersion, 4);
        std::memcpy(buffer + storage::kHeaderBytes, slots.data(), storage::kPayloadBytes);

        bool ok = storage::WriteFileAtomic(profilePath, buffer, sizeof(buffer));
        if (!ok && fallbackDir[0] != L'\0' && std::wcscmp(dataDir, fallbackDir) != 0) {
            wcscpy_s(dataDir, fallbackDir);
            UpdateActiveBankPaths();
            ok = storage::WriteFileAtomic(profilePath, buffer, sizeof(buffer));
        }

        // 2. Complex Sequences, Color Triggers, Remaps, Settings
        wchar_t seqPath[MAX_PATH]{};
        wchar_t trigPath[MAX_PATH]{};
        wchar_t remapPath[MAX_PATH]{};
        BuildBankFilePath(seqPath,   std::size(seqPath),   dataDir, L"sequences", bank);
        BuildBankFilePath(trigPath,  std::size(trigPath),  dataDir, L"triggers",  bank);
        BuildBankFilePath(remapPath, std::size(remapPath), dataDir, L"remaps",    bank);

        ok &= mask::workflows::ComplexSequenceEngine::SaveSequences(seqPath, sequences);
        ok &= mask::triggers::PixelTriggerEngine::SaveTriggers(trigPath, triggers);
        ok &= mask::remap::RemapEngine::SaveRemaps(remapPath, remaps);
        ok &= SaveSettingsOnly();

        saveState = ok ? SaveState::Saved : SaveState::Failed;
        return ok;
    }

    /**
     * @brief Loads saved macros, complex sequences, color triggers, and remap rules for the active bank.
     * @return 0 = nothing found, 1 = loaded, 2 = main macro file was damaged and backup restored.
     */
    int LoadFromDisk() noexcept {
        UpdateActiveBankPaths();
        const uint8_t bank = (settings.activeProfileBank < mask::settings::kProfileBankCount)
            ? settings.activeProfileBank : 0U;

        // Macros
        std::array<MacroConfig, mask::core::kMaxMacroSlots> loaded{};
        wchar_t candidate[MAX_PATH + 16]{};
        int result = 0;
        const wchar_t* bases[2] = { profilePath, fallbackPath };
        for (int b = 0; b < 2 && result == 0; ++b) {
            if (bases[b][0] == L'\0') continue;
            if (storage::ReadProfileFile(bases[b], loaded)) {
                result = 1;
            } else {
                std::swprintf(candidate, std::size(candidate), L"%ls.bak", bases[b]);
                if (storage::ReadProfileFile(candidate, loaded)) result = 2;
            }
            if (result != 0 && b == 1 && fallbackDir[0] != L'\0') {
                wcscpy_s(dataDir, fallbackDir);
                UpdateActiveBankPaths();
            }
        }

        if (result != 0) {
            for (int i = 0; i < kSlotCount; ++i) {
                storage::Sanitize(loaded[i], i);
            }
            slots = loaded;
        } else {
            // For empty secondary banks, start clean; for bank 0, keep example macros
            if (bank != 0U) {
                for (int i = 0; i < kSlotCount; ++i) ResetSlot(i);
            }
        }

        // Complex Sequences
        for (int i = 0; i < kComplexSeqCount; ++i) ResetSequence(i);
        wchar_t path[MAX_PATH]{};
        BuildBankFilePath(path, std::size(path), dataDir, L"sequences", bank);
        if (!mask::workflows::ComplexSequenceEngine::LoadSequences(path, sequences) && fallbackDir[0] != L'\0') {
            BuildBankFilePath(path, std::size(path), fallbackDir, L"sequences", bank);
            (void)mask::workflows::ComplexSequenceEngine::LoadSequences(path, sequences);
        }

        // Color Triggers
        for (int i = 0; i < kColorTriggerCount; ++i) ResetTrigger(i);
        BuildBankFilePath(path, std::size(path), dataDir, L"triggers", bank);
        if (!mask::triggers::PixelTriggerEngine::LoadTriggers(path, triggers) && fallbackDir[0] != L'\0') {
            BuildBankFilePath(path, std::size(path), fallbackDir, L"triggers", bank);
            (void)mask::triggers::PixelTriggerEngine::LoadTriggers(path, triggers);
        }

        // Remap Rules
        for (int i = 0; i < kRemapRuleCount; ++i) ResetRemap(i);
        BuildBankFilePath(path, std::size(path), dataDir, L"remaps", bank);
        if (!mask::remap::RemapEngine::LoadRemaps(path, remaps) && fallbackDir[0] != L'\0') {
            BuildBankFilePath(path, std::size(path), fallbackDir, L"remaps", bank);
            (void)mask::remap::RemapEngine::LoadRemaps(path, remaps);
        }

        g_SharedState.flushAllRequested.store(true, std::memory_order_release);
        SyncAllSlotsToEngine();
        EnsureValidSelection();
        saveState = SaveState::Saved;
        return result;
    }
};

inline AppModel g_Ui{};
inline mask::ui::UiAnimator g_Animator{};

} // namespace mask::app
