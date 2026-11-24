#include "app_model.hpp"
#include "profile_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <memory>
#include <new>

namespace mask::app {

namespace {

[[nodiscard]] inline bool RawBool(const bool& b) noexcept {
    uint8_t raw = 0U;
    std::memcpy(&raw, &b, 1U);
    return raw != 0U;
}

[[nodiscard]] inline uint8_t NormalizeAutoclickerMode(uint8_t mode) noexcept {
    if (mode < static_cast<uint8_t>(kModeCount)) {
        return mode;
    }
    return static_cast<uint8_t>(MacroMode::HoldToLoop);
}

} // namespace

// =====================================================================================
//  Names & Step / Remap / Trigger Descriptions
// =====================================================================================

KeyName NameOf(uint16_t vk) noexcept {
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

const wchar_t* ModeName(int mode) noexcept {
    switch (static_cast<MacroMode>(mode)) {
        case MacroMode::HoldToLoop:         return L"Hold";
        case MacroMode::InfiniteToggleLoop: return L"Toggle";
        case MacroMode::RepeatNTimes:       return L"Burst";
        default:                            return L"Hold";
    }
}

const wchar_t* ModeDescription(int mode) noexcept {
    switch (static_cast<MacroMode>(mode)) {
        case MacroMode::HoldToLoop:         return L"Clicks repeatedly for as long as the trigger is held down.";
        case MacroMode::InfiniteToggleLoop: return L"Press the trigger to start clicking. Press it again to stop.";
        case MacroMode::RepeatNTimes:       return L"Press the trigger once to burst a set number of clicks.";
        default:                            return L"Clicks repeatedly for as long as the trigger is held down.";
    }
}

bool IsTimedMode(int mode) noexcept {
    return mode >= 0 && mode < kModeCount;
}

const wchar_t* StepKindName(ComplexStepKind kind) noexcept {
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
        case ComplexStepKind::ToggleMacro:   return L"Toggle clicker";
        case ComplexStepKind::IfCheck:       return L"If check";
        case ComplexStepKind::ElseBranch:    return L"Else";
        case ComplexStepKind::EndIf:         return L"End if";
        default:                             return L"Step";
    }
}

const wchar_t* ConditionKindName(ComplexConditionKind cond) noexcept {
    switch (cond) {
        case ComplexConditionKind::KeyHeld:       return L"Key held";
        case ComplexConditionKind::ColorMatches:  return L"Pixel color";
        case ComplexConditionKind::LayerIs:       return L"Layer";
        case ComplexConditionKind::UnlockStateIs: return L"Lock state";
        case ComplexConditionKind::MacroActive:   return L"Clicker on";
        case ComplexConditionKind::RandomChance:  return L"Random %";
        default:                                  return L"Key held";
    }
}

int CountStepKind(const ComplexSequenceConfig& seq, ComplexStepKind target) noexcept {
    const int cnt = std::clamp<int>(seq.stepCount, 0, kComplexStepMax);
    int total = 0;
    for (int i = 0; i < cnt; ++i) {
        if (seq.steps[static_cast<size_t>(i)].kind == static_cast<uint8_t>(target)) {
            ++total;
        }
    }
    return total;
}

int FindMatchingEndIf(const ComplexSequenceConfig& seq, int ifIndex) noexcept {
    const int cnt = std::clamp<int>(seq.stepCount, 0, kComplexStepMax);
    int depth = 0;
    for (int i = ifIndex + 1; i < cnt; ++i) {
        const auto k = static_cast<ComplexStepKind>(seq.steps[static_cast<size_t>(i)].kind);
        if (k == ComplexStepKind::IfCheck) {
            ++depth;
        } else if (k == ComplexStepKind::EndIf) {
            if (depth == 0) {
                return i;
            }
            --depth;
        }
    }
    return -1;
}

bool HasElseInIfBlock(const ComplexSequenceConfig& seq, int ifIndex) noexcept {
    const int cnt = std::clamp<int>(seq.stepCount, 0, kComplexStepMax);
    int depth = 0;
    for (int i = ifIndex + 1; i < cnt; ++i) {
        const auto k = static_cast<ComplexStepKind>(seq.steps[static_cast<size_t>(i)].kind);
        if (k == ComplexStepKind::IfCheck) {
            ++depth;
        } else if (k == ComplexStepKind::ElseBranch) {
            if (depth == 0) {
                return true;
            }
        } else if (k == ComplexStepKind::EndIf) {
            if (depth == 0) {
                return false;
            }
            --depth;
        }
    }
    return false;
}

const wchar_t* SeqTriggerModeName(uint8_t mode) noexcept {
    switch (mode) {
        case 0:  return L"Once";
        case 1:  return L"Toggle";
        case 2:  return L"Hold";
        default: return L"Once";
    }
}

const wchar_t* LayerName(uint8_t layer) noexcept {
    switch (layer & 3U) {
        case 0:  return L"Base";
        case 1:  return L"L1";
        case 2:  return L"L2";
        case 3:  return L"L3";
        default: return L"Base";
    }
}

void DescribeComplexStep(const ComplexStep& st, wchar_t* buf, size_t n) noexcept {
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
            std::swprintf(buf, n, L"Toggle clicker #%u", static_cast<unsigned>((st.param % mask::core::kMaxMacroSlots) + 1U));
            break;
        case ComplexStepKind::IfCheck: {
            const bool neg = mask::core::IsStepConditionNegated(st);
            switch (mask::core::GetStepConditionKind(st)) {
                case ComplexConditionKind::KeyHeld:
                    std::swprintf(buf, n, L"If %ls is %ls",
                                  st.vk != 0U ? NameOf(st.vk).text : L"(pick key)",
                                  neg ? L"not held" : L"held");
                    break;
                case ComplexConditionKind::ColorMatches:
                    std::swprintf(buf, n,
                                  neg ? L"If color isn\u2019t at (%d, %d)" : L"If color matches at (%d, %d)",
                                  static_cast<int>(st.x), static_cast<int>(st.y));
                    break;
                case ComplexConditionKind::LayerIs:
                    std::swprintf(buf, n,
                                  neg ? L"If layer isn\u2019t %ls" : L"If layer is %ls",
                                  LayerName(static_cast<uint8_t>(st.param)));
                    break;
                case ComplexConditionKind::UnlockStateIs: {
                    const bool effLocked = (st.param != 0U) ^ neg;
                    wcscpy_s(buf, n, effLocked ? L"If controls are locked" : L"If controls are unlocked");
                    break;
                }
                case ComplexConditionKind::MacroActive:
                    std::swprintf(buf, n,
                                  neg ? L"If Clicker #%u isn\u2019t running" : L"If Clicker #%u is running",
                                  static_cast<unsigned>((st.param % mask::core::kMaxMacroSlots) + 1U));
                    break;
                case ComplexConditionKind::RandomChance:
                    std::swprintf(buf, n,
                                  neg ? L"If %u%% chance misses" : L"If %u%% chance hits",
                                  static_cast<unsigned>((std::min)(st.param, 100U)));
                    break;
                default:
                    wcscpy_s(buf, n, L"If check");
                    break;
            }
            break;
        }
        case ComplexStepKind::ElseBranch:
            wcscpy_s(buf, n, L"Otherwise (Else)");
            break;
        case ComplexStepKind::EndIf:
            wcscpy_s(buf, n, L"End if block");
            break;
        default:
            wcscpy_s(buf, n, L"Step");
            break;
    }
}

void DescribeComplexSequence(const ComplexSequenceConfig& q, wchar_t* buf, size_t n) noexcept {
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

const wchar_t* RemapActionName(RemapLayerAction action) noexcept {
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

const wchar_t* UnlockPolicyName(RemapUnlockPolicy pol) noexcept {
    switch (pol) {
        case RemapUnlockPolicy::Always:       return L"Always";
        case RemapUnlockPolicy::WhenUnlocked: return L"When unlocked";
        case RemapUnlockPolicy::WhenLocked:   return L"When locked";
        default:                              return L"Always";
    }
}

void DescribeRemapRule(const RemapRuleConfig& r, wchar_t* buf, size_t n) noexcept {
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

const wchar_t* TriggerModeName(ColorTriggerMode mode) noexcept {
    switch (mode) {
        case ColorTriggerMode::ColorChanged: return L"Color Change";
        case ColorTriggerMode::MatchColor:
        default:                             return L"Color Match";
    }
}

const wchar_t* TriggerMatchRuleName(ColorTriggerMatchRule rule) noexcept {
    switch (rule) {
        case ColorTriggerMatchRule::ZoneAverage: return L"Zone Average";
        case ColorTriggerMatchRule::AnyPixel:
        default:                                 return L"Any Pixel";
    }
}

const wchar_t* ToleranceWord(uint8_t tol) noexcept {
    if (tol <= 12U) return L"Very strict";
    if (tol <= 26U) return L"Strict";
    if (tol <= 48U) return L"Balanced";
    if (tol <= 85U) return L"Wide";
    return L"Very wide";
}

void DescribeColorTrigger(const ColorTriggerConfig& t, wchar_t* buf, size_t n) noexcept {
    wchar_t spotBuf[48]{};
    const unsigned monNum = static_cast<unsigned>(t.monitorIndex + 1U);
    if (t.sampleX < 0 || t.sampleY < 0) {
        std::swprintf(spotBuf, std::size(spotBuf), L"the center of Screen %u", monNum);
    } else {
        std::swprintf(spotBuf, std::size(spotBuf), L"Screen %u (%d, %d)",
                      monNum, static_cast<int>(t.sampleX), static_cast<int>(t.sampleY));
    }

    const wchar_t* modeStr = (static_cast<ColorTriggerMode>(t.mode) == ColorTriggerMode::ColorChanged)
        ? L"for any color change"
        : L"for this color";
    const wchar_t* ruleStr = (GetTriggerMatchRule(t) == ColorTriggerMatchRule::ZoneAverage && t.sampleRadius > 0U)
        ? L" (zone average)"
        : L"";

    wchar_t armBuf[48]{};
    if (t.armMode == 1U) {
        std::swprintf(armBuf, std::size(armBuf), L" while holding %ls", NameOf(t.armVk).text);
    } else if (t.armMode == 2U) {
        std::swprintf(armBuf, std::size(armBuf), L" when toggled with %ls", NameOf(t.armVk).text);
    }

    wchar_t actBuf[56]{};
    switch (static_cast<ColorTriggerActionKind>(t.actionKind)) {
        case ColorTriggerActionKind::ClickKey:
            std::swprintf(actBuf, std::size(actBuf), L"clicks %ls", NameOf(t.actionVk).text);
            break;
        case ColorTriggerActionKind::HoldKey:
            std::swprintf(actBuf, std::size(actBuf), L"holds %ls", NameOf(t.actionVk).text);
            break;
        case ColorTriggerActionKind::RunMacroSlot:
            std::swprintf(actBuf, std::size(actBuf), L"runs Clicker #%u", static_cast<unsigned>(t.linkedIndex + 1U));
            break;
        case ColorTriggerActionKind::RunComplexSeq:
            std::swprintf(actBuf, std::size(actBuf), L"runs Sequence #%u", static_cast<unsigned>(t.linkedIndex + 1U));
            break;
        default:
            wcscpy_s(actBuf, L"runs an action");
            break;
    }
    std::swprintf(buf, n, L"Watches %ls %ls%ls%ls, then %ls.", spotBuf, modeStr, ruleStr, armBuf, actBuf);
}

// =====================================================================================
//  Number formatting & unrestricted timing maths
// =====================================================================================

void FormatNumber(double value, int maxDecimals, wchar_t* buf, size_t n) noexcept {
    if (!std::isfinite(value)) {
        wcscpy_s(buf, n, L"0");
        return;
    }
    std::swprintf(buf, n, L"%.*f", maxDecimals, value);
    if (std::wcschr(buf, L'.') == nullptr) {
        return;
    }
    size_t len = std::wcslen(buf);
    while (len > 0U && buf[len - 1U] == L'0') {
        buf[--len] = L'\0';
    }
    if (len > 0U && buf[len - 1U] == L'.') {
        buf[--len] = L'\0';
    }
    if (std::wcscmp(buf, L"-0") == 0) {
        wcscpy_s(buf, n, L"0");
    }
}

void FormatMs(double ms, wchar_t* buf, size_t n) noexcept {
    int dec = 0;
    if (ms > 0.0 && ms < 0.001) {
        dec = 6;
    } else if (ms < 1.0) {
        dec = 3;
    } else if (ms < 100.0) {
        dec = 2;
    }
    FormatNumber(ms, dec, buf, n);
}

void FormatDuration(double ms, wchar_t* buf, size_t n) noexcept {
    wchar_t num[32]{};
    if (ms >= 1000.0) {
        FormatNumber(ms / 1000.0, 2, num, std::size(num));
        std::swprintf(buf, n, L"%ls s", num);
    } else {
        FormatMs(ms, num, std::size(num));
        std::swprintf(buf, n, L"%ls ms", num);
    }
}

double RatePerSecond(const MacroConfig& c) noexcept {
    const double period = PeriodMs(c);
    if (!(period > 0.0) || !std::isfinite(period)) {
        return 0.0;
    }
    return 1000.0 / period;
}

double DutyPercent(const MacroConfig& c) noexcept {
    const double period = PeriodMs(c);
    if (!(period > 0.0) || !std::isfinite(period)) {
        return 50.0;
    }
    return std::clamp(100.0 * DownMs(c) / period, 0.0, 100.0);
}

uint64_t MsToNs(double ms) noexcept {
    if (!(ms > 0.0) || !std::isfinite(ms)) {
        return 0ULL;
    }
    constexpr double kMaxSafeNs = 9.0e18; // safely inside signed int64_t for llround (~285 years)
    const double scaled = ms * 1e6;
    if (scaled >= kMaxSafeNs) {
        return static_cast<uint64_t>(kMaxSafeNs);
    }
    return static_cast<uint64_t>(std::llround(scaled));
}

void SetPeriodAndDuty(MacroConfig& c, double periodMs, double dutyFraction) noexcept {
    if (!(periodMs > 0.0) || !std::isfinite(periodMs)) {
        c.dwellDurationNs = 0ULL;
        c.recoveryDurationNs = 0ULL;
        return;
    }
    const double frac = std::isfinite(dutyFraction) ? std::clamp(dutyFraction, 0.0, 1.0) : 0.5;
    const double down = periodMs * frac;
    const double up   = periodMs - down;
    c.dwellDurationNs    = MsToNs(down);
    c.recoveryDurationNs = MsToNs(up);
}

void SetRate(MacroConfig& c, double perSecond) noexcept {
    if (!(perSecond > 0.0) || !std::isfinite(perSecond)) {
        return;
    }
    const double period = PeriodMs(c);
    const double fraction = (period > 0.0 && std::isfinite(period))
        ? std::clamp(DownMs(c) / period, 0.0, 1.0)
        : 0.5;
    SetPeriodAndDuty(c, 1000.0 / perSecond, fraction);
}

void SetDuty(MacroConfig& c, double percent) noexcept {
    const double clamped = std::isfinite(percent) ? std::clamp(percent, 0.0, 100.0) : 50.0;
    SetPeriodAndDuty(c, PeriodMs(c), clamped / 100.0);
}

void FormatDuty(const MacroConfig& c, wchar_t* buf, size_t n) noexcept {
    const double duty = DutyPercent(c);
    const int dec = (duty < 10.0) ? 2 : 1;
    FormatNumber(duty, dec, buf, n);
}

void FormatRate(double perSecond, wchar_t* buf, size_t n) noexcept {
    if (!(perSecond > 0.0) || !std::isfinite(perSecond)) {
        wcscpy_s(buf, n, L"0");
        return;
    }
    const int dec = (perSecond >= 100.0) ? 0 : (perSecond >= 10.0 ? 1 : 2);
    FormatNumber(perSecond, dec, buf, n);
}

bool IsAutoclickerTimingRisky(const MacroConfig& c) noexcept {
    const double down = DownMs(c);
    const double up = UpMs(c);
    const double period = PeriodMs(c);
    return (down < 0.05) || (up < 0.05) || (period > 0.0 && RatePerSecond(c) > 1000.0);
}

void DescribeRate(double perSecond, wchar_t* buf, size_t n) noexcept {
    if (!(perSecond > 0.0) || !std::isfinite(perSecond)) {
        wcscpy_s(buf, n, L"with 0 ms period");
        return;
    }
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

void DescribeAutoclicker(const MacroConfig& c, wchar_t* buf, size_t n) noexcept {
    const auto mode = static_cast<MacroMode>(NormalizeAutoclickerMode(c.mode));
    const KeyName trig = NameOf(c.triggerVk);
    const KeyName out = NameOf(c.outputVk);

    if (c.triggerVk == 0U) {
        wcscpy_s(buf, n, L"Choose a trigger to finish setting up this autoclicker.");
        return;
    }

    wchar_t rate[48]{};
    DescribeRate(RatePerSecond(c), rate, std::size(rate));

    switch (mode) {
        case MacroMode::HoldToLoop:
            std::swprintf(buf, n, L"While %ls is held, clicks %ls %ls.", trig.text, out.text, rate);
            break;
        case MacroMode::InfiniteToggleLoop:
            std::swprintf(buf, n, L"%ls starts and stops clicking %ls %ls.", trig.text, out.text, rate);
            break;
        case MacroMode::RepeatNTimes:
            if (c.repeatCount == 1U) {
                std::swprintf(buf, n, L"%ls clicks %ls 1 time, %ls.", trig.text, out.text, rate);
            } else {
                std::swprintf(buf, n, L"%ls clicks %ls %u times, %ls.", trig.text, out.text,
                              static_cast<unsigned>(c.repeatCount), rate);
            }
            break;
        default:
            buf[0] = L'\0';
            break;
    }

    const uint8_t hum = mask::core::HumanizePercent(c);
    if (hum > 0U) {
        const size_t len = std::wcslen(buf);
        if (len > 0U && buf[len - 1U] == L'.' && len + 26U < n) {
            std::swprintf(buf + len - 1U, n - (len - 1U), L" (\u00B1%u%% jitter).", static_cast<unsigned>(hum));
        } else if (len + 26U < n) {
            std::swprintf(buf + len, n - len, L" (\u00B1%u%% jitter)", static_cast<unsigned>(hum));
        }
    }
}

// =====================================================================================
//  Editable fields
// =====================================================================================

double FieldGet(const MacroConfig& s, Field f) noexcept {
    switch (f) {
        case Field::Rate:   return RatePerSecond(s);
        case Field::Duty:   return DutyPercent(s);
        case Field::Down:   return DownMs(s);
        case Field::Up:     return UpMs(s);
        case Field::Repeat: return static_cast<double>(s.repeatCount);
        case Field::MoveX:  return static_cast<double>(s.mouseDeltaX);
        case Field::MoveY:  return static_cast<double>(s.mouseDeltaY);
        default:            return 0.0;
    }
}

void FieldSet(MacroConfig& s, Field f, double v) noexcept {
    if (!std::isfinite(v)) {
        return;
    }
    switch (f) {
        case Field::Rate:   SetRate(s, v); return;
        case Field::Duty:   SetDuty(s, v); return;
        case Field::Down:   s.dwellDurationNs = MsToNs(std::max(v, 0.0)); return;
        case Field::Up:     s.recoveryDurationNs = MsToNs(std::max(v, 0.0)); return;
        case Field::Repeat:
            s.repeatCount = static_cast<uint32_t>(std::clamp<long long>(
                std::llround(v), 1LL, static_cast<long long>((std::numeric_limits<int32_t>::max)())));
            return;
        case Field::MoveX:  s.mouseDeltaX = static_cast<int32_t>(std::clamp<long long>(std::llround(v), -500LL, 500LL)); return;
        case Field::MoveY:  s.mouseDeltaY = static_cast<int32_t>(std::clamp<long long>(std::llround(v), -500LL, 500LL)); return;
        default:            return;
    }
}

void FormatFieldNumber(const MacroConfig& s, Field f, wchar_t* buf, size_t n) noexcept {
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

const wchar_t* FieldSuffix(Field f) noexcept {
    if (IsTimeField(f) || f == Field::SeqStepDuration || f == Field::SettingsAutosaveMs ||
        f == Field::TriggerDelay || f == Field::TriggerHold || f == Field::TriggerCooldown) {
        return L" ms";
    }
    if (f == Field::Rate) return L" CPS";
    if (f == Field::Duty) return L"%";
    if (f == Field::MoveX || f == Field::MoveY) return L" px";
    if (f == Field::Repeat) return L"\u00D7";
    return L"";
}

void FieldStep(MacroConfig& s, Field f, int direction, bool coarse, bool fine) noexcept {
    const double cur = FieldGet(s, f);
    const bool fractional = AllowsDecimal(f);
    double step = coarse ? 10.0 : 1.0;

    if (IsTimeField(f)) {
        if (fine) {
            step = 0.01;
        } else if (!coarse && (cur < 1.0 || (cur == 1.0 && direction < 0))) {
            step = 0.1;
        }
    } else if (f == Field::Rate) {
        if (coarse) {
            step = (cur >= 1000.0) ? 100.0 : 10.0;
        } else if (fine) {
            step = 0.1;
        } else if (cur >= 1000.0) {
            step = 10.0;
        }
    } else if (fine && fractional) {
        step = 0.1;
    }

    double next = cur + static_cast<double>(direction) * step;
    if (fractional && step >= 1.0 && cur >= 1.0 && step == 1.0) {
        next = (direction > 0 ? std::floor(cur + 1e-9) : std::ceil(cur - 1e-9)) + static_cast<double>(direction) * step;
    }

    if (f == Field::Duty) {
        next = std::clamp(next, 0.0, 100.0);
    } else if (IsTimeField(f)) {
        next = std::max(0.0, next);
    } else if (f == Field::Rate) {
        if (cur <= 0.0 && direction > 0) {
            next = 100.0;
        } else if (next <= 0.0) {
            next = (cur > 0.1) ? 0.1 : 0.01;
        }
    }
    FieldSet(s, f, next);
}

// =====================================================================================
//  Profile storage helpers (v1 / v2 -> v3 migration + unrestricted timings)
// =====================================================================================

namespace storage {

bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept {
    if (path == nullptr || path[0] == L'\0' || data == nullptr) {
        return false;
    }
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
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
                    written == size &&
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

bool ReadProfileFile(const wchar_t* path,
                     std::array<MacroConfig, mask::core::kMaxMacroSlots>& out,
                     uint32_t* outFileVersion) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    const bool sizeOk = GetFileSizeEx(h, &fileSize) != FALSE &&
                        fileSize.QuadPart == static_cast<LONGLONG>(kHeaderBytes + kPayloadBytes);
    if (!sizeOk) {
        CloseHandle(h);
        return false;
    }

    uint32_t header[2]{0U, 0U};
    DWORD got = 0U;
    bool ok = ReadFile(h, header, sizeof(header), &got, nullptr) != FALSE &&
              got == sizeof(header) &&
              header[0] == kMagic &&
              (header[1] == kVersion || header[1] == kVersionV2 || header[1] == kVersionV1);

    auto staging = std::make_unique<std::array<MacroConfig, mask::core::kMaxMacroSlots>>();
    if (ok && staging) {
        ok = ReadFile(h, static_cast<void*>(staging->data()), static_cast<DWORD>(kPayloadBytes), &got, nullptr) != FALSE &&
             got == kPayloadBytes;
    } else {
        ok = false;
    }
    CloseHandle(h);

    if (!ok) {
        return false;
    }

    // Migrate legacy v1/v2 MacroMode::Sequence (3) / SocdPair (4) -> HoldToLoop (0)
    if (header[1] < kVersion) {
        for (auto& s : *staging) {
            s.mode = NormalizeAutoclickerMode(s.mode);
        }
    }

    out = *staging;
    if (outFileVersion != nullptr) {
        *outFileVersion = header[1];
    }
    return true;
}

void Sanitize(MacroConfig& s, int index) noexcept {
    s.slotIndex = static_cast<uint8_t>(index);
    s.enabled   = RawBool(s.enabled);
    s.name[std::size(s.name) - 1] = L'\0';
    if (s.name[0] == L'\0') {
        std::swprintf(s.name, std::size(s.name), L"Autoclicker %d", index + 1);
    }
    s.mode        = NormalizeAutoclickerMode(s.mode);
    s.triggerVk   = static_cast<uint16_t>(s.triggerVk & 0xFFU);
    s.outputVk    = static_cast<uint16_t>(s.outputVk & 0xFFU);
    s.isMouse     = mask::input::InputDispatcher::IsMouseVirtualKey(s.outputVk);
    s.stepCount   = static_cast<uint8_t>(std::clamp<int>(s.stepCount, 1, static_cast<int>(mask::core::kMaxSequenceSteps)));
    s.repeatCount = (std::max<uint32_t>)(s.repeatCount, 1U);
    s.mouseDeltaX = std::clamp<int32_t>(s.mouseDeltaX, -500, 500);
    s.mouseDeltaY = std::clamp<int32_t>(s.mouseDeltaY, -500, 500);
    // Preserve raw dwellDurationNs / recoveryDurationNs with zero floor/ceiling clamping (supports 0 ms and <0.05 ms).
    if (s.triggerVk == 0U) {
        s.enabled = false;
    }
    s.reserved[0] = (s.reserved[0] != 0U || s.triggerVk != 0U || s.enabled) ? 1U : 0U;
    s.reserved[1] = (s.reserved[1] <= 50U) ? s.reserved[1] : 0U;
    s.reserved[2] = 0U;
    for (auto& step : s.steps) {
        step.outputVk = static_cast<uint16_t>(step.outputVk & 0xFFU);
        step.isMouse  = mask::input::InputDispatcher::IsMouseVirtualKey(step.outputVk);
        step.enabled  = RawBool(step.enabled);
    }
}

void SanitizeTrigger(ColorTriggerConfig& c, int index) noexcept {
    c.slotIndex = static_cast<uint8_t>(index);
    c.inUse     = RawBool(c.inUse);
    c.enabled   = c.inUse ? RawBool(c.enabled) : false;
    if (c.mode > static_cast<uint8_t>(ColorTriggerMode::ColorChanged)) {
        c.mode = static_cast<uint8_t>(ColorTriggerMode::MatchColor);
    }
    SetTriggerMatchRule(c, GetTriggerMatchRule(c));
    if (c.actionKind > static_cast<uint8_t>(ColorTriggerActionKind::RunComplexSeq)) {
        c.actionKind = static_cast<uint8_t>(ColorTriggerActionKind::ClickKey);
    }
    if (c.armMode > 2U) c.armMode = 0U;
    if (c.monitorIndex >= static_cast<uint8_t>(mask::triggers::kMaxMonitors)) c.monitorIndex = 0U;
    if (c.sampleRadius > 7U) c.sampleRadius = 7U;
    c.armVk    = static_cast<uint16_t>(c.armVk & 0xFFU);
    c.actionVk = static_cast<uint16_t>(c.actionVk & 0xFFU);
    c.reserved[1] = 0U;
    c.reserved[2] = 0U;
    if (c.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot) &&
        c.linkedIndex >= static_cast<uint8_t>(kSlotCount)) {
        c.linkedIndex = 0U;
    } else if (c.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunComplexSeq) &&
               c.linkedIndex >= static_cast<uint8_t>(kComplexSeqCount)) {
        c.linkedIndex = 0U;
    }
    c.name[std::size(c.name) - 1U] = L'\0';
    if (c.name[0] == L'\0') {
        std::swprintf(c.name, std::size(c.name), L"Color Trigger %d", index + 1);
    }
}

bool RunProfileMigrationSelfTest() noexcept {
    wchar_t tempDir[MAX_PATH]{};
    const DWORD tLen = GetTempPathW(MAX_PATH, tempDir);
    if (tLen == 0U || tLen >= MAX_PATH - 48U) {
        return false;
    }

    const unsigned long long stamp = static_cast<unsigned long long>(GetCurrentProcessId()) ^
                                     (GetTickCount64() << 16);
    wchar_t v1Path[MAX_PATH]{};
    wchar_t v2Path[MAX_PATH]{};
    wchar_t v3Path[MAX_PATH]{};
    wchar_t badPath[MAX_PATH]{};
    std::swprintf(v1Path,  std::size(v1Path),  L"%lsmask_prof_v1_%llu.bin",  tempDir, stamp);
    std::swprintf(v2Path,  std::size(v2Path),  L"%lsmask_prof_v2_%llu.bin",  tempDir, stamp);
    std::swprintf(v3Path,  std::size(v3Path),  L"%lsmask_prof_v3_%llu.bin",  tempDir, stamp);
    std::swprintf(badPath, std::size(badPath), L"%lsmask_prof_bad_%llu.bin", tempDir, stamp);

    auto writeLegacy = [](const wchar_t* path, uint32_t ver,
                          const std::array<MacroConfig, mask::core::kMaxMacroSlots>& slots) noexcept -> bool {
        auto buf = std::make_unique<uint8_t[]>(kHeaderBytes + kPayloadBytes);
        if (!buf) return false;
        std::memcpy(buf.get(), &kMagic, 4U);
        std::memcpy(buf.get() + 4U, &ver, 4U);
        std::memcpy(buf.get() + kHeaderBytes, slots.data(), kPayloadBytes);
        return WriteFileAtomic(path, buf.get(), kHeaderBytes + kPayloadBytes);
    };

    auto cleanup = [&]() noexcept {
        wchar_t bak[MAX_PATH + 16]{};
        for (const wchar_t* p : { v1Path, v2Path, v3Path, badPath }) {
            DeleteFileW(p);
            std::swprintf(bak, std::size(bak), L"%ls.bak", p);
            DeleteFileW(bak);
        }
    };

    auto seed = std::make_unique<std::array<MacroConfig, mask::core::kMaxMacroSlots>>();
    auto loaded = std::make_unique<std::array<MacroConfig, mask::core::kMaxMacroSlots>>();
    if (!seed || !loaded) {
        return false;
    }

    // Slot 0: Legacy Sequence (3) -> must migrate to HoldToLoop (0)
    (*seed)[0].slotIndex = 0U;
    (*seed)[0].triggerVk = VK_F7;
    (*seed)[0].outputVk  = '1';
    (*seed)[0].mode      = static_cast<uint8_t>(MacroMode::Sequence);
    (*seed)[0].enabled   = true;
    (*seed)[0].reserved[0] = 1U;
    wcscpy_s((*seed)[0].name, L"Legacy Seq");

    // Slot 1: Legacy SocdPair (4) -> must migrate to HoldToLoop (0)
    (*seed)[1].slotIndex = 1U;
    (*seed)[1].triggerVk = 'A';
    (*seed)[1].outputVk  = 'D';
    (*seed)[1].mode      = static_cast<uint8_t>(MacroMode::SocdPair);
    (*seed)[1].enabled   = true;
    (*seed)[1].reserved[0] = 1U;
    wcscpy_s((*seed)[1].name, L"Legacy SOCD");

    // Slot 2: Unrestricted 0 ms dwell & 0 ms recovery
    (*seed)[2].slotIndex = 2U;
    (*seed)[2].triggerVk = VK_XBUTTON1;
    (*seed)[2].outputVk  = VK_LBUTTON;
    (*seed)[2].mode      = static_cast<uint8_t>(MacroMode::HoldToLoop);
    (*seed)[2].dwellDurationNs    = 0ULL;
    (*seed)[2].recoveryDurationNs = 0ULL;
    (*seed)[2].reserved[0] = 1U;

    // Slot 3: Sub-0.05 ms (0.01 ms = 10,000 ns) & >1000 CPS (50,000 CPS) & Repeat > 9999
    (*seed)[3].slotIndex = 3U;
    (*seed)[3].triggerVk = VK_F6;
    (*seed)[3].outputVk  = VK_LBUTTON;
    (*seed)[3].mode      = static_cast<uint8_t>(MacroMode::RepeatNTimes);
    (*seed)[3].dwellDurationNs    = 10'000ULL;
    (*seed)[3].recoveryDurationNs = 10'000ULL;
    (*seed)[3].repeatCount        = 25000U;
    (*seed)[3].reserved[0]        = 1U;

    bool pass = true;
    uint32_t readVer = 0U;

    // 1. Test v1 -> v3 migration
    if (!writeLegacy(v1Path, kVersionV1, *seed) || !ReadProfileFile(v1Path, *loaded, &readVer) || readVer != kVersionV1) {
        pass = false;
    } else {
        for (int i = 0; i < kSlotCount; ++i) Sanitize((*loaded)[static_cast<size_t>(i)], i);
        pass &= ((*loaded)[0].mode == static_cast<uint8_t>(MacroMode::HoldToLoop));
        pass &= ((*loaded)[1].mode == static_cast<uint8_t>(MacroMode::HoldToLoop));
        pass &= ((*loaded)[2].dwellDurationNs == 0ULL && (*loaded)[2].recoveryDurationNs == 0ULL);
        pass &= ((*loaded)[3].dwellDurationNs == 10'000ULL && (*loaded)[3].recoveryDurationNs == 10'000ULL);
        pass &= ((*loaded)[3].repeatCount == 25000U);
    }

    // 2. Test v2 -> v3 migration
    (*loaded) = {};
    readVer = 0U;
    if (!writeLegacy(v2Path, kVersionV2, *seed) || !ReadProfileFile(v2Path, *loaded, &readVer) || readVer != kVersionV2) {
        pass = false;
    } else {
        for (int i = 0; i < kSlotCount; ++i) Sanitize((*loaded)[static_cast<size_t>(i)], i);
        pass &= ((*loaded)[0].mode == static_cast<uint8_t>(MacroMode::HoldToLoop));
        pass &= ((*loaded)[1].mode == static_cast<uint8_t>(MacroMode::HoldToLoop));
        pass &= ((*loaded)[2].dwellDurationNs == 0ULL && (*loaded)[2].recoveryDurationNs == 0ULL);
        pass &= ((*loaded)[3].dwellDurationNs == 10'000ULL && (*loaded)[3].recoveryDurationNs == 10'000ULL);
    }

    // 3. Test v3 write + read round-trip
    if (!writeLegacy(v3Path, kVersion, *loaded)) {
        pass = false;
    } else {
        auto v3Round = std::make_unique<std::array<MacroConfig, mask::core::kMaxMacroSlots>>();
        readVer = 0U;
        if (!v3Round || !ReadProfileFile(v3Path, *v3Round, &readVer) || readVer != kVersion) {
            pass = false;
        } else {
            pass &= ((*v3Round)[0].mode == 0U && (*v3Round)[1].mode == 0U);
            pass &= ((*v3Round)[2].dwellDurationNs == 0ULL && (*v3Round)[2].recoveryDurationNs == 0ULL);
            pass &= ((*v3Round)[3].dwellDurationNs == 10'000ULL && (*v3Round)[3].recoveryDurationNs == 10'000ULL);
            pass &= ((*v3Round)[3].repeatCount == 25000U);
        }
    }

    // 4. Reject corrupt magic / unsupported future version without mutating output
    {
        uint32_t badPayload[2]{ 0xDEADBEEFU, kVersion };
        if (!WriteFileAtomic(badPath, badPayload, sizeof(badPayload))) {
            pass = false;
        } else {
            const uint16_t sentinelVk = (*loaded)[0].triggerVk;
            if (ReadProfileFile(badPath, *loaded, &readVer)) {
                pass = false;
            }
            pass &= ((*loaded)[0].triggerVk == sentinelVk);
        }
    }

    cleanup();
    return pass;
}

} // namespace storage

// =====================================================================================
//  AppModel implementation
// =====================================================================================

AppModel::AppModel(mask::core::EngineSharedState& sharedState) noexcept
    : sharedState_(sharedState) {}

void AppModel::RefreshDevicesAndMonitors(const mask::remap::RemapEngine* remap) noexcept {
    physicalDeviceCount = 0;
    physicalDevices = {};

    // 1. Immediate Raw Input device list from RemapEngine (available right at startup)
    if (remap != nullptr) {
        std::array<mask::input::PhysicalDeviceInfo, mask::input::kMaxPhysicalGameInputDevices> rawDevs{};
        const int rawCnt = std::clamp<int>(remap->GetRawDevices(rawDevs), 0, static_cast<int>(physicalDevices.size()));
        for (int i = 0; i < rawCnt; ++i) {
            physicalDevices[static_cast<size_t>(physicalDeviceCount++)] = rawDevs[static_cast<size_t>(i)];
        }
    }

    // 2. Merge any active GameInput physical devices not already present
    std::array<mask::input::PhysicalDeviceInfo, mask::input::kMaxPhysicalGameInputDevices> giDevs{};
    const int giCnt = std::clamp<int>(mask::input::CopyPhysicalDeviceInventory(giDevs), 0, static_cast<int>(giDevs.size()));
    for (int g = 0; g < giCnt && physicalDeviceCount < static_cast<int>(physicalDevices.size()); ++g) {
        const auto& d = giDevs[static_cast<size_t>(g)];
        bool duplicate = false;
        for (int e = 0; e < physicalDeviceCount; ++e) {
            const auto& existing = physicalDevices[static_cast<size_t>(e)];
            if (std::memcmp(existing.deviceIdBytes, d.deviceIdBytes, sizeof(existing.deviceIdBytes)) == 0) {
                duplicate = true;
                break;
            }
            const bool sameKind = (existing.isKeyboard && d.isKeyboard) || (existing.isMouse && d.isMouse);
            if (sameKind && d.vendorId != 0U && existing.vendorId == d.vendorId && existing.productId == d.productId) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            physicalDevices[static_cast<size_t>(physicalDeviceCount++)] = d;
        }
    }

    monitorCount = 0;
    if (remap != nullptr) {
        monitorCount = remap->GetMonitors(monitors);
    }
    if (monitorCount <= 0) {
        std::array<mask::triggers::MonitorInfoEntry, mask::triggers::kMaxMonitors> tmp{};
        const int cnt = mask::triggers::PixelTriggerEngine::EnumerateMonitors(tmp);
        monitorCount = std::clamp(cnt, 0, 8);
        for (int i = 0; i < monitorCount; ++i) {
            monitors[static_cast<size_t>(i)].left      = tmp[static_cast<size_t>(i)].left;
            monitors[static_cast<size_t>(i)].top       = tmp[static_cast<size_t>(i)].top;
            monitors[static_cast<size_t>(i)].right     = tmp[static_cast<size_t>(i)].left + tmp[static_cast<size_t>(i)].width;
            monitors[static_cast<size_t>(i)].bottom    = tmp[static_cast<size_t>(i)].top + tmp[static_cast<size_t>(i)].height;
            monitors[static_cast<size_t>(i)].dpi       = tmp[static_cast<size_t>(i)].dpi;
            monitors[static_cast<size_t>(i)].isPrimary = tmp[static_cast<size_t>(i)].isPrimary;
            wcsncpy_s(monitors[static_cast<size_t>(i)].name, tmp[static_cast<size_t>(i)].name, _TRUNCATE);
        }
    }
}

bool AppModel::IsAutoclickerInUse(int i) const noexcept {
    if (i < 0 || i >= kSlotCount) {
        return false;
    }
    const auto& s = slots[static_cast<size_t>(i)];
    return s.reserved[0] != 0U || s.triggerVk != 0U || s.enabled;
}

bool AppModel::IsSeqInUse(int i) const noexcept {
    return i >= 0 && i < kComplexSeqCount && sequences[static_cast<size_t>(i)].inUse;
}

bool AppModel::IsTriggerInUse(int i) const noexcept {
    return i >= 0 && i < kColorTriggerCount && triggers[static_cast<size_t>(i)].inUse;
}

bool AppModel::IsRemapInUse(int i) const noexcept {
    return i >= 0 && i < kRemapRuleCount && remaps[static_cast<size_t>(i)].inUse;
}

bool AppModel::IsItemInUse(ItemKind kind, int idx) const noexcept {
    switch (kind) {
        case ItemKind::Autoclicker: return IsAutoclickerInUse(idx);
        case ItemKind::Sequence:    return IsSeqInUse(idx);
        case ItemKind::Trigger:     return IsTriggerInUse(idx);
        case ItemKind::Remap:       return IsRemapInUse(idx);
        default:                    return false;
    }
}

bool AppModel::HasSelection() const noexcept {
    return IsItemInUse(selectedKind, selectedIndex);
}

MacroConfig& AppModel::SelectedAutoclicker() noexcept {
    const int idx = (selectedKind == ItemKind::Autoclicker && selectedIndex >= 0 && selectedIndex < kSlotCount)
        ? selectedIndex : std::clamp(selectedSlot, 0, kSlotCount - 1);
    return slots[static_cast<size_t>(idx)];
}

const MacroConfig& AppModel::SelectedAutoclicker() const noexcept {
    const int idx = (selectedKind == ItemKind::Autoclicker && selectedIndex >= 0 && selectedIndex < kSlotCount)
        ? selectedIndex : std::clamp(selectedSlot, 0, kSlotCount - 1);
    return slots[static_cast<size_t>(idx)];
}

ComplexSequenceConfig& AppModel::SelectedSeq() noexcept {
    return sequences[static_cast<size_t>(std::clamp(selectedIndex, 0, kComplexSeqCount - 1))];
}

const ComplexSequenceConfig& AppModel::SelectedSeq() const noexcept {
    return sequences[static_cast<size_t>(std::clamp(selectedIndex, 0, kComplexSeqCount - 1))];
}

ColorTriggerConfig& AppModel::SelectedTrigger() noexcept {
    return triggers[static_cast<size_t>(std::clamp(selectedIndex, 0, kColorTriggerCount - 1))];
}

const ColorTriggerConfig& AppModel::SelectedTrigger() const noexcept {
    return triggers[static_cast<size_t>(std::clamp(selectedIndex, 0, kColorTriggerCount - 1))];
}

RemapRuleConfig& AppModel::SelectedRemap() noexcept {
    return remaps[static_cast<size_t>(std::clamp(selectedIndex, 0, kRemapRuleCount - 1))];
}

const RemapRuleConfig& AppModel::SelectedRemap() const noexcept {
    return remaps[static_cast<size_t>(std::clamp(selectedIndex, 0, kRemapRuleCount - 1))];
}

bool AppModel::AnyRemapEnabled() const noexcept {
    for (const auto& r : remaps) {
        if (r.inUse && r.enabled) {
            return true;
        }
    }
    return false;
}

int AppModel::FindFirstFreeSlot() const noexcept {
    for (int i = 0; i < kSlotCount; ++i) {
        if (!IsAutoclickerInUse(i)) return i;
    }
    return -1;
}

int AppModel::FindFirstFreeSeq() const noexcept {
    for (int i = 0; i < kComplexSeqCount; ++i) {
        if (!sequences[static_cast<size_t>(i)].inUse) return i;
    }
    return -1;
}

int AppModel::FindFirstFreeTrigger() const noexcept {
    for (int i = 0; i < kColorTriggerCount; ++i) {
        if (!triggers[static_cast<size_t>(i)].inUse) return i;
    }
    return -1;
}

int AppModel::FindFirstFreeRemap() const noexcept {
    for (int i = 0; i < kRemapRuleCount; ++i) {
        if (!remaps[static_cast<size_t>(i)].inUse) return i;
    }
    return -1;
}

int AppModel::BuildSidebarList(std::array<SidebarEntry, 128>& out) const noexcept {
    int count = 0;
    if (sidebarFilter == 0 || sidebarFilter == 1) {
        for (int i = 0; i < kSlotCount; ++i) {
            if (IsAutoclickerInUse(i)) out[static_cast<size_t>(count++)] = SidebarEntry{ ItemKind::Autoclicker, i };
        }
    }
    if (sidebarFilter == 0 || sidebarFilter == 2) {
        for (int i = 0; i < kComplexSeqCount; ++i) {
            if (IsSeqInUse(i)) out[static_cast<size_t>(count++)] = SidebarEntry{ ItemKind::Sequence, i };
        }
    }
    if (sidebarFilter == 0 || sidebarFilter == 3) {
        for (int i = 0; i < kColorTriggerCount; ++i) {
            if (IsTriggerInUse(i)) out[static_cast<size_t>(count++)] = SidebarEntry{ ItemKind::Trigger, i };
        }
    }
    if (sidebarFilter == 0 || sidebarFilter == 4) {
        for (int i = 0; i < kRemapRuleCount; ++i) {
            if (IsRemapInUse(i)) out[static_cast<size_t>(count++)] = SidebarEntry{ ItemKind::Remap, i };
        }
    }
    return count;
}

int AppModel::ListedCount() const noexcept {
    std::array<SidebarEntry, 128> tmp{};
    return BuildSidebarList(tmp);
}

void AppModel::EnsureValidSelection() noexcept {
    if (HasSelection()) {
        if (selectedKind == ItemKind::Autoclicker) selectedSlot = selectedIndex;
        return;
    }
    std::array<SidebarEntry, 128> list{};
    const int n = BuildSidebarList(list);
    if (n > 0) {
        selectedKind  = list[0].kind;
        selectedIndex = list[0].index;
        if (selectedKind == ItemKind::Autoclicker) selectedSlot = selectedIndex;
        return;
    }
    for (int i = 0; i < kSlotCount; ++i) {
        if (IsAutoclickerInUse(i)) { selectedKind = ItemKind::Autoclicker; selectedIndex = i; selectedSlot = i; return; }
    }
    for (int i = 0; i < kComplexSeqCount; ++i) {
        if (IsSeqInUse(i)) { selectedKind = ItemKind::Sequence; selectedIndex = i; return; }
    }
    for (int i = 0; i < kColorTriggerCount; ++i) {
        if (IsTriggerInUse(i)) { selectedKind = ItemKind::Trigger; selectedIndex = i; return; }
    }
    for (int i = 0; i < kRemapRuleCount; ++i) {
        if (IsRemapInUse(i)) { selectedKind = ItemKind::Remap; selectedIndex = i; return; }
    }
    selectedKind  = ItemKind::Autoclicker;
    selectedIndex = 0;
    selectedSlot  = 0;
}

void AppModel::ResetSlot(int i) noexcept {
    if (i < 0 || i >= kSlotCount) {
        return;
    }
    auto& s = slots[static_cast<size_t>(i)];
    s = MacroConfig{};
    s.slotIndex          = static_cast<uint8_t>(i);
    s.enabled            = false;
    s.triggerVk          = 0;
    s.outputVk           = VK_LBUTTON;
    s.isMouse            = true;
    s.mode               = static_cast<uint8_t>(MacroMode::HoldToLoop);
    s.dwellDurationNs    = 5'000'000ULL;
    s.recoveryDurationNs = 5'000'000ULL;
    s.repeatCount        = 10;
    s.stepCount          = 1;
    std::swprintf(s.name, std::size(s.name), L"Autoclicker %d", i + 1);
}

void AppModel::ResetSequence(int i) noexcept {
    if (i < 0 || i >= kComplexSeqCount) {
        return;
    }
    auto& q = sequences[static_cast<size_t>(i)];
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

void AppModel::ResetTrigger(int i) noexcept {
    if (i < 0 || i >= kColorTriggerCount) {
        return;
    }
    auto& t = triggers[static_cast<size_t>(i)];
    t = ColorTriggerConfig{};
    t.slotIndex    = static_cast<uint8_t>(i);
    t.enabled      = false;
    t.inUse        = false;
    t.mode         = static_cast<uint8_t>(ColorTriggerMode::MatchColor);
    SetTriggerMatchRule(t, ColorTriggerMatchRule::AnyPixel);
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

int AppModel::CreateNewAutoclicker(MacroMode mode, uint32_t repeatCount) noexcept {
    const int target = FindFirstFreeSlot();
    if (target < 0) return -1;
    ResetSlot(target);
    auto& slot = slots[static_cast<size_t>(target)];
    slot.reserved[0] = 1U;
    slot.mode = NormalizeAutoclickerMode(static_cast<uint8_t>(mode));
    slot.repeatCount = (std::max<uint32_t>)(repeatCount, 1U);
    wcscpy_s(slot.name, (slot.mode == static_cast<uint8_t>(MacroMode::RepeatNTimes))
        ? L"Burst clicker"
        : L"New autoclicker");
    return target;
}

int AppModel::CreateNewSequence() noexcept {
    const int target = FindFirstFreeSeq();
    if (target < 0) return -1;
    ResetSequence(target);
    auto& seq = sequences[static_cast<size_t>(target)];
    seq.inUse = true;
    seq.stepCount = 2;
    seq.steps[0].kind = static_cast<uint8_t>(ComplexStepKind::KeyClick);
    seq.steps[0].vk = '1';
    seq.steps[0].durationMs = 25;
    seq.steps[1].kind = static_cast<uint8_t>(ComplexStepKind::WaitMs);
    seq.steps[1].durationMs = 100;
    selectedSeqStep = 0;
    return target;
}

int AppModel::CreateNewTrigger() noexcept {
    const int target = FindFirstFreeTrigger();
    if (target < 0) return -1;
    ResetTrigger(target);
    triggers[static_cast<size_t>(target)].inUse = true;
    return target;
}

int AppModel::CreateNewRemap() noexcept {
    const int target = FindFirstFreeRemap();
    if (target < 0) return -1;
    ResetRemap(target);
    remaps[static_cast<size_t>(target)].inUse = true;
    return target;
}

void AppModel::ResetRemap(int i) noexcept {
    if (i < 0 || i >= kRemapRuleCount) {
        return;
    }
    auto& r = remaps[static_cast<size_t>(i)];
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

void AppModel::InitializeDefaults() noexcept {
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

    auto& burst = slots[2];
    burst.triggerVk   = VK_F7;
    burst.outputVk    = VK_LBUTTON;
    burst.mode        = static_cast<uint8_t>(MacroMode::RepeatNTimes);
    burst.repeatCount = 10U;
    SetPeriodAndDuty(burst, 10.0, 0.5);
    wcscpy_s(burst.name, L"Burst click");

    for (int i = 0; i < 3; ++i) {
        slots[static_cast<size_t>(i)].reserved[0] = 1U;
    }
    selectedKind  = ItemKind::Autoclicker;
    selectedIndex = 0;
    selectedSlot  = 0;
}

void AppModel::PublishWatchedKeys() noexcept {
    uint64_t bits[4]{0ULL, 0ULL, 0ULL, 0ULL};
    auto watch = [&bits](unsigned vk) noexcept {
        vk &= 0xFFU;
        if (vk != 0U) {
            bits[vk >> 6] |= (1ULL << (vk & 63U));
        }
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
        if (!s.enabled || s.triggerVk == 0U) continue;
        anyEnabled = true;
        watch(s.triggerVk);
    }
    for (const auto& q : sequences) {
        if (!q.inUse || !q.enabled) continue;
        if (q.triggerVk != 0U) {
            anyEnabled = true;
            watch(q.triggerVk);
        }
        const int cnt = std::clamp<int>(q.stepCount, 0, kComplexStepMax);
        for (int si = 0; si < cnt; ++si) {
            const auto& st = q.steps[static_cast<size_t>(si)];
            if (st.kind == static_cast<uint8_t>(ComplexStepKind::IfCheck) &&
                mask::core::GetStepConditionKind(st) == ComplexConditionKind::KeyHeld &&
                st.vk != 0U) {
                anyEnabled = true;
                watch(st.vk);
            }
        }
    }
    for (const auto& t : triggers) {
        if (!t.inUse || !t.enabled) continue;
        anyEnabled = true;
        if (t.armVk != 0U) watch(t.armVk);
    }
    for (const auto& r : remaps) {
        if (!r.inUse || !r.enabled || r.sourceVk == 0U) continue;
        anyEnabled = true;
        watch(r.sourceVk);
    }

    for (size_t i = 0; i < 4U; ++i) {
        sharedState_.watchedVkBits[i].store(bits[i], std::memory_order_release);
    }
    sharedState_.anyMacroEnabled.store(anyEnabled, std::memory_order_release);
}

void AppModel::MarkDirty() noexcept {
    saveState = SaveState::Pending;
    lastChangeTick = GetTickCount64();
}

void AppModel::SyncSlotToEngine(int slotIndex, bool markChanged) noexcept {
    if (slotIndex < 0 || slotIndex >= kSlotCount) {
        return;
    }
    auto& s = slots[static_cast<size_t>(slotIndex)];
    s.slotIndex = static_cast<uint8_t>(slotIndex);
    s.mode      = NormalizeAutoclickerMode(s.mode);
    s.name[std::size(s.name) - 1] = L'\0';
    s.isMouse   = mask::input::InputDispatcher::IsMouseVirtualKey(s.outputVk);
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

void AppModel::FlushPendingSync() noexcept {
    for (int i = 0; i < kSlotCount && pendingSyncMask != 0ULL; ++i) {
        const uint64_t bit = (1ULL << i);
        if ((pendingSyncMask & bit) == 0ULL) continue;
        if (!sharedState_.uiCommandQueue.try_push(slots[static_cast<size_t>(i)])) {
            break;
        }
        pendingSyncMask &= ~bit;
    }
    mask::input::WakeEngine();
}

void AppModel::SyncAllSlotsToEngine() noexcept {
    pendingSyncMask = ~0ULL;
    FlushPendingSync();
    PublishWatchedKeys();
}

void AppModel::ApplySettings() noexcept {
    mask::settings::SanitizeSettings(settings);
    wcsncpy_s(mouseProfileId, settings.mouseProfileId, _TRUNCATE);
    wcsncpy_s(keyboardProfileId, settings.keyboardProfileId, _TRUNCATE);
    mask::settings::ApplySettingsToSharedState(settings, sharedState_);
    PublishWatchedKeys();
}

void AppModel::SetToast(const wchar_t* message, UiAction action, const wchar_t* actionLabel) noexcept {
    wcsncpy_s(toast, message ? message : L"", _TRUNCATE);
    wcsncpy_s(toastActionLabel, actionLabel ? actionLabel : L"", _TRUNCATE);
    toastAction = action;
    toastTick = GetTickCount64();
}

void AppModel::ClearToast() noexcept {
    toast[0] = L'\0';
    toastAction = UiAction{};
}

void AppModel::ResolveProfilePaths() noexcept {
    wchar_t exePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len > 0U && len < MAX_PATH) {
        if (wchar_t* slash = std::wcsrchr(exePath, L'\\')) {
            *(slash + 1) = L'\0';
        }
        wcscpy_s(dataDir, exePath);
    } else {
        wcscpy_s(dataDir, L".\\");
    }

    wchar_t localAppData[MAX_PATH]{};
    const DWORD envLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (envLen > 0U && envLen < MAX_PATH - 24U) {
        std::swprintf(fallbackDir, std::size(fallbackDir), L"%ls\\Mask\\", localAppData);
    }
    UpdateActiveBankPaths();
}

void AppModel::BuildBankFilePath(wchar_t* out, size_t cap, const wchar_t* dir, const wchar_t* stem, uint8_t bank) const noexcept {
    if (bank == 0U) {
        std::swprintf(out, cap, L"%ls%ls.bin", dir, stem);
    } else {
        std::swprintf(out, cap, L"%ls%ls_%u.bin", dir, stem, static_cast<unsigned>(bank + 1U));
    }
}

void AppModel::UpdateActiveBankPaths() noexcept {
    const uint8_t bank = (settings.activeProfileBank < mask::settings::kProfileBankCount)
        ? settings.activeProfileBank : 0U;
    BuildBankFilePath(profilePath, std::size(profilePath), dataDir, L"profiles", bank);
    if (fallbackDir[0] != L'\0') {
        BuildBankFilePath(fallbackPath, std::size(fallbackPath), fallbackDir, L"profiles", bank);
    }
}

bool AppModel::SaveSettingsOnly() noexcept {
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

bool AppModel::LoadSettingsFromDisk() noexcept {
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

bool AppModel::SaveToDisk() noexcept {
    UpdateActiveBankPaths();
    const uint8_t bank = (settings.activeProfileBank < mask::settings::kProfileBankCount)
        ? settings.activeProfileBank : 0U;

    auto buffer = std::make_unique<uint8_t[]>(storage::kHeaderBytes + storage::kPayloadBytes);
    if (!buffer) {
        saveState = SaveState::Failed;
        return false;
    }
    std::memcpy(buffer.get(), &storage::kMagic, 4U);
    std::memcpy(buffer.get() + 4U, &storage::kVersion, 4U);
    std::memcpy(buffer.get() + storage::kHeaderBytes, slots.data(), storage::kPayloadBytes);

    bool ok = storage::WriteFileAtomic(profilePath, buffer.get(), storage::kHeaderBytes + storage::kPayloadBytes);
    if (!ok && fallbackDir[0] != L'\0' && std::wcscmp(dataDir, fallbackDir) != 0) {
        wcscpy_s(dataDir, fallbackDir);
        UpdateActiveBankPaths();
        ok = storage::WriteFileAtomic(profilePath, buffer.get(), storage::kHeaderBytes + storage::kPayloadBytes);
    }

    wchar_t seqPath[MAX_PATH]{};
    wchar_t trigPath[MAX_PATH]{};
    wchar_t remapPath[MAX_PATH]{};
    BuildBankFilePath(seqPath,   std::size(seqPath),   dataDir, L"sequences", bank);
    BuildBankFilePath(trigPath,  std::size(trigPath),  dataDir, L"triggers",  bank);
    BuildBankFilePath(remapPath, std::size(remapPath), dataDir, L"remaps",    bank);

    ok = mask::workflows::ComplexSequenceEngine::SaveSequences(seqPath, sequences) && ok;
    ok = mask::triggers::PixelTriggerEngine::SaveTriggers(trigPath, triggers) && ok;
    ok = mask::remap::RemapEngine::SaveRemaps(remapPath, remaps) && ok;
    ok = SaveSettingsOnly() && ok;

    saveState = ok ? SaveState::Saved : SaveState::Failed;
    return ok;
}

int AppModel::LoadFromDisk() noexcept {
    UpdateActiveBankPaths();
    const uint8_t bank = (settings.activeProfileBank < mask::settings::kProfileBankCount)
        ? settings.activeProfileBank : 0U;

    auto loaded = std::make_unique<std::array<MacroConfig, mask::core::kMaxMacroSlots>>();
    int result = 0;
    if (loaded) {
        wchar_t candidate[MAX_PATH + 16]{};
        const wchar_t* bases[2] = { profilePath, fallbackPath };
        for (int b = 0; b < 2 && result == 0; ++b) {
            if (bases[b][0] == L'\0') continue;
            if (storage::ReadProfileFile(bases[b], *loaded)) {
                result = 1;
            } else {
                std::swprintf(candidate, std::size(candidate), L"%ls.bak", bases[b]);
                if (storage::ReadProfileFile(candidate, *loaded)) result = 2;
            }
            if (result != 0 && b == 1 && fallbackDir[0] != L'\0') {
                wcscpy_s(dataDir, fallbackDir);
                UpdateActiveBankPaths();
            }
        }
    }

    if (result != 0 && loaded) {
        for (int i = 0; i < kSlotCount; ++i) {
            storage::Sanitize((*loaded)[static_cast<size_t>(i)], i);
        }
        slots = *loaded;
    } else if (bank != 0U) {
        for (int i = 0; i < kSlotCount; ++i) {
            ResetSlot(i);
        }
    }

    for (int i = 0; i < kComplexSeqCount; ++i) ResetSequence(i);
    wchar_t path[MAX_PATH]{};
    BuildBankFilePath(path, std::size(path), dataDir, L"sequences", bank);
    if (!mask::workflows::ComplexSequenceEngine::LoadSequences(path, sequences) && fallbackDir[0] != L'\0') {
        BuildBankFilePath(path, std::size(path), fallbackDir, L"sequences", bank);
        (void)mask::workflows::ComplexSequenceEngine::LoadSequences(path, sequences);
    }

    for (int i = 0; i < kColorTriggerCount; ++i) ResetTrigger(i);
    BuildBankFilePath(path, std::size(path), dataDir, L"triggers", bank);
    if (!mask::triggers::PixelTriggerEngine::LoadTriggers(path, triggers) && fallbackDir[0] != L'\0') {
        BuildBankFilePath(path, std::size(path), fallbackDir, L"triggers", bank);
        (void)mask::triggers::PixelTriggerEngine::LoadTriggers(path, triggers);
    }

    for (int i = 0; i < kRemapRuleCount; ++i) ResetRemap(i);
    BuildBankFilePath(path, std::size(path), dataDir, L"remaps", bank);
    if (!mask::remap::RemapEngine::LoadRemaps(path, remaps) && fallbackDir[0] != L'\0') {
        BuildBankFilePath(path, std::size(path), fallbackDir, L"remaps", bank);
        (void)mask::remap::RemapEngine::LoadRemaps(path, remaps);
    }

    sharedState_.flushAllRequested.store(true, std::memory_order_release);
    SyncAllSlotsToEngine();
    EnsureValidSelection();
    saveState = SaveState::Saved;
    return result;
}

bool AppModel::ExportBankToJson(const wchar_t* path) const noexcept {
    return mask::profile_json::ExportProfileBankToJsonFile(path, slots, sequences, triggers, remaps);
}

bool AppModel::ImportBankFromJson(const wchar_t* path, bool& outHasImportedCommands) noexcept {
    auto impSlots = std::make_unique<std::array<MacroConfig, mask::core::kMaxMacroSlots>>();
    auto impSeqs  = std::make_unique<std::array<ComplexSequenceConfig, mask::core::kMaxComplexSequences>>();
    auto impTrigs = std::make_unique<std::array<ColorTriggerConfig, mask::core::kMaxColorTriggers>>();
    auto impMaps  = std::make_unique<std::array<RemapRuleConfig, mask::core::kMaxRemapRules>>();
    if (!impSlots || !impSeqs || !impTrigs || !impMaps) {
        return false;
    }

    bool hasImportedCommands = false;
    if (!mask::profile_json::ImportProfileBankFromJsonFile(
            path, *impSlots, *impSeqs, *impTrigs, *impMaps, hasImportedCommands)) {
        return false;
    }

    for (int i = 0; i < kSlotCount; ++i) {
        storage::Sanitize((*impSlots)[static_cast<size_t>(i)], i);
    }
    for (int i = 0; i < kColorTriggerCount; ++i) {
        storage::SanitizeTrigger((*impTrigs)[static_cast<size_t>(i)], i);
    }

    slots     = *impSlots;
    sequences = *impSeqs;
    triggers  = *impTrigs;
    remaps    = *impMaps;
    outHasImportedCommands = hasImportedCommands;

    sharedState_.flushAllRequested.store(true, std::memory_order_release);
    SyncAllSlotsToEngine();
    EnsureValidSelection();
    MarkDirty();
    return true;
}

} // namespace mask::app
