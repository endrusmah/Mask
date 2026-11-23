#pragma once

// Secondary Cosmic Glass Editor Views:
//  1. 128-Step Complex Sequence Studio (preserving all 17 step kinds & nested loop/conditional rails)
//  2. Reworked Multi-Device / Multi-Monitor Remap Editor with Hero [Source ---> Target] Banner
//  3. Simplified & Rock-Solid Trigger Bot View (Color Match / Color Change + 15x15 Live Loupe)
//  4. 5-Tab Cosmic Settings Overlay (General, Appearance, Performance, Hotkeys, Hardware & Profiles)
//  5. Animated `+ New` Creation Picker Modal

namespace mask::app {

// =====================================================================================
//  1. Complex Sequence Editor (up to 128 workflow steps)
// =====================================================================================

inline void UiRenderer::DrawComplexSequenceEditor(const D2D1_RECT_F& panel) noexcept {
    const auto& u = Ui();
    const auto& a = Anim();
    const int idx = std::clamp(u.selectedIndex, 0, kComplexSeqCount - 1);
    const auto& seq = u.sequences[idx];
    const bool running = (frame_.activeSeqMask & (1U << idx)) != 0U;
    const uint16_t execStep = frame_.activeSeqStep[idx];
    const uint8_t bindTarget = frame_.bindTarget;
    const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Sequence, idx));

    const float L = panel.left + 24.0f;
    const float R = panel.right - 24.0f;
    float y = panel.top + 14.0f;

    // ---- Header: inline rename | On/Off | Duplicate | Delete ----------------------
    const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
    Button(delBtn, u.pendingDeleteConfirm ? L"Confirm?" : L"Delete", act::DeleteItem,
           u.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

    const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
    Button(dupBtn, L"Duplicate", act::DuplicateItem, BtnStyle::Ghost);

    const float switchX = dupBtn.left - 16.0f - 36.0f;
    Switch(switchX, y + 8.0f, seq.enabled, act::ToggleEnabled, running, animIdx);
    Text(running ? L"Running" : (seq.enabled ? L"On" : L"Off"), gpu_.fBody,
         Rc(switchX - 72.0f, y, switchX - 10.0f, y + 36.0f),
         running ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Hit(Rc(switchX - 72.0f, y, switchX, y + 36.0f), act::ToggleEnabled);

    const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 80.0f, y + 36.0f);
    const int nameA = act::FieldFocus + static_cast<int>(Field::SeqName);
    if (u.editField == Field::SeqName) {
        Fill(nameRect, 6.0f, theme::Inset, 0.92f);
        Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
        Text(u.editBuffer, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
        const float caretX = Snap(L + MeasureWidth(u.editBuffer, gpu_.fTitle) + 1.5f);
        Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignalHi(), 1.5f);
    } else {
        if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised, 0.72f);
        Text(seq.name, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
    }
    Hit(nameRect, nameA);

    wchar_t seqDesc[180]{};
    DescribeComplexSequence(seq, seqDesc, std::size(seqDesc));
    Text(seqDesc, gpu_.fBody, Rc(L, y + 36.0f, R, y + 56.0f), seq.triggerVk == 0U ? theme::Warn : theme::Muted);
    y += 62.0f;

    // ---- Trigger & Run Mode Row ---------------------------------------------------
    constexpr float kTrigRowH = 34.0f;
    Text(L"Trigger", gpu_.fLabel, RcW(L, y, 56.0f, kTrigRowH), theme::Muted);
    KeyChip(RcW(L + 58.0f, y, 134.0f, kTrigRowH), seq.triggerVk, act::SeqBindTrigger, bindTarget == 20U);

    const float segX = L + 202.0f;
    const float segW = std::min(204.0f, R - segX - 118.0f);
    const D2D1_RECT_F seg = RcW(segX, y, segW, kTrigRowH);
    Fill(seg, 8.0f, theme::Inset, 0.85f);
    Stroke(seg, 8.0f, theme::Line, 1.0f, 0.85f);
    const float segCellW = (segW - 6.0f) / 3.0f;
    for (int m = 0; m < 3; ++m) {
        const D2D1_RECT_F cell = RcW(seg.left + 3.0f + segCellW * static_cast<float>(m), seg.top + 3.0f, segCellW, kTrigRowH - 6.0f);
        const bool sel = (seq.triggerMode == static_cast<uint8_t>(m));
        const int actId = act::SeqTriggerMode + m;
        if (sel) {
            gpu_.FillLinearRect(cell, 6.0f, theme::RaisedHi, 0.95f, theme::Raised, 0.90f, false);
            GlowLine(cell.left + 12.0f, cell.bottom - 1.5f, cell.right - 12.0f, cell.bottom - 1.5f,
                     ColSignalHi(), 2.0f, 5.5f, 0.92f);
        } else if (Hovered(actId)) {
            Fill(cell, 6.0f, theme::Raised, 0.65f);
        }
        Text(SeqTriggerModeName(static_cast<uint8_t>(m)), sel ? gpu_.fBodyStrong : gpu_.fBody, cell,
             (sel || Hovered(actId)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(cell, actId);
    }

    const wchar_t* humVal = kHumanizePresets[0].label;
    for (int i = 0; i < kHumanizeCount; ++i) {
        if (seq.humanizePct == kHumanizePresets[i].pct) {
            humVal = kHumanizePresets[i].label;
            break;
        }
    }
    wchar_t humBtnLbl[32]{};
    std::swprintf(humBtnLbl, std::size(humBtnLbl), L"Timing: %ls", humVal);
    const float humW = std::ceil(MeasureWidth(humBtnLbl, gpu_.fSmall)) + 18.0f;
    Button(Rc(R - humW, y + 4.0f, R, y + 30.0f), humBtnLbl, act::SeqCycleHumanize,
           seq.humanizePct > 0U ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
    y += kTrigRowH + 12.0f;

    // ---- Two-Column Studio: Workflow Timeline (left) | Step Inspector (right) -----
    const float wsBot = panel.bottom - 14.0f;
    const float innerW = R - L;
    const float colGap = 14.0f;
    const float leftW = std::clamp((innerW - colGap) * 0.54f, 316.0f, 450.0f);
    const D2D1_RECT_F leftCard  = Rc(L, y, L + leftW, wsBot);
    const D2D1_RECT_F rightCard = Rc(leftCard.right + colGap, y, R, wsBot);

    GlassCard(leftCard, 10.0f, true, false, theme::GlassBorder, ColSignal(), 0.16f);

    const int stepCount = std::clamp<int>(seq.stepCount, 0, kComplexStepMax);
    const bool hasSelStep = (stepCount > 0 && u.selectedSeqStep >= 0 && u.selectedSeqStep < stepCount);

    // Left Card Header
    Text(L"Steps", gpu_.fBodyStrong, RcW(leftCard.left + 14.0f, leftCard.top + 8.0f, 48.0f, 24.0f), theme::Text);
    wchar_t cntBuf[24]{};
    std::swprintf(cntBuf, std::size(cntBuf), L"%u / %u",
                  static_cast<unsigned>(seq.stepCount), static_cast<unsigned>(kComplexStepMax));
    Text(cntBuf, gpu_.fSmall, RcW(leftCard.left + 62.0f, leftCard.top + 8.0f, 70.0f, 24.0f), theme::Faint);

    Button(Rc(leftCard.right - 72.0f, leftCard.top + 8.0f, leftCard.right - 10.0f, leftCard.top + 32.0f),
           L"+ Add", act::SeqStepAdd, BtnStyle::Primary, gpu_.fSmall);
    Button(Rc(leftCard.right - 130.0f, leftCard.top + 8.0f, leftCard.right - 78.0f, leftCard.top + 32.0f),
           L"Insert", act::SeqStepInsert, BtnStyle::Ghost, gpu_.fSmall);

    Line(leftCard.left + 1.0f, Snap(leftCard.top + 40.0f), leftCard.right - 1.0f, Snap(leftCard.top + 40.0f), theme::Line, 1.0f, 0.72f);

    seqListRect_ = Rc(leftCard.left + 6.0f, leftCard.top + 42.0f, leftCard.right - 6.0f, leftCard.bottom - 6.0f);
    const float rowPitch = kSeqStepRowH + 4.0f;
    const float contentH = static_cast<float>(stepCount) * rowPitch + 8.0f;
    const float viewH = seqListRect_.bottom - seqListRect_.top;
    seqStepMaxScroll_ = std::max(0.0f, contentH - viewH);
    const float seqScrollSrc = (a.seqScrollSmooth.value > 0.0f || a.seqScrollSmooth.target > 0.0f)
        ? a.seqScrollSmooth.value : u.seqStepScroll;
    const float seqScroll = std::clamp(seqScrollSrc, 0.0f, seqStepMaxScroll_);

    if (stepCount == 0) {
        const float midY = (seqListRect_.top + seqListRect_.bottom) * 0.5f - 18.0f;
        Text(L"No steps yet", gpu_.fBodyStrong, Rc(seqListRect_.left + 12.0f, midY, seqListRect_.right - 12.0f, midY + 20.0f),
             theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"Click + Add above to create your first step.", gpu_.fSmall,
             Rc(seqListRect_.left + 12.0f, midY + 20.0f, seqListRect_.right - 12.0f, midY + 38.0f),
             theme::Faint, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        PushClip(seqListRect_);
        float sy = seqListRect_.top + 4.0f - seqScroll;
        enum class ScopeKind : uint8_t { Loop = 0, Cond = 1 };
        ScopeKind scopeStack[16]{};
        int scopeDepth = 0;

        constexpr float kIndentStep = 20.0f;
        const float availW = (seqListRect_.right - 8.0f) - (seqListRect_.left + 2.0f);
        const int maxVisDepth = std::max(0, std::min(5, static_cast<int>((availW - 170.0f) / kIndentStep)));

        for (int i = 0; i < stepCount; ++i) {
            const auto& step = seq.steps[i];
            const auto kind = static_cast<ComplexStepKind>(step.kind);

            bool closedScope = false;
            UINT32 closedCol = theme::Violet;
            if (kind == ComplexStepKind::LoopEnd) {
                if (scopeDepth > 0 && scopeStack[scopeDepth - 1] == ScopeKind::Loop) {
                    --scopeDepth;
                    closedScope = true;
                } else {
                    for (int p = scopeDepth - 1; p >= 0; --p) {
                        if (scopeStack[p] == ScopeKind::Loop) {
                            scopeDepth = p;
                            closedScope = true;
                            break;
                        }
                    }
                }
                closedCol = theme::Violet;
            } else if (kind == ComplexStepKind::EndIf) {
                if (scopeDepth > 0 && scopeStack[scopeDepth - 1] == ScopeKind::Cond) {
                    --scopeDepth;
                    closedScope = true;
                } else {
                    for (int p = scopeDepth - 1; p >= 0; --p) {
                        if (scopeStack[p] == ScopeKind::Cond) {
                            scopeDepth = p;
                            closedScope = true;
                            break;
                        }
                    }
                }
                closedCol = theme::Cyan;
            }

            int displayDepth = scopeDepth;
            const bool isElseAligned = (kind == ComplexStepKind::ElseBranch &&
                                        scopeDepth > 0 &&
                                        scopeStack[scopeDepth - 1] == ScopeKind::Cond);
            if (isElseAligned) {
                displayDepth = scopeDepth - 1;
            }

            const int visDepth = std::min(displayDepth, maxVisDepth);
            const float indent = static_cast<float>(visDepth) * kIndentStep;

            const D2D1_RECT_F rr = Rc(seqListRect_.left + 2.0f, sy, seqListRect_.right - 8.0f, sy + kSeqStepRowH);
            if (rr.bottom >= seqListRect_.top && rr.top <= seqListRect_.bottom) {
                const D2D1_RECT_F cardR = Rc(rr.left + indent, rr.top, rr.right, rr.bottom);
                for (int d = 0; d < visDepth; ++d) {
                    const UINT32 railCol = (scopeStack[d] == ScopeKind::Cond) ? theme::Cyan : theme::Violet;
                    const float gx = Snap(rr.left + 8.0f + static_cast<float>(d) * kIndentStep);
                    Line(gx, rr.top - 2.0f, gx, rr.bottom + 2.0f, railCol, 1.5f, 0.62f);
                }
                if (visDepth > 0) {
                    const UINT32 innerCol = (scopeStack[visDepth - 1] == ScopeKind::Cond) ? theme::Cyan : theme::Violet;
                    const float gx = Snap(rr.left + 8.0f + static_cast<float>(visDepth - 1) * kIndentStep);
                    const float midY = Snap((rr.top + rr.bottom) * 0.5f);
                    Line(gx, midY, cardR.left - 1.0f, midY, innerCol, 1.5f, 0.58f);
                }
                if ((kind == ComplexStepKind::LoopStart || kind == ComplexStepKind::IfCheck) && displayDepth < maxVisDepth) {
                    const UINT32 startCol = (kind == ComplexStepKind::IfCheck) ? theme::Cyan : theme::Violet;
                    const float gx = Snap(rr.left + 8.0f + static_cast<float>(displayDepth) * kIndentStep);
                    Line(gx, rr.bottom, gx, rr.bottom + 4.0f, startCol, 1.5f, 0.62f);
                }
                if (closedScope && displayDepth < maxVisDepth) {
                    const float gx = Snap(rr.left + 8.0f + static_cast<float>(displayDepth) * kIndentStep);
                    Line(gx, rr.top - 4.0f, gx, rr.top, closedCol, 1.5f, 0.62f);
                }
                if (isElseAligned && displayDepth < maxVisDepth) {
                    const float gx = Snap(rr.left + 8.0f + static_cast<float>(displayDepth) * kIndentStep);
                    Line(gx, rr.top - 4.0f, gx, rr.top, theme::Cyan, 1.5f, 0.62f);
                    Line(gx, rr.bottom, gx, rr.bottom + 4.0f, theme::Cyan, 1.5f, 0.62f);
                }

                const bool sel = (i == u.selectedSeqStep);
                const bool isLive = running && (execStep == static_cast<uint16_t>(i));
                const int actId = act::SelectSeqStep + i;

                if (isLive) {
                    gpu_.FillLinearRect(cardR, 6.0f, ColSignal(), 0.28f, ColSecondary(), 0.14f, true);
                    Stroke(cardR, 6.0f, ColSignalHi(), 1.3f, 0.95f);
                } else if (sel) {
                    gpu_.FillLinearRect(cardR, 6.0f, theme::RaisedHi, 0.92f, theme::Raised, 0.88f, true);
                    Stroke(cardR, 6.0f, ColSignal(), 1.1f, 0.72f);
                } else if (Hovered(actId)) {
                    Fill(cardR, 6.0f, theme::Raised, 0.80f);
                } else {
                    Fill(cardR, 6.0f, theme::Panel, 0.72f);
                    if (kind == ComplexStepKind::LoopStart || kind == ComplexStepKind::LoopEnd) {
                        Fill(cardR, 6.0f, theme::Violet, 0.09f);
                    } else if (kind == ComplexStepKind::IfCheck ||
                               kind == ComplexStepKind::ElseBranch ||
                               kind == ComplexStepKind::EndIf) {
                        Fill(cardR, 6.0f, theme::Cyan, 0.09f);
                    }
                }
                Hit(cardR, actId);

                UINT32 catCol = ColSecondary();
                if (kind == ComplexStepKind::WaitMs || kind == ComplexStepKind::WaitForColor) catCol = theme::Warn;
                else if (kind == ComplexStepKind::LoopStart || kind == ComplexStepKind::LoopEnd) catCol = theme::Violet;
                else if (kind == ComplexStepKind::IfCheck || kind == ComplexStepKind::ElseBranch || kind == ComplexStepKind::EndIf) catCol = theme::Cyan;
                else if (kind == ComplexStepKind::LaunchProgram || kind == ComplexStepKind::RunCommand ||
                         kind == ComplexStepKind::SetLayer || kind == ComplexStepKind::ToggleMacro) catCol = theme::Ok;

                if (isLive || sel) {
                    GlowLine(cardR.left + 8.0f, cardR.top + 8.0f, cardR.left + 8.0f, cardR.bottom - 8.0f,
                             catCol, 2.5f, 6.5f, 0.92f);
                } else {
                    Fill(RcW(cardR.left + 7.0f, cardR.top + 8.0f, 3.2f, kSeqStepRowH - 16.0f), 1.6f, catCol);
                }

                wchar_t numBuf[8]{};
                std::swprintf(numBuf, std::size(numBuf), L"%d", i + 1);
                Text(numBuf, gpu_.fAxis, RcW(cardR.left + 13.0f, cardR.top, 24.0f, kSeqStepRowH),
                     isLive ? ColSignalHi() : theme::Faint, DWRITE_TEXT_ALIGNMENT_CENTER);

                float descRight = cardR.right - 10.0f;
                const bool hasColorSwatch = (kind == ComplexStepKind::WaitForColor) ||
                    (kind == ComplexStepKind::IfCheck &&
                     mask::core::GetStepConditionKind(step) == ComplexConditionKind::ColorMatches);
                if (hasColorSwatch) {
                    const UINT32 rgb = (static_cast<UINT32>(step.r) << 16) |
                                       (static_cast<UINT32>(step.g) << 8)  |
                                       static_cast<UINT32>(step.b);
                    const D2D1_RECT_F swR = RcW(cardR.right - 22.0f, cardR.top + 12.0f, 14.0f, 14.0f);
                    Fill(swR, 4.0f, rgb);
                    Stroke(swR, 4.0f, theme::LineHi);
                    descRight = swR.left - 8.0f;
                }

                wchar_t desc[160]{};
                DescribeComplexStep(step, desc, std::size(desc));
                Text(desc, sel ? gpu_.fBodyStrong : gpu_.fBody, Rc(cardR.left + 40.0f, cardR.top, descRight, cardR.bottom),
                     (sel || isLive) ? theme::White : theme::Text);
            }

            if (kind == ComplexStepKind::LoopStart && scopeDepth < 16) {
                scopeStack[scopeDepth++] = ScopeKind::Loop;
            } else if (kind == ComplexStepKind::IfCheck && scopeDepth < 16) {
                scopeStack[scopeDepth++] = ScopeKind::Cond;
            }
            sy += rowPitch;
        }
        PopClip();

        if (seqStepMaxScroll_ > 0.5f) {
            const float trackH = viewH - 8.0f;
            const float thumbH = std::max(28.0f, trackH * (viewH / contentH));
            const float t = seqScroll / seqStepMaxScroll_;
            Fill(RcW(leftCard.right - 5.0f, seqListRect_.top + 4.0f + t * (trackH - thumbH), 3.0f, thumbH), 1.5f, theme::LineHi, 0.80f);
        }
    }

    // ---- Right Card: Selected Step Inspector --------------------------------------
    GlassCard(rightCard, 10.0f, true, false, theme::GlassBorderHi, ColSignal(), 0.18f);

    if (!hasSelStep) {
        Text(L"Select a step on the left, or click + Add.", gpu_.fBody,
             Rc(rightCard.left + 16.0f, rightCard.top, rightCard.right - 16.0f, rightCard.bottom),
             theme::Faint, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    const ComplexStep& st = seq.steps[u.selectedSeqStep];
    const auto kind = static_cast<ComplexStepKind>(st.kind);
    const float iL = rightCard.left + 14.0f;
    const float iR = rightCard.right - 14.0f;
    const float iW = iR - iL;
    float iy = rightCard.top + 12.0f;

    wchar_t stepLbl[24]{};
    std::swprintf(stepLbl, std::size(stepLbl), L"Step %d", u.selectedSeqStep + 1);
    Text(stepLbl, gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
    iy += 20.0f;

    {
        seqStepKindBtnRect_ = RcW(iL, iy, iW, 32.0f);
        const D2D1_RECT_F sr = a.ApplySqueeze(seqStepKindBtnRect_, act::SeqToggleStepKindMenu);
        const bool open = u.showStepKindMenu;
        const bool hov = Hovered(act::SeqToggleStepKindMenu);
        Fill(sr, 7.0f, (open || hov) ? theme::RaisedHi : theme::Raised, 0.92f);
        Stroke(sr, 7.0f, open ? ColSignal() : (hov ? theme::LineHi : theme::Line), open ? 1.4f : 1.0f);
        Text(StepKindName(kind), gpu_.fBodyStrong, Rc(sr.left + 12.0f, sr.top, sr.right - 28.0f, sr.bottom),
             open ? ColSignalHi() : theme::Text);
        Text(L"\u25BE", gpu_.fBodyStrong, Rc(sr.right - 26.0f, sr.top, sr.right - 8.0f, sr.bottom),
             open ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(seqStepKindBtnRect_, act::SeqToggleStepKindMenu);
    }

    iy += 42.0f;
    Line(iL, Snap(iy), iR, Snap(iy), theme::Line, 1.0f, 0.72f);
    iy += 12.0f;

    wchar_t vbuf[48]{};
    switch (kind) {
        case ComplexStepKind::KeyClick: {
            Text(L"Key", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            KeyChip(RcW(iL, iy + 22.0f, iW, 34.0f), st.vk, act::SeqStepBindKey, bindTarget == 21U);
            Text(L"Hold for", gpu_.fSmall, RcW(iL, iy + 68.0f, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%u ms", static_cast<unsigned>(st.durationMs));
            GenericField(RcW(iL, iy + 90.0f, iW, 34.0f), Field::SeqStepDuration, vbuf, true);
            break;
        }
        case ComplexStepKind::KeyDown:
        case ComplexStepKind::KeyUp: {
            Text(L"Key", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            KeyChip(RcW(iL, iy + 22.0f, iW, 34.0f), st.vk, act::SeqStepBindKey, bindTarget == 21U);
            Text(kind == ComplexStepKind::KeyDown
                     ? L"Keeps this key pressed until a Release key step or Stop."
                     : L"Releases a key previously held down.",
                 gpu_.fSmall, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::MouseMoveRel:
        case ComplexStepKind::MouseMoveAbs: {
            const bool rel = (kind == ComplexStepKind::MouseMoveRel);
            const float hw = (iW - 10.0f) * 0.5f;
            Text(rel ? L"Move X" : L"Screen X", gpu_.fSmall, RcW(iL, iy, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.x));
            GenericField(RcW(iL, iy + 22.0f, hw, 34.0f), Field::SeqStepX, vbuf, true);

            Text(rel ? L"Move Y" : L"Screen Y", gpu_.fSmall, RcW(iL + hw + 10.0f, iy, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.y));
            GenericField(RcW(iL + hw + 10.0f, iy + 22.0f, hw, 34.0f), Field::SeqStepY, vbuf, true);

            Text(rel ? L"Positive right / down, negative left / up." : L"Moves pointer to exact screen coordinates.",
                 gpu_.fSmall, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::MouseScroll: {
            const int32_t delta = static_cast<int32_t>(st.param != 0U ? static_cast<int32_t>(st.param) : st.y);
            Text(L"Scroll amount", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%+d", static_cast<int>(delta));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepParam, vbuf, true);
            const float hw = (iW - 8.0f) * 0.5f;
            Button(RcW(iL, iy + 66.0f, hw, 28.0f), L"Scroll down", act::SeqStepQuickScroll + 0, BtnStyle::Chip, gpu_.fSmall);
            Button(RcW(iL + hw + 8.0f, iy + 66.0f, hw, 28.0f), L"Scroll up", act::SeqStepQuickScroll + 1, BtnStyle::Chip, gpu_.fSmall);
            break;
        }
        case ComplexStepKind::WaitMs: {
            Text(L"Wait for", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%u ms", static_cast<unsigned>(st.durationMs));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepDuration, vbuf, true);
            Text(L"Quick presets", gpu_.fSmall, RcW(iL, iy + 66.0f, iW, 18.0f), theme::Faint);
            static constexpr const wchar_t* kWaitLabels[4] = { L"10 ms", L"50 ms", L"250 ms", L"1 s" };
            const float hw = (iW - 8.0f) * 0.5f;
            for (int w = 0; w < 4; ++w) {
                const float px = iL + static_cast<float>(w % 2) * (hw + 8.0f);
                const float py = iy + 88.0f + static_cast<float>(w / 2) * 34.0f;
                Button(RcW(px, py, hw, 28.0f), kWaitLabels[w], act::SeqStepQuickWait + w, BtnStyle::Chip, gpu_.fSmall);
            }
            break;
        }
        case ComplexStepKind::WaitForColor: {
            const UINT32 rgb = (static_cast<UINT32>(st.r) << 16) | (static_cast<UINT32>(st.g) << 8) | static_cast<UINT32>(st.b);
            Fill(RcW(iL, iy, 32.0f, 32.0f), 6.0f, rgb);
            Stroke(RcW(iL, iy, 32.0f, 32.0f), 6.0f, theme::LineHi);
            Button(Rc(iL + 40.0f, iy, iR, iy + 32.0f),
                   u.seqColorPickActive ? L"Click screen\u2026" : L"Pick color & spot",
                   act::SeqStepPickColor, u.seqColorPickActive ? BtnStyle::Primary : BtnStyle::Secondary, gpu_.fSmall);

            const float hw = (iW - 10.0f) * 0.5f;
            const float r2 = iy + 44.0f;
            Text(L"Screen X", gpu_.fSmall, RcW(iL, r2, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.x));
            GenericField(RcW(iL, r2 + 20.0f, hw, 32.0f), Field::SeqStepX, vbuf, false);

            Text(L"Screen Y", gpu_.fSmall, RcW(iL + hw + 10.0f, r2, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.y));
            GenericField(RcW(iL + hw + 10.0f, r2 + 20.0f, hw, 32.0f), Field::SeqStepY, vbuf, false);

            const float r3 = r2 + 62.0f;
            Text(L"Leeway", gpu_.fSmall, RcW(iL, r3, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"\u00B1%u", static_cast<unsigned>(st.tolerance));
            GenericField(RcW(iL, r3 + 20.0f, hw, 32.0f), Field::SeqStepTol, vbuf, true);

            Text(L"Timeout", gpu_.fSmall, RcW(iL + hw + 10.0f, r3, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%u ms", static_cast<unsigned>(st.param));
            GenericField(RcW(iL + hw + 10.0f, r3 + 20.0f, hw, 32.0f), Field::SeqStepParam, vbuf, false);
            break;
        }
        case ComplexStepKind::LoopStart: {
            Text(L"Repeat count", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            if (st.param == 0U) wcscpy_s(vbuf, L"Forever");
            else std::swprintf(vbuf, std::size(vbuf), L"%u\u00D7", static_cast<unsigned>(st.param));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepParam, vbuf, true);
            Text(L"0 repeats forever until stopped. End loop is added automatically.",
                 gpu_.fSmall, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::LoopEnd: {
            Text(L"Ends the matching Start loop step above.", gpu_.fBody,
                 Rc(iL, iy + 4.0f, iR, iy + 32.0f), theme::Muted);
            break;
        }
        case ComplexStepKind::IfCheck: {
            const auto cond = mask::core::GetStepConditionKind(st);
            const bool neg = mask::core::IsStepConditionNegated(st);
            Text(L"Condition", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            const float cgGap = 6.0f;
            const float cw = (iW - 2.0f * cgGap) / 3.0f;
            for (int c = 0; c < 6; ++c) {
                const float cx = iL + static_cast<float>(c % 3) * (cw + cgGap);
                const float cy = iy + 22.0f + static_cast<float>(c / 3) * 30.0f;
                const bool active = (cond == static_cast<ComplexConditionKind>(c));
                Button(RcW(cx, cy, cw, 26.0f),
                       ConditionKindName(static_cast<ComplexConditionKind>(c)),
                       act::SeqStepCondKind + c,
                       active ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            }

            float cyBody = iy + 88.0f;
            switch (cond) {
                case ComplexConditionKind::KeyHeld: {
                    Text(L"Key to check", gpu_.fSmall, RcW(iL, cyBody, iW, 18.0f), theme::Muted);
                    KeyChip(RcW(iL, cyBody + 20.0f, iW, 34.0f), st.vk, act::SeqStepBindKey, bindTarget == 21U);
                    const float hw = (iW - 8.0f) * 0.5f;
                    Button(RcW(iL, cyBody + 62.0f, hw, 28.0f), L"Is held", act::SeqStepCondNegate + 0,
                           !neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    Button(RcW(iL + hw + 8.0f, cyBody + 62.0f, hw, 28.0f), L"Is NOT held", act::SeqStepCondNegate + 1,
                           neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    cyBody += 100.0f;
                    break;
                }
                case ComplexConditionKind::ColorMatches: {
                    const UINT32 rgb = (static_cast<UINT32>(st.r) << 16) |
                                       (static_cast<UINT32>(st.g) << 8)  |
                                       static_cast<UINT32>(st.b);
                    Fill(RcW(iL, cyBody, 32.0f, 32.0f), 6.0f, rgb);
                    Stroke(RcW(iL, cyBody, 32.0f, 32.0f), 6.0f, theme::LineHi);
                    Button(Rc(iL + 40.0f, cyBody, iR, cyBody + 32.0f),
                           u.seqColorPickActive ? L"Click screen\u2026" : L"Pick color & spot",
                           act::SeqStepPickColor,
                           u.seqColorPickActive ? BtnStyle::Primary : BtnStyle::Secondary, gpu_.fSmall);

                    const float hw = (iW - 10.0f) * 0.5f;
                    const float r2 = cyBody + 40.0f;
                    Text(L"Screen X", gpu_.fSmall, RcW(iL, r2, hw, 16.0f), theme::Muted);
                    std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.x));
                    GenericField(RcW(iL, r2 + 18.0f, hw, 30.0f), Field::SeqStepX, vbuf, false);

                    Text(L"Screen Y", gpu_.fSmall, RcW(iL + hw + 10.0f, r2, hw, 16.0f), theme::Muted);
                    std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.y));
                    GenericField(RcW(iL + hw + 10.0f, r2 + 18.0f, hw, 30.0f), Field::SeqStepY, vbuf, false);

                    const float r3 = r2 + 54.0f;
                    Text(L"Leeway", gpu_.fSmall, RcW(iL, r3, hw, 16.0f), theme::Muted);
                    std::swprintf(vbuf, std::size(vbuf), L"\u00B1%u", static_cast<unsigned>(st.tolerance));
                    GenericField(RcW(iL, r3 + 18.0f, hw, 30.0f), Field::SeqStepTol, vbuf, true);

                    Text(L"Mode", gpu_.fSmall, RcW(iL + hw + 10.0f, r3, hw, 16.0f), theme::Muted);
                    const float bw = (hw - 6.0f) * 0.5f;
                    Button(RcW(iL + hw + 10.0f, r3 + 18.0f, bw, 30.0f), L"Matches", act::SeqStepCondNegate + 0,
                           !neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    Button(RcW(iL + hw + 10.0f + bw + 6.0f, r3 + 18.0f, bw, 30.0f), L"Not", act::SeqStepCondNegate + 1,
                           neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    cyBody = r3 + 56.0f;
                    break;
                }
                case ComplexConditionKind::LayerIs: {
                    Text(L"Active layer", gpu_.fSmall, RcW(iL, cyBody, iW, 18.0f), theme::Muted);
                    const float lw = (iW - 15.0f) / 4.0f;
                    for (int l = 0; l < kRemapLayerCount; ++l) {
                        Button(RcW(iL + static_cast<float>(l) * (lw + 5.0f), cyBody + 20.0f, lw, 28.0f),
                               LayerName(static_cast<uint8_t>(l)), act::SeqStepLayer + l,
                               (st.param & 3U) == static_cast<uint32_t>(l) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    }
                    const float hw = (iW - 8.0f) * 0.5f;
                    Button(RcW(iL, cyBody + 56.0f, hw, 28.0f), L"Is active", act::SeqStepCondNegate + 0,
                           !neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    Button(RcW(iL + hw + 8.0f, cyBody + 56.0f, hw, 28.0f), L"Is NOT active", act::SeqStepCondNegate + 1,
                           neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    cyBody += 94.0f;
                    break;
                }
                case ComplexConditionKind::UnlockStateIs: {
                    Text(L"Control lock state", gpu_.fSmall, RcW(iL, cyBody, iW, 18.0f), theme::Muted);
                    const float hw = (iW - 8.0f) * 0.5f;
                    Button(RcW(iL, cyBody + 20.0f, hw, 30.0f), L"Unlocked", act::SeqStepCondUnlock + 0,
                           (st.param == 0U) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    Button(RcW(iL + hw + 8.0f, cyBody + 20.0f, hw, 30.0f), L"Locked", act::SeqStepCondUnlock + 1,
                           (st.param != 0U) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    cyBody += 60.0f;
                    break;
                }
                case ComplexConditionKind::MacroActive: {
                    const uint32_t slotIdx = st.param % static_cast<uint32_t>(kSlotCount);
                    Text(L"Macro slot", gpu_.fSmall, RcW(iL, cyBody, iW, 18.0f), theme::Muted);
                    std::swprintf(vbuf, std::size(vbuf), L"#%u", static_cast<unsigned>(slotIdx + 1U));
                    GenericField(RcW(iL, cyBody + 20.0f, iW, 32.0f), Field::SeqStepParam, vbuf, true);
                    wchar_t mLabel[64]{};
                    std::swprintf(mLabel, std::size(mLabel), L"Target: \u201C%ls\u201D", u.slots[slotIdx].name);
                    Text(mLabel, gpu_.fSmall, Rc(iL, cyBody + 56.0f, iR, cyBody + 74.0f), theme::Muted);
                    const float hw = (iW - 8.0f) * 0.5f;
                    Button(RcW(iL, cyBody + 80.0f, hw, 28.0f), L"Is running", act::SeqStepCondNegate + 0,
                           !neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    Button(RcW(iL + hw + 8.0f, cyBody + 80.0f, hw, 28.0f), L"Not running", act::SeqStepCondNegate + 1,
                           neg ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    cyBody += 118.0f;
                    break;
                }
                case ComplexConditionKind::RandomChance: {
                    const uint32_t pct = (std::min)(st.param, 100U);
                    Text(L"Chance", gpu_.fSmall, RcW(iL, cyBody, iW, 18.0f), theme::Muted);
                    std::swprintf(vbuf, std::size(vbuf), L"%u%%", static_cast<unsigned>(pct));
                    GenericField(RcW(iL, cyBody + 20.0f, iW, 32.0f), Field::SeqStepParam, vbuf, true);
                    static constexpr const wchar_t* kPctLabels[4] = { L"10%", L"25%", L"50%", L"75%" };
                    static constexpr uint32_t kPctVals[4] = { 10U, 25U, 50U, 75U };
                    const float qw = (iW - 18.0f) / 4.0f;
                    for (int p = 0; p < 4; ++p) {
                        Button(RcW(iL + static_cast<float>(p) * (qw + 6.0f), cyBody + 60.0f, qw, 28.0f),
                               kPctLabels[p], act::SeqStepCondQuickPct + p,
                               pct == kPctVals[p] ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                    }
                    cyBody += 98.0f;
                    break;
                }
            }

            if (!HasElseInIfBlock(seq, u.selectedSeqStep) && stepCount < kComplexStepMax) {
                Button(RcW(iL, cyBody, iW, 28.0f), L"+ Add Else branch", act::SeqStepCondAddElse,
                       BtnStyle::Secondary, gpu_.fSmall);
            } else {
                Text(L"Indented steps run when true; Else runs when false.", gpu_.fSmall,
                     Rc(iL, cyBody, iR, cyBody + 22.0f), theme::Faint);
            }
            break;
        }
        case ComplexStepKind::ElseBranch: {
            Text(L"Runs the indented steps below when the If check above is false.", gpu_.fBody,
                 Rc(iL, iy + 4.0f, iR, iy + 44.0f), theme::Muted);
            break;
        }
        case ComplexStepKind::EndIf: {
            Text(L"Ends the matching If check block above.", gpu_.fBody,
                 Rc(iL, iy + 4.0f, iR, iy + 32.0f), theme::Muted);
            break;
        }
        case ComplexStepKind::LaunchProgram:
        case ComplexStepKind::RunCommand: {
            const bool isApp = (kind == ComplexStepKind::LaunchProgram);
            Text(isApp ? L"Application path" : L"Command", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepText, st.text, false, true,
                         isApp ? L"notepad.exe or C:\\..." : L"powershell -c \"...\"");
            Text(L"Click to type, or press Ctrl+V to paste.", gpu_.fSmall,
                 Rc(iL, iy + 66.0f, iR, iy + 86.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::SetLayer: {
            Text(L"Switch to layer", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            const float lw = (iW - 15.0f) / 4.0f;
            for (int l = 0; l < kRemapLayerCount; ++l) {
                Button(RcW(iL + static_cast<float>(l) * (lw + 5.0f), iy + 22.0f, lw, 30.0f),
                       LayerName(static_cast<uint8_t>(l)), act::SeqStepLayer + l,
                       (st.param & 3U) == static_cast<uint32_t>(l) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            }
            break;
        }
        case ComplexStepKind::ToggleMacro: {
            const uint32_t slotIdx = st.param % static_cast<uint32_t>(kSlotCount);
            Text(L"Macro slot", gpu_.fSmall, RcW(iL, iy, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"#%u", static_cast<unsigned>(slotIdx + 1U));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepParam, vbuf, true);
            wchar_t mLabel[64]{};
            std::swprintf(mLabel, std::size(mLabel), L"Target: \u201C%ls\u201D", u.slots[slotIdx].name);
            Text(mLabel, gpu_.fBody, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Muted);
            break;
        }
    }

    // Bottom Pinned Inspector Actions
    const float footY = rightCard.bottom - 42.0f;
    Line(iL, Snap(footY - 9.0f), iR, Snap(footY - 9.0f), theme::Line, 1.0f, 0.72f);
    const float fw = (iW - 18.0f) / 4.0f;
    Button(RcW(iL + 0.0f * (fw + 6.0f), footY, fw, 30.0f), L"\u2191 Up",   act::SeqStepUp,   BtnStyle::Ghost,  gpu_.fSmall);
    Button(RcW(iL + 1.0f * (fw + 6.0f), footY, fw, 30.0f), L"\u2193 Down", act::SeqStepDown, BtnStyle::Ghost,  gpu_.fSmall);
    Button(RcW(iL + 2.0f * (fw + 6.0f), footY, fw, 30.0f), L"Copy",        act::SeqStepDup,  BtnStyle::Ghost,  gpu_.fSmall);
    Button(RcW(iL + 3.0f * (fw + 6.0f), footY, fw, 30.0f), L"Delete",      act::SeqStepDel,  BtnStyle::Danger, gpu_.fSmall);
}

inline void UiRenderer::DrawStepKindPopover(float W, float H) noexcept {
    const auto& u = Ui();
    Hit(Rc(0.0f, 0.0f, W, H), act::PopoverBackdrop);

    const auto& seq = u.SelectedSeq();
    const uint8_t curKind = (u.selectedSeqStep >= 0 && u.selectedSeqStep < static_cast<int>(seq.stepCount))
        ? seq.steps[u.selectedSeqStep].kind : 0U;

    constexpr float popW = 438.0f;
    constexpr float popH = 236.0f;
    const float anchorR = (seqStepKindBtnRect_.right > seqStepKindBtnRect_.left)
        ? seqStepKindBtnRect_.right : (W + popW) * 0.5f;
    const float px = std::clamp(anchorR - popW, 12.0f, std::max(12.0f, W - popW - 12.0f));
    float py = seqStepKindBtnRect_.bottom + 6.0f;
    if (py + popH > H - 12.0f) {
        py = std::max(60.0f, seqStepKindBtnRect_.top - popH - 6.0f);
    }

    const D2D1_RECT_F pop = RcW(px, py, popW, popH);
    Fill(Rc(pop.left - 3.0f, pop.top - 3.0f, pop.right + 3.0f, pop.bottom + 7.0f), 12.0f, theme::Window, 0.58f);
    gpu_.FillGlassCard(pop, 10.0f, theme::GlassModalTop, theme::GlassModalBot, theme::GlassBorderHi, 0.24f,
                       Anim().cursorX.value, Anim().cursorY.value, true, ColSignal());
    Hit(pop, act::ModalSurface);

    struct ColGroup {
        const wchar_t* title;
        UINT32         dotCol;
        int            kinds[7];
        int            count;
    };
    const ColGroup groups[3] = {
        { L"INPUT",         ColSecondary(), { 0, 1, 2, 3, 4, 5, -1 },       6 },
        { L"FLOW & CHECKS", theme::Cyan,    { 6, 7, 8, 9, 14, 15, 16 },     7 },
        { L"SYSTEM",        theme::Ok,      { 10, 11, 12, 13, -1, -1, -1 }, 4 }
    };

    const float pad = 12.0f;
    const float gap = 8.0f;
    const float colW = (popW - 2.0f * pad - 2.0f * gap) / 3.0f;

    for (int c = 0; c < 3; ++c) {
        const float cx = pop.left + pad + static_cast<float>(c) * (colW + gap);
        float cy = pop.top + 11.0f;
        Dot(cx + 8.0f, cy + 8.0f, 3.0f, groups[c].dotCol);
        Text(groups[c].title, gpu_.fMini, RcW(cx + 16.0f, cy, colW - 18.0f, 16.0f), theme::Faint);
        cy += 22.0f;

        for (int r = 0; r < groups[c].count; ++r) {
            const int k = groups[c].kinds[r];
            const int actId = act::SeqStepKindBase + k;
            const D2D1_RECT_F itemR = RcW(cx, cy, colW, 25.0f);
            const bool active = (curKind == static_cast<uint8_t>(k));
            const bool hov = Hovered(actId);
            if (active) {
                Fill(itemR, 5.0f, ColSignal(), 0.18f);
                Stroke(itemR, 5.0f, ColSignal(), 1.0f, 0.85f);
            } else if (hov) {
                Fill(itemR, 5.0f, theme::Raised, 0.85f);
            }
            Text(StepKindName(static_cast<ComplexStepKind>(k)),
                 active ? gpu_.fBodyStrong : gpu_.fSmall,
                 Rc(itemR.left + 8.0f, itemR.top, itemR.right - 6.0f, itemR.bottom),
                 active ? ColSignalHi() : (hov ? theme::Text : theme::Muted));
            Hit(itemR, actId);
            cy += 27.0f;
        }
    }
}

// =====================================================================================
//  2. Reworked Multi-Device / Multi-Monitor Remapping Editor
// =====================================================================================

inline void UiRenderer::DrawRemapEditor(const D2D1_RECT_F& panel) noexcept {
    const auto& u = Ui();
    const int idx = std::clamp(u.selectedIndex, 0, kRemapRuleCount - 1);
    const auto& r = u.remaps[idx];
    const uint8_t bindTarget = frame_.bindTarget;
    const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Remap, idx));

    const float L = panel.left + 24.0f;
    const float R = panel.right - 24.0f;
    const float C = L + 110.0f;
    float y = panel.top + 14.0f;

    // ---- Header: inline rename | On/Off | Duplicate | Delete ----------------------
    const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
    Button(delBtn, u.pendingDeleteConfirm ? L"Confirm?" : L"Delete", act::DeleteItem,
           u.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

    const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
    Button(dupBtn, L"Duplicate", act::DuplicateItem, BtnStyle::Ghost);

    const float switchX = dupBtn.left - 16.0f - 36.0f;
    Switch(switchX, y + 8.0f, r.enabled, act::ToggleEnabled, r.enabled, animIdx);
    Text(r.enabled ? L"On" : L"Off", gpu_.fBody, Rc(switchX - 64.0f, y, switchX - 10.0f, y + 36.0f),
         r.enabled ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Hit(Rc(switchX - 64.0f, y, switchX, y + 36.0f), act::ToggleEnabled);

    const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 72.0f, y + 36.0f);
    const int nameA = act::FieldFocus + static_cast<int>(Field::RemapName);
    if (u.editField == Field::RemapName) {
        Fill(nameRect, 6.0f, theme::Inset, 0.92f);
        Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
        Text(u.editBuffer, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
        const float caretX = Snap(L + MeasureWidth(u.editBuffer, gpu_.fTitle) + 1.5f);
        Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignalHi(), 1.5f);
    } else {
        if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised, 0.72f);
        Text(r.name, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
    }
    Hit(nameRect, nameA);

    wchar_t sentence[200]{};
    DescribeRemapRule(r, sentence, std::size(sentence));
    Text(sentence, gpu_.fBody, Rc(L, y + 36.0f, R, y + 56.0f), r.sourceVk == 0 ? theme::Warn : theme::Muted);
    y += 60.0f;

    // ---- Live Layer & Lock State Strip --------------------------------------------
    const D2D1_RECT_F liveBar = Rc(L, y, R, y + 38.0f);
    GlassCard(liveBar, 9.0f, true, false, theme::GlassBorder, ColSecondary(), 0.14f);

    Text(L"Live Layer", gpu_.fSmall, Rc(liveBar.left + 12.0f, liveBar.top, liveBar.left + 84.0f, liveBar.bottom), theme::Muted);
    float lx = liveBar.left + 88.0f;
    for (int l = 0; l < kRemapLayerCount; ++l) {
        const bool active = (frame_.activeRemapLayer == static_cast<uint8_t>(l));
        Button(RcW(lx, liveBar.top + 6.0f, 54.0f, 26.0f), LayerName(static_cast<uint8_t>(l)),
               act::RemapLiveLayer + l, active ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
        lx += 60.0f;
    }

    const D2D1_RECT_F lockBtn = Rc(liveBar.right - 116.0f, liveBar.top + 6.0f, liveBar.right - 10.0f, liveBar.bottom - 6.0f);
    Button(lockBtn, frame_.remapUnlocked ? L"Unlocked" : L"Locked",
           act::RemapToggleUnlockLive, frame_.remapUnlocked ? BtnStyle::Secondary : BtnStyle::ChipOn, gpu_.fSmall);
    y = liveBar.bottom + 10.0f;

    // ---- Cosmic Hero Mapping Banner: [Source Key] ---> [Mapped Target / Action] ---
    const auto actKind = static_cast<RemapLayerAction>(r.actionKind);
    const D2D1_RECT_F hero = Rc(L, y, R, y + 84.0f);
    GlassCard(hero, 11.0f, true, r.enabled,
              r.enabled ? mask::ui::Rgba(ColSignal(), 0.60f) : theme::GlassBorder,
              ColSignal(), 0.22f);

    const float hL = hero.left + 16.0f;
    const float hR = hero.right - 16.0f;
    const float midX = (hL + hR) * 0.5f;
    const float srcW = std::clamp((hR - hL) * 0.34f, 148.0f, 210.0f);

    Text(L"SOURCE KEY", gpu_.fMini, RcW(hL, hero.top + 10.0f, srcW, 14.0f), theme::Faint);
    const D2D1_RECT_F srcBox = RcW(hL, hero.top + 30.0f, srcW, 38.0f);
    KeyChip(srcBox, r.sourceVk, act::RemapBindSource, bindTarget == 22U);

    // Center glowing directional arrow & action tag
    const float arrowL = srcBox.right + 16.0f;
    const float arrowR = hR - srcW - 16.0f;
    const float arrowLen = std::max(36.0f, arrowR - arrowL);
    const float arrowY = Snap(srcBox.top + 19.0f);
    ArrowRight(arrowL, arrowY, arrowLen, r.enabled ? ColSignalHi() : theme::LineHi, 2.0f, r.enabled);
    Text(RemapActionName(actKind), gpu_.fMini, Rc(arrowL, hero.top + 10.0f, arrowR, hero.top + 26.0f),
         r.enabled ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);

    const float tgtX = hR - srcW;
    const float tgtW = srcW;
    const int monCount = std::max(1, std::min(u.monitorCount, 6));
    if (actKind == RemapLayerAction::RemapKey) {
        Text(L"MAPPED TARGET", gpu_.fMini, RcW(tgtX, hero.top + 10.0f, tgtW, 14.0f), theme::Faint);
        KeyChip(RcW(tgtX, hero.top + 30.0f, tgtW, 38.0f), r.targetVk, act::RemapBindTarget, bindTarget == 23U);
    } else if (actKind == RemapLayerAction::ShiftLayerHold || actKind == RemapLayerAction::ToggleLayer) {
        Text(L"TARGET LAYER", gpu_.fMini, RcW(tgtX, hero.top + 10.0f, tgtW, 14.0f), theme::Faint);
        const float lw = (tgtW - 12.0f) * 0.25f;
        for (int l = 0; l < kRemapLayerCount; ++l) {
            Button(RcW(tgtX + static_cast<float>(l) * (lw + 4.0f), hero.top + 32.0f, lw, 32.0f),
                   LayerName(static_cast<uint8_t>(l)), act::RemapTargetLayer + l,
                   r.targetLayer == static_cast<uint8_t>(l) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
        }
    } else if (actKind == RemapLayerAction::WarpToMonitor) {
        Text(L"TARGET MONITOR", gpu_.fMini, RcW(tgtX, hero.top + 10.0f, tgtW, 14.0f), theme::Faint);
        const int shown = std::min(monCount, 4);
        const float mw = (tgtW - static_cast<float>(shown - 1) * 4.0f) / static_cast<float>(shown);
        for (int m = 0; m < shown; ++m) {
            wchar_t ml[16]{};
            std::swprintf(ml, std::size(ml), L"Mon %d", m + 1);
            const bool sel = (r.monitorIndex == static_cast<int8_t>(m)) || (r.monitorIndex < 0 && m == 0);
            Button(RcW(tgtX + static_cast<float>(m) * (mw + 4.0f), hero.top + 32.0f, mw, 32.0f),
                   ml, act::RemapMonitor + 1 + m, sel ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fMini);
        }
    } else {
        Text(L"EFFECT", gpu_.fMini, RcW(tgtX, hero.top + 10.0f, tgtW, 14.0f), theme::Faint);
        const D2D1_RECT_F badgeR = RcW(tgtX, hero.top + 30.0f, tgtW, 38.0f);
        Fill(badgeR, 7.0f, theme::Inset, 0.90f);
        Stroke(badgeR, 7.0f, actKind == RemapLayerAction::BlockKey ? theme::Danger : ColSecondary(), 1.0f, 0.72f);
        Text(actKind == RemapLayerAction::BlockKey ? L"Block original key" : L"Toggle global lock",
             gpu_.fBodyStrong, badgeR, actKind == RemapLayerAction::BlockKey ? theme::Danger : ColSignalHi(),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    (void)midX;
    y = hero.bottom + 12.0f;

    constexpr float kRowH = 32.0f;
    constexpr float kGap  = 9.0f;

    // ---- Physical Device Selector (Raw Input immediate list) + Filter -------------
    RowLabel(L, y, kRowH, L"Device");
    {
        const float devBtnW = std::min(256.0f, R - C - 252.0f);
        wchar_t devText[64]{};
        if (r.anyDevice) {
            std::swprintf(devText, std::size(devText), L"Any device (%d detected)", u.physicalDeviceCount);
        } else {
            wcsncpy_s(devText, r.deviceLabel, _TRUNCATE);
        }
        Button(RcW(C, y, devBtnW, kRowH), devText, act::RemapCycleDevice, BtnStyle::Secondary, gpu_.fBodyStrong);
        Button(RcW(C + devBtnW + 6.0f, y + 1.0f, 30.0f, kRowH - 2.0f), L"\u21BB",
               act::RemapRefreshDevices, BtnStyle::Ghost, gpu_.fBodyStrong);

        static constexpr const wchar_t* kDevKinds[3] = { L"Any", L"Keyboard", L"Mouse" };
        float dx = C + devBtnW + 44.0f;
        for (int k = 0; k < 3; ++k) {
            const float kw = std::ceil(MeasureWidth(kDevKinds[k], gpu_.fSmall)) + 16.0f;
            if (dx + kw > R) break;
            Button(RcW(dx, y + 3.0f, kw, 26.0f), kDevKinds[k], act::RemapDevKind + k,
                   r.deviceKind == static_cast<uint8_t>(k) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            dx += kw + 5.0f;
        }
    }
    y += kRowH + kGap;

    // ---- Action Kind --------------------------------------------------------------
    RowLabel(L, y, kRowH, L"Action Kind");
    {
        static constexpr const wchar_t* kActLabels[6] = {
            L"Remap Key", L"Block Key", L"Hold Layer", L"Toggle Layer", L"Toggle Lock", L"Warp Cursor"
        };
        float ax = C;
        for (int k = 0; k < 6; ++k) {
            const float aw = std::ceil(MeasureWidth(kActLabels[k], gpu_.fSmall)) + 15.0f;
            if (ax + aw > R) break;
            Button(RcW(ax, y + 3.0f, aw, 26.0f), kActLabels[k], act::RemapActionKind + k,
                   r.actionKind == static_cast<uint8_t>(k) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            ax += aw + 5.0f;
        }
    }
    y += kRowH + kGap;

    // ---- Rule Layer ---------------------------------------------------------------
    RowLabel(L, y, kRowH, L"Rule Layer");
    {
        float rlx = C;
        for (int l = 0; l < kRemapLayerCount; ++l) {
            Button(RcW(rlx, y + 3.0f, 58.0f, 26.0f), LayerName(static_cast<uint8_t>(l)),
                   act::RemapRuleLayer + l, r.layerIndex == static_cast<uint8_t>(l) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            rlx += 64.0f;
        }
    }
    y += kRowH + kGap;

    // ---- Lock State Policy --------------------------------------------------------
    RowLabel(L, y, kRowH, L"Lock State");
    {
        float upx = C;
        for (int p = 0; p < 3; ++p) {
            const wchar_t* pl = UnlockPolicyName(static_cast<RemapUnlockPolicy>(p));
            const float pw = std::ceil(MeasureWidth(pl, gpu_.fSmall)) + 18.0f;
            Button(RcW(upx, y + 3.0f, pw, 26.0f), pl, act::RemapUnlockPolicy + p,
                   r.unlockPolicy == static_cast<uint8_t>(p) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            upx += pw + 6.0f;
        }
    }
    y += kRowH + kGap;

    // ---- Monitor Scope ------------------------------------------------------------
    RowLabel(L, y, kRowH, L"Monitor");
    {
        float mx = C;
        Button(RcW(mx, y + 3.0f, 56.0f, 26.0f), L"All", act::RemapMonitor + 0,
               r.monitorIndex < 0 ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
        mx += 62.0f;
        for (int m = 0; m < monCount; ++m) {
            wchar_t mLbl[24]{};
            std::swprintf(mLbl, std::size(mLbl), L"Monitor %d%ls", m + 1,
                          (m < u.monitorCount && u.monitors[m].isPrimary) ? L"*" : L"");
            const float mw = std::ceil(MeasureWidth(mLbl, gpu_.fSmall)) + 16.0f;
            if (mx + mw > R) break;
            Button(RcW(mx, y + 3.0f, mw, 26.0f), mLbl, act::RemapMonitor + 1 + m,
                   r.monitorIndex == static_cast<int8_t>(m) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            mx += mw + 5.0f;
        }
    }
    y += kRowH + kGap;

    // ---- Suppress Original Key Switch ---------------------------------------------
    RowLabel(L, y, kRowH, L"Suppress");
    const bool suppressOn = r.suppressOriginal || (actKind == RemapLayerAction::BlockKey);
    Switch(C, y + 6.0f, suppressOn, act::RemapToggleSuppress, false);
    Text(L"Suppress Original Key from reaching foreground apps", gpu_.fSmall,
         Rc(C + 46.0f, y, R, y + kRowH), theme::Muted);
}

// =====================================================================================
//  3. Simplified & Rock-Solid Trigger Bot View
// =====================================================================================

inline void UiRenderer::DrawTriggerEditor(const D2D1_RECT_F& panel) noexcept {
    const auto& u = Ui();
    const int idx = std::clamp(u.selectedIndex, 0, kColorTriggerCount - 1);
    const auto& t = u.triggers[idx];
    const auto& pv = frame_.triggerPreview;
    const uint8_t bindTarget = frame_.bindTarget;
    const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Trigger, idx));

    const float L = panel.left + 24.0f;
    const float R = panel.right - 24.0f;
    const float C = L + 100.0f;
    float y = panel.top + 14.0f;

    // ---- Header: inline rename | On/Off | Duplicate | Delete ----------------------
    const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
    Button(delBtn, u.pendingDeleteConfirm ? L"Confirm?" : L"Delete", act::DeleteItem,
           u.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

    const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
    Button(dupBtn, L"Duplicate", act::DuplicateItem, BtnStyle::Ghost);

    const bool matchedLive = pv.isMatched && t.enabled;
    const float switchX = dupBtn.left - 16.0f - 36.0f;
    Switch(switchX, y + 8.0f, t.enabled, act::ToggleEnabled, matchedLive, animIdx);
    Text(matchedLive ? L"Triggered" : (t.enabled ? L"On" : L"Off"), gpu_.fBody,
         Rc(switchX - 78.0f, y, switchX - 10.0f, y + 36.0f),
         matchedLive ? theme::Ok : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Hit(Rc(switchX - 78.0f, y, switchX, y + 36.0f), act::ToggleEnabled);

    const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 86.0f, y + 36.0f);
    const int nameA = act::FieldFocus + static_cast<int>(Field::TriggerName);
    if (u.editField == Field::TriggerName) {
        Fill(nameRect, 6.0f, theme::Inset, 0.92f);
        Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
        Text(u.editBuffer, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
        const float caretX = Snap(L + MeasureWidth(u.editBuffer, gpu_.fTitle) + 1.5f);
        Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignalHi(), 1.5f);
    } else {
        if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised, 0.72f);
        Text(t.name, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
    }
    Hit(nameRect, nameA);

    wchar_t sentence[200]{};
    DescribeColorTrigger(t, sentence, std::size(sentence));
    Text(sentence, gpu_.fBody, Rc(L, y + 36.0f, R, y + 54.0f), theme::Muted);
    y += 58.0f;

    // ---- Hero Glass Card: 15x15 Live Pixel Loupe | Swatches & Delta | Tolerance ---
    const float cardH = 148.0f;
    const D2D1_RECT_F card = Rc(L, y, R, y + cardH);
    GlassCard(card, 11.0f, true, matchedLive,
              matchedLive ? mask::ui::Rgba(theme::Ok, 0.85f) : theme::GlassBorder,
              matchedLive ? theme::Ok : ColSignal(), 0.20f);

    const float loupeSz = 126.0f;
    const D2D1_RECT_F loupeFrame = RcW(card.left + 11.0f, card.top + 11.0f, loupeSz, loupeSz);
    const UINT32 loupeBorder = matchedLive ? theme::Ok : ((t.enabled && pv.isArmed) ? ColSignal() : theme::LineHi);
    Fill(loupeFrame, 8.0f, theme::Window, 0.96f);
    Stroke(loupeFrame, 8.0f, loupeBorder, matchedLive ? 1.8f : 1.0f);

    // 15x15 Zoomed Pixel Loupe with aliased crisp squares, cyan match highlights, & crosshair
    {
        constexpr int N = mask::triggers::ColorTriggerPreviewSnapshot::kLoupeSize; // 15
        const float gridL = loupeFrame.left + 3.0f;
        const float gridT = loupeFrame.top + 3.0f;
        const float cell  = (loupeSz - 6.0f) / static_cast<float>(N);
        ID2D1DeviceContext* ctx = gpu_.Context();
        D2D1_ANTIALIAS_MODE prevAA = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        if (ctx != nullptr) {
            prevAA = ctx->GetAntialiasMode();
            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        }

        for (int py = 0; py < N; ++py) {
            for (int px = 0; px < N; ++px) {
                const uint8_t pMask = pv.loupeMatchMask[py * N + px];
                const bool inZone = (pMask & 1U) != 0U;
                const bool pixHit = (pMask & 2U) != 0U;
                const D2D1_RECT_F pxR = RcW(gridL + static_cast<float>(px) * cell,
                                            gridT + static_cast<float>(py) * cell, cell, cell);
                const uint32_t rgb = pv.loupePixels[py * N + px] & 0x00FFFFFFU;
                Fill(pxR, 0.0f, rgb, 1.0f);
                if (!inZone) {
                    Fill(pxR, 0.0f, theme::Window, 0.48f);
                }
                if (pixHit) {
                    Stroke(Rc(pxR.left + 0.8f, pxR.top + 0.8f, pxR.right - 0.8f, pxR.bottom - 0.8f),
                           0.0f, matchedLive ? theme::Ok : ColSignalHi(), 1.25f, 0.95f);
                }
            }
        }
        for (int i = 1; i < N; ++i) {
            const float gx = gridL + static_cast<float>(i) * cell;
            const float gy = gridT + static_cast<float>(i) * cell;
            Line(gx, gridT, gx, gridT + cell * N, 0x000000U, 1.0f, 0.25f);
            Line(gridL, gy, gridL + cell * N, gy, 0x000000U, 1.0f, 0.25f);
        }

        const int half = N / 2;
        const int rad = std::clamp<int>(t.sampleRadius, 0, half);
        if (rad > 0) {
            const D2D1_RECT_F roiR = Rc(gridL + static_cast<float>(half - rad) * cell,
                                        gridT + static_cast<float>(half - rad) * cell,
                                        gridL + static_cast<float>(half + rad + 1) * cell,
                                        gridT + static_cast<float>(half + rad + 1) * cell);
            Stroke(roiR, 0.0f, ColSignalHi(), 1.2f, 0.78f);
        }

        // High-contrast center pixel crosshair
        const D2D1_RECT_F cCell = RcW(gridL + static_cast<float>(half) * cell,
                                      gridT + static_cast<float>(half) * cell, cell, cell);
        Stroke(Rc(cCell.left - 1.0f, cCell.top - 1.0f, cCell.right + 1.0f, cCell.bottom + 1.0f), 0.0f, 0x000000U, 1.0f, 0.78f);
        Stroke(cCell, 0.0f, theme::White, 1.3f, 0.96f);

        if (ctx != nullptr) {
            ctx->SetAntialiasMode(prevAA);
        }
    }

    const float rx = loupeFrame.right + 14.0f;
    const float rr = card.right - 14.0f;
    float ry = card.top + 10.0f;

    // Hero Row 1: Current RGB vs Target RGB + Delta + DXGI/GDI us + Triggered badge
    {
        const UINT32 liveRgb = (static_cast<UINT32>(pv.sampledR) << 16) |
                               (static_cast<UINT32>(pv.sampledG) << 8)  |
                               static_cast<UINT32>(pv.sampledB);
        const UINT32 tgtRgb  = (static_cast<UINT32>(t.r) << 16) |
                               (static_cast<UINT32>(t.g) << 8)  |
                               static_cast<UINT32>(t.b);

        const D2D1_RECT_F swLive = RcW(rx, ry + 2.0f, 22.0f, 22.0f);
        Fill(swLive, 5.0f, liveRgb);
        Stroke(swLive, 5.0f, theme::LineHi);

        wchar_t liveRgbTxt[24]{};
        std::swprintf(liveRgbTxt, std::size(liveRgbTxt), L"%u,%u,%u",
                      static_cast<unsigned>(pv.sampledR),
                      static_cast<unsigned>(pv.sampledG),
                      static_cast<unsigned>(pv.sampledB));
        Text(liveRgbTxt, gpu_.fAxis, RcW(swLive.right + 5.0f, ry, 68.0f, 26.0f), theme::Text);

        ArrowRight(swLive.right + 74.0f, Snap(ry + 13.0f), 10.0f, theme::Faint);

        const float tgtX = swLive.right + 88.0f;
        const auto mKind = static_cast<ColorTriggerMode>(t.mode);
        float rightOfTarget = tgtX;
        if (mKind == ColorTriggerMode::ColorChanged) {
            Text(L"Baseline change", gpu_.fBodyStrong, RcW(tgtX, ry, 108.0f, 26.0f), ColSignalHi());
            rightOfTarget = tgtX + 112.0f;
        } else {
            const D2D1_RECT_F swTgt = RcW(tgtX, ry + 2.0f, 22.0f, 22.0f);
            Fill(swTgt, 5.0f, tgtRgb);
            Stroke(swTgt, 5.0f, theme::LineHi);
            wchar_t tgtRgbTxt[24]{};
            std::swprintf(tgtRgbTxt, std::size(tgtRgbTxt), L"%u,%u,%u",
                          static_cast<unsigned>(t.r),
                          static_cast<unsigned>(t.g),
                          static_cast<unsigned>(t.b));
            Text(tgtRgbTxt, gpu_.fAxis, RcW(swTgt.right + 5.0f, ry, 68.0f, 26.0f), theme::Muted);
            rightOfTarget = swTgt.right + 75.0f;
        }

        // Status pill: Triggered | Watching | Standby | Off
        const wchar_t* stTxt = !t.enabled ? L"Off"
                             : (matchedLive ? L"Triggered" : (pv.isArmed ? L"Watching" : L"Standby"));
        const UINT32 stCol = !t.enabled ? theme::Faint
                           : (matchedLive ? theme::Ok : (pv.isArmed ? ColSignalHi() : theme::Warn));
        const D2D1_RECT_F pill = Rc(rr - 70.0f, ry + 2.0f, rr, ry + 24.0f);
        Fill(pill, 11.0f, stCol, t.enabled ? 0.18f : 0.08f);
        Stroke(pill, 11.0f, stCol, 1.0f, t.enabled ? 0.78f : 0.35f);
        Text(stTxt, gpu_.fSmall, pill, stCol, DWRITE_TEXT_ALIGNMENT_CENTER);

        // DXGI/GDI status & pipeline timing (us) + delta indicator
        wchar_t infoTxt[48]{};
        const uint32_t us = (pv.avgPipelineUs > 0U) ? pv.avgPipelineUs : pv.lastPipelineUs;
        std::swprintf(infoTxt, std::size(infoTxt), L"%ls \u00B7 %u us \u00B7 \u0394%u",
                      pv.usingDxgi ? L"DXGI" : L"GDI",
                      static_cast<unsigned>(us),
                      static_cast<unsigned>(pv.colorDelta));
        const float infoW = std::ceil(MeasureWidth(infoTxt, gpu_.fMini)) + 14.0f;
        if (pill.left - infoW - 6.0f >= rightOfTarget) {
            const D2D1_RECT_F perfR = Rc(pill.left - infoW - 6.0f, ry + 2.0f, pill.left - 6.0f, ry + 24.0f);
            const bool withinTol = (pv.colorDelta <= t.tolerance);
            const UINT32 pCol = (t.enabled && withinTol) ? ColSignalHi() : theme::Muted;
            Fill(perfR, 6.0f, theme::Panel, 0.85f);
            Stroke(perfR, 6.0f, pv.usingDxgi ? ColSignal() : theme::Line, 1.0f, pv.usingDxgi ? 0.45f : 0.85f);
            Text(infoTxt, gpu_.fMini, perfR, pCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        ry += 34.0f;
    }

    // Hero Row 2: Pick on Screen + Screen Center + Color Presets (MatchColor)
    {
        const bool isCenter = (t.sampleX < 0 || t.sampleY < 0);
        Button(RcW(rx, ry, 126.0f, 27.0f),
               u.triggerColorPickActive ? L"Click screen\u2026" : L"Pick on Screen",
               act::TrigPickPixel, u.triggerColorPickActive ? BtnStyle::Primary : BtnStyle::Secondary, gpu_.fSmall);
        Button(RcW(rx + 132.0f, ry, 102.0f, 27.0f), L"Screen Center", act::TrigLockCenter,
               isCenter ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);

        const auto mKind = static_cast<ColorTriggerMode>(t.mode);
        if (mKind == ColorTriggerMode::MatchColor) {
            float px = rx + 242.0f;
            const float cy = ry + 13.5f;
            for (int i = 0; i < kTriggerColorPresetCount; ++i) {
                if (px + 20.0f > rr + 2.0f) break;
                const auto& p = kTriggerColorPresets[i];
                const UINT32 pRgb = (static_cast<UINT32>(p.r) << 16) | (static_cast<UINT32>(p.g) << 8) | static_cast<UINT32>(p.b);
                const bool matchPreset = (std::abs(static_cast<int>(t.r) - static_cast<int>(p.r)) <= 6 &&
                                          std::abs(static_cast<int>(t.g) - static_cast<int>(p.g)) <= 6 &&
                                          std::abs(static_cast<int>(t.b) - static_cast<int>(p.b)) <= 6);
                const int actId = act::TrigColorPreset + i;
                const bool hov = Hovered(actId);
                const float cx = px + 10.0f;
                if (matchPreset || hov) {
                    const D2D1_RECT_F ring = Rc(cx - 10.0f, cy - 10.0f, cx + 10.0f, cy + 10.0f);
                    Stroke(ring, 10.0f, matchPreset ? theme::White : theme::Muted, 1.4f);
                }
                Dot(cx, cy, 7.0f, pRgb);
                Hit(RcW(px - 2.0f, ry, 24.0f, 27.0f), actId);
                px += 25.0f;
            }
        } else {
            Text(L"Captures baseline on arm and fires when zone changes", gpu_.fSmall,
                 Rc(rx + 242.0f, ry, rr, ry + 27.0f), theme::Faint);
        }
        ry += 35.0f;
    }

    // Hero Row 3: Tolerance Smooth Glass Slider (0..150)
    {
        Text(L"Tolerance", gpu_.fSmall, RcW(rx, ry, 140.0f, 18.0f), theme::Muted);
        wchar_t tolDesc[40]{};
        std::swprintf(tolDesc, std::size(tolDesc), L"%ls (\u00B1%u)",
                      ToleranceWord(t.tolerance), static_cast<unsigned>(t.tolerance));
        Text(tolDesc, gpu_.fBodyStrong, Rc(rx + 144.0f, ry, rr, ry + 18.0f), theme::Text, DWRITE_TEXT_ALIGNMENT_TRAILING);

        const float trL = rx + 6.0f;
        const float trR = rr - 6.0f;
        const float trackY = Snap(ry + 28.0f);
        trigTolTrack_ = Rc(trL, trackY - 11.0f, trR, trackY + 11.0f);

        const float frac = std::clamp(static_cast<float>(t.tolerance) / 150.0f, 0.0f, 1.0f);
        const float thumbX = trL + frac * std::max(0.0f, trR - trL);
        const bool hot = (u.draggingTriggerSlider == act::TrigSliderTol) || Hovered(act::TrigSliderTol);

        Fill(Rc(trL, trackY - 3.0f, trR, trackY + 3.0f), 3.0f, theme::Panel, 0.90f);
        Stroke(Rc(trL, trackY - 3.0f, trR, trackY + 3.0f), 3.0f, theme::Line);
        if (thumbX > trL + 1.0f) {
            gpu_.FillLinearRect(Rc(trL, trackY - 3.0f, thumbX, trackY + 3.0f), 3.0f,
                                ColSignal(), 0.95f, ColSignalHi(), 0.95f, true);
        }
        if (hot) {
            gpu_.FillRadialGlow(thumbX, trackY, 13.0f, 13.0f, ColSignal(), 0.30f, ColSignal(), 0.0f);
        }
        Dot(thumbX, trackY, 7.5f, ColSignal());
        Dot(thumbX, trackY, 2.8f, theme::IceCore);
        Hit(Rc(rx, trackY - 12.0f, rr, trackY + 12.0f), act::TrigSliderTol);

        Text(L"Exact (0)", gpu_.fMini, RcW(rx, ry + 39.0f, 90.0f, 14.0f), theme::Faint);
        Text(L"Wide (150)", gpu_.fMini, Rc(rr - 100.0f, ry + 39.0f, rr, ry + 53.0f), theme::Faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    y = card.bottom + 10.0f;

    // ---- Parameter Rows (strictly solid, zero dead features) ----------------------
    constexpr float kRowH = 30.0f;
    constexpr float kGap  = 7.0f;

    auto drawSegmented = [&](float sx, float sy, float sw, const wchar_t* const* labels,
                             int count, int curVal, int baseAction) noexcept {
        const D2D1_RECT_F seg = RcW(sx, sy, sw, kRowH);
        Fill(seg, 8.0f, theme::Inset, 0.85f);
        Stroke(seg, 8.0f, theme::Line, 1.0f, 0.85f);
        const float cellW = (sw - 6.0f) / static_cast<float>(count);
        for (int m = 0; m < count; ++m) {
            const D2D1_RECT_F cell = RcW(seg.left + 3.0f + cellW * static_cast<float>(m),
                                         seg.top + 3.0f, cellW, kRowH - 6.0f);
            const bool sel = (curVal == m);
            const int actId = baseAction + m;
            if (sel) {
                gpu_.FillLinearRect(cell, 6.0f, theme::RaisedHi, 0.95f, theme::Raised, 0.90f, false);
                GlowLine(cell.left + 10.0f, cell.bottom - 1.5f, cell.right - 10.0f, cell.bottom - 1.5f,
                         ColSignalHi(), 2.0f, 5.0f, 0.92f);
            } else if (Hovered(actId)) {
                Fill(cell, 6.0f, theme::Raised, 0.65f);
            }
            Text(labels[m], sel ? gpu_.fBodyStrong : gpu_.fSmall, cell,
                 (sel || Hovered(actId)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
            Hit(cell, actId);
        }
    };

    // 1. Detection Mode (2 pills): Color Match | Color Change
    RowLabel(L, y, kRowH, L"Mode");
    static constexpr const wchar_t* kTrigModeLbls[2] = { L"Color Match", L"Color Change" };
    drawSegmented(C, y, 250.0f, kTrigModeLbls, 2, static_cast<int>(t.mode), act::TrigMode);
    y += kRowH + kGap;

    // 2. Match Rule (2 pills): Any Pixel | Zone Average
    RowLabel(L, y, kRowH, L"Match Rule");
    static constexpr const wchar_t* kRuleLbls[2] = { L"Any Pixel", L"Zone Average" };
    const int curRule = (GetTriggerMatchRule(t) == ColorTriggerMatchRule::ZoneAverage) ? 1 : 0;
    drawSegmented(C, y, 250.0f, kRuleLbls, 2, curRule, act::TrigMatchRule);
    y += kRowH + kGap;

    // 3. Zone Size chips: 1x1, 3x3, 5x5, 9x9, 15x15
    RowLabel(L, y, kRowH, L"Zone Size");
    {
        float zx = C;
        static constexpr uint8_t kRadVals[5] = { 0U, 1U, 2U, 4U, 7U };
        static constexpr const wchar_t* kRadLbls[5] = { L"1x1", L"3x3", L"5x5", L"9x9", L"15x15" };
        for (int k = 0; k < 5; ++k) {
            const float kw = (k == 4) ? 54.0f : 46.0f;
            if (zx + kw > R) break;
            Button(RcW(zx, y + 3.0f, kw, 24.0f), kRadLbls[k], act::TrigRadius + k,
                   t.sampleRadius == kRadVals[k] ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            zx += kw + 5.0f;
        }
    }
    y += kRowH + kGap;

    // 4. Monitor selector
    RowLabel(L, y, kRowH, L"Monitor");
    {
        const int monCount = std::max(1, std::min(u.monitorCount, 8));
        float mx = C;
        for (int m = 0; m < monCount; ++m) {
            const auto& mn = u.monitors[m];
            wchar_t mLbl[36]{};
            if (monCount <= 2) {
                std::swprintf(mLbl, std::size(mLbl), L"Monitor %d (%dx%d)%ls",
                              m + 1, static_cast<int>(mn.right - mn.left), static_cast<int>(mn.bottom - mn.top),
                              mn.isPrimary ? L"*" : L"");
            } else {
                std::swprintf(mLbl, std::size(mLbl), L"Monitor %d%ls", m + 1, mn.isPrimary ? L"*" : L"");
            }
            const float mw = std::ceil(MeasureWidth(mLbl, gpu_.fSmall)) + 18.0f;
            if (mx + mw > R) break;
            Button(RcW(mx, y + 3.0f, mw, 24.0f), mLbl, act::TrigMonitor + m,
                   t.monitorIndex == static_cast<uint8_t>(m) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            mx += mw + 5.0f;
        }
    }
    y += kRowH + kGap;

    // 5. Arming: Always Active | While Key Held | Toggle Key + Bind Arm Key / Clear
    RowLabel(L, y, kRowH, L"Arming");
    static constexpr const wchar_t* kArmLbls[3] = { L"Always Active", L"While Key Held", L"Toggle Key" };
    drawSegmented(C, y, 312.0f, kArmLbls, 3, t.armMode, act::TrigArmMode);
    if (t.armMode != 0U) {
        KeyChip(RcW(C + 322.0f, y, 130.0f, kRowH), t.armVk, act::TrigBindArmVk, bindTarget == 26U);
        if (t.armVk != 0U) {
            Button(RcW(C + 458.0f, y + 3.0f, 24.0f, 24.0f), L"\u00D7", act::TrigClearArmVk, BtnStyle::Ghost, gpu_.fBodyStrong);
        }
    }
    y += kRowH + kGap;

    // 6. Action Kind (4 pills): Click Key | Hold Key | Run Macro | Run Sequence
    RowLabel(L, y, kRowH, L"Action");
    static constexpr const wchar_t* kActLbls[4] = { L"Click Key", L"Hold Key", L"Run Macro", L"Run Sequence" };
    drawSegmented(C, y, std::min(392.0f, R - C), kActLbls, 4, t.actionKind, act::TrigActionKind);
    y += kRowH + kGap;

    // 7. Output / Linked Target
    const auto actKind = static_cast<ColorTriggerActionKind>(t.actionKind);
    const bool isKeyAct = (actKind == ColorTriggerActionKind::ClickKey ||
                           actKind == ColorTriggerActionKind::HoldKey);
    RowLabel(L, y, kRowH, isKeyAct ? L"Output" : L"Target");
    if (isKeyAct) {
        KeyChip(RcW(C, y, 138.0f, kRowH), t.actionVk, act::TrigBindActionVk, bindTarget == 27U);
        static constexpr uint16_t kQuickVks[4] = { VK_LBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        static constexpr const wchar_t* kQuickLbls[4] = { L"Left click", L"Right click", L"Mouse 4", L"Mouse 5" };
        float qx = C + 148.0f;
        for (int q = 0; q < 4; ++q) {
            const float qw = std::ceil(MeasureWidth(kQuickLbls[q], gpu_.fSmall)) + 14.0f;
            if (qx + qw > R) break;
            Button(RcW(qx, y + 3.0f, qw, 24.0f), kQuickLbls[q], act::TrigQuickOut + q,
                   t.actionVk == kQuickVks[q] ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
            qx += qw + 5.0f;
        }
    } else {
        const bool isMacro = (actKind == ColorTriggerActionKind::RunMacroSlot);
        const uint32_t maxSlots = isMacro ? static_cast<uint32_t>(kSlotCount) : static_cast<uint32_t>(kComplexSeqCount);
        const uint32_t slotIdx = t.linkedIndex % maxSlots;
        wchar_t slotStr[24]{};
        std::swprintf(slotStr, std::size(slotStr), L"#%u", static_cast<unsigned>(slotIdx + 1U));
        GenericField(RcW(C, y, 114.0f, kRowH), Field::TriggerLinked, slotStr, true);

        const wchar_t* linkedName = isMacro ? u.slots[slotIdx].name : u.sequences[slotIdx].name;
        wchar_t targetLbl[64]{};
        std::swprintf(targetLbl, std::size(targetLbl), L"\u201C%ls\u201D", linkedName);
        Text(targetLbl, gpu_.fBody, Rc(C + 126.0f, y, R, y + kRowH), theme::Muted);
    }
    y += kRowH + kGap;

    // 8. Reaction Timing (Delay / Hold / Cooldown)
    RowLabel(L, y, kRowH, L"Timing");
    wchar_t dBuf[24]{}, hBuf[24]{}, cBuf[24]{};
    std::swprintf(dBuf, std::size(dBuf), L"%u ms", static_cast<unsigned>(t.delayMs));
    std::swprintf(hBuf, std::size(hBuf), L"%u ms", static_cast<unsigned>(t.holdMs));
    std::swprintf(cBuf, std::size(cBuf), L"%u ms", static_cast<unsigned>(t.cooldownMs));

    float tx = C;
    Text(L"Delay", gpu_.fSmall, RcW(tx, y, 36.0f, kRowH), theme::Muted);
    tx += 38.0f;
    GenericField(RcW(tx, y, 92.0f, kRowH), Field::TriggerDelay, dBuf, true);
    tx += 102.0f;

    Text(L"Hold", gpu_.fSmall, RcW(tx, y, 30.0f, kRowH), theme::Muted);
    tx += 32.0f;
    GenericField(RcW(tx, y, 92.0f, kRowH), Field::TriggerHold, hBuf, true);
    tx += 102.0f;

    Text(L"Cooldown", gpu_.fSmall, RcW(tx, y, 58.0f, kRowH), theme::Muted);
    tx += 60.0f;
    GenericField(RcW(tx, y, std::min(100.0f, R - tx), kRowH), Field::TriggerCooldown, cBuf, true);
}

// =====================================================================================
//  4. 5-Tab Cosmic Settings Menu
// =====================================================================================

inline void UiRenderer::DrawSettingsView(const D2D1_RECT_F& panel) noexcept {
    const auto& u = Ui();
    const auto& a = Anim();
    const float L = panel.left + 22.0f;
    const float R = panel.right - 22.0f;
    float y = panel.top + 12.0f;

    // Header
    Button(Rc(R - 74.0f, y + 2.0f, R, y + 32.0f), L"Done", act::ToggleSettings, BtnStyle::Secondary);
    Text(L"Settings", gpu_.fTitle, Rc(L, y, R - 86.0f, y + 34.0f), theme::Text);
    y += 40.0f;

    // Animated 5-tab strip
    static constexpr const wchar_t* kTabNames[5] = {
        L"General", L"Appearance", L"Performance", L"Hotkeys", L"Hardware & Profiles"
    };
    const D2D1_RECT_F tabStrip = Rc(L, y, R, y + 34.0f);
    Fill(tabStrip, 8.0f, theme::Inset, 0.88f);
    Stroke(tabStrip, 8.0f, theme::Line, 1.0f, 0.88f);

    float tx = tabStrip.left + 4.0f;
    const float stripInnerW = tabStrip.right - tabStrip.left - 8.0f;
    float totalWeight = 0.0f;
    float tabWeights[5]{};
    for (int i = 0; i < 5; ++i) {
        tabWeights[i] = MeasureWidth(kTabNames[i], gpu_.fBodyStrong) + 26.0f;
        totalWeight += tabWeights[i];
    }
    for (int i = 0; i < 5; ++i) {
        const float tw = stripInnerW * (tabWeights[i] / std::max(1.0f, totalWeight));
        settingsTabRects_[i] = Rc(tx, tabStrip.top + 3.0f, tx + tw, tabStrip.bottom - 3.0f);
        tx += tw;
    }

    const int curTab = std::clamp(u.settingsTab, 0, 4);
    const float indX = (a.settingsTabX.value > 1.0f) ? a.settingsTabX.value : settingsTabRects_[curTab].left;
    const float indW = (a.settingsTabW.value > 10.0f) ? a.settingsTabW.value : (settingsTabRects_[curTab].right - settingsTabRects_[curTab].left);
    const D2D1_RECT_F pillR = RcW(indX, tabStrip.top + 3.0f, indW, 28.0f);
    gpu_.FillLinearRect(pillR, 6.0f, theme::RaisedHi, 0.95f, theme::Raised, 0.90f, false);
    GlowLine(pillR.left + 12.0f, pillR.bottom - 1.5f, pillR.right - 12.0f, pillR.bottom - 1.5f,
             ColSignalHi(), 2.0f, 5.5f, 0.92f);

    for (int i = 0; i < 5; ++i) {
        const bool sel = (curTab == i);
        const int actId = act::SettingsTab + i;
        if (!sel && Hovered(actId)) Fill(settingsTabRects_[i], 6.0f, theme::Raised, 0.55f);
        Text(kTabNames[i], sel ? gpu_.fBodyStrong : gpu_.fBody, settingsTabRects_[i],
             (sel || Hovered(actId)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(settingsTabRects_[i], actId);
    }
    y = tabStrip.bottom + 12.0f;

    const auto& s = u.settings;

    auto toggleRow = [&](float& ry, const wchar_t* title, const wchar_t* sub, bool val, int action) noexcept {
        const D2D1_RECT_F rowR = Rc(L, ry, R, ry + 43.0f);
        Fill(rowR, 8.0f, theme::Inset, 0.78f);
        Stroke(rowR, 8.0f, theme::Line, 1.0f, 0.80f);
        Text(title, gpu_.fBodyStrong, Rc(L + 14.0f, ry + 4.0f, R - 64.0f, ry + 22.0f), theme::Text);
        Text(sub, gpu_.fSmall, Rc(L + 14.0f, ry + 21.0f, R - 64.0f, ry + 39.0f), theme::Faint);
        Switch(R - 50.0f, ry + 11.5f, val, action);
        ry += 48.0f;
    };

    if (curTab == 0) {
        // ---- Tab 0: General -------------------------------------------------------
        toggleRow(y, L"Start with Windows", L"Launch Mask automatically at sign-in.", s.startWithWindows, act::SetToggleStartWin);
        toggleRow(y, L"Start minimized", L"Open quietly in the background.", s.startMinimized, act::SetToggleStartMin);
        toggleRow(y, L"Minimize to system tray", L"Hide the taskbar button when minimized.", s.minimizeToTray, act::SetToggleMinTray);
        toggleRow(y, L"Always on top", L"Keep Mask above other windows.", s.alwaysOnTop, act::SetToggleTopmost);
        toggleRow(y, L"Remember window bounds", L"Restore window size and monitor position.", s.rememberWindowBounds, act::SetToggleBounds);
        toggleRow(y, L"Confirm before deleting", L"Require a second click on Delete.", s.confirmDelete, act::SetToggleConfirmDel);

        const D2D1_RECT_F rowR = Rc(L, y, R, y + 46.0f);
        Fill(rowR, 8.0f, theme::Inset, 0.78f);
        Stroke(rowR, 8.0f, theme::Line, 1.0f, 0.80f);
        Text(L"Autosave delay", gpu_.fBodyStrong, Rc(L + 14.0f, y + 5.0f, R - 170.0f, y + 24.0f), theme::Text);
        Text(L"Delay after edits before writing profiles to disk.", gpu_.fSmall, Rc(L + 14.0f, y + 23.0f, R - 170.0f, y + 41.0f), theme::Faint);
        wchar_t asBuf[32]{};
        std::swprintf(asBuf, std::size(asBuf), L"%u ms", static_cast<unsigned>(s.autosaveDelayMs));
        GenericField(Rc(R - 144.0f, y + 8.0f, R - 12.0f, y + 38.0f), Field::SettingsAutosaveMs, asBuf, true);
    } else if (curTab == 1) {
        // ---- Tab 1: Appearance ----------------------------------------------------
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 54.0f);
            Fill(box, 8.0f, theme::Inset, 0.78f);
            Stroke(box, 8.0f, theme::Line, 1.0f, 0.80f);
            Text(L"Animation quality", gpu_.fBodyStrong, Rc(L + 14.0f, y + 6.0f, L + 280.0f, y + 26.0f), theme::Text);
            Text(L"Nebula drift, springs, ripples, and glass sheen.", gpu_.fSmall, Rc(L + 14.0f, y + 26.0f, R - 260.0f, y + 46.0f), theme::Faint);
            static constexpr const wchar_t* kQual[3] = { L"Full", L"Reduced", L"Off" };
            float qx = R - 238.0f;
            for (int q = 0; q < 3; ++q) {
                Button(RcW(qx, y + 13.0f, 72.0f, 28.0f), kQual[q], act::SetAnimQuality + q,
                       s.animationQuality == static_cast<uint8_t>(q) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                qx += 78.0f;
            }
            y += 62.0f;
        }
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 76.0f);
            GlassCard(box, 9.0f, true, false, theme::GlassBorder, ColSignal(), 0.16f);
            Text(L"Cosmic Accent", gpu_.fBodyStrong, Rc(L + 14.0f, y + 8.0f, R - 14.0f, y + 26.0f), theme::Text);
            const float swW = (R - L - 28.0f - 4.0f * 8.0f) / 5.0f;
            for (int acc = 0; acc < 5; ++acc) {
                const auto pal = mask::ui::GetAccentPalette(static_cast<uint8_t>(acc));
                const D2D1_RECT_F sr = RcW(L + 14.0f + static_cast<float>(acc) * (swW + 8.0f), y + 32.0f, swW, 32.0f);
                const bool sel = (s.accentPreset == static_cast<uint8_t>(acc));
                const int actId = act::SetAccent + acc;
                Fill(sr, 7.0f, sel ? pal.signal : theme::Raised, sel ? 0.18f : 0.85f);
                Stroke(sr, 7.0f, sel ? pal.signal : (Hovered(actId) ? theme::LineHi : theme::Line), sel ? 1.5f : 1.0f);
                if (sel) {
                    gpu_.FillRadialGlow(sr.left + 15.0f, (sr.top + sr.bottom) * 0.5f, 10.0f, 10.0f, pal.signal, 0.36f, pal.signal, 0.0f);
                }
                Dot(sr.left + 13.0f, (sr.top + sr.bottom) * 0.5f, 5.0f, pal.signal);
                Text(mask::ui::GetAccentName(static_cast<uint8_t>(acc)),
                     sel ? gpu_.fBodyStrong : gpu_.fSmall,
                     Rc(sr.left + 22.0f, sr.top, sr.right - 4.0f, sr.bottom),
                     sel ? pal.signalHi : theme::Text);
                Hit(sr, actId);
            }
            y += 84.0f;
        }
        toggleRow(y, L"Compact sidebar", L"Use single-line workspace rows.", s.compactSidebar, act::SetToggleCompact);
        toggleRow(y, L"Signal view", L"Display the glowing pulse oscilloscope in Macro view.", s.showSignalView, act::SetToggleSignal);
        toggleRow(y, L"Live indicators", L"Highlight keycaps while physical keys are held down.", s.showLiveKeyIndicators, act::SetToggleLiveKeys);
    } else if (curTab == 2) {
        // ---- Tab 2: Performance ---------------------------------------------------
        toggleRow(y, L"Focused-only background performance", L"Pause cosmic shader animation and live preview when unfocused.", s.lowResourceWhenUnfocused, act::SetToggleLowRes);
        toggleRow(y, L"Memory + GPU trim on minimize", L"Release GPU staging buffers and RAM pages when minimized.", s.trimMemoryWhenMinimized, act::SetToggleTrimMem);

        auto choiceRow = [&](float& ry, const wchar_t* title, const wchar_t* sub, const wchar_t* const* labels, int cur, int baseAct) noexcept {
            const D2D1_RECT_F box = Rc(L, ry, R, ry + 54.0f);
            Fill(box, 8.0f, theme::Inset, 0.78f);
            Stroke(box, 8.0f, theme::Line, 1.0f, 0.80f);
            Text(title, gpu_.fBodyStrong, Rc(L + 14.0f, ry + 6.0f, R - 290.0f, ry + 26.0f), theme::Text);
            Text(sub, gpu_.fSmall, Rc(L + 14.0f, ry + 26.0f, R - 290.0f, ry + 46.0f), theme::Faint);
            float cx = R - 282.0f;
            for (int i = 0; i < 3; ++i) {
                Button(RcW(cx, ry + 13.0f, 88.0f, 28.0f), labels[i], baseAct + i,
                       cur == i ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                cx += 92.0f;
            }
            ry += 62.0f;
        };

        static constexpr const wchar_t* kPowerLabels[3] = { L"Balanced", L"Max Perf", L"Eco" };
        choiceRow(y, L"Power mode", L"Polling cadence and timer precision.", kPowerLabels, s.powerMode, act::SetPowerMode);

        static constexpr const wchar_t* kPollLabels[3] = { L"250 Hz", L"500 Hz", L"1000 Hz" };
        const int pollIdx = (s.armedPollHz <= 375U) ? 0 : (s.armedPollHz <= 750U ? 1 : 2);
        choiceRow(y, L"Armed poll Hz", L"Physical input check rate while armed.", kPollLabels, pollIdx, act::SetPollHz);

        static constexpr const wchar_t* kSpinLabels[3] = { L"Auto", L"Max Prec", L"Eco" };
        choiceRow(y, L"Pacing policy", L"Final RDTSCP wait window before firing.", kSpinLabels, s.spinPolicy, act::SetSpinPolicy);
    } else if (curTab == 3) {
        // ---- Tab 3: Hotkeys -------------------------------------------------------
        const uint8_t bt = frame_.bindTarget;
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 54.0f);
            Fill(box, 8.0f, theme::Inset, 0.78f);
            Stroke(box, 8.0f, theme::Line, 1.0f, 0.80f);
            Text(L"Global pause / resume", gpu_.fBodyStrong, Rc(L + 14.0f, y + 6.0f, R - 300.0f, y + 26.0f), theme::Text);
            Text(L"Pause or resume all automation anytime.", gpu_.fSmall, Rc(L + 14.0f, y + 26.0f, R - 300.0f, y + 46.0f), theme::Faint);
            KeyChip(Rc(R - 294.0f, y + 11.0f, R - 154.0f, y + 43.0f), s.pauseHotkeyVk, act::SetBindPauseVk, bt == 24U);
            Button(Rc(R - 146.0f, y + 13.0f, R - 82.0f, y + 41.0f), L"Reset F8", act::SetResetPauseVk, BtnStyle::Ghost, gpu_.fSmall);
            Button(Rc(R - 76.0f, y + 13.0f, R - 12.0f, y + 41.0f), L"Clear", act::SetClearPauseVk, BtnStyle::Ghost, gpu_.fSmall);
            y += 62.0f;
        }
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 54.0f);
            Fill(box, 8.0f, theme::Inset, 0.78f);
            Stroke(box, 8.0f, theme::Line, 1.0f, 0.80f);
            Text(L"Global Stop All", gpu_.fBodyStrong, Rc(L + 14.0f, y + 6.0f, R - 230.0f, y + 26.0f), theme::Text);
            Text(L"Stop all active outputs and release held keys.", gpu_.fSmall, Rc(L + 14.0f, y + 26.0f, R - 230.0f, y + 46.0f), theme::Faint);
            KeyChip(Rc(R - 224.0f, y + 11.0f, R - 84.0f, y + 43.0f), s.stopAllHotkeyVk, act::SetBindStopAllVk, bt == 25U);
            Button(Rc(R - 76.0f, y + 13.0f, R - 12.0f, y + 41.0f), L"Clear", act::SetClearStopAllVk, BtnStyle::Ghost, gpu_.fSmall);
            y += 62.0f;
        }
        toggleRow(y, L"Emergency panic chord (Left Ctrl + Right Ctrl + Esc)", L"Immediately stop automation and release all outputs.", s.panicChordEnabled, act::SetTogglePanic);
        toggleRow(y, L"Stop on Windows lock", L"Stop active macros and workflows when the session locks.", s.stopOnSessionLock, act::SetToggleSessionLock);
        toggleRow(y, L"Suppress triggers while typing in Mask", L"Prevent triggers while editing text or numbers in Mask.", s.suppressWhileTypingInApp, act::SetToggleSuppressType);
        toggleRow(y, L"Sound feedback", L"Play a subtle tone on pause, resume, or emergency stop.", s.soundFeedback, act::SetToggleSound);
    } else {
        // ---- Tab 4: Hardware & Profiles -------------------------------------------
        const float bottomLimit = panel.bottom - 10.0f;

        // Card 1: Profile Bank Switcher + Data Folder + Reset All
        {
            const D2D1_RECT_F bankCard = Rc(L, y, R, y + 48.0f);
            Fill(bankCard, 8.0f, theme::Inset, 0.82f);
            Stroke(bankCard, 8.0f, theme::Line);
            Text(L"Profile bank", gpu_.fBodyStrong, Rc(L + 12.0f, y, L + 104.0f, y + 48.0f), theme::Text);
            float bx = L + 108.0f;
            for (int b = 0; b < 3; ++b) {
                wchar_t bl[16]{};
                std::swprintf(bl, std::size(bl), L"Bank %d", b + 1);
                Button(RcW(bx, y + 10.0f, 68.0f, 28.0f), bl, act::SetProfileBank + b,
                       s.activeProfileBank == static_cast<uint8_t>(b) ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
                bx += 74.0f;
            }
            Button(Rc(R - 230.0f, y + 10.0f, R - 110.0f, y + 38.0f), L"Data Folder", act::SetOpenDataFolder, BtnStyle::Secondary, gpu_.fSmall);
            Button(Rc(R - 102.0f, y + 10.0f, R - 10.0f, y + 38.0f),
                   u.pendingResetConfirm ? L"Confirm?" : L"Reset All",
                   act::SetResetAll, BtnStyle::Danger, gpu_.fSmall);
            y = bankCard.bottom + 10.0f;
        }

        // Card 2: Virtual Mouse & Keyboard Device Identity Presets
        {
            const D2D1_RECT_F idCard = Rc(L, y, R, y + 150.0f);
            GlassCard(idCard, 9.0f, true, false, theme::GlassBorder, ColSignal(), 0.14f);
            const float cL = L + 12.0f;
            const float cR = R - 12.0f;
            Text(L"Virtual Device Identity", gpu_.fBodyStrong, Rc(cL, idCard.top + 8.0f, cL + 220.0f, idCard.top + 28.0f), theme::Text);
            wchar_t busBadge[96]{};
            UINT32 badgeColor = theme::Muted;
            if (u.outputState == OutputState::Ready) {
                std::swprintf(busBadge, std::size(busBadge), L"Live \u2022 Mouse #%d / Keyboard #%d", u.mouseTransportIdx, u.kbTransportIdx);
                badgeColor = theme::Ok;
            } else if (u.outputState == OutputState::NeedsAdmin) {
                wcscpy_s(busBadge, L"Admin required");
                badgeColor = theme::Warn;
            } else if (u.outputState == OutputState::Starting) {
                wcscpy_s(busBadge, L"Connecting\u2026");
                badgeColor = theme::Faint;
            } else {
                wcscpy_s(busBadge, L"Inactive");
                badgeColor = theme::Danger;
            }
            Text(busBadge, gpu_.fSmall, Rc(cL + 210.0f, idCard.top + 8.0f, cR, idCard.top + 28.0f), badgeColor, DWRITE_TEXT_ALIGNMENT_TRAILING);

            const float colW = (cR - cL - 12.0f) * 0.5f;
            auto pill = [&](const D2D1_RECT_F& pr, const wchar_t* lbl, bool active, int action) noexcept {
                const bool hov = Hovered(action);
                if (active) {
                    Fill(pr, 5.0f, ColSignal(), 0.16f);
                    Stroke(pr, 5.0f, ColSignal(), 1.0f, 0.85f);
                    Dot(pr.left + 11.0f, (pr.top + pr.bottom) * 0.5f, 3.2f, ColSignalHi());
                } else {
                    if (hov) Fill(pr, 5.0f, theme::Raised, 0.85f);
                    Stroke(pr, 5.0f, hov ? theme::LineHi : theme::Line);
                    Dot(pr.left + 11.0f, (pr.top + pr.bottom) * 0.5f, 2.8f, hov ? theme::Muted : theme::LineHi);
                }
                Text(lbl, active ? gpu_.fBodyStrong : gpu_.fSmall, Rc(pr.left + 22.0f, pr.top, pr.right - 6.0f, pr.bottom),
                     active ? ColSignalHi() : (hov ? theme::Text : theme::Muted));
                Hit(pr, action);
            };

            const float topP = idCard.top + 34.0f;
            for (int i = 0; i < kMouseProfileCount; ++i) {
                pill(RcW(cL, topP + static_cast<float>(i) * 28.0f, colW, 24.0f), kMouseProfiles[i].label,
                     std::wcscmp(u.mouseProfileId, kMouseProfiles[i].id) == 0, act::SelectMouseProfile + i);
            }
            for (int i = 0; i < kKbProfileCount; ++i) {
                pill(RcW(cL + colW + 12.0f, topP + static_cast<float>(i) * 28.0f, colW, 24.0f), kKbProfiles[i].label,
                     std::wcscmp(u.keyboardProfileId, kKbProfiles[i].id) == 0, act::SelectKbProfile + i);
            }
            y = idCard.bottom + 10.0f;
        }

        // Card 3: Live Device Telemetry & Connected Monitors + Timing Diagnostics
        const float remH = std::max(210.0f, bottomLimit - y);
        const D2D1_RECT_F diagCard = Rc(L, y, R, y + remH);
        GlassCard(diagCard, 9.0f, true, false, theme::GlassBorder, ColSignal(), 0.14f);

        const float cL = L + 12.0f;
        const float cR = R - 12.0f;
        const float innerW = cR - cL;

        const auto& snap = frame_.snap;
        const float tGap = 8.0f;
        const float tW = (innerW - 3.0f * tGap) * 0.25f;
        const float tY = diagCard.top + 10.0f;
        auto tile = [&](int col, const wchar_t* k, const wchar_t* v, UINT32 vc, const wchar_t* sub) noexcept {
            const D2D1_RECT_F tr = RcW(cL + static_cast<float>(col) * (tW + tGap), tY, tW, 54.0f);
            Fill(tr, 6.0f, theme::Panel, 0.85f);
            Stroke(tr, 6.0f, theme::Line);
            Text(k, gpu_.fMini, Rc(tr.left + 8.0f, tr.top + 4.0f, tr.right - 6.0f, tr.top + 17.0f), theme::Faint);
            Text(v, gpu_.fBodyStrong, Rc(tr.left + 8.0f, tr.top + 18.0f, tr.right - 6.0f, tr.top + 36.0f), vc);
            Text(sub, gpu_.fMini, Rc(tr.left + 8.0f, tr.top + 36.0f, tr.right - 6.0f, tr.bottom - 3.0f), theme::Muted);
        };

        const bool giActive = g_SharedState.gameInputConnected.load(std::memory_order_relaxed);
        const int reportedDevs = std::max<int>(u.physicalDeviceCount, static_cast<int>(snap.gameInputDeviceCount));
        wchar_t s0[44]{}, p50[32]{}, p99[44]{}, tsc[36]{}, evP[32]{}, evS[36]{};
        std::swprintf(s0, std::size(s0), L"%d devices \u2022 %d screens", reportedDevs, u.monitorCount);
        tile(0, L"Input & Displays", giActive ? L"GameInput + Raw" : L"Raw Input", giActive ? ColSecondary() : ColSignalHi(), s0);

        std::swprintf(p50, std::size(p50), L"P50 %llu ns", static_cast<unsigned long long>(snap.p50JitterNs));
        std::swprintf(p99, std::size(p99), L"P99 %llu / Max %llu", static_cast<unsigned long long>(snap.p99JitterNs), static_cast<unsigned long long>(snap.maxJitterNs));
        tile(1, L"Timing error", p50, theme::Ok, p99);

        std::swprintf(tsc, std::size(tsc), L"%.2f GHz TSC", static_cast<double>(snap.calibratedTscFreqHz) / 1e9);
        tile(2, L"Fast path", snap.avx2Bmi2Active ? L"AVX2 + BMI2" : L"SSE2", ColSignalHi(), tsc);

        std::swprintf(evP, std::size(evP), L"%llu", static_cast<unsigned long long>(snap.totalEventsDispatched));
        std::swprintf(evS, std::size(evS), L"%u active", static_cast<unsigned>(snap.activeSlotCount));
        tile(3, L"Reports sent", evP, theme::Text, evS);

        const float botY = tY + 62.0f;
        const float botH = std::max(100.0f, diagCard.bottom - botY - 10.0f);
        const float splitW = (innerW - 10.0f) * 0.5f;

        const D2D1_RECT_F histR = RcW(cL, botY, splitW, botH);
        Fill(histR, 6.0f, theme::Panel, 0.85f);
        Stroke(histR, 6.0f, theme::Line);
        uint32_t maxCnt = 1U;
        for (int b = 0; b < 4; ++b) maxCnt = std::max(maxCnt, snap.jitterHistogram[b]);
        Text(L"Jitter Distribution", gpu_.fBodyStrong, Rc(histR.left + 10.0f, histR.top + 6.0f, histR.right - 10.0f, histR.top + 24.0f), theme::Text);
        static constexpr const wchar_t* kBkt[4] = { L"<100ns", L"100-500ns", L"0.5-2us", L">=2us" };
        const UINT32 bktCol[4] = { theme::Ok, ColSecondary(), ColSignal(), theme::Warn };
        const float bL = histR.left + 74.0f;
        const float bR = histR.right - 46.0f;
        const float bSpan = std::max(20.0f, bR - bL);
        for (int b = 0; b < 4; ++b) {
            const float ry = histR.top + 28.0f + static_cast<float>(b) * 19.0f;
            if (ry + 16.0f > histR.bottom) break;
            Text(kBkt[b], gpu_.fMini, Rc(histR.left + 10.0f, ry, bL - 4.0f, ry + 16.0f), theme::Muted);
            Fill(Rc(bL, ry + 4.0f, bR, ry + 11.0f), 3.5f, theme::Inset);
            const uint32_t cnt = snap.jitterHistogram[b];
            if (cnt > 0U) {
                const float fw = std::max(4.0f, bSpan * (static_cast<float>(cnt) / static_cast<float>(maxCnt)));
                Fill(RcW(bL, ry + 4.0f, fw, 7.0f), 3.5f, bktCol[b]);
            }
            wchar_t cb[16]{};
            std::swprintf(cb, std::size(cb), L"%u", static_cast<unsigned>(cnt));
            Text(cb, gpu_.fAxis, Rc(bR + 4.0f, ry, histR.right - 8.0f, ry + 16.0f), cnt > 0 ? theme::Text : theme::Faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        const D2D1_RECT_F devR = RcW(cL + splitW + 10.0f, botY, splitW, botH);
        Fill(devR, 6.0f, theme::Panel, 0.85f);
        Stroke(devR, 6.0f, theme::Line);
        Text(L"Devices & Monitors", gpu_.fBodyStrong, Rc(devR.left + 10.0f, devR.top + 6.0f, devR.right - 66.0f, devR.top + 24.0f), theme::Text);
        Button(Rc(devR.right - 34.0f, devR.top + 5.0f, devR.right - 6.0f, devR.top + 25.0f), L"\u21BB", act::SetRefreshHw, BtnStyle::Ghost, gpu_.fBodyStrong);

        const float reserveMonH = static_cast<float>(std::clamp(u.monitorCount, 1, 2)) * 16.0f + 4.0f;
        float dy = devR.top + 27.0f;
        if (u.physicalDeviceCount == 0) {
            Text(L"No physical HID devices reported.", gpu_.fMini, Rc(devR.left + 10.0f, dy, devR.right - 10.0f, dy + 16.0f), theme::Faint);
            dy += 17.0f;
        } else {
            for (int i = 0; i < u.physicalDeviceCount && dy + 16.0f <= devR.bottom - reserveMonH; ++i) {
                const auto& d = u.physicalDevices[i];
                wchar_t line[80]{};
                std::swprintf(line, std::size(line), L"\u2022 %ls (%04X:%04X)", d.displayName,
                              static_cast<unsigned>(d.vendorId), static_cast<unsigned>(d.productId));
                Text(line, gpu_.fMini, Rc(devR.left + 10.0f, dy, devR.right - 10.0f, dy + 16.0f), theme::Muted);
                dy += 16.0f;
            }
        }
        for (int m = 0; m < u.monitorCount && dy + 16.0f <= devR.bottom - 4.0f; ++m) {
            const auto& mn = u.monitors[m];
            wchar_t ml[80]{};
            std::swprintf(ml, std::size(ml), L"Mon %d: %dx%d @ %u DPI%ls", m + 1,
                          static_cast<int>(mn.right - mn.left), static_cast<int>(mn.bottom - mn.top),
                          static_cast<unsigned>(mn.dpi), mn.isPrimary ? L"*" : L"");
            Text(ml, gpu_.fMini, Rc(devR.left + 10.0f, dy, devR.right - 10.0f, dy + 16.0f), theme::Faint);
            dy += 16.0f;
        }
    }
}

// =====================================================================================
//  5. Clean Animated `+ New` Creation Picker Modal
// =====================================================================================

inline void UiRenderer::DrawNewPickerModal(float W, float H, float openT) noexcept {
    const auto& u = Ui();
    const auto& a = Anim();
    const D2D1_RECT_F full = Rc(0.0f, 0.0f, W, H);
    Fill(full, 0.0f, theme::Window, 0.68f * openT);
    if (u.showNewPicker) {
        Hit(full, act::NewPickerBackdrop);
    }

    const float scale = 0.92f + 0.08f * openT;
    const float slideY = (1.0f - openT) * 14.0f;
    const float baseW = std::min(520.0f, W - 48.0f);
    const float baseH = 264.0f;
    const float mw = baseW * scale;
    const float mh = baseH * scale;
    const float mx = (W - mw) * 0.5f;
    const float my = (H - mh) * 0.5f + slideY;
    const D2D1_RECT_F modal = RcW(mx, my, mw, mh);

    gpu_.FillRadialGlow((modal.left + modal.right) * 0.5f, (modal.top + modal.bottom) * 0.5f,
                        mw * 0.62f, mh * 0.70f, ColSignal(), 0.14f * openT, ColSignal(), 0.0f);
    gpu_.FillGlassCard(modal, 13.0f,
                       mask::ui::Rgba(0x050C1AU, 0.98f * openT),
                       mask::ui::Rgba(0x09152AU, 0.97f * openT),
                       mask::ui::Rgba(ColSignal(), 0.64f * openT),
                       0.26f * openT, a.cursorX.value, a.cursorY.value, false, ColSignal());

    if (u.showNewPicker) {
        Hit(modal, act::ModalSurface);
    }

    const float pad = 20.0f * scale;
    Text(L"New Item", gpu_.fTitle, Rc(modal.left + pad, modal.top + 14.0f * scale, modal.right - 70.0f, modal.top + 46.0f * scale),
         theme::Text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);

    Button(Rc(modal.right - pad - 28.0f, modal.top + 16.0f * scale, modal.right - pad, modal.top + 44.0f * scale),
           L"\u00D7", act::CloseNewPicker, BtnStyle::Ghost, gpu_.fHeading, openT);

    struct PickerCardDef {
        const wchar_t* badge;
        const wchar_t* title;
        const wchar_t* desc;
        UINT32         color;
        int            action;
    };
    const PickerCardDef cards[4] = {
        { L"M", L"Macro",       L"Rapid click, toggle, burst or snap-tap",  ColSignal(),    act::CreateMacro    },
        { L"T", L"Trigger Bot", L"React automatically to screen colors",    theme::Ok,      act::CreateTrigger  },
        { L"S", L"Sequence",    L"Multi-step workflow up to 128 actions",   ColSecondary(), act::CreateSequence },
        { L"R", L"Remap",       L"Rebind keys by device, screen or layer",  theme::Violet,  act::CreateRemap    },
    };

    const float gridTop = modal.top + 56.0f * scale;
    const float gap = 12.0f * scale;
    const float cw = (mw - 2.0f * pad - gap) * 0.5f;
    const float ch = (mh - 56.0f * scale - pad - gap) * 0.5f;

    for (int i = 0; i < 4; ++i) {
        const int row = i / 2;
        const int col = i % 2;
        const D2D1_RECT_F cr = RcW(modal.left + pad + static_cast<float>(col) * (cw + gap),
                                   gridTop + static_cast<float>(row) * (ch + gap), cw, ch);
        const D2D1_RECT_F sr = a.ApplySqueeze(cr, cards[i].action);
        const bool hov = Hovered(cards[i].action);

        gpu_.FillGlassCard(sr, 9.0f,
                           mask::ui::Rgba(hov ? theme::RaisedHi : theme::Raised, 0.88f * openT),
                           mask::ui::Rgba(hov ? theme::Raised : theme::Panel, 0.88f * openT),
                           mask::ui::Rgba(hov ? cards[i].color : theme::Line, (hov ? 0.90f : 0.82f) * openT),
                           hov ? 0.28f * openT : 0.14f * openT,
                           a.cursorX.value, a.cursorY.value, hov, cards[i].color);

        const D2D1_RECT_F br = RcW(sr.left + 14.0f, sr.top + 14.0f, 26.0f, 26.0f);
        Fill(br, 7.0f, cards[i].color, 0.18f * openT);
        Stroke(br, 7.0f, cards[i].color, 1.0f, 0.78f * openT);
        Text(cards[i].badge, gpu_.fBodyStrong, br, cards[i].color, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);

        Text(cards[i].title, gpu_.fHeading, Rc(br.right + 10.0f, sr.top + 13.0f, sr.right - 12.0f, sr.top + 41.0f),
             theme::Text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);
        Text(cards[i].desc, gpu_.fSmall, Rc(sr.left + 14.0f, sr.top + 48.0f, sr.right - 12.0f, sr.bottom - 10.0f),
             theme::Muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);

        if (u.showNewPicker) {
            Hit(cr, cards[i].action);
        }
    }
}

} // namespace mask::app
