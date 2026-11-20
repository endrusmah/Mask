#pragma once

// Secondary Editor Views, Inline Trigger Bot Editor, 5-Tab Settings Menu,
// Step-Kind Popover Menu, and Animated `+ New` Creation Picker Modal.

namespace mask::app {

// =====================================================================================
//  1. Complex Sequence Editor (up to 128 workflow steps)
// =====================================================================================

inline void UiRenderer::DrawComplexSequenceEditor(const D2D1_RECT_F& panel) noexcept {
    const int idx = g_Ui.selectedIndex;
    const auto& seq = g_Ui.sequences[idx];
    const bool running = (frame_.activeSeqMask & (1U << idx)) != 0U;
    const uint16_t execStep = frame_.activeSeqStep[idx];
    const uint8_t bindTarget = frame_.bindTarget;
    const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Sequence, idx));

    const float L = panel.left + 24.0f;
    const float R = panel.right - 24.0f;
    float y = panel.top + 14.0f;

    // ---- Header: inline rename | On/Off | Duplicate | Delete ----------------------
    const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
    Button(delBtn, g_Ui.pendingDeleteConfirm ? L"Confirm?" : L"Delete", act::DeleteItem,
           g_Ui.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

    const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
    Button(dupBtn, L"Duplicate", act::DuplicateItem, BtnStyle::Ghost);

    const float switchX = dupBtn.left - 16.0f - 34.0f;
    Switch(switchX, y + 8.0f, seq.enabled, act::ToggleEnabled, running, animIdx);
    Text(running ? L"Running" : (seq.enabled ? L"On" : L"Off"), fBody_,
         Rc(switchX - 72.0f, y, switchX - 10.0f, y + 36.0f),
         running ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Hit(Rc(switchX - 72.0f, y, switchX, y + 36.0f), act::ToggleEnabled);

    const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 80.0f, y + 36.0f);
    const int nameA = act::FieldFocus + static_cast<int>(Field::SeqName);
    if (g_Ui.editField == Field::SeqName) {
        Fill(nameRect, 6.0f, theme::Inset);
        Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
        Text(g_Ui.editBuffer, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
        const float caretX = Snap(L + MeasureWidth(g_Ui.editBuffer, fTitle_) + 1.5f);
        Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignal(), 1.5f);
    } else {
        if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised);
        Text(seq.name, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
    }
    Hit(nameRect, nameA);

    wchar_t seqDesc[180]{};
    DescribeComplexSequence(seq, seqDesc, std::size(seqDesc));
    Text(seqDesc, fBody_, Rc(L, y + 36.0f, R, y + 56.0f), seq.triggerVk == 0U ? theme::Warn : theme::Muted);
    y += 64.0f;

    // ---- Clean Trigger & Run Mode Row ---------------------------------------------
    constexpr float kTrigRowH = 34.0f;
    Text(L"Trigger", fLabel_, RcW(L, y, 56.0f, kTrigRowH), theme::Muted);
    KeyChip(RcW(L + 58.0f, y, 134.0f, kTrigRowH), seq.triggerVk, act::SeqBindTrigger, bindTarget == 20U);

    // Unified segmented control matching Macro Editor
    const float segX = L + 202.0f;
    const float segW = std::min(204.0f, R - segX - 118.0f);
    const D2D1_RECT_F seg = RcW(segX, y, segW, kTrigRowH);
    Fill(seg, 8.0f, theme::Inset);
    Stroke(seg, 8.0f, theme::Line);
    const float segCellW = (segW - 6.0f) / 3.0f;
    for (int m = 0; m < 3; ++m) {
        const D2D1_RECT_F cell = RcW(seg.left + 3.0f + segCellW * static_cast<float>(m), seg.top + 3.0f, segCellW, kTrigRowH - 6.0f);
        const bool sel = (seq.triggerMode == static_cast<uint8_t>(m));
        const int a = act::SeqTriggerMode + m;
        if (sel) {
            Fill(cell, 6.0f, theme::RaisedHi);
            Fill(Rc(cell.left + 14.0f, cell.bottom - 2.5f, cell.right - 14.0f, cell.bottom - 0.5f), 1.0f, ColSignal());
        } else if (Hovered(a)) {
            Fill(cell, 6.0f, theme::Raised);
        }
        Text(SeqTriggerModeName(static_cast<uint8_t>(m)), sel ? fBodyStrong_ : fBody_, cell,
             (sel || Hovered(a)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(cell, a);
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
    const float humW = std::ceil(MeasureWidth(humBtnLbl, fSmall_)) + 18.0f;
    Button(Rc(R - humW, y + 4.0f, R, y + 30.0f), humBtnLbl, act::SeqCycleHumanize,
           seq.humanizePct > 0U ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
    y += kTrigRowH + 12.0f;

    // ---- Two-Column Workspace: Workflow Timeline (left) | Step Inspector (right) ---
    const float wsBot = panel.bottom - 14.0f;
    const float innerW = R - L;
    const float colGap = 14.0f;
    const float leftW = std::clamp((innerW - colGap) * 0.54f, 316.0f, 450.0f);
    const D2D1_RECT_F leftCard  = Rc(L, y, L + leftW, wsBot);
    const D2D1_RECT_F rightCard = Rc(leftCard.right + colGap, y, R, wsBot);

    Fill(leftCard, 10.0f, theme::Inset);
    Stroke(leftCard, 10.0f, theme::Line);

    const int stepCount = std::clamp<int>(seq.stepCount, 0, kComplexStepMax);
    const bool hasSelStep = (stepCount > 0 && g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < stepCount);

    // Left Card Header
    Text(L"Steps", fBodyStrong_, RcW(leftCard.left + 14.0f, leftCard.top + 8.0f, 48.0f, 24.0f), theme::Text);
    wchar_t cntBuf[24]{};
    std::swprintf(cntBuf, std::size(cntBuf), L"%u / %u",
                  static_cast<unsigned>(seq.stepCount), static_cast<unsigned>(kComplexStepMax));
    Text(cntBuf, fSmall_, RcW(leftCard.left + 62.0f, leftCard.top + 8.0f, 70.0f, 24.0f), theme::Faint);

    Button(Rc(leftCard.right - 72.0f, leftCard.top + 8.0f, leftCard.right - 10.0f, leftCard.top + 32.0f),
           L"+ Add", act::SeqStepAdd, BtnStyle::Primary, fSmall_);
    Button(Rc(leftCard.right - 130.0f, leftCard.top + 8.0f, leftCard.right - 78.0f, leftCard.top + 32.0f),
           L"Insert", act::SeqStepInsert, BtnStyle::Ghost, fSmall_);

    Line(leftCard.left + 1.0f, Snap(leftCard.top + 40.0f), leftCard.right - 1.0f, Snap(leftCard.top + 40.0f), theme::Line);

    seqListRect_ = Rc(leftCard.left + 6.0f, leftCard.top + 42.0f, leftCard.right - 6.0f, leftCard.bottom - 6.0f);
    const float rowPitch = kSeqStepRowH + 4.0f;
    const float contentH = static_cast<float>(stepCount) * rowPitch + 8.0f;
    const float viewH = seqListRect_.bottom - seqListRect_.top;
    seqStepMaxScroll_ = std::max(0.0f, contentH - viewH);
    g_Ui.seqStepScroll = std::clamp(g_Ui.seqStepScroll, 0.0f, seqStepMaxScroll_);

    if (stepCount == 0) {
        const float midY = (seqListRect_.top + seqListRect_.bottom) * 0.5f - 18.0f;
        Text(L"No steps yet", fBodyStrong_, Rc(seqListRect_.left + 12.0f, midY, seqListRect_.right - 12.0f, midY + 20.0f),
             theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"Click + Add above to create your first step.", fSmall_,
             Rc(seqListRect_.left + 12.0f, midY + 20.0f, seqListRect_.right - 12.0f, midY + 38.0f),
             theme::Faint, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        PushClip(seqListRect_);
        float sy = seqListRect_.top + 4.0f - g_Ui.seqStepScroll;
        int loopDepth = 0;
        for (int i = 0; i < stepCount; ++i) {
            const auto& step = seq.steps[i];
            const auto kind = static_cast<ComplexStepKind>(step.kind);
            if (kind == ComplexStepKind::LoopEnd && loopDepth > 0) {
                --loopDepth;
            }
            const int visDepth = std::min(loopDepth, 3);
            const float indent = static_cast<float>(visDepth) * 12.0f;

            const D2D1_RECT_F rr = Rc(seqListRect_.left + 2.0f, sy, seqListRect_.right - 8.0f, sy + kSeqStepRowH);
            if (rr.bottom >= seqListRect_.top && rr.top <= seqListRect_.bottom) {
                for (int d = 0; d < visDepth; ++d) {
                    const float gx = Snap(rr.left + 6.0f + static_cast<float>(d) * 12.0f);
                    Line(gx, rr.top - 2.0f, gx, rr.bottom + 2.0f, theme::Violet, 1.0f, 0.30f);
                }

                const D2D1_RECT_F cardR = Rc(rr.left + indent, rr.top, rr.right, rr.bottom);
                const bool sel = (i == g_Ui.selectedSeqStep);
                const bool isLive = running && (execStep == static_cast<uint16_t>(i));
                const int actId = act::SelectSeqStep + i;

                if (isLive) {
                    Fill(cardR, 6.0f, ColSignal(), 0.22f);
                    Stroke(cardR, 6.0f, ColSignalHi(), 1.2f);
                } else if (sel) {
                    Fill(cardR, 6.0f, theme::RaisedHi);
                    Stroke(cardR, 6.0f, ColSecondary(), 1.0f, 0.55f);
                } else if (Hovered(actId)) {
                    Fill(cardR, 6.0f, theme::Raised, 0.85f);
                } else {
                    Fill(cardR, 6.0f, theme::Panel, 0.72f);
                }
                Hit(cardR, actId);

                UINT32 catCol = ColSecondary();
                if (kind == ComplexStepKind::WaitMs || kind == ComplexStepKind::WaitForColor) catCol = theme::Warn;
                else if (kind == ComplexStepKind::LoopStart || kind == ComplexStepKind::LoopEnd) catCol = theme::Violet;
                else if (kind == ComplexStepKind::LaunchProgram || kind == ComplexStepKind::RunCommand ||
                         kind == ComplexStepKind::SetLayer || kind == ComplexStepKind::ToggleMacro) catCol = theme::Ok;
                Fill(RcW(cardR.left + 7.0f, cardR.top + 8.0f, 3.5f, kSeqStepRowH - 16.0f), 1.8f, catCol);

                wchar_t numBuf[8]{};
                std::swprintf(numBuf, std::size(numBuf), L"%d", i + 1);
                Text(numBuf, fAxis_, RcW(cardR.left + 13.0f, cardR.top, 24.0f, kSeqStepRowH),
                     isLive ? ColSignalHi() : theme::Faint, DWRITE_TEXT_ALIGNMENT_CENTER);

                float descRight = cardR.right - 10.0f;
                if (kind == ComplexStepKind::WaitForColor) {
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
                Text(desc, sel ? fBodyStrong_ : fBody_, Rc(cardR.left + 40.0f, cardR.top, descRight, cardR.bottom),
                     (sel || isLive) ? theme::White : theme::Text);
            }

            if (kind == ComplexStepKind::LoopStart) {
                ++loopDepth;
            }
            sy += rowPitch;
        }
        PopClip();

        if (seqStepMaxScroll_ > 0.5f) {
            const float trackH = viewH - 8.0f;
            const float thumbH = std::max(28.0f, trackH * (viewH / contentH));
            const float t = g_Ui.seqStepScroll / seqStepMaxScroll_;
            Fill(RcW(leftCard.right - 5.0f, seqListRect_.top + 4.0f + t * (trackH - thumbH), 3.0f, thumbH), 1.5f, theme::LineHi);
        }
    }

    // ---- Right Card: Selected Step Inspector --------------------------------------
    Fill(rightCard, 10.0f, theme::Inset);
    Stroke(rightCard, 10.0f, theme::LineHi, 1.0f, 0.70f);

    if (!hasSelStep) {
        Text(L"Select a step on the left, or click + Add.", fBody_,
             Rc(rightCard.left + 16.0f, rightCard.top, rightCard.right - 16.0f, rightCard.bottom),
             theme::Faint, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    const ComplexStep& st = seq.steps[g_Ui.selectedSeqStep];
    const auto kind = static_cast<ComplexStepKind>(st.kind);
    const float iL = rightCard.left + 14.0f;
    const float iR = rightCard.right - 14.0f;
    const float iW = iR - iL;
    float iy = rightCard.top + 12.0f;

    wchar_t stepLbl[24]{};
    std::swprintf(stepLbl, std::size(stepLbl), L"Step %d", g_Ui.selectedSeqStep + 1);
    Text(stepLbl, fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
    iy += 20.0f;

    // Sleek Step-Kind Dropdown Button
    {
        seqStepKindBtnRect_ = RcW(iL, iy, iW, 32.0f);
        const D2D1_RECT_F sr = g_Animator.ApplySqueeze(seqStepKindBtnRect_, act::SeqToggleStepKindMenu);
        const bool open = g_Ui.showStepKindMenu;
        const bool hov = Hovered(act::SeqToggleStepKindMenu);
        Fill(sr, 7.0f, open ? theme::RaisedHi : (hov ? theme::RaisedHi : theme::Raised));
        Stroke(sr, 7.0f, open ? ColSignal() : (hov ? theme::LineHi : theme::Line), open ? 1.4f : 1.0f);
        Text(StepKindName(kind), fBodyStrong_, Rc(sr.left + 12.0f, sr.top, sr.right - 28.0f, sr.bottom),
             open ? ColSignalHi() : theme::Text);
        Text(L"\u25BE", fBodyStrong_, Rc(sr.right - 26.0f, sr.top, sr.right - 8.0f, sr.bottom),
             open ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(seqStepKindBtnRect_, act::SeqToggleStepKindMenu);
    }

    iy += 42.0f;
    Line(iL, Snap(iy), iR, Snap(iy), theme::Line);
    iy += 12.0f;

    wchar_t vbuf[48]{};
    switch (kind) {
        case ComplexStepKind::KeyClick: {
            Text(L"Key", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            KeyChip(RcW(iL, iy + 22.0f, iW, 34.0f), st.vk, act::SeqStepBindKey, bindTarget == 21U);
            Text(L"Hold for", fSmall_, RcW(iL, iy + 68.0f, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%u ms", static_cast<unsigned>(st.durationMs));
            GenericField(RcW(iL, iy + 90.0f, iW, 34.0f), Field::SeqStepDuration, vbuf, true);
            break;
        }
        case ComplexStepKind::KeyDown:
        case ComplexStepKind::KeyUp: {
            Text(L"Key", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            KeyChip(RcW(iL, iy + 22.0f, iW, 34.0f), st.vk, act::SeqStepBindKey, bindTarget == 21U);
            Text(kind == ComplexStepKind::KeyDown
                     ? L"Keeps this key pressed until a Release key step or Stop."
                     : L"Releases a key previously held down.",
                 fSmall_, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::MouseMoveRel:
        case ComplexStepKind::MouseMoveAbs: {
            const bool rel = (kind == ComplexStepKind::MouseMoveRel);
            const float hw = (iW - 10.0f) * 0.5f;
            Text(rel ? L"Move X" : L"Screen X", fSmall_, RcW(iL, iy, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.x));
            GenericField(RcW(iL, iy + 22.0f, hw, 34.0f), Field::SeqStepX, vbuf, true);

            Text(rel ? L"Move Y" : L"Screen Y", fSmall_, RcW(iL + hw + 10.0f, iy, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.y));
            GenericField(RcW(iL + hw + 10.0f, iy + 22.0f, hw, 34.0f), Field::SeqStepY, vbuf, true);

            Text(rel ? L"Positive right / down, negative left / up." : L"Moves pointer to exact screen coordinates.",
                 fSmall_, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::MouseScroll: {
            const int32_t delta = static_cast<int32_t>(st.param != 0U ? static_cast<int32_t>(st.param) : st.y);
            Text(L"Scroll amount", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%+d", static_cast<int>(delta));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepParam, vbuf, true);
            const float hw = (iW - 8.0f) * 0.5f;
            Button(RcW(iL, iy + 66.0f, hw, 28.0f), L"Scroll down", act::SeqStepQuickScroll + 0, BtnStyle::Chip, fSmall_);
            Button(RcW(iL + hw + 8.0f, iy + 66.0f, hw, 28.0f), L"Scroll up", act::SeqStepQuickScroll + 1, BtnStyle::Chip, fSmall_);
            break;
        }
        case ComplexStepKind::WaitMs: {
            Text(L"Wait for", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%u ms", static_cast<unsigned>(st.durationMs));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepDuration, vbuf, true);
            Text(L"Quick presets", fSmall_, RcW(iL, iy + 66.0f, iW, 18.0f), theme::Faint);
            static constexpr const wchar_t* kWaitLabels[4] = { L"10 ms", L"50 ms", L"250 ms", L"1 s" };
            const float hw = (iW - 8.0f) * 0.5f;
            for (int w = 0; w < 4; ++w) {
                const float px = iL + static_cast<float>(w % 2) * (hw + 8.0f);
                const float py = iy + 88.0f + static_cast<float>(w / 2) * 34.0f;
                Button(RcW(px, py, hw, 28.0f), kWaitLabels[w], act::SeqStepQuickWait + w, BtnStyle::Chip, fSmall_);
            }
            break;
        }
        case ComplexStepKind::WaitForColor: {
            const UINT32 rgb = (static_cast<UINT32>(st.r) << 16) | (static_cast<UINT32>(st.g) << 8) | static_cast<UINT32>(st.b);
            Fill(RcW(iL, iy, 32.0f, 32.0f), 6.0f, rgb);
            Stroke(RcW(iL, iy, 32.0f, 32.0f), 6.0f, theme::LineHi);
            Button(Rc(iL + 40.0f, iy, iR, iy + 32.0f),
                   g_Ui.seqColorPickActive ? L"Click screen\u2026" : L"Pick color & spot",
                   act::SeqStepPickColor, g_Ui.seqColorPickActive ? BtnStyle::Primary : BtnStyle::Secondary, fSmall_);

            const float hw = (iW - 10.0f) * 0.5f;
            const float r2 = iy + 44.0f;
            Text(L"Screen X", fSmall_, RcW(iL, r2, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.x));
            GenericField(RcW(iL, r2 + 20.0f, hw, 32.0f), Field::SeqStepX, vbuf, false);

            Text(L"Screen Y", fSmall_, RcW(iL + hw + 10.0f, r2, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%d", static_cast<int>(st.y));
            GenericField(RcW(iL + hw + 10.0f, r2 + 20.0f, hw, 32.0f), Field::SeqStepY, vbuf, false);

            const float r3 = r2 + 62.0f;
            Text(L"Leeway", fSmall_, RcW(iL, r3, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"\u00B1%u", static_cast<unsigned>(st.tolerance));
            GenericField(RcW(iL, r3 + 20.0f, hw, 32.0f), Field::SeqStepTol, vbuf, true);

            Text(L"Timeout", fSmall_, RcW(iL + hw + 10.0f, r3, hw, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"%u ms", static_cast<unsigned>(st.param));
            GenericField(RcW(iL + hw + 10.0f, r3 + 20.0f, hw, 32.0f), Field::SeqStepParam, vbuf, false);
            break;
        }
        case ComplexStepKind::LoopStart: {
            Text(L"Repeat count", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            if (st.param == 0U) wcscpy_s(vbuf, L"Forever");
            else std::swprintf(vbuf, std::size(vbuf), L"%u\u00D7", static_cast<unsigned>(st.param));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepParam, vbuf, true);
            Text(L"0 repeats forever until stopped. Pair with End loop.",
                 fSmall_, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::LoopEnd: {
            Text(L"Ends the matching Start loop step above.", fBody_,
                 Rc(iL, iy + 4.0f, iR, iy + 32.0f), theme::Muted);
            break;
        }
        case ComplexStepKind::LaunchProgram:
        case ComplexStepKind::RunCommand: {
            const bool isApp = (kind == ComplexStepKind::LaunchProgram);
            Text(isApp ? L"Application path" : L"Command", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepText, st.text, false, true,
                         isApp ? L"notepad.exe or C:\\..." : L"powershell -c \"...\"");
            Text(L"Click to type, or press Ctrl+V to paste.", fSmall_,
                 Rc(iL, iy + 66.0f, iR, iy + 86.0f), theme::Faint);
            break;
        }
        case ComplexStepKind::SetLayer: {
            Text(L"Switch to layer", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            const float lw = (iW - 15.0f) / 4.0f;
            for (int l = 0; l < kRemapLayerCount; ++l) {
                Button(RcW(iL + static_cast<float>(l) * (lw + 5.0f), iy + 22.0f, lw, 30.0f),
                       LayerName(static_cast<uint8_t>(l)), act::SeqStepLayer + l,
                       (st.param & 3U) == static_cast<uint32_t>(l) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            }
            break;
        }
        case ComplexStepKind::ToggleMacro: {
            const uint32_t slotIdx = st.param % static_cast<uint32_t>(kSlotCount);
            Text(L"Macro slot", fSmall_, RcW(iL, iy, iW, 18.0f), theme::Muted);
            std::swprintf(vbuf, std::size(vbuf), L"#%u", static_cast<unsigned>(slotIdx + 1U));
            GenericField(RcW(iL, iy + 22.0f, iW, 34.0f), Field::SeqStepParam, vbuf, true);
            wchar_t mLabel[64]{};
            std::swprintf(mLabel, std::size(mLabel), L"Target: \u201C%ls\u201D", g_Ui.slots[slotIdx].name);
            Text(mLabel, fBody_, Rc(iL, iy + 66.0f, iR, iy + 88.0f), theme::Muted);
            break;
        }
    }

    // Bottom Pinned Inspector Actions
    const float footY = rightCard.bottom - 42.0f;
    Line(iL, Snap(footY - 9.0f), iR, Snap(footY - 9.0f), theme::Line);
    const float fw = (iW - 18.0f) / 4.0f;
    Button(RcW(iL + 0.0f * (fw + 6.0f), footY, fw, 30.0f), L"\u2191 Up",   act::SeqStepUp,   BtnStyle::Ghost,  fSmall_);
    Button(RcW(iL + 1.0f * (fw + 6.0f), footY, fw, 30.0f), L"\u2193 Down", act::SeqStepDown, BtnStyle::Ghost,  fSmall_);
    Button(RcW(iL + 2.0f * (fw + 6.0f), footY, fw, 30.0f), L"Copy",        act::SeqStepDup,  BtnStyle::Ghost,  fSmall_);
    Button(RcW(iL + 3.0f * (fw + 6.0f), footY, fw, 30.0f), L"Delete",      act::SeqStepDel,  BtnStyle::Danger, fSmall_);
}

// Clean grouped popover for selecting any ComplexStepKind
inline void UiRenderer::DrawStepKindPopover(float W, float H) noexcept {
    Hit(Rc(0.0f, 0.0f, W, H), act::PopoverBackdrop);

    const auto& seq = g_Ui.SelectedSeq();
    const uint8_t curKind = (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount))
        ? seq.steps[g_Ui.selectedSeqStep].kind : 0U;

    constexpr float popW = 438.0f;
    constexpr float popH = 212.0f;
    const float anchorR = (seqStepKindBtnRect_.right > seqStepKindBtnRect_.left)
        ? seqStepKindBtnRect_.right : (W + popW) * 0.5f;
    const float px = std::clamp(anchorR - popW, 12.0f, std::max(12.0f, W - popW - 12.0f));
    float py = seqStepKindBtnRect_.bottom + 6.0f;
    if (py + popH > H - 12.0f) {
        py = std::max(60.0f, seqStepKindBtnRect_.top - popH - 6.0f);
    }

    const D2D1_RECT_F pop = RcW(px, py, popW, popH);
    Fill(Rc(pop.left - 2.0f, pop.top - 2.0f, pop.right + 2.0f, pop.bottom + 5.0f), 11.0f, 0x05070B, 0.50f);
    Fill(pop, 9.0f, theme::Panel);
    Stroke(pop, 9.0f, theme::LineHi, 1.2f);
    Hit(pop, act::ModalSurface);

    struct ColGroup {
        const wchar_t* title;
        UINT32         dotCol;
        int            kinds[6];
        int            count;
    };
    const ColGroup groups[3] = {
        { L"INPUT",          ColSecondary(),  { 0, 1, 2, 3, 4, 5 },       6 },
        { L"TIMING & LOOPS", theme::Warn,     { 6, 7, 8, 9, -1, -1 },     4 },
        { L"SYSTEM",         theme::Ok,       { 10, 11, 12, 13, -1, -1 }, 4 }
    };

    const float pad = 12.0f;
    const float gap = 8.0f;
    const float colW = (popW - 2.0f * pad - 2.0f * gap) / 3.0f;

    for (int c = 0; c < 3; ++c) {
        const float cx = pop.left + pad + static_cast<float>(c) * (colW + gap);
        float cy = pop.top + 11.0f;
        Dot(cx + 8.0f, cy + 8.0f, 3.0f, groups[c].dotCol);
        Text(groups[c].title, fMini_, RcW(cx + 16.0f, cy, colW - 18.0f, 16.0f), theme::Faint);
        cy += 22.0f;

        for (int r = 0; r < groups[c].count; ++r) {
            const int k = groups[c].kinds[r];
            const int actId = act::SeqStepKindBase + k;
            const D2D1_RECT_F itemR = RcW(cx, cy, colW, 25.0f);
            const bool active = (curKind == static_cast<uint8_t>(k));
            const bool hov = Hovered(actId);
            if (active) {
                Fill(itemR, 5.0f, ColSignal(), 0.16f);
                Stroke(itemR, 5.0f, ColSignal(), 1.0f, 0.85f);
            } else if (hov) {
                Fill(itemR, 5.0f, theme::Raised);
            }
            Text(StepKindName(static_cast<ComplexStepKind>(k)),
                 active ? fBodyStrong_ : fSmall_,
                 Rc(itemR.left + 8.0f, itemR.top, itemR.right - 6.0f, itemR.bottom),
                 active ? ColSignalHi() : (hov ? theme::Text : theme::Muted));
            Hit(itemR, actId);
            cy += 27.0f;
        }
    }
}

// =====================================================================================
//  2. Multi-Device / Multi-Monitor Remapping Editor
// =====================================================================================

inline void UiRenderer::DrawRemapEditor(const D2D1_RECT_F& panel) noexcept {
    const int idx = g_Ui.selectedIndex;
    const auto& r = g_Ui.remaps[idx];
    const uint8_t bindTarget = frame_.bindTarget;
    const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Remap, idx));

    const float L = panel.left + 24.0f;
    const float R = panel.right - 24.0f;
    const float C = L + 108.0f;
    float y = panel.top + 14.0f;

    // ---- Header: inline rename | On/Off | Duplicate | Delete ----------------------
    const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
    Button(delBtn, g_Ui.pendingDeleteConfirm ? L"Confirm?" : L"Delete", act::DeleteItem,
           g_Ui.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

    const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
    Button(dupBtn, L"Duplicate", act::DuplicateItem, BtnStyle::Ghost);

    const float switchX = dupBtn.left - 16.0f - 34.0f;
    Switch(switchX, y + 8.0f, r.enabled, act::ToggleEnabled, false, animIdx);
    Text(r.enabled ? L"On" : L"Off", fBody_, Rc(switchX - 64.0f, y, switchX - 10.0f, y + 36.0f),
         r.enabled ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Hit(Rc(switchX - 64.0f, y, switchX, y + 36.0f), act::ToggleEnabled);

    const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 72.0f, y + 36.0f);
    const int nameA = act::FieldFocus + static_cast<int>(Field::RemapName);
    if (g_Ui.editField == Field::RemapName) {
        Fill(nameRect, 6.0f, theme::Inset);
        Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
        Text(g_Ui.editBuffer, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
        const float caretX = Snap(L + MeasureWidth(g_Ui.editBuffer, fTitle_) + 1.5f);
        Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignal(), 1.5f);
    } else {
        if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised);
        Text(r.name, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
    }
    Hit(nameRect, nameA);

    wchar_t sentence[200]{};
    DescribeRemapRule(r, sentence, std::size(sentence));
    Text(sentence, fBody_, Rc(L, y + 36.0f, R, y + 56.0f), r.sourceVk == 0 ? theme::Warn : theme::Muted);
    y += 62.0f;

    // ---- Compact Live Layer & Lock State Strip ------------------------------------
    const D2D1_RECT_F liveBar = Rc(L, y, R, y + 40.0f);
    Fill(liveBar, 8.0f, theme::Inset);
    Stroke(liveBar, 8.0f, theme::Line);

    Text(L"Layer", fSmall_, Rc(liveBar.left + 12.0f, liveBar.top, liveBar.left + 64.0f, liveBar.bottom), theme::Muted);
    float lx = liveBar.left + 66.0f;
    for (int l = 0; l < kRemapLayerCount; ++l) {
        const bool active = (frame_.activeRemapLayer == static_cast<uint8_t>(l));
        Button(RcW(lx, liveBar.top + 7.0f, 54.0f, 26.0f), LayerName(static_cast<uint8_t>(l)),
               act::RemapLiveLayer + l, active ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
        lx += 60.0f;
    }

    const D2D1_RECT_F lockBtn = Rc(liveBar.right - 110.0f, liveBar.top + 7.0f, liveBar.right - 10.0f, liveBar.bottom - 7.0f);
    Button(lockBtn, frame_.remapUnlocked ? L"Unlocked" : L"Locked",
           act::RemapToggleUnlockLive, frame_.remapUnlocked ? BtnStyle::Secondary : BtnStyle::ChipOn, fSmall_);
    y = liveBar.bottom + 14.0f;

    constexpr float kRowH = 34.0f;
    constexpr float kGap  = 11.0f;

    // ---- Source Device Selector ---------------------------------------------------
    RowLabel(L, y, kRowH, L"Device");
    const float devBtnW = std::min(280.0f, R - C - 208.0f);
    Button(RcW(C, y, devBtnW, kRowH), r.anyDevice ? L"Any physical device" : r.deviceLabel,
           act::RemapCycleDevice, BtnStyle::Secondary, fBodyStrong_);
    Button(RcW(C + devBtnW + 6.0f, y + 2.0f, 30.0f, kRowH - 4.0f), L"\u21BB",
           act::RemapRefreshDevices, BtnStyle::Ghost, fBodyStrong_);
    static constexpr const wchar_t* kDevKinds[3] = { L"Any", L"KB", L"Mouse" };
    float dx = C + devBtnW + 44.0f;
    for (int k = 0; k < 3; ++k) {
        const float kw = std::ceil(MeasureWidth(kDevKinds[k], fSmall_)) + 16.0f;
        if (dx + kw <= R) {
            Button(RcW(dx, y + 4.0f, kw, 26.0f), kDevKinds[k], act::RemapDevKind + k,
                   r.deviceKind == static_cast<uint8_t>(k) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            dx += kw + 4.0f;
        }
    }
    y += kRowH + kGap;

    // ---- Monitor Scope ------------------------------------------------------------
    RowLabel(L, y, kRowH, L"Monitor");
    float mx = C;
    Button(RcW(mx, y + 4.0f, 88.0f, 26.0f), L"Any screen", act::RemapMonitor + 0,
           r.monitorIndex < 0 ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
    mx += 94.0f;
    const int monCount = std::max(1, std::min(g_Ui.monitorCount, 6));
    for (int m = 0; m < monCount; ++m) {
        wchar_t mLbl[24]{};
        std::swprintf(mLbl, std::size(mLbl), L"Mon %d%ls", m + 1, (m < g_Ui.monitorCount && g_Ui.monitors[m].isPrimary) ? L"*" : L"");
        const float mw = std::ceil(MeasureWidth(mLbl, fSmall_)) + 16.0f;
        if (mx + mw > R) break;
        Button(RcW(mx, y + 4.0f, mw, 26.0f), mLbl, act::RemapMonitor + 1 + m,
               r.monitorIndex == static_cast<int8_t>(m) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
        mx += mw + 5.0f;
    }
    y += kRowH + kGap;

    // ---- Action Kind (single concise row of 6 pills) ------------------------------
    RowLabel(L, y, kRowH, L"Action");
    float ax = C;
    for (int k = 0; k < 6; ++k) {
        const wchar_t* lbl = RemapActionName(static_cast<RemapLayerAction>(k));
        const float aw = std::ceil(MeasureWidth(lbl, fSmall_)) + 16.0f;
        if (ax + aw > R) break;
        Button(RcW(ax, y + 4.0f, aw, 26.0f), lbl, act::RemapActionKind + k,
               r.actionKind == static_cast<uint8_t>(k) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
        ax += aw + 5.0f;
    }
    y += kRowH + kGap;

    // ---- Source Key -> Target Key / Layer / Monitor -------------------------------
    RowLabel(L, y, kRowH, L"Binding");
    const D2D1_RECT_F srcChip = RcW(C, y, 148.0f, kRowH);
    KeyChip(srcChip, r.sourceVk, act::RemapBindSource, bindTarget == 22U);
    ArrowRight(srcChip.right + 10.0f, Snap(y + kRowH * 0.5f - 1.0f), 18.0f, theme::Faint);
    const float tgtX = srcChip.right + 38.0f;

    const auto actKind = static_cast<RemapLayerAction>(r.actionKind);
    if (actKind == RemapLayerAction::RemapKey) {
        KeyChip(RcW(tgtX, y, 148.0f, kRowH), r.targetVk, act::RemapBindTarget, bindTarget == 23U);
    } else if (actKind == RemapLayerAction::ShiftLayerHold || actKind == RemapLayerAction::ToggleLayer) {
        float tlx = tgtX;
        for (int l = 0; l < kRemapLayerCount; ++l) {
            Button(RcW(tlx, y + 4.0f, 54.0f, 26.0f), LayerName(static_cast<uint8_t>(l)),
                   act::RemapTargetLayer + l, r.targetLayer == static_cast<uint8_t>(l) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            tlx += 60.0f;
        }
    } else if (actKind == RemapLayerAction::WarpToMonitor) {
        float tmx = tgtX;
        for (int m = 0; m < monCount; ++m) {
            wchar_t ml[20]{};
            std::swprintf(ml, std::size(ml), L"Mon %d", m + 1);
            const bool sel = (r.monitorIndex == static_cast<int8_t>(m)) || (r.monitorIndex < 0 && m == 0);
            Button(RcW(tmx, y + 4.0f, 56.0f, 26.0f), ml, act::RemapMonitor + 1 + m,
                   sel ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            tmx += 62.0f;
        }
    } else {
        Text(actKind == RemapLayerAction::BlockKey ? L"Blocks original key" : L"Toggles Lock / Unlock",
             fSmall_, Rc(tgtX, y, R, y + kRowH), theme::Muted);
    }
    y += kRowH + kGap;

    // ---- Rule Layer ---------------------------------------------------------------
    RowLabel(L, y, kRowH, L"Rule layer");
    float rlx = C;
    for (int l = 0; l < kRemapLayerCount; ++l) {
        Button(RcW(rlx, y + 4.0f, 58.0f, 26.0f), LayerName(static_cast<uint8_t>(l)),
               act::RemapRuleLayer + l, r.layerIndex == static_cast<uint8_t>(l) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
        rlx += 64.0f;
    }
    y += kRowH + kGap;

    // ---- Unlock Policy ------------------------------------------------------------
    RowLabel(L, y, kRowH, L"Condition");
    float upx = C;
    for (int p = 0; p < 3; ++p) {
        const wchar_t* pl = UnlockPolicyName(static_cast<RemapUnlockPolicy>(p));
        const float pw = std::ceil(MeasureWidth(pl, fSmall_)) + 18.0f;
        Button(RcW(upx, y + 4.0f, pw, 26.0f), pl, act::RemapUnlockPolicy + p,
               r.unlockPolicy == static_cast<uint8_t>(p) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
        upx += pw + 6.0f;
    }
    y += kRowH + kGap;

    // ---- Suppress Original Key Switch ---------------------------------------------
    RowLabel(L, y, kRowH, L"Suppress");
    Switch(C, y + 7.0f, r.suppressOriginal || actKind == RemapLayerAction::BlockKey, act::RemapToggleSuppress, false);
    Text(L"Block original key from reaching apps", fSmall_, Rc(C + 46.0f, y, R, y + kRowH), theme::Muted);
}

// =====================================================================================
//  3. Inline Color Trigger ("Trigger Bot") Editor
// =====================================================================================

inline void UiRenderer::DrawTriggerEditor(const D2D1_RECT_F& panel) noexcept {
    const int idx = g_Ui.selectedIndex;
    const auto& t = g_Ui.triggers[idx];
    const auto& pv = frame_.triggerPreview;
    const uint8_t bindTarget = frame_.bindTarget;
    const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Trigger, idx));

    const float L = panel.left + 24.0f;
    const float R = panel.right - 24.0f;
    const float C = L + 104.0f;
    float y = panel.top + 14.0f;

    // ---- Header: inline rename | On/Off | Duplicate | Delete ----------------------
    const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
    Button(delBtn, g_Ui.pendingDeleteConfirm ? L"Confirm?" : L"Delete", act::DeleteItem,
           g_Ui.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

    const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
    Button(dupBtn, L"Duplicate", act::DuplicateItem, BtnStyle::Ghost);

    const bool matchedLive = pv.isMatched && t.enabled;
    const float switchX = dupBtn.left - 16.0f - 34.0f;
    Switch(switchX, y + 8.0f, t.enabled, act::ToggleEnabled, matchedLive, animIdx);
    Text(matchedLive ? L"Matched" : (t.enabled ? L"On" : L"Off"), fBody_,
         Rc(switchX - 72.0f, y, switchX - 10.0f, y + 36.0f),
         matchedLive ? theme::Ok : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Hit(Rc(switchX - 72.0f, y, switchX, y + 36.0f), act::ToggleEnabled);

    const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 80.0f, y + 36.0f);
    const int nameA = act::FieldFocus + static_cast<int>(Field::TriggerName);
    if (g_Ui.editField == Field::TriggerName) {
        Fill(nameRect, 6.0f, theme::Inset);
        Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
        Text(g_Ui.editBuffer, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
        const float caretX = Snap(L + MeasureWidth(g_Ui.editBuffer, fTitle_) + 1.5f);
        Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignal(), 1.5f);
    } else {
        if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised);
        Text(t.name, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
    }
    Hit(nameRect, nameA);

    wchar_t sentence[200]{};
    DescribeColorTrigger(t, sentence, std::size(sentence));
    Text(sentence, fBody_, Rc(L, y + 36.0f, R, y + 56.0f), theme::Muted);
    y += 62.0f;

    // ---- Visual Watch Card: 15x15 Live Preview | Swatches | Presets | Leeway ------
    const float cardH = 164.0f;
    const D2D1_RECT_F card = Rc(L, y, R, y + cardH);
    Fill(card, 10.0f, theme::Inset);
    Stroke(card, 10.0f, theme::Line);

    const float loupeSz = 140.0f;
    const D2D1_RECT_F loupeFrame = RcW(card.left + 12.0f, card.top + 12.0f, loupeSz, loupeSz);
    const UINT32 loupeBorder = matchedLive ? theme::Ok : ((t.enabled && pv.isArmed) ? ColSignal() : theme::LineHi);
    Fill(loupeFrame, 8.0f, theme::Window);
    Stroke(loupeFrame, 8.0f, loupeBorder, matchedLive ? 1.8f : 1.0f);

    {
        constexpr int N = mask::triggers::ColorTriggerPreviewSnapshot::kLoupeSize; // 15
        const float gridL = loupeFrame.left + 3.0f;
        const float gridT = loupeFrame.top + 3.0f;
        const float cell  = (loupeSz - 6.0f) / static_cast<float>(N);
        const D2D1_ANTIALIAS_MODE prevAA = rt_->GetAntialiasMode();
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

        for (int py = 0; py < N; ++py) {
            for (int px = 0; px < N; ++px) {
                const uint32_t rgb = pv.loupePixels[py * N + px] & 0x00FFFFFFU;
                brush_->SetColor(D2D1::ColorF(rgb));
                rt_->FillRectangle(RcW(gridL + static_cast<float>(px) * cell,
                                       gridT + static_cast<float>(py) * cell, cell, cell), brush_);
            }
        }
        brush_->SetColor(D2D1::ColorF(0x000000U, 0.22f));
        for (int i = 1; i < N; ++i) {
            const float gx = gridL + static_cast<float>(i) * cell;
            const float gy = gridT + static_cast<float>(i) * cell;
            rt_->DrawLine(D2D1::Point2F(gx, gridT), D2D1::Point2F(gx, gridT + cell * N), brush_, 1.0f);
            rt_->DrawLine(D2D1::Point2F(gridL, gy), D2D1::Point2F(gridL + cell * N, gy), brush_, 1.0f);
        }

        const int half = N / 2;
        const int rad = std::clamp<int>(t.sampleRadius, 0, half);
        if (rad > 0) {
            const D2D1_RECT_F roiR = Rc(gridL + static_cast<float>(half - rad) * cell,
                                        gridT + static_cast<float>(half - rad) * cell,
                                        gridL + static_cast<float>(half + rad + 1) * cell,
                                        gridT + static_cast<float>(half + rad + 1) * cell);
            brush_->SetColor(D2D1::ColorF(ColSignalHi(), 0.85f));
            rt_->DrawRectangle(roiR, brush_, 1.4f);
        }

        const D2D1_RECT_F cCell = RcW(gridL + static_cast<float>(half) * cell,
                                      gridT + static_cast<float>(half) * cell, cell, cell);
        brush_->SetColor(D2D1::ColorF(0x000000U, 0.75f));
        rt_->DrawRectangle(Rc(cCell.left - 1.0f, cCell.top - 1.0f, cCell.right + 1.0f, cCell.bottom + 1.0f), brush_, 1.0f);
        brush_->SetColor(D2D1::ColorF(theme::White, 0.95f));
        rt_->DrawRectangle(cCell, brush_, 1.4f);
        rt_->SetAntialiasMode(prevAA);
    }

    const float rx = loupeFrame.right + 16.0f;
    const float rr = card.right - 14.0f;
    float ry = card.top + 12.0f;

    // Row 1 inside card: Human color comparison ("Seeing now" -> "Looking for") + human status pill
    {
        const UINT32 liveRgb = (static_cast<UINT32>(pv.sampledR) << 16) |
                               (static_cast<UINT32>(pv.sampledG) << 8)  |
                               static_cast<UINT32>(pv.sampledB);
        const UINT32 tgtRgb  = (static_cast<UINT32>(t.r) << 16) |
                               (static_cast<UINT32>(t.g) << 8)  |
                               static_cast<UINT32>(t.b);

        const D2D1_RECT_F swLive = RcW(rx, ry + 2.0f, 24.0f, 24.0f);
        Fill(swLive, 6.0f, liveRgb);
        Stroke(swLive, 6.0f, theme::LineHi);
        Text(L"Seeing now", fSmall_, RcW(swLive.right + 8.0f, ry, 72.0f, 28.0f), theme::Muted);

        ArrowRight(swLive.right + 82.0f, Snap(ry + 14.0f), 14.0f, theme::Faint);

        const float tgtX = swLive.right + 104.0f;
        if (t.mode == static_cast<uint8_t>(ColorTriggerMode::ColorChanged)) {
            Text(L"Any color change", fBodyStrong_, RcW(tgtX, ry, 130.0f, 28.0f), theme::Text);
        } else {
            const D2D1_RECT_F swTgt = RcW(tgtX, ry + 2.0f, 24.0f, 24.0f);
            Fill(swTgt, 6.0f, tgtRgb);
            Stroke(swTgt, 6.0f, theme::LineHi);
            Text(L"Target color", fSmall_, RcW(swTgt.right + 8.0f, ry, 86.0f, 28.0f), theme::Text);
        }

        const wchar_t* stTxt = !t.enabled ? L"Off"
                             : (matchedLive ? L"Matched" : (pv.isArmed ? L"Watching" : L"Standby"));
        const UINT32 stCol = !t.enabled ? theme::Faint
                           : (matchedLive ? theme::Ok : (pv.isArmed ? ColSignal() : theme::Warn));
        const D2D1_RECT_F pill = Rc(rr - 74.0f, ry + 2.0f, rr, ry + 26.0f);
        Fill(pill, 12.0f, stCol, t.enabled ? 0.16f : 0.08f);
        Stroke(pill, 12.0f, stCol, 1.0f, t.enabled ? 0.72f : 0.35f);
        Text(stTxt, fSmall_, pill, stCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        ry += 38.0f;
    }

    // Row 2 inside card: Pick button + Screen center chip + 6 clickable color preset dots
    {
        const bool isCenter = (t.sampleX < 0 || t.sampleY < 0);
        Button(RcW(rx, ry, 136.0f, 28.0f),
               g_Ui.triggerColorPickActive ? L"Click anywhere\u2026" : L"Pick color & spot",
               act::TrigPickPixel, g_Ui.triggerColorPickActive ? BtnStyle::Primary : BtnStyle::Secondary, fSmall_);
        Button(RcW(rx + 142.0f, ry, 98.0f, 28.0f), L"Screen center", act::TrigLockCenter,
               isCenter ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);

        if (t.mode == static_cast<uint8_t>(ColorTriggerMode::MatchColor)) {
            float px = rx + 250.0f;
            const float cy = ry + 14.0f;
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
                Dot(cx, cy, 7.2f, pRgb);
                Hit(RcW(px - 2.0f, ry, 24.0f, 28.0f), actId);
                px += 26.0f;
            }
        }
        ry += 40.0f;
    }

    // Row 3 inside card: Intuitive Color Leeway slider (only active visual for match tolerance)
    {
        Text(L"Color leeway", fSmall_, RcW(rx, ry, 120.0f, 18.0f), theme::Muted);
        wchar_t tolDesc[40]{};
        std::swprintf(tolDesc, std::size(tolDesc), L"%ls (\u00B1%u)",
                      ToleranceWord(t.tolerance), static_cast<unsigned>(t.tolerance));
        Text(tolDesc, fBodyStrong_, Rc(rx + 124.0f, ry, rr, ry + 18.0f), theme::Text, DWRITE_TEXT_ALIGNMENT_TRAILING);

        const float trL = rx + 6.0f;
        const float trR = rr - 6.0f;
        const float trackY = Snap(ry + 30.0f);
        trigTolTrack_ = Rc(trL, trackY - 11.0f, trR, trackY + 11.0f);

        const float frac = std::clamp(static_cast<float>(t.tolerance) / 150.0f, 0.0f, 1.0f);
        const float thumbX = trL + frac * std::max(0.0f, trR - trL);
        const bool hot = (g_Ui.draggingTriggerSlider == act::TrigSliderTol) || Hovered(act::TrigSliderTol);

        Fill(Rc(trL, trackY - 3.0f, trR, trackY + 3.0f), 3.0f, theme::Panel);
        Stroke(Rc(trL, trackY - 3.0f, trR, trackY + 3.0f), 3.0f, theme::Line);
        if (thumbX > trL + 1.0f) {
            Fill(Rc(trL, trackY - 3.0f, thumbX, trackY + 3.0f), 3.0f, ColSignal());
        }
        if (hot) Dot(thumbX, trackY, 12.0f, ColSignal(), 0.18f);
        Dot(thumbX, trackY, 7.5f, ColSignal());
        Dot(thumbX, trackY, 2.8f, ColOnSignal());
        Hit(Rc(rx, trackY - 12.0f, rr, trackY + 12.0f), act::TrigSliderTol);

        Text(L"Exact color", fMini_, RcW(rx, ry + 41.0f, 90.0f, 14.0f), theme::Faint);
        Text(L"Loose shades", fMini_, Rc(rr - 100.0f, ry + 41.0f, rr, ry + 55.0f), theme::Faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    y = card.bottom + 14.0f;

    // ---- Parameter Rows (matching Macro Editor segmented style) -------------------
    constexpr float kRowH = 34.0f;
    constexpr float kGap  = 10.0f;

    auto drawSegmented = [&](float sx, float sy, float sw, const wchar_t* const* labels,
                             int count, int curVal, int baseAction) noexcept {
        const D2D1_RECT_F seg = RcW(sx, sy, sw, kRowH);
        Fill(seg, 8.0f, theme::Inset);
        Stroke(seg, 8.0f, theme::Line);
        const float cellW = (sw - 6.0f) / static_cast<float>(count);
        for (int m = 0; m < count; ++m) {
            const D2D1_RECT_F cell = RcW(seg.left + 3.0f + cellW * static_cast<float>(m),
                                         seg.top + 3.0f, cellW, kRowH - 6.0f);
            const bool sel = (curVal == m);
            const int a = baseAction + m;
            if (sel) {
                Fill(cell, 6.0f, theme::RaisedHi);
                Fill(Rc(cell.left + 12.0f, cell.bottom - 2.5f, cell.right - 12.0f, cell.bottom - 0.5f), 1.0f, ColSignal());
            } else if (Hovered(a)) {
                Fill(cell, 6.0f, theme::Raised);
            }
            Text(labels[m], sel ? fBodyStrong_ : fSmall_, cell,
                 (sel || Hovered(a)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
            Hit(cell, a);
        }
    };

    // 1. Trigger on (Match / Change) + Area size
    RowLabel(L, y, kRowH, L"Trigger on");
    static constexpr const wchar_t* kTrigModeLbls[2] = { L"Color matches", L"Color changes" };
    drawSegmented(C, y, 224.0f, kTrigModeLbls, 2, t.mode, act::TrigMode);

    float ax = C + 238.0f;
    Text(L"Area", fSmall_, RcW(ax, y, 36.0f, kRowH), theme::Faint);
    ax += 38.0f;
    static constexpr uint8_t kRadVals[4] = { 0U, 1U, 2U, 4U };
    static constexpr const wchar_t* kRadLbls[4] = { L"1px", L"3px", L"5px", L"9px" };
    for (int k = 0; k < 4; ++k) {
        if (ax + 40.0f > R) break;
        Button(RcW(ax, y + 4.0f, 40.0f, 26.0f), kRadLbls[k], act::TrigRadius + k,
               t.sampleRadius == kRadVals[k] ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
        ax += 44.0f;
    }
    y += kRowH + kGap;

    // 2. Screen selector (only shown on multi-monitor systems to avoid unnecessary clutter)
    const int monCount = std::max(1, std::min(g_Ui.monitorCount, 8));
    if (monCount > 1) {
        RowLabel(L, y, kRowH, L"Screen");
        float mx = C;
        for (int m = 0; m < monCount; ++m) {
            const auto& mn = g_Ui.monitors[m];
            wchar_t mLbl[32]{};
            if (monCount <= 2) {
                std::swprintf(mLbl, std::size(mLbl), L"Screen %d (%dx%d)",
                              m + 1, static_cast<int>(mn.right - mn.left), static_cast<int>(mn.bottom - mn.top));
            } else {
                std::swprintf(mLbl, std::size(mLbl), L"Screen %d%ls", m + 1, mn.isPrimary ? L"*" : L"");
            }
            const float mw = std::ceil(MeasureWidth(mLbl, fSmall_)) + 18.0f;
            if (mx + mw > R) break;
            Button(RcW(mx, y + 4.0f, mw, 26.0f), mLbl, act::TrigMonitor + m,
                   t.monitorIndex == static_cast<uint8_t>(m) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            mx += mw + 5.0f;
        }
        y += kRowH + kGap;
    }

    // 3. Only when (Always / Hold key / Toggle) + KeyChip
    RowLabel(L, y, kRowH, L"Only when");
    static constexpr const wchar_t* kArmLbls[3] = { L"Always", L"Hold key", L"Toggle" };
    drawSegmented(C, y, 234.0f, kArmLbls, 3, t.armMode, act::TrigArmMode);
    if (t.armMode != 0U) {
        KeyChip(RcW(C + 244.0f, y, 138.0f, kRowH), t.armVk, act::TrigBindArmVk, bindTarget == 26U);
        if (t.armVk != 0U) {
            Button(RcW(C + 388.0f, y + 4.0f, 26.0f, 26.0f), L"\u00D7", act::TrigClearArmVk, BtnStyle::Ghost, fBodyStrong_);
        }
    }
    y += kRowH + kGap;

    // 4. Then do (Click key / Hold key / Run macro / Run sequence)
    RowLabel(L, y, kRowH, L"Then do");
    static constexpr const wchar_t* kActLbls[4] = { L"Click key", L"Hold key", L"Run macro", L"Sequence" };
    drawSegmented(C, y, std::min(340.0f, R - C), kActLbls, 4, t.actionKind, act::TrigActionKind);
    y += kRowH + kGap;

    // 5. Press / Target
    const auto actKind = static_cast<ColorTriggerActionKind>(t.actionKind);
    const bool isKeyAct = (actKind == ColorTriggerActionKind::ClickKey || actKind == ColorTriggerActionKind::HoldKey);
    RowLabel(L, y, kRowH, isKeyAct ? L"Press" : L"Target");
    if (isKeyAct) {
        KeyChip(RcW(C, y, 144.0f, kRowH), t.actionVk, act::TrigBindActionVk, bindTarget == 27U);
        static constexpr uint16_t kQuickVks[4] = { VK_LBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        static constexpr const wchar_t* kQuickLbls[4] = { L"Left click", L"Right click", L"Mouse 4", L"Mouse 5" };
        float qx = C + 154.0f;
        for (int q = 0; q < 4; ++q) {
            const float qw = std::ceil(MeasureWidth(kQuickLbls[q], fSmall_)) + 16.0f;
            if (qx + qw > R) break;
            Button(RcW(qx, y + 4.0f, qw, 26.0f), kQuickLbls[q], act::TrigQuickOut + q,
                   t.actionVk == kQuickVks[q] ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            qx += qw + 5.0f;
        }
    } else {
        const bool isMacro = (actKind == ColorTriggerActionKind::RunMacroSlot);
        const uint32_t maxSlots = isMacro ? static_cast<uint32_t>(kSlotCount) : static_cast<uint32_t>(kComplexSeqCount);
        const uint32_t slotIdx = t.linkedIndex % maxSlots;
        wchar_t slotStr[24]{};
        std::swprintf(slotStr, std::size(slotStr), L"#%u", static_cast<unsigned>(slotIdx + 1U));
        GenericField(RcW(C, y, 118.0f, kRowH), Field::TriggerLinked, slotStr, true);

        const wchar_t* linkedName = isMacro ? g_Ui.slots[slotIdx].name : g_Ui.sequences[slotIdx].name;
        wchar_t targetLbl[64]{};
        std::swprintf(targetLbl, std::size(targetLbl), L"\u201C%ls\u201D", linkedName);
        Text(targetLbl, fBody_, Rc(C + 130.0f, y, R, y + kRowH), theme::Muted);
    }
    y += kRowH + kGap;

    // 6. Reaction Timing (Delay / Hold / Cooldown)
    RowLabel(L, y, kRowH, L"Timing");
    wchar_t dBuf[24]{}, hBuf[24]{}, cBuf[24]{};
    std::swprintf(dBuf, std::size(dBuf), L"%u ms", static_cast<unsigned>(t.delayMs));
    std::swprintf(hBuf, std::size(hBuf), L"%u ms", static_cast<unsigned>(t.holdMs));
    std::swprintf(cBuf, std::size(cBuf), L"%u ms", static_cast<unsigned>(t.cooldownMs));

    float tx = C;
    Text(L"Delay", fSmall_, RcW(tx, y, 38.0f, kRowH), theme::Muted);
    tx += 40.0f;
    GenericField(RcW(tx, y, 96.0f, kRowH), Field::TriggerDelay, dBuf, true);
    tx += 108.0f;

    Text(L"Hold", fSmall_, RcW(tx, y, 32.0f, kRowH), theme::Muted);
    tx += 34.0f;
    GenericField(RcW(tx, y, 96.0f, kRowH), Field::TriggerHold, hBuf, true);
    tx += 108.0f;

    Text(L"Cooldown", fSmall_, RcW(tx, y, 60.0f, kRowH), theme::Muted);
    tx += 62.0f;
    GenericField(RcW(tx, y, std::min(104.0f, R - tx), kRowH), Field::TriggerCooldown, cBuf, true);
}

// =====================================================================================
//  4. 5-Tab Settings Menu
// =====================================================================================

inline void UiRenderer::DrawSettingsView(const D2D1_RECT_F& panel) noexcept {
    const float L = panel.left + 22.0f;
    const float R = panel.right - 22.0f;
    float y = panel.top + 12.0f;

    // Header
    Button(Rc(R - 74.0f, y + 2.0f, R, y + 32.0f), L"Done", act::ToggleSettings, BtnStyle::Secondary);
    Text(L"Settings", fTitle_, Rc(L, y, R - 86.0f, y + 34.0f), theme::Text);
    y += 40.0f;

    // Animated 5-tab strip
    static constexpr const wchar_t* kTabNames[5] = {
        L"General", L"Appearance", L"Performance", L"Hotkeys & Safety", L"Hardware & Profiles"
    };
    const D2D1_RECT_F tabStrip = Rc(L, y, R, y + 34.0f);
    Fill(tabStrip, 8.0f, theme::Inset);
    Stroke(tabStrip, 8.0f, theme::Line);

    float tx = tabStrip.left + 4.0f;
    const float stripInnerW = tabStrip.right - tabStrip.left - 8.0f;
    float totalWeight = 0.0f;
    float tabWeights[5]{};
    for (int i = 0; i < 5; ++i) {
        tabWeights[i] = MeasureWidth(kTabNames[i], fBodyStrong_) + 26.0f;
        totalWeight += tabWeights[i];
    }
    for (int i = 0; i < 5; ++i) {
        const float tw = stripInnerW * (tabWeights[i] / std::max(1.0f, totalWeight));
        settingsTabRects_[i] = Rc(tx, tabStrip.top + 3.0f, tx + tw, tabStrip.bottom - 3.0f);
        tx += tw;
    }

    // Sliding animated indicator pill
    const float indX = (g_Animator.settingsTabX.value > 1.0f) ? g_Animator.settingsTabX.value : settingsTabRects_[g_Ui.settingsTab].left;
    const float indW = (g_Animator.settingsTabW.value > 10.0f) ? g_Animator.settingsTabW.value : (settingsTabRects_[g_Ui.settingsTab].right - settingsTabRects_[g_Ui.settingsTab].left);
    const D2D1_RECT_F pillR = RcW(indX, tabStrip.top + 3.0f, indW, 28.0f);
    Fill(pillR, 6.0f, theme::RaisedHi);
    Fill(Rc(pillR.left + 12.0f, pillR.bottom - 2.5f, pillR.right - 12.0f, pillR.bottom - 0.5f), 1.0f, ColSignal());

    for (int i = 0; i < 5; ++i) {
        const bool sel = (g_Ui.settingsTab == i);
        const int a = act::SettingsTab + i;
        if (!sel && Hovered(a)) Fill(settingsTabRects_[i], 6.0f, theme::Raised, 0.55f);
        Text(kTabNames[i], sel ? fBodyStrong_ : fBody_, settingsTabRects_[i],
             (sel || Hovered(a)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(settingsTabRects_[i], a);
    }
    y = tabStrip.bottom + 14.0f;

    const auto& s = g_Ui.settings;

    auto toggleRow = [&](float& ry, const wchar_t* title, const wchar_t* sub, bool val, int action) noexcept {
        const D2D1_RECT_F rowR = Rc(L, ry, R, ry + 44.0f);
        Fill(rowR, 7.0f, theme::Inset);
        Stroke(rowR, 7.0f, theme::Line);
        Text(title, fBodyStrong_, Rc(L + 14.0f, ry + 4.0f, R - 64.0f, ry + 23.0f), theme::Text);
        Text(sub, fSmall_, Rc(L + 14.0f, ry + 22.0f, R - 64.0f, ry + 40.0f), theme::Faint);
        Switch(R - 48.0f, ry + 12.0f, val, action);
        ry += 50.0f;
    };

    if (g_Ui.settingsTab == 0) {
        // ---- Tab 0: General -------------------------------------------------------
        toggleRow(y, L"Start with Windows", L"Launch Mask automatically at sign-in.", s.startWithWindows, act::SetToggleStartWin);
        toggleRow(y, L"Start minimized", L"Open quietly in the background.", s.startMinimized, act::SetToggleStartMin);
        toggleRow(y, L"Minimize to system tray", L"Hide the taskbar button when minimized.", s.minimizeToTray, act::SetToggleMinTray);
        toggleRow(y, L"Always on top", L"Keep Mask above other windows.", s.alwaysOnTop, act::SetToggleTopmost);
        toggleRow(y, L"Remember window bounds", L"Restore window size and monitor position.", s.rememberWindowBounds, act::SetToggleBounds);
        toggleRow(y, L"Confirm before deleting", L"Require a second click on Delete.", s.confirmDelete, act::SetToggleConfirmDel);

        const D2D1_RECT_F rowR = Rc(L, y, R, y + 46.0f);
        Fill(rowR, 7.0f, theme::Inset);
        Stroke(rowR, 7.0f, theme::Line);
        Text(L"Autosave delay", fBodyStrong_, Rc(L + 14.0f, y + 5.0f, R - 170.0f, y + 24.0f), theme::Text);
        Text(L"Delay after edits before writing profiles to disk.", fSmall_, Rc(L + 14.0f, y + 23.0f, R - 170.0f, y + 41.0f), theme::Faint);
        wchar_t asBuf[32]{};
        std::swprintf(asBuf, std::size(asBuf), L"%u ms", static_cast<unsigned>(s.autosaveDelayMs));
        GenericField(Rc(R - 144.0f, y + 8.0f, R - 12.0f, y + 38.0f), Field::SettingsAutosaveMs, asBuf, true);
    } else if (g_Ui.settingsTab == 1) {
        // ---- Tab 1: Appearance ----------------------------------------------------
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 54.0f);
            Fill(box, 7.0f, theme::Inset);
            Stroke(box, 7.0f, theme::Line);
            Text(L"Animation quality", fBodyStrong_, Rc(L + 14.0f, y + 6.0f, L + 280.0f, y + 26.0f), theme::Text);
            Text(L"UI springs, squeezes, and transitions.", fSmall_, Rc(L + 14.0f, y + 26.0f, R - 260.0f, y + 46.0f), theme::Faint);
            static constexpr const wchar_t* kQual[3] = { L"Full", L"Reduced", L"Off" };
            float qx = R - 238.0f;
            for (int q = 0; q < 3; ++q) {
                Button(RcW(qx, y + 13.0f, 72.0f, 28.0f), kQual[q], act::SetAnimQuality + q,
                       s.animationQuality == static_cast<uint8_t>(q) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
                qx += 78.0f;
            }
            y += 62.0f;
        }
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 74.0f);
            Fill(box, 7.0f, theme::Inset);
            Stroke(box, 7.0f, theme::Line);
            Text(L"Accent theme", fBodyStrong_, Rc(L + 14.0f, y + 8.0f, R - 14.0f, y + 26.0f), theme::Text);
            static constexpr const wchar_t* kAccNames[5] = { L"Amber", L"Cyan", L"Emerald", L"Violet", L"Crimson" };
            const float swW = (R - L - 28.0f - 4.0f * 8.0f) / 5.0f;
            for (int a = 0; a < 5; ++a) {
                const auto pal = mask::ui::GetAccentPalette(static_cast<uint8_t>(a));
                const D2D1_RECT_F sr = RcW(L + 14.0f + static_cast<float>(a) * (swW + 8.0f), y + 32.0f, swW, 32.0f);
                const bool sel = (s.accentPreset == static_cast<uint8_t>(a));
                const int actId = act::SetAccent + a;
                Fill(sr, 7.0f, sel ? pal.signal : theme::Raised, sel ? 0.18f : 1.0f);
                Stroke(sr, 7.0f, sel ? pal.signal : (Hovered(actId) ? theme::LineHi : theme::Line), sel ? 1.5f : 1.0f);
                Dot(sr.left + 16.0f, (sr.top + sr.bottom) * 0.5f, 6.0f, pal.signal);
                Text(kAccNames[a], sel ? fBodyStrong_ : fBody_, Rc(sr.left + 28.0f, sr.top, sr.right - 6.0f, sr.bottom),
                     sel ? pal.signalHi : theme::Text);
                Hit(sr, actId);
            }
            y += 82.0f;
        }
        toggleRow(y, L"Compact sidebar", L"Use single-line workspace rows.", s.compactSidebar, act::SetToggleCompact);
        toggleRow(y, L"Show signal view", L"Display the pulse oscilloscope in Macro view.", s.showSignalView, act::SetToggleSignal);
        toggleRow(y, L"Live key indicators", L"Highlight keycaps while keys are held down.", s.showLiveKeyIndicators, act::SetToggleLiveKeys);
    } else if (g_Ui.settingsTab == 2) {
        // ---- Tab 2: Performance & Background --------------------------------------
        toggleRow(y, L"Low-resource mode when unfocused", L"Pause UI animations and live preview in the background.", s.lowResourceWhenUnfocused, act::SetToggleLowRes);
        toggleRow(y, L"Trim memory when minimized", L"Release unused RAM pages 5 seconds after minimizing.", s.trimMemoryWhenMinimized, act::SetToggleTrimMem);

        auto choiceRow = [&](float& ry, const wchar_t* title, const wchar_t* sub, const wchar_t* const* labels, int cur, int baseAct) noexcept {
            const D2D1_RECT_F box = Rc(L, ry, R, ry + 54.0f);
            Fill(box, 7.0f, theme::Inset);
            Stroke(box, 7.0f, theme::Line);
            Text(title, fBodyStrong_, Rc(L + 14.0f, ry + 6.0f, R - 290.0f, ry + 26.0f), theme::Text);
            Text(sub, fSmall_, Rc(L + 14.0f, ry + 26.0f, R - 290.0f, ry + 46.0f), theme::Faint);
            float cx = R - 282.0f;
            for (int i = 0; i < 3; ++i) {
                Button(RcW(cx, ry + 13.0f, 88.0f, 28.0f), labels[i], baseAct + i,
                       cur == i ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
                cx += 92.0f;
            }
            ry += 62.0f;
        };

        static constexpr const wchar_t* kPowerLabels[3] = { L"Balanced", L"Max Perf", L"Eco" };
        choiceRow(y, L"Engine power mode", L"Polling cadence and timer precision.", kPowerLabels, s.powerMode, act::SetPowerMode);

        static constexpr const wchar_t* kPollLabels[3] = { L"250 Hz", L"500 Hz", L"1000 Hz" };
        const int pollIdx = (s.armedPollHz <= 375U) ? 0 : (s.armedPollHz <= 750U ? 1 : 2);
        choiceRow(y, L"Armed input poll rate", L"Physical input check rate while armed.", kPollLabels, pollIdx, act::SetPollHz);

        static constexpr const wchar_t* kSpinLabels[3] = { L"Auto", L"Max Prec", L"Eco" };
        choiceRow(y, L"Timing spin policy", L"Final RDTSCP wait window before firing.", kSpinLabels, s.spinPolicy, act::SetSpinPolicy);
    } else if (g_Ui.settingsTab == 3) {
        // ---- Tab 3: Hotkeys & Safety ----------------------------------------------
        const uint8_t bt = frame_.bindTarget;
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 54.0f);
            Fill(box, 7.0f, theme::Inset);
            Stroke(box, 7.0f, theme::Line);
            Text(L"Global pause / resume", fBodyStrong_, Rc(L + 14.0f, y + 6.0f, R - 300.0f, y + 26.0f), theme::Text);
            Text(L"Pause or resume all automation anytime.", fSmall_, Rc(L + 14.0f, y + 26.0f, R - 300.0f, y + 46.0f), theme::Faint);
            KeyChip(Rc(R - 294.0f, y + 11.0f, R - 154.0f, y + 43.0f), s.pauseHotkeyVk, act::SetBindPauseVk, bt == 24U);
            Button(Rc(R - 146.0f, y + 13.0f, R - 82.0f, y + 41.0f), L"Reset F8", act::SetResetPauseVk, BtnStyle::Ghost, fSmall_);
            Button(Rc(R - 76.0f, y + 13.0f, R - 12.0f, y + 41.0f), L"Clear", act::SetClearPauseVk, BtnStyle::Ghost, fSmall_);
            y += 62.0f;
        }
        {
            const D2D1_RECT_F box = Rc(L, y, R, y + 54.0f);
            Fill(box, 7.0f, theme::Inset);
            Stroke(box, 7.0f, theme::Line);
            Text(L"Global Stop All", fBodyStrong_, Rc(L + 14.0f, y + 6.0f, R - 230.0f, y + 26.0f), theme::Text);
            Text(L"Stop all active outputs and release held keys.", fSmall_, Rc(L + 14.0f, y + 26.0f, R - 230.0f, y + 46.0f), theme::Faint);
            KeyChip(Rc(R - 224.0f, y + 11.0f, R - 84.0f, y + 43.0f), s.stopAllHotkeyVk, act::SetBindStopAllVk, bt == 25U);
            Button(Rc(R - 76.0f, y + 13.0f, R - 12.0f, y + 41.0f), L"Clear", act::SetClearStopAllVk, BtnStyle::Ghost, fSmall_);
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
            Fill(bankCard, 8.0f, theme::Inset);
            Stroke(bankCard, 8.0f, theme::Line);
            Text(L"Profile bank", fBodyStrong_, Rc(L + 12.0f, y, L + 104.0f, y + 48.0f), theme::Text);
            float bx = L + 108.0f;
            for (int b = 0; b < 3; ++b) {
                wchar_t bl[16]{};
                std::swprintf(bl, std::size(bl), L"Bank %d", b + 1);
                Button(RcW(bx, y + 10.0f, 68.0f, 28.0f), bl, act::SetProfileBank + b,
                       s.activeProfileBank == static_cast<uint8_t>(b) ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
                bx += 74.0f;
            }
            Button(Rc(R - 230.0f, y + 10.0f, R - 110.0f, y + 38.0f), L"Data Folder", act::SetOpenDataFolder, BtnStyle::Secondary, fSmall_);
            Button(Rc(R - 102.0f, y + 10.0f, R - 10.0f, y + 38.0f),
                   g_Ui.pendingResetConfirm ? L"Confirm?" : L"Reset All",
                   act::SetResetAll, BtnStyle::Danger, fSmall_);
            y = bankCard.bottom + 10.0f;
        }

        // Card 2: Virtual HIDMaestro Identity Pickers
        {
            const D2D1_RECT_F idCard = Rc(L, y, R, y + 150.0f);
            Fill(idCard, 8.0f, theme::Inset);
            Stroke(idCard, 8.0f, theme::Line);
            const float cL = L + 12.0f;
            const float cR = R - 12.0f;
            Text(L"Virtual Device Identity", fBodyStrong_, Rc(cL, idCard.top + 8.0f, cL + 220.0f, idCard.top + 28.0f), theme::Text);
            wchar_t busBadge[96]{};
            UINT32 badgeColor = theme::Muted;
            if (g_Ui.outputState == OutputState::Ready) {
                std::swprintf(busBadge, std::size(busBadge), L"Live \u2022 Mouse #%d / Keyboard #%d", g_Ui.mouseTransportIdx, g_Ui.kbTransportIdx);
                badgeColor = theme::Ok;
            } else if (g_Ui.outputState == OutputState::NeedsAdmin) {
                wcscpy_s(busBadge, L"Admin required");
                badgeColor = theme::Warn;
            } else if (g_Ui.outputState == OutputState::Starting) {
                wcscpy_s(busBadge, L"Connecting\u2026");
                badgeColor = theme::Faint;
            } else {
                wcscpy_s(busBadge, L"Inactive");
                badgeColor = theme::Danger;
            }
            Text(busBadge, fSmall_, Rc(cL + 210.0f, idCard.top + 8.0f, cR, idCard.top + 28.0f), badgeColor, DWRITE_TEXT_ALIGNMENT_TRAILING);

            const float colW = (cR - cL - 12.0f) * 0.5f;
            auto pill = [&](const D2D1_RECT_F& pr, const wchar_t* lbl, bool active, int action) noexcept {
                const bool hov = Hovered(action);
                if (active) {
                    Fill(pr, 5.0f, ColSignal(), 0.14f);
                    Stroke(pr, 5.0f, ColSignal(), 1.0f, 0.85f);
                    Dot(pr.left + 11.0f, (pr.top + pr.bottom) * 0.5f, 3.2f, ColSignalHi());
                } else {
                    if (hov) Fill(pr, 5.0f, theme::Raised);
                    Stroke(pr, 5.0f, hov ? theme::LineHi : theme::Line);
                    Dot(pr.left + 11.0f, (pr.top + pr.bottom) * 0.5f, 2.8f, hov ? theme::Muted : theme::LineHi);
                }
                Text(lbl, active ? fBodyStrong_ : fSmall_, Rc(pr.left + 22.0f, pr.top, pr.right - 6.0f, pr.bottom),
                     active ? ColSignalHi() : (hov ? theme::Text : theme::Muted));
                Hit(pr, action);
            };

            const float topP = idCard.top + 34.0f;
            for (int i = 0; i < kMouseProfileCount; ++i) {
                pill(RcW(cL, topP + static_cast<float>(i) * 28.0f, colW, 24.0f), kMouseProfiles[i].label,
                     std::wcscmp(g_Ui.mouseProfileId, kMouseProfiles[i].id) == 0, act::SelectMouseProfile + i);
            }
            for (int i = 0; i < kKbProfileCount; ++i) {
                pill(RcW(cL + colW + 12.0f, topP + static_cast<float>(i) * 28.0f, colW, 24.0f), kKbProfiles[i].label,
                     std::wcscmp(g_Ui.keyboardProfileId, kKbProfiles[i].id) == 0, act::SelectKbProfile + i);
            }
            y = idCard.bottom + 10.0f;
        }

        // Card 3: Physical GameInput Devices & Connected Monitors + Timing Diagnostics
        const float remH = std::max(210.0f, bottomLimit - y);
        const D2D1_RECT_F diagCard = Rc(L, y, R, y + remH);
        Fill(diagCard, 8.0f, theme::Inset);
        Stroke(diagCard, 8.0f, theme::Line);

        const float cL = L + 12.0f;
        const float cR = R - 12.0f;
        const float innerW = cR - cL;

        const auto& snap = frame_.snap;
        const float tGap = 8.0f;
        const float tW = (innerW - 3.0f * tGap) * 0.25f;
        const float tY = diagCard.top + 10.0f;
        auto tile = [&](int col, const wchar_t* k, const wchar_t* v, UINT32 vc, const wchar_t* sub) noexcept {
            const D2D1_RECT_F tr = RcW(cL + static_cast<float>(col) * (tW + tGap), tY, tW, 54.0f);
            Fill(tr, 6.0f, theme::Panel);
            Stroke(tr, 6.0f, theme::Line);
            Text(k, fMini_, Rc(tr.left + 8.0f, tr.top + 4.0f, tr.right - 6.0f, tr.top + 17.0f), theme::Faint);
            Text(v, fBodyStrong_, Rc(tr.left + 8.0f, tr.top + 18.0f, tr.right - 6.0f, tr.top + 36.0f), vc);
            Text(sub, fMini_, Rc(tr.left + 8.0f, tr.top + 36.0f, tr.right - 6.0f, tr.bottom - 3.0f), theme::Muted);
        };

        const bool giActive = g_SharedState.gameInputConnected.load(std::memory_order_relaxed);
        wchar_t s0[40]{}, p50[32]{}, p99[44]{}, tsc[36]{}, evP[32]{}, evS[36]{};
        std::swprintf(s0, std::size(s0), L"%u devices \u2022 %d screens", static_cast<unsigned>(snap.gameInputDeviceCount), g_Ui.monitorCount);
        tile(0, L"Input & Displays", giActive ? L"GameInput" : L"Fallback", giActive ? ColSecondary() : theme::Warn, s0);

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
        Fill(histR, 6.0f, theme::Panel);
        Stroke(histR, 6.0f, theme::Line);
        uint32_t maxCnt = 1U;
        for (int b = 0; b < 4; ++b) maxCnt = std::max(maxCnt, snap.jitterHistogram[b]);
        Text(L"Jitter Distribution", fBodyStrong_, Rc(histR.left + 10.0f, histR.top + 6.0f, histR.right - 10.0f, histR.top + 24.0f), theme::Text);
        static constexpr const wchar_t* kBkt[4] = { L"<100ns", L"100-500ns", L"0.5-2us", L">=2us" };
        const UINT32 bktCol[4] = { theme::Ok, ColSecondary(), ColSignal(), theme::Warn };
        const float bL = histR.left + 74.0f;
        const float bR = histR.right - 46.0f;
        const float bSpan = std::max(20.0f, bR - bL);
        for (int b = 0; b < 4; ++b) {
            const float ry = histR.top + 28.0f + static_cast<float>(b) * 19.0f;
            if (ry + 16.0f > histR.bottom) break;
            Text(kBkt[b], fMini_, Rc(histR.left + 10.0f, ry, bL - 4.0f, ry + 16.0f), theme::Muted);
            Fill(Rc(bL, ry + 4.0f, bR, ry + 11.0f), 3.5f, theme::Inset);
            const uint32_t cnt = snap.jitterHistogram[b];
            if (cnt > 0U) {
                const float fw = std::max(4.0f, bSpan * (static_cast<float>(cnt) / static_cast<float>(maxCnt)));
                Fill(RcW(bL, ry + 4.0f, fw, 7.0f), 3.5f, bktCol[b]);
            }
            wchar_t cb[16]{};
            std::swprintf(cb, std::size(cb), L"%u", static_cast<unsigned>(cnt));
            Text(cb, fAxis_, Rc(bR + 4.0f, ry, histR.right - 8.0f, ry + 16.0f), cnt > 0 ? theme::Text : theme::Faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        const D2D1_RECT_F devR = RcW(cL + splitW + 10.0f, botY, splitW, botH);
        Fill(devR, 6.0f, theme::Panel);
        Stroke(devR, 6.0f, theme::Line);
        Text(L"Devices & Monitors", fBodyStrong_, Rc(devR.left + 10.0f, devR.top + 6.0f, devR.right - 66.0f, devR.top + 24.0f), theme::Text);
        Button(Rc(devR.right - 34.0f, devR.top + 5.0f, devR.right - 6.0f, devR.top + 25.0f), L"\u21BB", act::SetRefreshHw, BtnStyle::Ghost, fBodyStrong_);

        float dy = devR.top + 27.0f;
        if (g_Ui.physicalDeviceCount == 0) {
            Text(L"Press any key/mouse to list devices.", fMini_, Rc(devR.left + 10.0f, dy, devR.right - 10.0f, dy + 16.0f), theme::Faint);
            dy += 17.0f;
        } else {
            for (int i = 0; i < g_Ui.physicalDeviceCount && dy + 16.0f <= devR.bottom - 22.0f; ++i) {
                const auto& d = g_Ui.physicalDevices[i];
                wchar_t line[80]{};
                std::swprintf(line, std::size(line), L"\u2022 %ls (%04X:%04X)", d.displayName,
                              static_cast<unsigned>(d.vendorId), static_cast<unsigned>(d.productId));
                Text(line, fMini_, Rc(devR.left + 10.0f, dy, devR.right - 10.0f, dy + 16.0f), theme::Muted);
                dy += 16.0f;
            }
        }
        for (int m = 0; m < g_Ui.monitorCount && dy + 16.0f <= devR.bottom - 4.0f; ++m) {
            const auto& mn = g_Ui.monitors[m];
            wchar_t ml[80]{};
            std::swprintf(ml, std::size(ml), L"Mon %d: %dx%d @ %u DPI%ls", m + 1,
                          static_cast<int>(mn.right - mn.left), static_cast<int>(mn.bottom - mn.top),
                          static_cast<unsigned>(mn.dpi), mn.isPrimary ? L"*" : L"");
            Text(ml, fMini_, Rc(devR.left + 10.0f, dy, devR.right - 10.0f, dy + 16.0f), theme::Faint);
            dy += 16.0f;
        }
    }
}

// =====================================================================================
//  5. Clean Animated `+ New` Creation Picker Modal
// =====================================================================================

inline void UiRenderer::DrawNewPickerModal(float W, float H, float openT) noexcept {
    const D2D1_RECT_F full = Rc(0.0f, 0.0f, W, H);
    Fill(full, 0.0f, 0x07090E, 0.62f * openT);
    if (g_Ui.showNewPicker) {
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

    Fill(Rc(modal.left - 4.0f, modal.top - 4.0f, modal.right + 4.0f, modal.bottom + 8.0f), 16.0f, 0x05070B, 0.45f * openT);
    Fill(modal, 12.0f, theme::Panel, openT);
    Stroke(modal, 12.0f, theme::LineHi, 1.2f, openT);

    if (g_Ui.showNewPicker) {
        Hit(modal, act::ModalSurface);
    }

    const float pad = 20.0f * scale;
    Text(L"New Item", fTitle_, Rc(modal.left + pad, modal.top + 14.0f * scale, modal.right - 70.0f, modal.top + 46.0f * scale),
         theme::Text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);

    Button(Rc(modal.right - pad - 28.0f, modal.top + 16.0f * scale, modal.right - pad, modal.top + 44.0f * scale),
           L"\u00D7", act::CloseNewPicker, BtnStyle::Ghost, fHeading_, openT);

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
        const D2D1_RECT_F sr = g_Animator.ApplySqueeze(cr, cards[i].action);
        const bool hov = Hovered(cards[i].action);

        Fill(sr, 9.0f, hov ? theme::RaisedHi : theme::Raised, openT);
        Stroke(sr, 9.0f, hov ? cards[i].color : theme::Line, hov ? 1.5f : 1.0f, openT);

        const D2D1_RECT_F br = RcW(sr.left + 14.0f, sr.top + 14.0f, 26.0f, 26.0f);
        Fill(br, 7.0f, cards[i].color, 0.18f * openT);
        Stroke(br, 7.0f, cards[i].color, 1.0f, 0.78f * openT);
        Text(cards[i].badge, fBodyStrong_, br, cards[i].color, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);

        Text(cards[i].title, fHeading_, Rc(br.right + 10.0f, sr.top + 13.0f, sr.right - 12.0f, sr.top + 41.0f),
             theme::Text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);
        Text(cards[i].desc, fSmall_, Rc(sr.left + 14.0f, sr.top + 48.0f, sr.right - 12.0f, sr.bottom - 10.0f),
             theme::Muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, openT);

        if (g_Ui.showNewPicker) {
            Hit(cr, cards[i].action);
        }
    }
}

} // namespace mask::app
