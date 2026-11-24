#include "app.hpp"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>

namespace mask::app {

// =====================================================================================
//  Editing, Binding, Sliders & Selection Helpers
// =====================================================================================

void App::SetEditing(Field f) noexcept {
    model_.editField = f;
    UpdateTriggerSuppression();
}

void App::CancelEdit() noexcept {
    if (model_.editField != Field::None) {
        SetEditing(Field::None);
    }
    model_.editBuffer[0] = L'\0';
}

void App::CommitEdit() noexcept {
    const Field f = model_.editField;
    if (f == Field::None) return;

    auto trimStr = [](wchar_t* buf) noexcept -> wchar_t* {
        wchar_t* start = buf;
        while (*start == L' ') ++start;
        size_t len = std::wcslen(start);
        while (len > 0U && start[len - 1U] == L' ') start[--len] = L'\0';
        return start;
    };

    if (f == Field::Name && model_.selectedKind == ItemKind::Autoclicker) {
        wchar_t* s = trimStr(model_.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(model_.SelectedAutoclicker().name, s, _TRUNCATE);
        CancelEdit();
        model_.SyncSlotToEngine(model_.selectedIndex);
        return;
    }
    if (f == Field::SeqName && model_.selectedKind == ItemKind::Sequence) {
        wchar_t* s = trimStr(model_.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(model_.SelectedSeq().name, s, _TRUNCATE);
        CancelEdit();
        SyncSelectedSequence();
        return;
    }
    if (f == Field::RemapName && model_.selectedKind == ItemKind::Remap) {
        wchar_t* s = trimStr(model_.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(model_.SelectedRemap().name, s, _TRUNCATE);
        CancelEdit();
        SyncSelectedRemap();
        return;
    }
    if (f == Field::TriggerName && model_.selectedKind == ItemKind::Trigger) {
        wchar_t* s = trimStr(model_.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(model_.SelectedTrigger().name, s, _TRUNCATE);
        CancelEdit();
        SyncSelectedTrigger();
        return;
    }
    if (f == Field::SeqStepText && model_.selectedKind == ItemKind::Sequence) {
        auto& seq = model_.SelectedSeq();
        if (model_.selectedSeqStep >= 0 && model_.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            wcsncpy_s(seq.steps[static_cast<size_t>(model_.selectedSeqStep)].text, model_.editBuffer, _TRUNCATE);
        }
        CancelEdit();
        SyncSelectedSequence();
        return;
    }

    wchar_t* end = nullptr;
    const double v = std::wcstod(model_.editBuffer, &end);
    const bool validNum = (end != model_.editBuffer && std::isfinite(v));

    if (validNum && f == Field::SettingsAutosaveMs) {
        model_.settings.autosaveDelayMs = static_cast<uint32_t>(
            std::clamp<long long>(std::llround(v), 200LL, 30'000LL));
        model_.ApplySettings();
        model_.MarkDirty();
    } else if (validNum && IsAutoclickerField(f) && model_.selectedKind == ItemKind::Autoclicker) {
        FieldSet(model_.SelectedAutoclicker(), f, v);
        model_.SyncSlotToEngine(model_.selectedIndex);
    } else if (validNum && IsTriggerField(f) && model_.selectedKind == ItemKind::Trigger) {
        auto& t = model_.SelectedTrigger();
        const long long iv = std::llround(v);
        constexpr long long kMaxU32 = static_cast<long long>((std::numeric_limits<uint32_t>::max)());
        switch (f) {
            case Field::TriggerDelay:    t.delayMs    = static_cast<uint32_t>(std::clamp<long long>(iv, 0LL, kMaxU32)); break;
            case Field::TriggerHold:     t.holdMs     = static_cast<uint32_t>(std::clamp<long long>(iv, 0LL, kMaxU32)); break;
            case Field::TriggerCooldown: t.cooldownMs = static_cast<uint32_t>(std::clamp<long long>(iv, 0LL, kMaxU32)); break;
            case Field::TriggerLinked: {
                const bool isMacro = (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot));
                const long long maxSlot = isMacro ? kSlotCount : kComplexSeqCount;
                t.linkedIndex = static_cast<uint8_t>(std::clamp<long long>(iv - 1LL, 0LL, maxSlot - 1LL));
                break;
            }
            default: break;
        }
        SyncSelectedTrigger();
    } else if (validNum && IsSeqStepField(f) && model_.selectedKind == ItemKind::Sequence) {
        auto& seq = model_.SelectedSeq();
        if (model_.selectedSeqStep >= 0 && model_.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            auto& st = seq.steps[static_cast<size_t>(model_.selectedSeqStep)];
            const long long iv = std::llround(v);
            constexpr long long kMaxU32 = static_cast<long long>((std::numeric_limits<uint32_t>::max)());
            switch (f) {
                case Field::SeqStepDuration:
                    st.durationMs = static_cast<uint32_t>(std::clamp<long long>(iv, 0LL, kMaxU32));
                    break;
                case Field::SeqStepX: st.x = static_cast<int32_t>(std::clamp<long long>(iv, -16384LL, 16384LL)); break;
                case Field::SeqStepY: st.y = static_cast<int32_t>(std::clamp<long long>(iv, -16384LL, 16384LL)); break;
                case Field::SeqStepParam: {
                    const auto kind = static_cast<ComplexStepKind>(st.kind);
                    if (kind == ComplexStepKind::MouseScroll) {
                        st.param = static_cast<uint32_t>(static_cast<int32_t>(std::clamp<long long>(iv, -120LL, 120LL)));
                    } else if (kind == ComplexStepKind::ToggleMacro ||
                               (kind == ComplexStepKind::IfCheck &&
                                mask::core::GetStepConditionKind(st) == ComplexConditionKind::MacroActive)) {
                        st.param = static_cast<uint32_t>(std::clamp<long long>(iv - 1LL, 0LL, kSlotCount - 1LL));
                    } else if (kind == ComplexStepKind::IfCheck &&
                               mask::core::GetStepConditionKind(st) == ComplexConditionKind::RandomChance) {
                        st.param = static_cast<uint32_t>(std::clamp<long long>(iv, 1LL, 99LL));
                    } else {
                        st.param = static_cast<uint32_t>(std::clamp<long long>(iv, 0LL, kMaxU32));
                    }
                    break;
                }
                case Field::SeqStepR:   st.r = static_cast<uint8_t>(std::clamp<long long>(iv, 0LL, 255LL)); break;
                case Field::SeqStepG:   st.g = static_cast<uint8_t>(std::clamp<long long>(iv, 0LL, 255LL)); break;
                case Field::SeqStepB:   st.b = static_cast<uint8_t>(std::clamp<long long>(iv, 0LL, 255LL)); break;
                case Field::SeqStepTol: st.tolerance = static_cast<uint8_t>(std::clamp<long long>(iv, 0LL, 255LL)); break;
                default: break;
            }
            SyncSelectedSequence();
        }
    }
    CancelEdit();
}

void App::BeginEdit(Field f) noexcept {
    if (model_.editField == f) return;
    CommitEdit();
    model_.editBuffer[0] = L'\0';
    if (f == Field::Name && model_.selectedKind == ItemKind::Autoclicker) {
        wcsncpy_s(model_.editBuffer, model_.SelectedAutoclicker().name, _TRUNCATE);
    } else if (f == Field::SeqName && model_.selectedKind == ItemKind::Sequence) {
        wcsncpy_s(model_.editBuffer, model_.SelectedSeq().name, _TRUNCATE);
    } else if (f == Field::RemapName && model_.selectedKind == ItemKind::Remap) {
        wcsncpy_s(model_.editBuffer, model_.SelectedRemap().name, _TRUNCATE);
    } else if (f == Field::TriggerName && model_.selectedKind == ItemKind::Trigger) {
        wcsncpy_s(model_.editBuffer, model_.SelectedTrigger().name, _TRUNCATE);
    } else if (f == Field::SettingsAutosaveMs) {
        std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u",
                      static_cast<unsigned>(model_.settings.autosaveDelayMs));
    } else if (IsAutoclickerField(f) && model_.selectedKind == ItemKind::Autoclicker) {
        FormatFieldNumber(model_.SelectedAutoclicker(), f, model_.editBuffer, std::size(model_.editBuffer));
    } else if (IsTriggerField(f) && model_.selectedKind == ItemKind::Trigger) {
        const auto& t = model_.SelectedTrigger();
        switch (f) {
            case Field::TriggerDelay:    std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(t.delayMs)); break;
            case Field::TriggerHold:     std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(t.holdMs)); break;
            case Field::TriggerCooldown: std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(t.cooldownMs)); break;
            case Field::TriggerLinked: {
                const bool isMacro = (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot));
                const uint32_t maxSlots = isMacro ? static_cast<uint32_t>(kSlotCount) : static_cast<uint32_t>(kComplexSeqCount);
                std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u",
                              static_cast<unsigned>((t.linkedIndex % maxSlots) + 1U));
                break;
            }
            default: break;
        }
    } else if (IsSeqStepField(f) && model_.selectedKind == ItemKind::Sequence) {
        const auto& seq = model_.SelectedSeq();
        if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
        const auto& st = seq.steps[static_cast<size_t>(model_.selectedSeqStep)];
        switch (f) {
            case Field::SeqStepText:     wcsncpy_s(model_.editBuffer, st.text, _TRUNCATE); break;
            case Field::SeqStepDuration: std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(st.durationMs)); break;
            case Field::SeqStepX:        std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%d", static_cast<int>(st.x)); break;
            case Field::SeqStepY:        std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%d", static_cast<int>(st.y)); break;
            case Field::SeqStepParam: {
                const auto kind = static_cast<ComplexStepKind>(st.kind);
                if (kind == ComplexStepKind::MouseScroll) {
                    std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%d", static_cast<int>(static_cast<int32_t>(st.param)));
                } else if (kind == ComplexStepKind::ToggleMacro ||
                           (kind == ComplexStepKind::IfCheck &&
                            mask::core::GetStepConditionKind(st) == ComplexConditionKind::MacroActive)) {
                    std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u",
                                  static_cast<unsigned>((st.param % kSlotCount) + 1U));
                } else {
                    std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(st.param));
                }
                break;
            }
            case Field::SeqStepR:   std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(st.r)); break;
            case Field::SeqStepG:   std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(st.g)); break;
            case Field::SeqStepB:   std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(st.b)); break;
            case Field::SeqStepTol: std::swprintf(model_.editBuffer, std::size(model_.editBuffer), L"%u", static_cast<unsigned>(st.tolerance)); break;
            default: break;
        }
    }
    SetEditing(f);
}

void App::StepField(Field f, int direction, bool coarse, bool fine) noexcept {
    if (f == Field::SettingsAutosaveMs) {
        const int delta = (coarse ? 500 : 100) * direction;
        model_.settings.autosaveDelayMs = static_cast<uint32_t>(
            std::clamp<int>(static_cast<int>(model_.settings.autosaveDelayMs) + delta, 200, 30'000));
        model_.ApplySettings();
        model_.MarkDirty();
        return;
    }
    if (IsAutoclickerField(f) && model_.selectedKind == ItemKind::Autoclicker) {
        FieldStep(model_.SelectedAutoclicker(), f, direction, coarse, fine);
        model_.SyncSlotToEngine(model_.selectedIndex);
        return;
    }
    constexpr long long kMaxU32 = static_cast<long long>((std::numeric_limits<uint32_t>::max)());
    if (IsTriggerField(f) && model_.selectedKind == ItemKind::Trigger) {
        auto& t = model_.SelectedTrigger();
        switch (f) {
            case Field::TriggerDelay:
                t.delayMs = static_cast<uint32_t>(std::clamp<long long>(
                    static_cast<long long>(t.delayMs) + static_cast<long long>(direction) * (coarse ? 50 : (fine ? 1 : 5)), 0LL, kMaxU32));
                break;
            case Field::TriggerHold:
                t.holdMs = static_cast<uint32_t>(std::clamp<long long>(
                    static_cast<long long>(t.holdMs) + static_cast<long long>(direction) * (coarse ? 50 : (fine ? 1 : 5)), 0LL, kMaxU32));
                break;
            case Field::TriggerCooldown:
                t.cooldownMs = static_cast<uint32_t>(std::clamp<long long>(
                    static_cast<long long>(t.cooldownMs) + static_cast<long long>(direction) * (coarse ? 100 : (fine ? 1 : 10)), 0LL, kMaxU32));
                break;
            case Field::TriggerLinked: {
                const bool isMacro = (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot));
                const int maxSlots = isMacro ? kSlotCount : kComplexSeqCount;
                const int cur = static_cast<int>(t.linkedIndex % static_cast<uint32_t>(maxSlots));
                t.linkedIndex = static_cast<uint8_t>((cur + direction + maxSlots) % maxSlots);
                break;
            }
            default: break;
        }
        SyncSelectedTrigger();
        return;
    }
    if (IsSeqStepField(f) && model_.selectedKind == ItemKind::Sequence) {
        auto& seq = model_.SelectedSeq();
        if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
        auto& st = seq.steps[static_cast<size_t>(model_.selectedSeqStep)];
        const long long step = coarse ? 50LL : (fine ? 1LL : 10LL);
        switch (f) {
            case Field::SeqStepDuration:
                st.durationMs = static_cast<uint32_t>(std::clamp<long long>(
                    static_cast<long long>(st.durationMs) + static_cast<long long>(direction) * step, 0LL, kMaxU32));
                break;
            case Field::SeqStepX:
                st.x = std::clamp<int32_t>(st.x + direction * (coarse ? 50 : 5), -16384, 16384);
                break;
            case Field::SeqStepY:
                st.y = std::clamp<int32_t>(st.y + direction * (coarse ? 50 : 5), -16384, 16384);
                break;
            case Field::SeqStepParam: {
                const auto kind = static_cast<ComplexStepKind>(st.kind);
                if (kind == ComplexStepKind::MouseScroll) {
                    const int32_t cur = static_cast<int32_t>(st.param);
                    st.param = static_cast<uint32_t>(std::clamp<int32_t>(cur + direction * (coarse ? 5 : 1), -120, 120));
                } else if (kind == ComplexStepKind::ToggleMacro ||
                           (kind == ComplexStepKind::IfCheck &&
                            mask::core::GetStepConditionKind(st) == ComplexConditionKind::MacroActive)) {
                    const int cur = static_cast<int>(st.param % static_cast<uint32_t>(kSlotCount));
                    st.param = static_cast<uint32_t>((cur + direction + kSlotCount) % kSlotCount);
                } else if (kind == ComplexStepKind::IfCheck &&
                           mask::core::GetStepConditionKind(st) == ComplexConditionKind::RandomChance) {
                    st.param = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(st.param) + direction * (coarse ? 10 : 1), 1, 99));
                } else if (kind == ComplexStepKind::WaitForColor) {
                    st.param = static_cast<uint32_t>(std::clamp<long long>(
                        static_cast<long long>(st.param) + static_cast<long long>(direction) * (coarse ? 1000LL : 250LL), 0LL, kMaxU32));
                } else {
                    st.param = static_cast<uint32_t>(std::clamp<long long>(
                        static_cast<long long>(st.param) + static_cast<long long>(direction) * (coarse ? 10LL : 1LL), 0LL, kMaxU32));
                }
                break;
            }
            case Field::SeqStepR:   st.r = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.r) + direction * (coarse ? 15 : 1), 0, 255)); break;
            case Field::SeqStepG:   st.g = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.g) + direction * (coarse ? 15 : 1), 0, 255)); break;
            case Field::SeqStepB:   st.b = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.b) + direction * (coarse ? 15 : 1), 0, 255)); break;
            case Field::SeqStepTol: st.tolerance = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.tolerance) + direction * (coarse ? 10 : 2), 0, 255)); break;
            default: break;
        }
        SyncSelectedSequence();
    }
}

void App::ApplyDutyFromPointer(int xPx) noexcept {
    if (model_.selectedKind != ItemKind::Autoclicker) return;
    auto& slot = model_.SelectedAutoclicker();
    const double wanted = renderer_.DutyFromSliderXPx(xPx);
    if (std::fabs(DutyPercent(slot) - wanted) >= 0.01) {
        SetDuty(slot, wanted);
        model_.SyncSlotToEngine(model_.selectedIndex);
    }
}

void App::ApplyTriggerSliderFromPointer(int xPx) noexcept {
    if (model_.selectedKind != ItemKind::Trigger || !model_.HasSelection()) return;
    const int nv = renderer_.TriggerToleranceFromXPx(xPx);
    auto& t = model_.SelectedTrigger();
    if (t.tolerance != static_cast<uint8_t>(nv)) {
        t.tolerance = static_cast<uint8_t>(nv);
        SyncSelectedTrigger();
    }
}

void App::CancelBinding() noexcept {
    sharedState_.captureBindTarget.store(0U, std::memory_order_release);
    mask::input::WakeEngine();
}

void App::StartBinding(HWND hwnd, uint8_t target, const wchar_t* message) noexcept {
    sharedState_.flushAllRequested.store(true, std::memory_order_release);
    sharedState_.captureSerial.fetch_add(1U, std::memory_order_acq_rel);
    sharedState_.captureBindTarget.store(target, std::memory_order_release);
    mask::input::WakeEngine();
    model_.captureStartTick = GetTickCount64();
    ShowToast(hwnd, message);
}

void App::BeginBindGrace(HWND hwnd, uint16_t vk) noexcept {
    bindGraceVk_    = vk;
    bindGraceUntil_ = GetTickCount64() + 2000ULL;
    UpdateTriggerSuppression();
    SetTimer(hwnd, kBindGraceTimerId, 25U, nullptr);
}

void App::OnBindGraceTimer(HWND hwnd) noexcept {
    const bool stillHeld = bindGraceVk_ != 0U &&
                           (GetAsyncKeyState(static_cast<int>(bindGraceVk_)) & 0x8000) != 0;
    if (!stillHeld || GetTickCount64() >= bindGraceUntil_) {
        KillTimer(hwnd, kBindGraceTimerId);
        bindGraceVk_ = 0U;
        UpdateTriggerSuppression();
    }
}

UiAction App::BindActionFor(uint8_t target) noexcept {
    switch (target) {
        case 1U:  return UiAction{ ActionKind::BindTrigger };
        case 2U:  return UiAction{ ActionKind::BindOutput };
        case 20U: return UiAction{ ActionKind::SeqBindTrigger };
        case 21U: return UiAction{ ActionKind::SeqStepBindKey };
        case 22U: return UiAction{ ActionKind::RemapBindSource };
        case 23U: return UiAction{ ActionKind::RemapBindTarget };
        case 24U: return UiAction{ ActionKind::SetBindPauseVk };
        case 25U: return UiAction{ ActionKind::SetBindStopAllVk };
        case 26U: return UiAction{ ActionKind::TrigBindArmVk };
        case 27U: return UiAction{ ActionKind::TrigBindActionVk };
        default:  return UiAction{};
    }
}

void App::AssignCapturedKey(HWND hwnd, uint16_t vk) noexcept {
    const uint8_t target = sharedState_.captureBindTarget.load(std::memory_order_acquire);
    if (target == 0U) {
        CancelBinding();
        return;
    }

    if (target == 24U || target == 25U) {
        BeginBindGrace(hwnd, vk);
        CancelBinding();
        wchar_t msg[120]{};
        if (target == 24U) {
            model_.settings.pauseHotkeyVk = vk;
            std::swprintf(msg, std::size(msg), L"Global pause hotkey set to %ls.", NameOf(vk).text);
        } else {
            model_.settings.stopAllHotkeyVk = vk;
            std::swprintf(msg, std::size(msg), L"Global Stop All hotkey set to %ls.", NameOf(vk).text);
        }
        model_.ApplySettings();
        model_.MarkDirty();
        ShowToast(hwnd, msg);
        suppressKeyNavUntil_ = GetTickCount64() + 300ULL;
        return;
    }

    if (!model_.HasSelection()) {
        CancelBinding();
        return;
    }

    if ((target == 26U || target == 27U) && model_.selectedKind == ItemKind::Trigger) {
        if (target == 26U) BeginBindGrace(hwnd, vk);
        CancelBinding();
        auto& t = model_.SelectedTrigger();
        wchar_t msg[120]{};
        if (target == 26U) {
            t.armVk = vk;
            if (t.armMode == 0U) t.armMode = 1U;
            std::swprintf(msg, std::size(msg), L"Arm key set to %ls.", NameOf(vk).text);
        } else {
            t.actionVk = vk;
            std::swprintf(msg, std::size(msg), L"Trigger output set to %ls.", NameOf(vk).text);
        }
        SyncSelectedTrigger();
        ShowToast(hwnd, msg);
        suppressKeyNavUntil_ = GetTickCount64() + 300ULL;
        return;
    }

    if ((target == 20U || target == 21U) && model_.selectedKind == ItemKind::Sequence) {
        if (target == 20U) BeginBindGrace(hwnd, vk);
        CancelBinding();
        auto& seq = model_.SelectedSeq();
        wchar_t msg[120]{};
        if (target == 20U) {
            const bool firstTrig = (seq.triggerVk == 0U && !seq.enabled);
            seq.triggerVk = vk;
            if (firstTrig && seq.stepCount > 0U) {
                seq.enabled = true;
                animator_.SetToggle(AnimSlotFor(ItemKind::Sequence, model_.selectedIndex), true);
            }
            std::swprintf(msg, std::size(msg), L"Sequence trigger set to %ls.", NameOf(vk).text);
        } else if (model_.selectedSeqStep >= 0 && model_.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            seq.steps[static_cast<size_t>(model_.selectedSeqStep)].vk = vk;
            std::swprintf(msg, std::size(msg), L"Step %d key set to %ls.",
                          model_.selectedSeqStep + 1, NameOf(vk).text);
        }
        SyncSelectedSequence();
        ShowToast(hwnd, msg);
        suppressKeyNavUntil_ = GetTickCount64() + 300ULL;
        return;
    }

    if ((target == 22U || target == 23U) && model_.selectedKind == ItemKind::Remap) {
        if (target == 22U) BeginBindGrace(hwnd, vk);
        CancelBinding();
        auto& r = model_.SelectedRemap();
        wchar_t msg[120]{};
        if (target == 22U) {
            r.sourceVk = vk;
            std::swprintf(msg, std::size(msg), L"Remap source key set to %ls.", NameOf(vk).text);
        } else {
            r.targetVk = vk;
            std::swprintf(msg, std::size(msg), L"Remap target key set to %ls.", NameOf(vk).text);
        }
        SyncSelectedRemap();
        ShowToast(hwnd, msg);
        suppressKeyNavUntil_ = GetTickCount64() + 300ULL;
        return;
    }

    if (model_.selectedKind != ItemKind::Autoclicker) {
        CancelBinding();
        return;
    }
    if (target == 1U) {
        BeginBindGrace(hwnd, vk);
    }
    CancelBinding();
    auto& slot = model_.SelectedAutoclicker();
    wchar_t msg[120]{};
    if (target == 1U) {
        const bool firstTrigger = (slot.triggerVk == 0U && !slot.enabled);
        slot.triggerVk = vk;
        if (firstTrigger) {
            slot.enabled = true;
            animator_.SetToggle(AnimSlotFor(ItemKind::Autoclicker, model_.selectedIndex), true);
            std::swprintf(msg, std::size(msg), L"%ls now starts \u201C%ls\u201D, and it's switched on.",
                          NameOf(vk).text, slot.name);
        } else {
            std::swprintf(msg, std::size(msg), L"Trigger set to %ls.", NameOf(vk).text);
        }
    } else if (target == 2U) {
        slot.outputVk = vk;
        std::swprintf(msg, std::size(msg), L"Output set to %ls.", NameOf(vk).text);
    }
    ShowToast(hwnd, msg);
    model_.SyncSlotToEngine(model_.selectedIndex);
    suppressKeyNavUntil_ = GetTickCount64() + 300ULL;
}

void App::OnKeyCaptured(HWND hwnd, uint16_t vk, bool fromMouse) noexcept {
    const uint8_t target = sharedState_.captureBindTarget.load(std::memory_order_acquire);
    if (target == 0U) return;

    if (vk == VK_ESCAPE) {
        CancelBinding();
        ShowToast(hwnd, L"Cancelled. Nothing was changed.");
        suppressKeyNavUntil_ = GetTickCount64() + 300ULL;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (fromMouse && vk == VK_LBUTTON) {
        if (GetTickCount64() - model_.captureStartTick < 150ULL) return;
        POINT pt{};
        GetCursorPos(&pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        ScreenToClient(hwnd, &pt);
        if (GetForegroundWindow() == hwnd && PtInRect(&rc, pt)) return;
    }

    AssignCapturedKey(hwnd, vk);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void App::EnsureSelectedSeqStepVisible() noexcept {
    if (model_.selectedKind != ItemKind::Sequence || !model_.HasSelection()) return;
    const auto& seq = model_.SelectedSeq();
    const bool snapScroll = !windowFocused_ ||
        model_.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off);
    if (seq.stepCount == 0U || model_.selectedSeqStep < 0) {
        model_.seqStepScroll = 0.0f;
        animator_.SetSeqScrollTarget(0.0f, snapScroll);
        return;
    }
    const float pitch = UiRenderer::kSeqStepRowH + 4.0f;
    const float top = static_cast<float>(model_.selectedSeqStep) * pitch;
    const float view = renderer_.SeqStepViewHeight();
    float scroll = animator_.seqScrollSmooth.target;
    if (top < scroll) scroll = top;
    if (top + pitch + 8.0f > scroll + view) scroll = top + pitch + 8.0f - view;
    const float maxScroll = (std::max)(0.0f, static_cast<float>(seq.stepCount) * pitch + 8.0f - view);
    scroll = std::clamp(scroll, 0.0f, maxScroll);
    animator_.SetSeqScrollTarget(scroll, snapScroll);
    model_.seqStepScroll = animator_.seqScrollSmooth.value;
}

void App::SelectItem(HWND hwnd, ItemKind kind, int index) noexcept {
    const bool wasSettings = model_.showSettings;
    const bool changed = wasSettings || model_.selectedKind != kind || model_.selectedIndex != index;
    model_.showSettings     = false;
    model_.showStepKindMenu = false;
    settingsHwTabOpen_.store(false, std::memory_order_release);
    if (wasSettings) {
        animator_.SetSettingsOpen(false);
        animator_.SetSettingsTab(0.0f, 0.0f, true);
        settingsTabSnapped_ = false;
        sidebarIndSnapped_  = false;
    }
    model_.pendingDeleteConfirm = false;
    model_.pendingResetConfirm  = false;
    model_.selectedKind  = kind;
    model_.selectedIndex = index;
    if (kind == ItemKind::Autoclicker) model_.selectedSlot = index;
    if (kind == ItemKind::Sequence) {
        const int maxStep = (std::max)(0, static_cast<int>(model_.sequences[static_cast<size_t>(index)].stepCount) - 1);
        model_.selectedSeqStep = std::clamp(model_.selectedSeqStep, 0, maxStep);
        animator_.SetSeqScrollTarget(0.0f, true);
        model_.seqStepScroll = 0.0f;
    }
    {
        std::array<SidebarEntry, 128> entries{};
        const int n = model_.BuildSidebarList(entries);
        for (int pos = 0; pos < n; ++pos) {
            const auto& entry = entries[static_cast<size_t>(pos)];
            if (entry.kind == kind && entry.index == index) {
                const float pitch = renderer_.CurrentListRowHeight() + UiRenderer::kListRowGap;
                const float top = static_cast<float>(pos) * pitch;
                const float view = renderer_.ListViewHeight();
                float scroll = animator_.listScrollSmooth.target;
                if (top < scroll) scroll = top;
                if (top + pitch + 12.0f > scroll + view) scroll = top + pitch + 12.0f - view;
                scroll = (std::max)(0.0f, scroll);
                const bool snapScroll = !windowFocused_ ||
                    model_.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off);
                animator_.SetListScrollTarget(scroll, snapScroll);
                model_.listScroll = animator_.listScrollSmooth.value;
                break;
            }
        }
    }
    if (changed) {
        animator_.TriggerViewTransition();
        EnsureAnimTimer(hwnd);
    }
    UpdateTriggerPreviewState(hwnd);
}

void App::SelectNeighbour(HWND hwnd, int direction) noexcept {
    std::array<SidebarEntry, 128> entries{};
    const int n = model_.BuildSidebarList(entries);
    if (n <= 0) return;
    int cur = -1;
    for (int i = 0; i < n; ++i) {
        const auto& e = entries[static_cast<size_t>(i)];
        if (e.kind == model_.selectedKind && e.index == model_.selectedIndex) {
            cur = i;
            break;
        }
    }
    const int next = std::clamp(cur + direction, 0, n - 1);
    if (next != cur) {
        const auto& target = entries[static_cast<size_t>(next)];
        SelectItem(hwnd, target.kind, target.index);
    }
}

void App::ToggleItemEnabled(HWND hwnd, ItemKind kind, int idx) noexcept {
    switch (kind) {
        case ItemKind::Autoclicker: {
            if (idx < 0 || idx >= kSlotCount) return;
            auto& s = model_.slots[static_cast<size_t>(idx)];
            if (!s.enabled && s.triggerVk == 0U) {
                SelectItem(hwnd, ItemKind::Autoclicker, idx);
                StartBinding(hwnd, 1U, L"Press the key or mouse button that should start this autoclicker.");
            } else {
                s.enabled = !s.enabled;
                animator_.SetToggle(AnimSlotFor(ItemKind::Autoclicker, idx), s.enabled);
                model_.SyncSlotToEngine(idx);
                EnsureAnimTimer(hwnd);
            }
            break;
        }
        case ItemKind::Sequence: {
            if (idx < 0 || idx >= kComplexSeqCount) return;
            auto& seq = model_.sequences[static_cast<size_t>(idx)];
            seq.enabled = !seq.enabled;
            animator_.SetToggle(AnimSlotFor(ItemKind::Sequence, idx), seq.enabled);
            seqEngine_.UpdateConfig(static_cast<size_t>(idx), seq);
            model_.PublishWatchedKeys();
            model_.MarkDirty();
            EnsureAnimTimer(hwnd);
            break;
        }
        case ItemKind::Trigger: {
            if (idx < 0 || idx >= kColorTriggerCount) return;
            auto& t = model_.triggers[static_cast<size_t>(idx)];
            t.enabled = !t.enabled;
            animator_.SetToggle(AnimSlotFor(ItemKind::Trigger, idx), t.enabled);
            pixelEngine_.UpdateConfig(static_cast<size_t>(idx), t);
            model_.PublishWatchedKeys();
            model_.MarkDirty();
            EnsureAnimTimer(hwnd);
            break;
        }
        case ItemKind::Remap: {
            if (idx < 0 || idx >= kRemapRuleCount) return;
            auto& r = model_.remaps[static_cast<size_t>(idx)];
            if (!r.enabled && r.sourceVk == 0U) {
                SelectItem(hwnd, ItemKind::Remap, idx);
                StartBinding(hwnd, 22U, L"Choose a source key before enabling this remap rule.");
            } else {
                r.enabled = !r.enabled;
                animator_.SetToggle(AnimSlotFor(ItemKind::Remap, idx), r.enabled);
                remapEngine_.UpdateRule(static_cast<size_t>(idx), r);
                model_.PublishWatchedKeys();
                model_.MarkDirty();
                EnsureAnimTimer(hwnd);
            }
            break;
        }
        default:
            break;
    }
}

void App::FinalizeItemDelete(HWND hwnd, int deleteId) noexcept {
    const ItemKind kind = DecodeDeleteKind(deleteId);
    const int idx = DecodeDeleteIndex(deleteId);
    wchar_t msg[120]{};

    model_.hasUndo  = true;
    model_.undoKind = kind;
    model_.undoSlot = idx;

    switch (kind) {
        case ItemKind::Autoclicker:
            if (idx >= 0 && idx < kSlotCount) {
                model_.undoConfig = model_.slots[static_cast<size_t>(idx)];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", model_.undoConfig.name);
                model_.ResetSlot(idx);
                model_.SyncSlotToEngine(idx);
            }
            break;
        case ItemKind::Sequence:
            if (idx >= 0 && idx < kComplexSeqCount) {
                model_.undoSeq = model_.sequences[static_cast<size_t>(idx)];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", model_.undoSeq.name);
                model_.ResetSequence(idx);
                seqEngine_.UpdateConfig(static_cast<size_t>(idx), model_.sequences[static_cast<size_t>(idx)]);
                model_.PublishWatchedKeys();
                model_.MarkDirty();
            }
            break;
        case ItemKind::Trigger:
            if (idx >= 0 && idx < kColorTriggerCount) {
                model_.undoTrigger = model_.triggers[static_cast<size_t>(idx)];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", model_.undoTrigger.name);
                model_.ResetTrigger(idx);
                pixelEngine_.UpdateConfig(static_cast<size_t>(idx), model_.triggers[static_cast<size_t>(idx)]);
                model_.PublishWatchedKeys();
                model_.MarkDirty();
            }
            break;
        case ItemKind::Remap:
            if (idx >= 0 && idx < kRemapRuleCount) {
                model_.undoRemap = model_.remaps[static_cast<size_t>(idx)];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", model_.undoRemap.name);
                model_.ResetRemap(idx);
                remapEngine_.UpdateRule(static_cast<size_t>(idx), model_.remaps[static_cast<size_t>(idx)]);
                model_.PublishWatchedKeys();
                model_.MarkDirty();
            }
            break;
    }

    model_.EnsureValidSelection();
    model_.showStepKindMenu = false;
    UpdateTriggerPreviewState(hwnd);
    SyncAnimatorToggles(false);
    ShowToast(hwnd, msg, UiAction{ ActionKind::ToastAction }, L"Undo");
    InvalidateRect(hwnd, nullptr, FALSE);
}

void App::OpenSettingsAtTab(HWND hwnd, int tab) noexcept {
    model_.showNewPicker    = false;
    model_.showStepKindMenu = false;
    animator_.SetNewPickerOpen(false);
    const bool wasOpen = model_.showSettings;
    model_.showSettings = true;
    model_.settingsTab  = std::clamp(tab, 0, 4);
    animator_.SetSettingsOpen(true);
    settingsHwTabOpen_.store(model_.settingsTab == 4, std::memory_order_release);
    if (model_.settingsTab == 4) {
        model_.RefreshDevicesAndMonitors(&remapEngine_);
        mask::input::WakeEngine();
    }
    if (!wasOpen) {
        settingsTabSnapped_ = false;
        animator_.SetSettingsTab(0.0f, 0.0f, true);
        animator_.TriggerViewTransition();
    } else {
        float tx = 0.0f, tw = 80.0f;
        if (renderer_.GetSettingsTabRect(model_.settingsTab, tx, tw)) {
            animator_.SetSettingsTab(tx, tw, !settingsTabSnapped_ || !windowFocused_);
            settingsTabSnapped_ = true;
        }
    }
    EnsureAnimTimer(hwnd);
    UpdateTriggerPreviewState(hwnd);
}

// =====================================================================================
//  Single O(1) Command Dispatch Table
// =====================================================================================

const std::array<App::CommandFn, static_cast<size_t>(ActionKind::Count)>& App::DispatchTable() noexcept {
    static const auto kTable = []() noexcept {
        std::array<CommandFn, static_cast<size_t>(ActionKind::Count)> t{};
        t.fill(&App::CmdNoOp);

        auto bind = [&](ActionKind k, CommandFn fn) noexcept {
            t[static_cast<size_t>(k)] = fn;
        };

        bind(ActionKind::TogglePause,         &App::CmdTogglePause);
        bind(ActionKind::StopAll,             &App::CmdStopAll);
        bind(ActionKind::OpenNewPicker,       &App::CmdOpenNewPicker);
        bind(ActionKind::CloseNewPicker,      &App::CmdCloseNewPicker);
        bind(ActionKind::CreateAutoclicker,   &App::CmdCreateAutoclicker);
        bind(ActionKind::CreateSequence,      &App::CmdCreateSequence);
        bind(ActionKind::CreateTrigger,       &App::CmdCreateTrigger);
        bind(ActionKind::CreateRemap,         &App::CmdCreateRemap);
        bind(ActionKind::ToggleSettings,      &App::CmdToggleSettings);
        bind(ActionKind::OutputAction,        &App::CmdOutputAction);
        bind(ActionKind::ToastAction,         &App::CmdToastAction);
        bind(ActionKind::DuplicateItem,       &App::CmdDuplicateItem);
        bind(ActionKind::DeleteItem,          &App::CmdDeleteItem);
        bind(ActionKind::ToggleEnabled,       &App::CmdToggleEnabled);
        bind(ActionKind::BindTrigger,         &App::CmdBindTrigger);
        bind(ActionKind::BindOutput,          &App::CmdBindOutput);
        bind(ActionKind::OpenHardwareTab,     &App::CmdOpenHardwareTab);
        bind(ActionKind::CycleTopLayer,       &App::CmdCycleTopLayer);
        bind(ActionKind::ToggleTopLock,       &App::CmdToggleTopLock);

        bind(ActionKind::SeqStepAdd,          &App::CmdSeqStepAdd);
        bind(ActionKind::SeqStepInsert,       &App::CmdSeqStepInsert);
        bind(ActionKind::SeqStepDup,          &App::CmdSeqStepDup);
        bind(ActionKind::SeqStepUp,           &App::CmdSeqStepUp);
        bind(ActionKind::SeqStepDown,         &App::CmdSeqStepDown);
        bind(ActionKind::SeqStepDel,          &App::CmdSeqStepDel);
        bind(ActionKind::SeqBindTrigger,      &App::CmdSeqBindTrigger);
        bind(ActionKind::SeqStepBindKey,      &App::CmdSeqStepBindKey);
        bind(ActionKind::SeqStepPickColor,    &App::CmdSeqStepPickColor);

        bind(ActionKind::RemapBindSource,     &App::CmdRemapBindSource);
        bind(ActionKind::RemapBindTarget,     &App::CmdRemapBindTarget);
        bind(ActionKind::RemapCycleDevice,    &App::CmdRemapCycleDevice);
        bind(ActionKind::RemapRefreshDevices, &App::CmdSetRefreshHw);
        bind(ActionKind::RemapToggleSuppress, &App::CmdRemapToggleSuppress);
        bind(ActionKind::RemapToggleUnlockLive, &App::CmdToggleTopLock);

        bind(ActionKind::SetBindPauseVk,      &App::CmdSetBindPauseVk);
        bind(ActionKind::SetClearPauseVk,     &App::CmdSetClearPauseVk);
        bind(ActionKind::SetResetPauseVk,     &App::CmdSetResetPauseVk);
        bind(ActionKind::SetBindStopAllVk,    &App::CmdSetBindStopAllVk);
        bind(ActionKind::SetClearStopAllVk,   &App::CmdSetClearStopAllVk);
        bind(ActionKind::SetOpenDataFolder,   &App::CmdSetOpenDataFolder);
        bind(ActionKind::SetResetAll,         &App::CmdSetResetAll);
        bind(ActionKind::SetRefreshHw,        &App::CmdSetRefreshHw);
        bind(ActionKind::ExportProfileJson,   &App::CmdExportProfileJson);
        bind(ActionKind::ImportProfileJson,   &App::CmdImportProfileJson);

        bind(ActionKind::NewPickerBackdrop,     &App::CmdCloseNewPicker);
        bind(ActionKind::SeqToggleStepKindMenu, &App::CmdSeqToggleStepKindMenu);
        bind(ActionKind::SeqCycleHumanize,      &App::CmdSeqCycleHumanize);

        bind(ActionKind::TrigPickPixel,       &App::CmdTrigPickPixel);
        bind(ActionKind::TrigLockCenter,      &App::CmdTrigLockCenter);
        bind(ActionKind::TrigBindArmVk,       &App::CmdTrigBindArmVk);
        bind(ActionKind::TrigClearArmVk,      &App::CmdTrigClearArmVk);
        bind(ActionKind::TrigBindActionVk,    &App::CmdTrigBindActionVk);

        bind(ActionKind::SidebarFilter,       &App::CmdSidebarFilter);
        bind(ActionKind::SettingsTab,         &App::CmdSettingsTab);

        bind(ActionKind::SetToggleStartWin,     &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleStartMin,     &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleMinTray,      &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleTopmost,      &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleBounds,       &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleConfirmDel,   &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleCompact,      &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleSignal,       &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleLiveKeys,     &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleLowRes,       &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleTrimMem,      &App::CmdSetToggleFlag);
        bind(ActionKind::SetTogglePanic,        &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleSessionLock,  &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleSuppressType, &App::CmdSetToggleFlag);
        bind(ActionKind::SetToggleSound,        &App::CmdSetToggleFlag);

        bind(ActionKind::SetAnimQuality,      &App::CmdSetAnimQuality);
        bind(ActionKind::SetAccent,           &App::CmdSetAccent);
        bind(ActionKind::SetPowerMode,        &App::CmdSetPowerMode);
        bind(ActionKind::SetPollHz,           &App::CmdSetPollHz);
        bind(ActionKind::SetSpinPolicy,       &App::CmdSetSpinPolicy);
        bind(ActionKind::SetProfileBank,      &App::CmdSetProfileBank);

        bind(ActionKind::SeqTriggerMode,      &App::CmdSeqTriggerMode);
        bind(ActionKind::SeqStepKindBase,     &App::CmdSeqStepKindBase);
        bind(ActionKind::SeqStepLayer,        &App::CmdSeqStepLayer);
        bind(ActionKind::SeqStepQuickWait,    &App::CmdSeqStepQuickWait);
        bind(ActionKind::SeqStepQuickScroll,  &App::CmdSeqStepQuickScroll);

        bind(ActionKind::RemapLiveLayer,      &App::CmdRemapLiveLayer);
        bind(ActionKind::RemapRuleLayer,      &App::CmdRemapRuleLayer);
        bind(ActionKind::RemapTargetLayer,    &App::CmdRemapTargetLayer);
        bind(ActionKind::RemapActionKind,     &App::CmdRemapActionKind);
        bind(ActionKind::RemapUnlockPolicy,   &App::CmdRemapUnlockPolicy);
        bind(ActionKind::RemapDevKind,        &App::CmdRemapDevKind);
        bind(ActionKind::RemapMonitor,        &App::CmdRemapMonitor);

        bind(ActionKind::TrigMode,            &App::CmdTrigMode);
        bind(ActionKind::TrigMatchRule,       &App::CmdTrigMatchRule);
        bind(ActionKind::TrigRadius,          &App::CmdTrigRadius);
        bind(ActionKind::TrigMonitor,         &App::CmdTrigMonitor);
        bind(ActionKind::TrigArmMode,         &App::CmdTrigArmMode);
        bind(ActionKind::TrigActionKind,      &App::CmdTrigActionKind);
        bind(ActionKind::TrigQuickOut,        &App::CmdTrigQuickOut);
        bind(ActionKind::TrigColorPreset,     &App::CmdTrigColorPreset);

        bind(ActionKind::SelectSeqStep,       &App::CmdSelectSeqStep);
        bind(ActionKind::SeqStepCondKind,     &App::CmdSeqStepCondKind);
        bind(ActionKind::SeqStepCondNegate,   &App::CmdSeqStepCondNegate);
        bind(ActionKind::SeqStepCondUnlock,   &App::CmdSeqStepCondUnlock);
        bind(ActionKind::SeqStepCondAddElse,  &App::CmdSeqStepCondAddElse);
        bind(ActionKind::SeqStepCondQuickPct, &App::CmdSeqStepCondQuickPct);

        bind(ActionKind::Mode,                &App::CmdMode);
        bind(ActionKind::HumanizeBase,        &App::CmdHumanizeBase);
        bind(ActionKind::QuickOut,            &App::CmdQuickOut);
        bind(ActionKind::SelectMouseProfile,  &App::CmdSelectMouseProfile);
        bind(ActionKind::SelectKbProfile,     &App::CmdSelectKbProfile);

        bind(ActionKind::FieldFocus,          &App::CmdFieldFocus);
        bind(ActionKind::FieldDec,            &App::CmdFieldDec);
        bind(ActionKind::FieldInc,            &App::CmdFieldInc);
        bind(ActionKind::SelectItemBase,      &App::CmdSelectItemBase);
        bind(ActionKind::ToggleItemBase,      &App::CmdToggleItemBase);

        return t;
    }();
    return kTable;
}

void App::DispatchAction(HWND hwnd, UiAction act, bool shift, bool ctrl) noexcept {
    if (act.IsNone() || act.kind == ActionKind::ModalSurface) return;

    if (act.kind != ActionKind::SeqToggleStepKindMenu) {
        model_.showStepKindMenu = false;
    }
    if (act.kind == ActionKind::PopoverBackdrop) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (model_.editField != Field::None &&
        !(act.kind == ActionKind::FieldFocus && act.index == static_cast<int16_t>(model_.editField))) {
        CommitEdit();
    }
    if (sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U) {
        CancelBinding();
    }
    if (act.kind != ActionKind::DeleteItem) model_.pendingDeleteConfirm = false;
    if (act.kind != ActionKind::SetResetAll) model_.pendingResetConfirm = false;

    const size_t rawKind = static_cast<size_t>(act.kind);
    if (rawKind < static_cast<size_t>(ActionKind::Count)) {
        const CommandFn fn = DispatchTable()[rawKind];
        (this->*fn)(hwnd, act, shift, ctrl);
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

// =====================================================================================
//  Command Handlers
// =====================================================================================

void App::CmdNoOp(HWND, UiAction, bool, bool) noexcept {}

void App::CmdOpenNewPicker(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.showNewPicker = true;
    animator_.SetNewPickerOpen(true);
    EnsureAnimTimer(hwnd);
}

void App::CmdCloseNewPicker(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.showNewPicker = false;
    animator_.SetNewPickerOpen(false);
    EnsureAnimTimer(hwnd);
}

void App::CmdCreateAutoclicker(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.showNewPicker = false;
    animator_.SetNewPickerOpen(false);
    const int target = model_.CreateNewAutoclicker();
    if (target < 0) {
        ShowToast(hwnd, L"All 64 autoclicker slots are in use. Delete one to make room.");
    } else {
        model_.SyncSlotToEngine(target);
        animator_.SetToggle(AnimSlotFor(ItemKind::Autoclicker, target), false, true);
        animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Autoclicker, target));
        if (model_.sidebarFilter != 0 && model_.sidebarFilter != 1) model_.sidebarFilter = 0;
        SelectItem(hwnd, ItemKind::Autoclicker, target);
        StartBinding(hwnd, 1U, L"Press the key or mouse button that should start this autoclicker.");
    }
    EnsureAnimTimer(hwnd);
}

void App::CmdCreateSequence(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.showNewPicker = false;
    animator_.SetNewPickerOpen(false);
    const int target = model_.CreateNewSequence();
    if (target < 0) {
        ShowToast(hwnd, L"All 16 complex sequences are in use. Delete one to make room.");
    } else {
        seqEngine_.UpdateConfig(static_cast<size_t>(target), model_.sequences[static_cast<size_t>(target)]);
        model_.PublishWatchedKeys();
        model_.MarkDirty();
        animator_.SetToggle(AnimSlotFor(ItemKind::Sequence, target), false, true);
        animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Sequence, target));
        if (model_.sidebarFilter != 0 && model_.sidebarFilter != 2) model_.sidebarFilter = 0;
        SelectItem(hwnd, ItemKind::Sequence, target);
        ShowToast(hwnd, L"Created Complex Sequence. Add up to 128 steps and bind a trigger.");
    }
    EnsureAnimTimer(hwnd);
}

void App::CmdCreateTrigger(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.showNewPicker = false;
    animator_.SetNewPickerOpen(false);
    const int target = model_.CreateNewTrigger();
    if (target < 0) {
        ShowToast(hwnd, L"All 8 Trigger Bots are in use. Delete one to make room.");
    } else {
        model_.RefreshDevicesAndMonitors(&remapEngine_);
        pixelEngine_.UpdateConfig(static_cast<size_t>(target), model_.triggers[static_cast<size_t>(target)]);
        model_.PublishWatchedKeys();
        model_.MarkDirty();
        animator_.SetToggle(AnimSlotFor(ItemKind::Trigger, target), false, true);
        animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Trigger, target));
        if (model_.sidebarFilter != 0 && model_.sidebarFilter != 3) model_.sidebarFilter = 0;
        SelectItem(hwnd, ItemKind::Trigger, target);
        ShowToast(hwnd, L"Created Trigger Bot. Pick a pixel color and set an action.");
    }
    EnsureAnimTimer(hwnd);
}

void App::CmdCreateRemap(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.showNewPicker = false;
    animator_.SetNewPickerOpen(false);
    const int target = model_.CreateNewRemap();
    if (target < 0) {
        ShowToast(hwnd, L"All 32 remap rules are in use. Delete one to make room.");
    } else {
        model_.RefreshDevicesAndMonitors(&remapEngine_);
        remapEngine_.UpdateRule(static_cast<size_t>(target), model_.remaps[static_cast<size_t>(target)]);
        model_.PublishWatchedKeys();
        model_.MarkDirty();
        animator_.SetToggle(AnimSlotFor(ItemKind::Remap, target), false, true);
        animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Remap, target));
        if (model_.sidebarFilter != 0 && model_.sidebarFilter != 4) model_.sidebarFilter = 0;
        SelectItem(hwnd, ItemKind::Remap, target);
        StartBinding(hwnd, 22U, L"Press the source key or mouse button to remap.");
    }
    EnsureAnimTimer(hwnd);
}

void App::CmdToggleSettings(HWND hwnd, UiAction, bool, bool) noexcept {
    if (model_.showSettings) {
        model_.showSettings = false;
        settingsHwTabOpen_.store(false, std::memory_order_release);
        animator_.SetSettingsOpen(false);
        animator_.SetSettingsTab(0.0f, 0.0f, true);
        settingsTabSnapped_ = false;
        sidebarIndSnapped_  = false;
        animator_.TriggerViewTransition();
        EnsureAnimTimer(hwnd);
        UpdateTriggerPreviewState(hwnd);
    } else {
        OpenSettingsAtTab(hwnd, model_.settingsTab);
    }
}

void App::CmdOpenHardwareTab(HWND hwnd, UiAction, bool, bool) noexcept {
    OpenSettingsAtTab(hwnd, 4);
}

void App::CmdSettingsTab(HWND hwnd, UiAction act, bool, bool) noexcept {
    OpenSettingsAtTab(hwnd, act.index);
}

void App::CmdSidebarFilter(HWND, UiAction act, bool, bool) noexcept {
    model_.sidebarFilter = std::clamp<int>(act.index, 0, 4);
    model_.listScroll    = 0.0f;
    animator_.SetListScrollTarget(0.0f, true);
    sidebarIndSnapped_ = false;
}

void App::CmdCycleTopLayer(HWND, UiAction, bool, bool) noexcept {
    const uint8_t next = static_cast<uint8_t>((sharedState_.activeRemapLayer.load(std::memory_order_relaxed) + 1U) & 3U);
    sharedState_.activeRemapLayer.store(next, std::memory_order_release);
    remapEngine_.RecomputeSuppressedMask();
}

void App::CmdToggleTopLock(HWND, UiAction, bool, bool) noexcept {
    const bool next = !sharedState_.remapUnlocked.load(std::memory_order_relaxed);
    sharedState_.remapUnlocked.store(next, std::memory_order_release);
    remapEngine_.RecomputeSuppressedMask();
}

void App::CmdTogglePause(HWND hwnd, UiAction, bool, bool) noexcept {
    const bool nowPaused = !sharedState_.automationPaused.load(std::memory_order_acquire);
    lastObservedPaused_ = nowPaused;
    sharedState_.automationPaused.store(nowPaused, std::memory_order_release);
    if (nowPaused) {
        StopEverythingRunning();
        PlayFeedbackTone(false);
        ShowToast(hwnd, L"Paused. Autoclickers, sequences, triggers and remaps are suspended.");
    } else {
        sharedState_.safeStopReason.store(0U, std::memory_order_release);
        remapEngine_.RecomputeSuppressedMask();
        pixelEngine_.ResetArmedStates();
        mask::input::WakeEngine();
        PlayFeedbackTone(true);
        ShowToast(hwnd, L"Automation is active again.");
    }
    UpdateTrayTooltip(hwnd);
}

void App::CmdStopAll(HWND hwnd, UiAction, bool, bool) noexcept {
    StopEverythingRunning();
    PlayFeedbackTone(false);
    ShowToast(hwnd, L"Stopped everything running and released all synthetic keys.");
}

void App::CmdOutputAction(HWND hwnd, UiAction, bool, bool) noexcept {
    if (model_.outputState == OutputState::NeedsAdmin) {
        RestartAsAdministrator(hwnd);
    } else if (!outputWorkerBusy_.load(std::memory_order_acquire)) {
        autoReconnectAttempts_ = 0;
        StartOutputWorker();
        ShowToast(hwnd, L"Connecting the virtual keyboard and mouse\u2026");
    }
}

void App::CmdToastAction(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.hasUndo) return;
    model_.hasUndo = false;
    const int u = model_.undoSlot;
    wchar_t msg[120]{};
    switch (model_.undoKind) {
        case ItemKind::Autoclicker:
            if (u >= 0 && u < kSlotCount) {
                model_.slots[static_cast<size_t>(u)] = model_.undoConfig;
                model_.SyncSlotToEngine(u);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Autoclicker, u));
                SelectItem(hwnd, ItemKind::Autoclicker, u);
                std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", model_.undoConfig.name);
            }
            break;
        case ItemKind::Sequence:
            if (u >= 0 && u < kComplexSeqCount) {
                model_.sequences[static_cast<size_t>(u)] = model_.undoSeq;
                seqEngine_.UpdateConfig(static_cast<size_t>(u), model_.sequences[static_cast<size_t>(u)]);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Sequence, u));
                SelectItem(hwnd, ItemKind::Sequence, u);
                std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", model_.undoSeq.name);
            }
            break;
        case ItemKind::Trigger:
            if (u >= 0 && u < kColorTriggerCount) {
                model_.triggers[static_cast<size_t>(u)] = model_.undoTrigger;
                pixelEngine_.UpdateConfig(static_cast<size_t>(u), model_.triggers[static_cast<size_t>(u)]);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Trigger, u));
                SelectItem(hwnd, ItemKind::Trigger, u);
                std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", model_.undoTrigger.name);
            }
            break;
        case ItemKind::Remap:
            if (u >= 0 && u < kRemapRuleCount) {
                model_.remaps[static_cast<size_t>(u)] = model_.undoRemap;
                remapEngine_.UpdateRule(static_cast<size_t>(u), model_.remaps[static_cast<size_t>(u)]);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Remap, u));
                SelectItem(hwnd, ItemKind::Remap, u);
                std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", model_.undoRemap.name);
            }
            break;
    }
    model_.PublishWatchedKeys();
    model_.MarkDirty();
    SyncAnimatorToggles(false);
    ShowToast(hwnd, msg);
}

void App::CmdSelectItemBase(HWND hwnd, UiAction act, bool, bool) noexcept {
    ItemKind kind = ItemKind::Autoclicker;
    int idx = 0;
    if (!DecodeAnimSlot(act.index, kind, idx)) return;
    if (kind == ItemKind::Trigger || kind == ItemKind::Remap) {
        model_.RefreshDevicesAndMonitors(&remapEngine_);
    }
    SelectItem(hwnd, kind, idx);
}

void App::CmdToggleItemBase(HWND hwnd, UiAction act, bool, bool) noexcept {
    ItemKind kind = ItemKind::Autoclicker;
    int idx = 0;
    if (!DecodeAnimSlot(act.index, kind, idx)) return;
    ToggleItemEnabled(hwnd, kind, idx);
}

// =====================================================================================
//  Settings Commands
// =====================================================================================

void App::CmdSetToggleFlag(HWND hwnd, UiAction act, bool, bool) noexcept {
    auto& st = model_.settings;
    switch (act.kind) {
        case ActionKind::SetToggleStartWin:
            st.startWithWindows = !st.startWithWindows;
            if (!mask::settings::SetStartWithWindows(st.startWithWindows, st.startMinimized)) {
                st.startWithWindows = mask::settings::QueryStartWithWindows();
                ShowToast(hwnd, L"Couldn't update Windows startup registry entry.");
            }
            break;
        case ActionKind::SetToggleStartMin:
            st.startMinimized = !st.startMinimized;
            if (st.startWithWindows) (void)mask::settings::SetStartWithWindows(true, st.startMinimized);
            break;
        case ActionKind::SetToggleMinTray:      st.minimizeToTray = !st.minimizeToTray; break;
        case ActionKind::SetToggleTopmost:      st.alwaysOnTop = !st.alwaysOnTop; ApplyAlwaysOnTop(hwnd); break;
        case ActionKind::SetToggleBounds:       st.rememberWindowBounds = !st.rememberWindowBounds; break;
        case ActionKind::SetToggleConfirmDel:   st.confirmDelete = !st.confirmDelete; break;
        case ActionKind::SetToggleCompact:      st.compactSidebar = !st.compactSidebar; sidebarIndSnapped_ = false; break;
        case ActionKind::SetToggleSignal:       st.showSignalView = !st.showSignalView; break;
        case ActionKind::SetToggleLiveKeys:     st.showLiveKeyIndicators = !st.showLiveKeyIndicators; break;
        case ActionKind::SetToggleLowRes:       st.lowResourceWhenUnfocused = !st.lowResourceWhenUnfocused; UpdateUiTimerCadence(hwnd); break;
        case ActionKind::SetToggleTrimMem:      st.trimMemoryWhenMinimized = !st.trimMemoryWhenMinimized; break;
        case ActionKind::SetTogglePanic:        st.panicChordEnabled = !st.panicChordEnabled; break;
        case ActionKind::SetToggleSessionLock:  st.stopOnSessionLock = !st.stopOnSessionLock; break;
        case ActionKind::SetToggleSuppressType: st.suppressWhileTypingInApp = !st.suppressWhileTypingInApp; UpdateTriggerSuppression(); break;
        case ActionKind::SetToggleSound:        st.soundFeedback = !st.soundFeedback; break;
        default: break;
    }
    model_.ApplySettings();
    model_.MarkDirty();
}

void App::CmdSetAnimQuality(HWND hwnd, UiAction act, bool, bool) noexcept {
    model_.settings.animationQuality = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    animator_.SetQuality(model_.settings.animationQuality);
    model_.listScroll    = animator_.listScrollSmooth.value;
    model_.seqStepScroll = animator_.seqScrollSmooth.value;
    model_.MarkDirty();
    EnsureAnimTimer(hwnd);
}

void App::CmdSetAccent(HWND, UiAction act, bool, bool) noexcept {
    model_.settings.accentPreset = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 4));
    model_.MarkDirty();
}

void App::CmdSetPowerMode(HWND, UiAction act, bool, bool) noexcept {
    model_.settings.powerMode = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    model_.ApplySettings();
    mask::input::WakeEngine();
    model_.MarkDirty();
}

void App::CmdSetPollHz(HWND, UiAction act, bool, bool) noexcept {
    static constexpr uint32_t kHz[3] = { 250U, 500U, 1000U };
    model_.settings.armedPollHz = kHz[std::clamp<int>(act.index, 0, 2)];
    model_.ApplySettings();
    model_.MarkDirty();
}

void App::CmdSetSpinPolicy(HWND, UiAction act, bool, bool) noexcept {
    model_.settings.spinPolicy = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    model_.ApplySettings();
    mask::input::WakeEngine();
    model_.MarkDirty();
}

void App::CmdSetBindPauseVk(HWND hwnd, UiAction, bool, bool) noexcept {
    StartBinding(hwnd, 24U, L"Press the key or mouse button for Global Pause / Resume.");
}

void App::CmdSetResetPauseVk(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.settings.pauseHotkeyVk = VK_F8;
    model_.ApplySettings();
    model_.MarkDirty();
    ShowToast(hwnd, L"Global pause hotkey reset to F8.");
}

void App::CmdSetClearPauseVk(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.settings.pauseHotkeyVk = 0U;
    model_.ApplySettings();
    model_.MarkDirty();
    ShowToast(hwnd, L"Global pause hotkey cleared.");
}

void App::CmdSetBindStopAllVk(HWND hwnd, UiAction, bool, bool) noexcept {
    StartBinding(hwnd, 25U, L"Press the key or mouse button for Global Stop All.");
}

void App::CmdSetClearStopAllVk(HWND hwnd, UiAction, bool, bool) noexcept {
    model_.settings.stopAllHotkeyVk = 0U;
    model_.ApplySettings();
    model_.MarkDirty();
    ShowToast(hwnd, L"Global Stop All hotkey cleared.");
}

void App::CmdSetProfileBank(HWND hwnd, UiAction act, bool, bool) noexcept {
    const uint8_t newBank = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    if (newBank != model_.settings.activeProfileBank) {
        StopEverythingRunning();
        (void)model_.SaveToDisk();
        model_.settings.activeProfileBank = newBank;
        (void)model_.SaveSettingsOnly();
        (void)model_.LoadFromDisk();
        SyncAllEnginesFromModel();
        SyncAnimatorToggles(true);
        UpdateTrayTooltip(hwnd);
        wchar_t msg[64]{};
        std::swprintf(msg, std::size(msg), L"Switched to Profile Bank %u.", static_cast<unsigned>(newBank + 1U));
        ShowToast(hwnd, msg);
    }
}

void App::CmdSetOpenDataFolder(HWND hwnd, UiAction, bool, bool) noexcept {
    ShellExecuteW(hwnd, L"open", model_.dataDir, nullptr, nullptr, SW_SHOWNORMAL);
}

void App::CmdSetResetAll(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.pendingResetConfirm) {
        model_.pendingResetConfirm = true;
        ShowToast(hwnd, L"Click Reset All again to reset the current profile bank.");
    } else {
        model_.pendingResetConfirm = false;
        StopEverythingRunning();
        model_.InitializeDefaults();
        SyncAllEnginesFromModel();
        SyncAnimatorToggles(true);
        (void)model_.SaveToDisk();
        ShowToast(hwnd, L"Current profile bank reset to clean defaults.");
    }
}

void App::CmdSetRefreshHw(HWND hwnd, UiAction, bool, bool) noexcept {
    remapEngine_.RefreshMonitors();
    model_.RefreshDevicesAndMonitors(&remapEngine_);
    ShowToast(hwnd, L"Refreshed physical GameInput devices and connected monitors.");
}

void App::CmdExportProfileJson(HWND hwnd, UiAction, bool, bool) noexcept {
    wchar_t filePath[MAX_PATH]{L"mask-profile.json"};
    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = hwnd;
    ofn.lpstrFilter     = L"Mask Profile JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile       = filePath;
    ofn.nMaxFile        = MAX_PATH;
    ofn.lpstrDefExt     = L"json";
    ofn.lpstrInitialDir = (model_.dataDir[0] != L'\0') ? model_.dataDir : nullptr;
    ofn.lpstrTitle      = L"Export Profile Bank to JSON";
    ofn.Flags           = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    if (GetSaveFileNameW(&ofn) != FALSE) {
        if (model_.ExportBankToJson(filePath)) {
            ShowToast(hwnd, L"Exported current profile bank to JSON.");
        } else {
            ShowToast(hwnd, L"Couldn't export profile bank to JSON.");
        }
    }
}

void App::CmdImportProfileJson(HWND hwnd, UiAction, bool, bool) noexcept {
    wchar_t filePath[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = hwnd;
    ofn.lpstrFilter     = L"Mask Profile JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile       = filePath;
    ofn.nMaxFile        = MAX_PATH;
    ofn.lpstrDefExt     = L"json";
    ofn.lpstrInitialDir = (model_.dataDir[0] != L'\0') ? model_.dataDir : nullptr;
    ofn.lpstrTitle      = L"Import Profile Bank from JSON";
    ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn) != FALSE) {
        StopEverythingRunning();
        bool hasImportedCmds = false;
        if (model_.ImportBankFromJson(filePath, hasImportedCmds)) {
            SyncAllEnginesFromModel();
            SyncAnimatorToggles(true);
            UpdateTriggerPreviewState(hwnd);
            if (hasImportedCmds) {
                ShowToast(hwnd, L"Imported profile JSON. Imported app/command steps will ask before running.");
            } else {
                ShowToast(hwnd, L"Imported profile bank from JSON.");
            }
        } else {
            ShowToast(hwnd, L"Couldn't import profile JSON. Check that the file is valid.");
        }
    }
}

void App::CmdSelectMouseProfile(HWND hwnd, UiAction act, bool, bool) noexcept {
    if (outputWorkerBusy_.load(std::memory_order_acquire)) {
        ShowToast(hwnd, L"Virtual devices are still updating. Try again in a moment.");
        return;
    }
    const int idx = std::clamp<int>(act.index, 0, kMouseProfileCount - 1);
    const auto& preset = kMouseProfiles[idx];
    wcsncpy_s(model_.mouseProfileId, preset.id, _TRUNCATE);
    wcsncpy_s(model_.settings.mouseProfileId, preset.id, _TRUNCATE);
    model_.MarkDirty();
    StartOutputWorker();
    wchar_t msg[96]{};
    std::swprintf(msg, std::size(msg), L"Switching virtual mouse to %ls\u2026", preset.label);
    ShowToast(hwnd, msg);
}

void App::CmdSelectKbProfile(HWND hwnd, UiAction act, bool, bool) noexcept {
    if (outputWorkerBusy_.load(std::memory_order_acquire)) {
        ShowToast(hwnd, L"Virtual devices are still updating. Try again in a moment.");
        return;
    }
    const int idx = std::clamp<int>(act.index, 0, kKbProfileCount - 1);
    const auto& preset = kKbProfiles[idx];
    wcsncpy_s(model_.keyboardProfileId, preset.id, _TRUNCATE);
    wcsncpy_s(model_.settings.keyboardProfileId, preset.id, _TRUNCATE);
    model_.MarkDirty();
    StartOutputWorker();
    wchar_t msg[96]{};
    std::swprintf(msg, std::size(msg), L"Switching virtual keyboard to %ls\u2026", preset.label);
    ShowToast(hwnd, msg);
}

// =====================================================================================
//  Shared Fields & Selected Item Common Commands
// =====================================================================================

void App::CmdFieldFocus(HWND, UiAction act, bool, bool) noexcept {
    BeginEdit(static_cast<Field>(act.index));
}

void App::CmdFieldDec(HWND, UiAction act, bool shift, bool ctrl) noexcept {
    StepField(static_cast<Field>(act.index), -1, shift, ctrl);
}

void App::CmdFieldInc(HWND, UiAction act, bool shift, bool ctrl) noexcept {
    StepField(static_cast<Field>(act.index), +1, shift, ctrl);
}

void App::CmdDeleteItem(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection()) return;
    const ItemKind kind = model_.selectedKind;
    const int sel = model_.selectedIndex;
    if (model_.settings.confirmDelete && !model_.pendingDeleteConfirm) {
        model_.pendingDeleteConfirm = true;
        ShowToast(hwnd, L"Click Delete again to confirm removal.");
    } else {
        model_.pendingDeleteConfirm = false;
        const int delId = EncodeDeleteId(kind, sel);
        const size_t animSlot = AnimSlotFor(kind, sel);
        if (model_.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            FinalizeItemDelete(hwnd, delId);
        } else {
            animator_.TriggerRowDelete(delId, animSlot);
            EnsureAnimTimer(hwnd);
        }
    }
}

void App::CmdToggleEnabled(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection()) return;
    ToggleItemEnabled(hwnd, model_.selectedKind, model_.selectedIndex);
}

void App::CmdDuplicateItem(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection()) return;
    switch (model_.selectedKind) {
        case ItemKind::Autoclicker: {
            const int target = model_.FindFirstFreeSlot();
            if (target < 0) {
                ShowToast(hwnd, L"All 64 autoclicker slots are in use.");
            } else {
                MacroConfig copy = model_.SelectedAutoclicker();
                copy.slotIndex   = static_cast<uint8_t>(target);
                copy.enabled     = false;
                copy.reserved[0] = 1U;
                std::swprintf(copy.name, std::size(copy.name), L"%.18ls copy", model_.SelectedAutoclicker().name);
                model_.slots[static_cast<size_t>(target)] = copy;
                model_.SyncSlotToEngine(target);
                animator_.SetToggle(AnimSlotFor(ItemKind::Autoclicker, target), false, true);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Autoclicker, target));
                SelectItem(hwnd, ItemKind::Autoclicker, target);
                ShowToast(hwnd, L"Duplicated autoclicker (switched off until you enable it).");
            }
            break;
        }
        case ItemKind::Sequence: {
            const int target = model_.FindFirstFreeSeq();
            if (target < 0) {
                ShowToast(hwnd, L"All 16 complex sequence slots are in use.");
            } else {
                ComplexSequenceConfig copy = model_.SelectedSeq();
                copy.slotIndex = static_cast<uint8_t>(target);
                copy.enabled   = false;
                copy.inUse     = true;
                std::swprintf(copy.name, std::size(copy.name), L"%.24ls copy", model_.SelectedSeq().name);
                model_.sequences[static_cast<size_t>(target)] = copy;
                seqEngine_.UpdateConfig(static_cast<size_t>(target), copy);
                model_.MarkDirty();
                animator_.SetToggle(AnimSlotFor(ItemKind::Sequence, target), false, true);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Sequence, target));
                SelectItem(hwnd, ItemKind::Sequence, target);
                ShowToast(hwnd, L"Duplicated complex sequence.");
            }
            break;
        }
        case ItemKind::Trigger: {
            const int target = model_.FindFirstFreeTrigger();
            if (target < 0) {
                ShowToast(hwnd, L"All 8 trigger slots are in use.");
            } else {
                ColorTriggerConfig copy = model_.SelectedTrigger();
                copy.slotIndex = static_cast<uint8_t>(target);
                copy.enabled   = false;
                copy.inUse     = true;
                std::swprintf(copy.name, std::size(copy.name), L"%.24ls copy", model_.SelectedTrigger().name);
                model_.triggers[static_cast<size_t>(target)] = copy;
                pixelEngine_.UpdateConfig(static_cast<size_t>(target), copy);
                model_.PublishWatchedKeys();
                model_.MarkDirty();
                animator_.SetToggle(AnimSlotFor(ItemKind::Trigger, target), false, true);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Trigger, target));
                SelectItem(hwnd, ItemKind::Trigger, target);
                ShowToast(hwnd, L"Duplicated trigger.");
            }
            break;
        }
        case ItemKind::Remap: {
            const int target = model_.FindFirstFreeRemap();
            if (target < 0) {
                ShowToast(hwnd, L"All 32 remap slots are in use.");
            } else {
                RemapRuleConfig copy = model_.SelectedRemap();
                copy.slotIndex = static_cast<uint8_t>(target);
                copy.enabled   = false;
                copy.inUse     = true;
                std::swprintf(copy.name, std::size(copy.name), L"%.24ls copy", model_.SelectedRemap().name);
                model_.remaps[static_cast<size_t>(target)] = copy;
                remapEngine_.UpdateRule(static_cast<size_t>(target), copy);
                model_.MarkDirty();
                animator_.SetToggle(AnimSlotFor(ItemKind::Remap, target), false, true);
                animator_.TriggerRowAdd(AnimSlotFor(ItemKind::Remap, target));
                SelectItem(hwnd, ItemKind::Remap, target);
                ShowToast(hwnd, L"Duplicated remap rule.");
            }
            break;
        }
    }
}

// =====================================================================================
//  Autoclicker Commands
// =====================================================================================

void App::CmdBindTrigger(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Autoclicker) return;
    StartBinding(hwnd, 1U, L"Press the key or mouse button that should start this autoclicker.");
}

void App::CmdBindOutput(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Autoclicker) return;
    StartBinding(hwnd, 2U, L"Press the key or mouse button this autoclicker should send.");
}

void App::CmdMode(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Autoclicker) return;
    model_.SelectedAutoclicker().mode = static_cast<uint8_t>(std::clamp<int>(act.index, 0, kModeCount - 1));
    model_.SyncSlotToEngine(model_.selectedIndex);
}

void App::CmdQuickOut(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Autoclicker) return;
    if (act.index >= 0 && act.index < kQuickOutputCount) {
        model_.SelectedAutoclicker().outputVk = kQuickOutputs[act.index].vk;
        model_.SyncSlotToEngine(model_.selectedIndex);
    }
}

void App::CmdHumanizeBase(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Autoclicker) return;
    if (act.index >= 0 && act.index < kHumanizeCount) {
        mask::core::SetHumanizePercent(model_.SelectedAutoclicker(), kHumanizePresets[act.index].pct);
        model_.SyncSlotToEngine(model_.selectedIndex);
    }
}

// =====================================================================================
//  Complex Sequence Commands
// =====================================================================================

void App::CmdSeqToggleStepKindMenu(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    model_.showStepKindMenu = !model_.showStepKindMenu;
}

void App::CmdSeqBindTrigger(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    StartBinding(hwnd, 20U, L"Press the key or mouse button to trigger this sequence.");
}

void App::CmdSeqStepBindKey(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    StartBinding(hwnd, 21U, L"Press the key or mouse button for this workflow step.");
}

void App::CmdSeqTriggerMode(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    model_.SelectedSeq().triggerMode = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    SyncSelectedSequence();
}

void App::CmdSeqCycleHumanize(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    int cur = 0;
    for (int i = 0; i < kHumanizeCount; ++i) {
        if (seq.humanizePct == kHumanizePresets[i].pct) {
            cur = i;
            break;
        }
    }
    seq.humanizePct = kHumanizePresets[(cur + 1) % kHumanizeCount].pct;
    SyncSelectedSequence();
}

void App::CmdSelectSeqStep(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    const auto& seq = model_.SelectedSeq();
    model_.selectedSeqStep = std::clamp<int>(act.index, 0, (std::max)(0, static_cast<int>(seq.stepCount) - 1));
}

void App::CmdSeqStepAdd(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (seq.stepCount >= kComplexStepMax) {
        ShowToast(hwnd, L"Maximum 128 steps reached for this sequence.");
        return;
    }
    const int insertPos = (seq.stepCount == 0U)
        ? 0
        : std::clamp(model_.selectedSeqStep + 1, 0, static_cast<int>(seq.stepCount));
    for (int i = static_cast<int>(seq.stepCount); i > insertPos; --i) {
        seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i - 1)];
    }
    ComplexStep ns{};
    ns.kind       = static_cast<uint8_t>(ComplexStepKind::KeyClick);
    ns.vk         = '1';
    ns.durationMs = 25U;
    seq.steps[static_cast<size_t>(insertPos)] = ns;
    ++seq.stepCount;
    model_.selectedSeqStep = insertPos;
    EnsureSelectedSeqStepVisible();
    SyncSelectedSequence();
}

void App::CmdSeqStepInsert(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (seq.stepCount >= kComplexStepMax) return;
    const int pos = std::clamp(model_.selectedSeqStep, 0, static_cast<int>(seq.stepCount));
    for (int i = static_cast<int>(seq.stepCount); i > pos; --i) {
        seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i - 1)];
    }
    ComplexStep ns{};
    ns.kind       = static_cast<uint8_t>(ComplexStepKind::WaitMs);
    ns.durationMs = 50U;
    seq.steps[static_cast<size_t>(pos)] = ns;
    ++seq.stepCount;
    model_.selectedSeqStep = pos;
    EnsureSelectedSeqStepVisible();
    SyncSelectedSequence();
}

void App::CmdSeqStepDup(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (seq.stepCount == 0U || seq.stepCount >= kComplexStepMax) return;
    const int pos = std::clamp(model_.selectedSeqStep, 0, static_cast<int>(seq.stepCount) - 1);
    for (int i = static_cast<int>(seq.stepCount); i > pos + 1; --i) {
        seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i - 1)];
    }
    seq.steps[static_cast<size_t>(pos + 1)] = seq.steps[static_cast<size_t>(pos)];
    ++seq.stepCount;

    const auto dupKind = static_cast<ComplexStepKind>(seq.steps[static_cast<size_t>(pos)].kind);
    if ((dupKind == ComplexStepKind::LoopStart || dupKind == ComplexStepKind::IfCheck) &&
        seq.stepCount < kComplexStepMax) {
        for (int i = static_cast<int>(seq.stepCount); i > pos + 2; --i) {
            seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i - 1)];
        }
        ComplexStep closer{};
        closer.kind = static_cast<uint8_t>(
            dupKind == ComplexStepKind::LoopStart ? ComplexStepKind::LoopEnd : ComplexStepKind::EndIf);
        seq.steps[static_cast<size_t>(pos + 2)] = closer;
        ++seq.stepCount;
    }

    model_.selectedSeqStep = pos + 1;
    EnsureSelectedSeqStepVisible();
    SyncSelectedSequence();
}

void App::CmdSeqStepUp(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep > 0 && model_.selectedSeqStep < static_cast<int>(seq.stepCount)) {
        std::swap(seq.steps[static_cast<size_t>(model_.selectedSeqStep)],
                  seq.steps[static_cast<size_t>(model_.selectedSeqStep - 1)]);
        --model_.selectedSeqStep;
        EnsureSelectedSeqStepVisible();
        SyncSelectedSequence();
    }
}

void App::CmdSeqStepDown(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep >= 0 && model_.selectedSeqStep + 1 < static_cast<int>(seq.stepCount)) {
        std::swap(seq.steps[static_cast<size_t>(model_.selectedSeqStep)],
                  seq.steps[static_cast<size_t>(model_.selectedSeqStep + 1)]);
        ++model_.selectedSeqStep;
        EnsureSelectedSeqStepVisible();
        SyncSelectedSequence();
    }
}

void App::CmdSeqStepDel(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (seq.stepCount > 0U && model_.selectedSeqStep >= 0 &&
        model_.selectedSeqStep < static_cast<int>(seq.stepCount)) {
        for (int i = model_.selectedSeqStep; i + 1 < static_cast<int>(seq.stepCount); ++i) {
            seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i + 1)];
        }
        --seq.stepCount;
        model_.selectedSeqStep = std::clamp(model_.selectedSeqStep, 0,
                                            (std::max)(0, static_cast<int>(seq.stepCount) - 1));
        EnsureSelectedSeqStepVisible();
        SyncSelectedSequence();
    }
}

void App::CmdSeqStepKindBase(HWND hwnd, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    const int pos = model_.selectedSeqStep;
    auto& st = seq.steps[static_cast<size_t>(pos)];
    const auto prevKind = static_cast<ComplexStepKind>(st.kind);
    st.kind = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 16));
    const auto nk = static_cast<ComplexStepKind>(st.kind);

    if (nk == ComplexStepKind::MouseScroll &&
        (st.param == 0U || static_cast<int32_t>(st.param) > 120 || static_cast<int32_t>(st.param) < -120)) {
        st.param = 1U;
    }
    if (nk == ComplexStepKind::WaitForColor) {
        st.durationMs = 0U;
        if (st.param == 0U) st.param = 3000U;
    }
    if (nk == ComplexStepKind::LoopStart && (prevKind != ComplexStepKind::LoopStart || st.param == 0U)) {
        st.param = 3U;
    }
    if (nk == ComplexStepKind::IfCheck && prevKind != ComplexStepKind::IfCheck) {
        mask::core::SetStepConditionKind(st, ComplexConditionKind::KeyHeld);
        mask::core::SetStepConditionNegated(st, false);
        if (st.vk == 0U) st.vk = VK_LSHIFT;
        if (st.tolerance == 0U) st.tolerance = 24U;
    }
    if (nk == ComplexStepKind::KeyClick && st.vk == 0U) st.vk = '1';

    auto removeStepAt = [&](int rIdx) noexcept {
        for (int i = rIdx; i + 1 < static_cast<int>(seq.stepCount); ++i) {
            seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i + 1)];
        }
        --seq.stepCount;
    };

    if (prevKind == ComplexStepKind::LoopStart && nk != ComplexStepKind::LoopStart) {
        if (pos + 1 < static_cast<int>(seq.stepCount) &&
            seq.steps[static_cast<size_t>(pos + 1)].kind == static_cast<uint8_t>(ComplexStepKind::LoopEnd) &&
            CountStepKind(seq, ComplexStepKind::LoopEnd) > CountStepKind(seq, ComplexStepKind::LoopStart)) {
            removeStepAt(pos + 1);
        }
    } else if (prevKind == ComplexStepKind::IfCheck && nk != ComplexStepKind::IfCheck) {
        if (pos + 1 < static_cast<int>(seq.stepCount) &&
            seq.steps[static_cast<size_t>(pos + 1)].kind == static_cast<uint8_t>(ComplexStepKind::EndIf) &&
            CountStepKind(seq, ComplexStepKind::EndIf) > CountStepKind(seq, ComplexStepKind::IfCheck)) {
            removeStepAt(pos + 1);
        }
    }

    if (nk == ComplexStepKind::LoopStart && prevKind != ComplexStepKind::LoopStart) {
        if (CountStepKind(seq, ComplexStepKind::LoopStart) > CountStepKind(seq, ComplexStepKind::LoopEnd)) {
            if (seq.stepCount < kComplexStepMax) {
                for (int i = static_cast<int>(seq.stepCount); i > pos + 1; --i) {
                    seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i - 1)];
                }
                ComplexStep endSt{};
                endSt.kind = static_cast<uint8_t>(ComplexStepKind::LoopEnd);
                seq.steps[static_cast<size_t>(pos + 1)] = endSt;
                ++seq.stepCount;
                model_.selectedSeqStep = pos;
                EnsureSelectedSeqStepVisible();
                ShowToast(hwnd, L"Added matching End loop step.");
            } else {
                ShowToast(hwnd, L"Sequence is full (128 steps); couldn\u2019t auto-add End loop.");
            }
        }
    } else if (nk == ComplexStepKind::IfCheck && prevKind != ComplexStepKind::IfCheck) {
        if (CountStepKind(seq, ComplexStepKind::IfCheck) > CountStepKind(seq, ComplexStepKind::EndIf)) {
            if (seq.stepCount < kComplexStepMax) {
                for (int i = static_cast<int>(seq.stepCount); i > pos + 1; --i) {
                    seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i - 1)];
                }
                ComplexStep endSt{};
                endSt.kind = static_cast<uint8_t>(ComplexStepKind::EndIf);
                seq.steps[static_cast<size_t>(pos + 1)] = endSt;
                ++seq.stepCount;
                model_.selectedSeqStep = pos;
                EnsureSelectedSeqStepVisible();
                ShowToast(hwnd, L"Added matching End if step.");
            } else {
                ShowToast(hwnd, L"Sequence is full (128 steps); couldn\u2019t auto-add End if.");
            }
        }
    }
    SyncSelectedSequence();
}

void App::CmdSeqStepCondKind(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    auto& st = seq.steps[static_cast<size_t>(model_.selectedSeqStep)];
    const auto ck = static_cast<ComplexConditionKind>(std::clamp<int>(act.index, 0, 5));
    mask::core::SetStepConditionKind(st, ck);
    if (ck == ComplexConditionKind::KeyHeld) {
        if (st.vk == 0U) st.vk = VK_LSHIFT;
    } else if (ck == ComplexConditionKind::ColorMatches) {
        if (st.tolerance == 0U) st.tolerance = 24U;
    } else if (ck == ComplexConditionKind::LayerIs ||
               ck == ComplexConditionKind::UnlockStateIs ||
               ck == ComplexConditionKind::MacroActive) {
        st.param = 0U;
    } else if (ck == ComplexConditionKind::RandomChance) {
        if (st.param < 1U || st.param > 99U) st.param = 50U;
    }
    if (ck == ComplexConditionKind::UnlockStateIs) {
        mask::core::SetStepConditionNegated(st, false);
    }
    SyncSelectedSequence();
}

void App::CmdSeqStepCondNegate(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    mask::core::SetStepConditionNegated(seq.steps[static_cast<size_t>(model_.selectedSeqStep)], act.index != 0);
    SyncSelectedSequence();
}

void App::CmdSeqStepCondUnlock(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    auto& st = seq.steps[static_cast<size_t>(model_.selectedSeqStep)];
    st.param = static_cast<uint32_t>(act.index != 0 ? 1 : 0);
    mask::core::SetStepConditionNegated(st, false);
    SyncSelectedSequence();
}

void App::CmdSeqStepCondAddElse(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    const int pos = model_.selectedSeqStep;
    if (seq.steps[static_cast<size_t>(pos)].kind == static_cast<uint8_t>(ComplexStepKind::IfCheck) &&
        !HasElseInIfBlock(seq, pos) &&
        seq.stepCount < kComplexStepMax) {
        const int endIdx = FindMatchingEndIf(seq, pos);
        const int insertAt = (endIdx >= 0) ? endIdx : (pos + 1);
        for (int i = static_cast<int>(seq.stepCount); i > insertAt; --i) {
            seq.steps[static_cast<size_t>(i)] = seq.steps[static_cast<size_t>(i - 1)];
        }
        ComplexStep elseSt{};
        elseSt.kind = static_cast<uint8_t>(ComplexStepKind::ElseBranch);
        seq.steps[static_cast<size_t>(insertAt)] = elseSt;
        ++seq.stepCount;
        model_.selectedSeqStep = insertAt;
        EnsureSelectedSeqStepVisible();
        SyncSelectedSequence();
    }
}

void App::CmdSeqStepCondQuickPct(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    static constexpr uint32_t kPcts[4] = { 10U, 25U, 50U, 75U };
    seq.steps[static_cast<size_t>(model_.selectedSeqStep)].param = kPcts[std::clamp<int>(act.index, 0, 3)];
    SyncSelectedSequence();
}

void App::CmdSeqStepLayer(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    seq.steps[static_cast<size_t>(model_.selectedSeqStep)].param =
        static_cast<uint32_t>(std::clamp<int>(act.index, 0, kRemapLayerCount - 1));
    SyncSelectedSequence();
}

void App::CmdSeqStepQuickWait(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    static constexpr uint32_t kWaits[4] = { 10U, 50U, 250U, 1000U };
    seq.steps[static_cast<size_t>(model_.selectedSeqStep)].durationMs = kWaits[std::clamp<int>(act.index, 0, 3)];
    SyncSelectedSequence();
}

void App::CmdSeqStepQuickScroll(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    auto& seq = model_.SelectedSeq();
    if (model_.selectedSeqStep < 0 || model_.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
    seq.steps[static_cast<size_t>(model_.selectedSeqStep)].param =
        static_cast<uint32_t>((act.index == 0) ? -1 : 1);
    SyncSelectedSequence();
}

void App::CmdSeqStepPickColor(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Sequence) return;
    model_.seqColorPickActive = true;
    SetCapture(hwnd);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    ShowToast(hwnd, L"Click anywhere on any screen to capture X/Y and RGB color (Esc cancels).");
}

// =====================================================================================
//  Trigger Bot Commands
// =====================================================================================

void App::CmdTrigPickPixel(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    if (model_.triggerColorPickActive) {
        CancelTriggerEyedropper(hwnd);
    } else {
        model_.triggerColorPickActive = true;
        model_.RefreshDevicesAndMonitors(&remapEngine_);
        POINT pt{};
        if (GetCursorPos(&pt)) SampleTriggerEyedropperLoupe(pt.x, pt.y);
        SetCapture(hwnd);
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        UpdateTriggerPreviewState(hwnd);
        ShowToast(hwnd, L"Click anywhere to capture screen pixel color and position (Esc cancels).");
    }
}

void App::CmdTrigLockCenter(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    auto& t = model_.SelectedTrigger();
    t.sampleX = -1;
    t.sampleY = -1;
    SyncSelectedTrigger();
}

void App::CmdTrigBindArmVk(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    StartBinding(hwnd, 26U, L"Press the key or mouse button to arm this trigger.");
}

void App::CmdTrigClearArmVk(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    model_.SelectedTrigger().armVk = 0U;
    SyncSelectedTrigger();
}

void App::CmdTrigBindActionVk(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    StartBinding(hwnd, 27U, L"Press the key or mouse button this trigger should press.");
}

void App::CmdTrigColorPreset(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    auto& t = model_.SelectedTrigger();
    const auto& p = kTriggerColorPresets[std::clamp<int>(act.index, 0, kTriggerColorPresetCount - 1)];
    t.r = p.r;
    t.g = p.g;
    t.b = p.b;
    if (t.mode == static_cast<uint8_t>(ColorTriggerMode::ColorChanged)) {
        t.mode = static_cast<uint8_t>(ColorTriggerMode::MatchColor);
    }
    SyncSelectedTrigger();
}

void App::CmdTrigMode(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    model_.SelectedTrigger().mode = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 1));
    SyncSelectedTrigger();
}

void App::CmdTrigMatchRule(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    const auto rule = (act.index != 0) ? ColorTriggerMatchRule::ZoneAverage : ColorTriggerMatchRule::AnyPixel;
    mask::core::SetTriggerMatchRule(model_.SelectedTrigger(), rule);
    SyncSelectedTrigger();
}

void App::CmdTrigRadius(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    static constexpr uint8_t kRadVals[5] = { 0U, 1U, 2U, 4U, 7U };
    model_.SelectedTrigger().sampleRadius = kRadVals[std::clamp<int>(act.index, 0, 4)];
    SyncSelectedTrigger();
}

void App::CmdTrigMonitor(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    model_.SelectedTrigger().monitorIndex = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 7));
    SyncSelectedTrigger();
}

void App::CmdTrigArmMode(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    model_.SelectedTrigger().armMode = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    SyncSelectedTrigger();
}

void App::CmdTrigActionKind(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    model_.SelectedTrigger().actionKind = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 3));
    SyncSelectedTrigger();
}

void App::CmdTrigQuickOut(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Trigger) return;
    static constexpr uint16_t kQ[4] = { VK_LBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    model_.SelectedTrigger().actionVk = kQ[std::clamp<int>(act.index, 0, 3)];
    SyncSelectedTrigger();
}

// =====================================================================================
//  Remap Rule Commands
// =====================================================================================

void App::CmdRemapLiveLayer(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    sharedState_.activeRemapLayer.store(static_cast<uint8_t>(std::clamp<int>(act.index, 0, kRemapLayerCount - 1)),
                                        std::memory_order_release);
    remapEngine_.RecomputeSuppressedMask();
}

void App::CmdRemapBindSource(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    StartBinding(hwnd, 22U, L"Press the source key or mouse button to remap.");
}

void App::CmdRemapBindTarget(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    StartBinding(hwnd, 23U, L"Press the target key or mouse button to emit.");
}

void App::CmdRemapCycleDevice(HWND hwnd, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    auto& r = model_.SelectedRemap();
    model_.RefreshDevicesAndMonitors(&remapEngine_);
    if (model_.physicalDeviceCount <= 0) {
        r.anyDevice = true;
        wcscpy_s(r.deviceLabel, L"Any device");
        ShowToast(hwnd, L"Press any key/mouse first so GameInput enumerates your physical devices.");
    } else if (r.anyDevice) {
        r.anyDevice = false;
        std::memcpy(r.deviceIdBytes, model_.physicalDevices[0].deviceIdBytes, 32U);
        wcsncpy_s(r.deviceLabel, model_.physicalDevices[0].displayName, _TRUNCATE);
    } else {
        int found = -1;
        for (int i = 0; i < model_.physicalDeviceCount; ++i) {
            if (std::memcmp(r.deviceIdBytes, model_.physicalDevices[static_cast<size_t>(i)].deviceIdBytes, 32U) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0 || found + 1 >= model_.physicalDeviceCount) {
            r.anyDevice = true;
            std::memset(r.deviceIdBytes, 0, 32U);
            wcscpy_s(r.deviceLabel, L"Any device");
        } else {
            const auto& d = model_.physicalDevices[static_cast<size_t>(found + 1)];
            std::memcpy(r.deviceIdBytes, d.deviceIdBytes, 32U);
            wcsncpy_s(r.deviceLabel, d.displayName, _TRUNCATE);
        }
    }
    SyncSelectedRemap();
}

void App::CmdRemapDevKind(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    model_.SelectedRemap().deviceKind = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    SyncSelectedRemap();
}

void App::CmdRemapMonitor(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    const int m = std::clamp<int>(act.index, 0, 8);
    model_.SelectedRemap().monitorIndex = (m == 0) ? static_cast<int8_t>(-1) : static_cast<int8_t>(m - 1);
    SyncSelectedRemap();
}

void App::CmdRemapActionKind(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    model_.SelectedRemap().actionKind = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 5));
    SyncSelectedRemap();
}

void App::CmdRemapRuleLayer(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    model_.SelectedRemap().layerIndex = static_cast<uint8_t>(std::clamp<int>(act.index, 0, kRemapLayerCount - 1));
    SyncSelectedRemap();
}

void App::CmdRemapTargetLayer(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    model_.SelectedRemap().targetLayer = static_cast<uint8_t>(std::clamp<int>(act.index, 0, kRemapLayerCount - 1));
    SyncSelectedRemap();
}

void App::CmdRemapUnlockPolicy(HWND, UiAction act, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    model_.SelectedRemap().unlockPolicy = static_cast<uint8_t>(std::clamp<int>(act.index, 0, 2));
    SyncSelectedRemap();
}

void App::CmdRemapToggleSuppress(HWND, UiAction, bool, bool) noexcept {
    if (!model_.HasSelection() || model_.selectedKind != ItemKind::Remap) return;
    auto& r = model_.SelectedRemap();
    r.suppressOriginal = !r.suppressOriginal;
    SyncSelectedRemap();
}

} // namespace mask::app
