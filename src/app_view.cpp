#include "app_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace mask::app {

D2D1_RECT_F UiRenderer::Rc(float l, float t, float r, float b) noexcept {
    return D2D1::RectF(l, t, r, b);
}

D2D1_RECT_F UiRenderer::RcW(float x, float y, float w, float h) noexcept {
    return D2D1::RectF(x, y, x + w, y + h);
}

float UiRenderer::Snap(float v) noexcept {
    return std::floor(v) + 0.5f;
}

bool UiRenderer::Initialize(HWND hwnd) noexcept {
    hwnd_ = hwnd;
    if (!gpu_.Initialize(hwnd)) {
        return false;
    }
    dpi_ = gpu_.Dpi();
    if (dpi_ <= 0.0f) dpi_ = 96.0f;
    if (hwnd != nullptr && IsWindow(hwnd)) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            pixelW_ = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
            pixelH_ = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
        }
    }
    hitBoxes_.reserve(384);
    return true;
}

void UiRenderer::Shutdown() noexcept {
    shieldGeo_.Reset();
    gpu_.Shutdown();
    hwnd_ = nullptr;
}

void UiRenderer::Resize(UINT pixelW, UINT pixelH) noexcept {
    if (pixelW > 0U && pixelH > 0U) {
        pixelW_ = pixelW;
        pixelH_ = pixelH;
        gpu_.Resize(pixelW, pixelH);
    }
}

void UiRenderer::OnResize(UINT widthPx, UINT heightPx) noexcept {
    Resize(widthPx, heightPx);
}

void UiRenderer::SetDpi(float dpi) noexcept {
    dpi_ = (dpi >= 48.0f && dpi <= 480.0f) ? dpi : 96.0f;
    gpu_.SetDpi(dpi_);
}

void UiRenderer::OnDpiChanged(UINT dpi) noexcept {
    SetDpi(static_cast<float>(dpi));
}

void UiRenderer::TrimGpu() noexcept {
    gpu_.TrimGpu();
}

void UiRenderer::Discard() noexcept {
    shieldGeo_.Reset();
    gpu_.Shutdown();
}

UiAction UiRenderer::HitTest(float dipX, float dipY) const noexcept {
    for (auto it = hitBoxes_.rbegin(); it != hitBoxes_.rend(); ++it) {
        if (dipX >= it->rect.left && dipX <= it->rect.right &&
            dipY >= it->rect.top  && dipY <= it->rect.bottom) {
            return it->action;
        }
    }
    return UiAction{};
}

UiAction UiRenderer::HitTestPx(int xPx, int yPx) const noexcept {
    const float s = Scale();
    return HitTest(static_cast<float>(xPx) / s, static_cast<float>(yPx) / s);
}

bool UiRenderer::PointInList(float dipX, float dipY) const noexcept {
    return dipX >= listRect_.left && dipX <= listRect_.right &&
           dipY >= listRect_.top  && dipY <= listRect_.bottom;
}

bool UiRenderer::PointInListPx(int xPx, int yPx) const noexcept {
    const float s = Scale();
    return PointInList(static_cast<float>(xPx) / s, static_cast<float>(yPx) / s);
}

bool UiRenderer::PointInSeqList(float dipX, float dipY) const noexcept {
    return dipX >= seqListRect_.left && dipX <= seqListRect_.right &&
           dipY >= seqListRect_.top  && dipY <= seqListRect_.bottom;
}

bool UiRenderer::PointInSeqListPx(int xPx, int yPx) const noexcept {
    const float s = Scale();
    return PointInSeqList(static_cast<float>(xPx) / s, static_cast<float>(yPx) / s);
}

float UiRenderer::ListViewHeight() const noexcept {
    return std::max(CurrentListRowHeight(), listRect_.bottom - listRect_.top);
}

float UiRenderer::SeqStepViewHeight() const noexcept {
    return std::max(kSeqStepRowH, seqListRect_.bottom - seqListRect_.top);
}

bool UiRenderer::GetSettingsTabRect(int tabIndex, float& outX, float& outW) const noexcept {
    const int idx = std::clamp(tabIndex, 0, 4);
    outX = settingsTabRects_[static_cast<size_t>(idx)].left;
    outW = std::max(0.0f, settingsTabRects_[static_cast<size_t>(idx)].right - settingsTabRects_[static_cast<size_t>(idx)].left);
    return outW > 1.0f;
}

bool UiRenderer::GetSettingsTabRect(int tabIndex, D2D1_RECT_F& outRect) const noexcept {
    const int idx = std::clamp(tabIndex, 0, 4);
    outRect = settingsTabRects_[static_cast<size_t>(idx)];
    return (outRect.right - outRect.left) > 1.0f;
}

void UiRenderer::QuerySettingsTabRect(int tabIndex, float& outX, float& outW) const noexcept {
    (void)GetSettingsTabRect(tabIndex, outX, outW);
    if (outW < 40.0f) outW = 40.0f;
}

bool UiRenderer::GetSelectedSidebarRowRect(float& outY, float& outH) const noexcept {
    outY = selectedSidebarRowY_;
    outH = selectedSidebarRowH_;
    return hasSelectedSidebarRow_ && selectedSidebarRowH_ > 1.0f;
}

bool UiRenderer::GetSelectedSidebarRowRect(D2D1_RECT_F& outRect) const noexcept {
    outRect = selectedSidebarRowRect_;
    return hasSelectedSidebarRow_ && (outRect.bottom - outRect.top) > 1.0f;
}

double UiRenderer::DutyFromSliderX(float dipX) const noexcept {
    const float span = std::max(1.0f, dutyTrack_.right - dutyTrack_.left);
    const float t = std::clamp((dipX - dutyTrack_.left) / span, 0.0f, 1.0f);
    return std::clamp(std::round(static_cast<double>(t) * 100.0), 0.0, 100.0);
}

double UiRenderer::DutyFromSliderXPx(int xPx) const noexcept {
    return DutyFromSliderX(static_cast<float>(xPx) / Scale());
}

int UiRenderer::TriggerToleranceFromX(float dipX) const noexcept {
    const float span = std::max(1.0f, trigTolTrack_.right - trigTolTrack_.left);
    const float t = std::clamp((dipX - trigTolTrack_.left) / span, 0.0f, 1.0f);
    return std::clamp(static_cast<int>(std::lround(t * 150.0f)), 0, 150);
}

int UiRenderer::TriggerToleranceFromXPx(int xPx) const noexcept {
    return TriggerToleranceFromX(static_cast<float>(xPx) / Scale());
}

void UiRenderer::Render(const AppModel& ui, const FrameState& frame, const mask::ui::UiAnimator& anim) noexcept {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rc{};
    UINT pw = pixelW_;
    UINT ph = pixelH_;
    if (IsWindow(hwnd_) && GetClientRect(hwnd_, &rc)) {
        pw = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
        ph = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
        pixelW_ = pw;
        pixelH_ = ph;
    }
    if (pw == 0U || ph == 0U) {
        return;
    }

    const auto pal = mask::ui::GetAccentPalette(ui.settings.accentPreset);
    if (!gpu_.BeginFrame(hwnd_, pw, ph, dpi_, frame.windowFocused,
                         anim.ambientPhase, anim.cursorX.value, anim.cursorY.value,
                         anim.cursorInside.value > 0.1f, pal.signal, pal.signalHi)) {
        return;
    }

    hitBoxes_.clear();
    ui_   = &ui;
    anim_ = &anim;
    frame_ = frame;
    accent_ = pal;
    lastCompactSidebar_ = ui.settings.compactSidebar;
    seqListRect_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);
    seqStepKindBtnRect_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);
    trigTolTrack_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);
    dutyTrack_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);
    seqStepMaxScroll_ = 0.0f;
    hasSelectedSidebarRow_ = false;
    selectedSidebarRowY_ = 0.0f;
    selectedSidebarRowH_ = 0.0f;
    selectedSidebarRowRect_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);

    const D2D1_SIZE_F size = gpu_.LogicalSize();
    const float W = size.width;
    const float H = size.height;

    const float openAlpha = anim.AppOpenAlpha();
    const float openY     = anim.AppOpenOffsetY(14.0f);
    if (openY > 0.05f) {
        gpu_.SetTransform(D2D1::Matrix3x2F::Translation(0.0f, openY));
    } else {
        gpu_.ResetTransform();
    }

    constexpr float kMargin  = 14.0f;
    constexpr float kTopBarH = 58.0f;
    constexpr float kStatusH = 38.0f;
    const float sidebarW = ui.settings.compactSidebar ? 248.0f : 282.0f;

    DrawTopBar(W, kMargin, kTopBarH);
    DrawSidebar(Rc(kMargin, kTopBarH, kMargin + sidebarW, H - kStatusH));
    DrawEditor(Rc(kMargin + sidebarW + 10.0f, kTopBarH, W - kMargin, H - kStatusH));
    DrawStatusBar(Rc(kMargin, H - kStatusH, W - kMargin, H));

    gpu_.ResetTransform();

    if (ui.showStepKindMenu && !ui.showSettings && ui.selectedKind == ItemKind::Sequence && !ui.showNewPicker) {
        DrawStepKindPopover(W, H);
    }

    if (openAlpha < 0.995f) {
        Fill(Rc(0.0f, 0.0f, W, H), 0.0f, theme::Window, (1.0f - openAlpha) * 0.88f);
    }

    const float modalT = anim.newPickerOpen.value;
    if (ui.showNewPicker || modalT > 0.005f) {
        DrawNewPickerModal(W, H, std::clamp(modalT, 0.0f, 1.0f));
    }

    gpu_.DrawClickRipples(anim.clickRipples);
    (void)gpu_.EndFrame(true);

    ui_ = nullptr;
    anim_ = nullptr;
}

// =====================================================================================
//  Primitives
// =====================================================================================

void UiRenderer::Fill(const D2D1_RECT_F& r, float radius, UINT32 rgb, float alpha) noexcept {
    gpu_.FillRoundRect(r, radius, rgb, alpha);
}

void UiRenderer::Stroke(const D2D1_RECT_F& r, float radius, UINT32 rgb, float width, float alpha) noexcept {
    gpu_.StrokeRoundRect(r, radius, rgb, width, alpha);
}

void UiRenderer::Line(float x0, float y0, float x1, float y1, UINT32 rgb, float width, float alpha) noexcept {
    gpu_.Line(x0, y0, x1, y1, rgb, width, alpha);
}

void UiRenderer::GlowLine(float x0, float y0, float x1, float y1, UINT32 rgb, float coreW, float glowW, float alpha) noexcept {
    gpu_.DrawGlowSegment(x0, y0, x1, y1, rgb, coreW, glowW, alpha);
}

void UiRenderer::Dot(float cx, float cy, float radius, UINT32 rgb, float alpha) noexcept {
    gpu_.FillCircle(cx, cy, radius, rgb, alpha);
}

void UiRenderer::GlassCard(const D2D1_RECT_F& r, float radius, bool inner, bool hoverHighlight,
                           UINT32 borderRgba, UINT32 glowRgb, float specAlpha) noexcept {
    const auto& a = Anim();
    const UINT32 topRgba = inner ? theme::GlassInnerTop : theme::GlassTop;
    const UINT32 botRgba = inner ? theme::GlassInnerBot : theme::GlassBot;
    const UINT32 bRgba   = (borderRgba != 0U) ? borderRgba : theme::GlassBorder;
    const UINT32 gRgb    = (glowRgb != 0U) ? glowRgb : ColSignal();
    gpu_.FillGlassCard(r, radius, topRgba, botRgba, bRgba, specAlpha,
                       a.cursorX.value, a.cursorY.value, hoverHighlight, gRgb);
}

void UiRenderer::Text(const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, UINT32 rgb,
                      DWRITE_TEXT_ALIGNMENT h, DWRITE_PARAGRAPH_ALIGNMENT v, float alpha) noexcept {
    gpu_.Text(text, fmt, r, rgb, h, v, alpha);
}

float UiRenderer::MeasureWidth(const wchar_t* text, IDWriteTextFormat* fmt) const noexcept {
    return gpu_.MeasureWidth(text, fmt);
}

void UiRenderer::DrawMaskMark(float x, float y, float sz) noexcept {
    const float cx = x + sz * 0.5f;
    const float cy = y + sz * 0.5f;

    gpu_.FillRadialGlow(cx, cy, sz * 0.80f, sz * 0.80f, ColSignal(), 0.24f, ColSignal(), 0.0f);

    const D2D1_RECT_F tile = RcW(x, y, sz, sz);
    gpu_.FillLinearRect(tile, sz * 0.26f, 0x030B1AU, 0.96f, 0x0C1E3DU, 0.96f, false);
    Stroke(tile, sz * 0.26f, ColSignal(), 1.25f, 0.72f);

    ID2D1DeviceContext* ctx = gpu_.Context();
    if (ctx != nullptr) {
        mask::ui::detail::ComPtr<ID2D1Factory> factory;
        ctx->GetFactory(factory.Put());
        if (factory) {
            mask::ui::detail::ComPtr<ID2D1PathGeometry> shield;
            if (SUCCEEDED(factory->CreatePathGeometry(shield.Put())) && shield) {
                mask::ui::detail::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(shield->Open(sink.Put())) && sink) {
                    sink->BeginFigure(D2D1::Point2F(x + sz * 0.20f, y + sz * 0.24f), D2D1_FIGURE_BEGIN_FILLED);
                    const D2D1_POINT_2F pts[] = {
                        D2D1::Point2F(x + sz * 0.50f, y + sz * 0.16f),
                        D2D1::Point2F(x + sz * 0.80f, y + sz * 0.24f),
                        D2D1::Point2F(x + sz * 0.78f, y + sz * 0.62f),
                        D2D1::Point2F(x + sz * 0.50f, y + sz * 0.84f),
                        D2D1::Point2F(x + sz * 0.22f, y + sz * 0.62f)
                    };
                    sink->AddLines(pts, static_cast<UINT32>(std::size(pts)));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();

                    mask::ui::detail::ComPtr<ID2D1SolidColorBrush> sBrush;
                    if (SUCCEEDED(ctx->CreateSolidColorBrush(D2D1::ColorF(0x040B16U, 0.92f), sBrush.Put())) && sBrush) {
                        ctx->FillGeometry(shield.Get(), sBrush.Get());
                        sBrush->SetColor(D2D1::ColorF(0x1E4B85U, 0.78f));
                        ctx->DrawGeometry(shield.Get(), sBrush.Get(), 1.0f);
                    }
                }
            }
        }
    }

    const float px[5] = {
        x + sz * 0.25f, x + sz * 0.36f, x + sz * 0.50f, x + sz * 0.64f, x + sz * 0.75f
    };
    const float py[5] = {
        y + sz * 0.56f, y + sz * 0.37f, y + sz * 0.52f, y + sz * 0.37f, y + sz * 0.56f
    };
    for (int i = 0; i < 4; ++i) {
        GlowLine(px[i], py[i], px[i + 1], py[i + 1], ColSignalHi(), 1.85f, 5.2f, 0.98f);
    }
    Dot(x + sz * 0.50f, y + sz * 0.52f, 1.4f, theme::IceCore, 0.95f);
}

void UiRenderer::ArrowRight(float x, float cy, float length, UINT32 rgb, float width, bool glow) noexcept {
    if (glow) {
        GlowLine(x, cy, x + length, cy, rgb, width, width * 3.5f, 0.95f);
        GlowLine(x + length - 5.0f, cy - 4.0f, x + length, cy, rgb, width, width * 2.5f, 0.95f);
        GlowLine(x + length - 5.0f, cy + 4.0f, x + length, cy, rgb, width, width * 2.5f, 0.95f);
    } else {
        Line(x, cy, x + length, cy, rgb, width);
        Line(x + length - 4.0f, cy - 3.5f, x + length, cy, rgb, width);
        Line(x + length - 4.0f, cy + 3.5f, x + length, cy, rgb, width);
    }
}

void UiRenderer::GlyphMinus(const D2D1_RECT_F& r, UINT32 rgb) noexcept {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = Snap((r.top + r.bottom) * 0.5f);
    Line(cx - 4.5f, cy, cx + 4.5f, cy, rgb, 1.5f);
}

void UiRenderer::GlyphPlus(const D2D1_RECT_F& r, UINT32 rgb) noexcept {
    const float cx = Snap((r.left + r.right) * 0.5f);
    const float cy = Snap((r.top + r.bottom) * 0.5f);
    Line(cx - 4.5f, cy, cx + 4.5f, cy, rgb, 1.5f);
    Line(cx, cy - 4.5f, cx, cy + 4.5f, rgb, 1.5f);
}

void UiRenderer::Hit(const D2D1_RECT_F& r, UiAction action) noexcept {
    if (action.IsNone()) return;
    D2D1_RECT_F clipped = r;
    D2D1_RECT_F curClip{};
    if (gpu_.CurrentClip(curClip)) {
        clipped.left   = std::max(clipped.left, curClip.left);
        clipped.top    = std::max(clipped.top, curClip.top);
        clipped.right  = std::min(clipped.right, curClip.right);
        clipped.bottom = std::min(clipped.bottom, curClip.bottom);
        if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) return;
    }
    hitBoxes_.push_back(HitBox{ clipped, action });
}

bool UiRenderer::Hovered(UiAction action) const noexcept {
    const auto& u = Ui();
    return !action.IsNone() && u.hoveredAction == action && !u.draggingDuty && u.draggingTriggerSlider.IsNone();
}

void UiRenderer::PushClip(const D2D1_RECT_F& r) noexcept {
    gpu_.PushClip(r, false);
}

void UiRenderer::PopClip() noexcept {
    gpu_.PopClip();
}

// =====================================================================================
//  Animated Cosmic Glass Widgets
// =====================================================================================

void UiRenderer::Button(const D2D1_RECT_F& r, const wchar_t* label, UiAction action, BtnStyle style,
                        IDWriteTextFormat* fmt, float alpha) noexcept {
    const D2D1_RECT_F sr = Anim().ApplySqueeze(r, EncodeUiActionKey(action));
    const bool hov = Hovered(action);
    const bool chip = (style == BtnStyle::Chip || style == BtnStyle::ChipOn);
    const float radius = chip ? (sr.bottom - sr.top) * 0.5f : 7.0f;
    UINT32 text = theme::Text;
    switch (style) {
        case BtnStyle::Primary:
            if (hov) {
                const float cx = (sr.left + sr.right) * 0.5f;
                const float cy = (sr.top + sr.bottom) * 0.5f;
                gpu_.FillRadialGlow(cx, cy, (sr.right - sr.left) * 0.65f, (sr.bottom - sr.top) * 0.85f,
                                    ColSignal(), 0.25f * alpha, ColSignal(), 0.0f);
            }
            gpu_.FillLinearRect(sr, radius, hov ? ColSignalHi() : mask::ui::LerpRgb(ColSignal(), ColSignalHi(), 0.25f),
                                alpha, ColSignal(), alpha, false);
            Stroke(sr, radius, theme::IceCore, 1.0f, (hov ? 0.45f : 0.22f) * alpha);
            text = ColOnSignal();
            break;
        case BtnStyle::Ghost:
            if (hov) {
                Fill(sr, radius, theme::Raised, 0.85f * alpha);
                Stroke(sr, radius, theme::Line, 1.0f, 0.70f * alpha);
            }
            text = hov ? theme::Text : theme::Muted;
            break;
        case BtnStyle::Danger:
            Fill(sr, radius, theme::Danger, (hov ? 0.20f : 0.07f) * alpha);
            Stroke(sr, radius, theme::Danger, 1.0f, (hov ? 0.92f : 0.52f) * alpha);
            text = theme::Danger;
            break;
        case BtnStyle::ChipOn:
            gpu_.FillLinearRect(sr, radius, ColSignal(), 0.20f * alpha, ColSecondary(), 0.12f * alpha, true);
            Stroke(sr, radius, ColSignal(), 1.0f, 0.85f * alpha);
            text = ColSignalHi();
            break;
        case BtnStyle::Chip:
            Fill(sr, radius, hov ? theme::RaisedHi : theme::Inset, (hov ? 0.86f : 0.60f) * alpha);
            Stroke(sr, radius, hov ? theme::LineHi : theme::Line, 1.0f, 0.85f * alpha);
            text = hov ? theme::Text : theme::Muted;
            break;
        case BtnStyle::Secondary:
        default:
            gpu_.FillLinearRect(sr, radius, hov ? theme::RaisedHi : theme::Raised, 0.90f * alpha,
                                theme::Panel, 0.90f * alpha, false);
            Stroke(sr, radius, hov ? theme::LineHi : theme::Line, 1.0f, 0.92f * alpha);
            break;
    }
    Text(label, fmt ? fmt : gpu_.fBodyStrong, sr, text, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alpha);
    Hit(r, action);
}

void UiRenderer::Switch(float x, float y, bool on, UiAction action, bool live, int animIdx, float alpha) noexcept {
    const auto& a = Anim();
    const float slide = (animIdx >= 0 && animIdx < static_cast<int>(mask::ui::UiAnimator::kSlotCount))
        ? std::clamp(a.toggleSlide[static_cast<size_t>(animIdx)].value, 0.0f, 1.0f)
        : (on ? 1.0f : 0.0f);
    const float glow = (animIdx >= 0 && animIdx < static_cast<int>(mask::ui::UiAnimator::kSlotCount))
        ? std::clamp(a.toggleGlow[static_cast<size_t>(animIdx)].value, 0.0f, 1.0f)
        : (on ? 1.0f : 0.0f);

    const D2D1_RECT_F track = a.ApplySqueeze(RcW(x, y, 36.0f, 20.0f), EncodeUiActionKey(action));
    const bool hov = Hovered(action);
    const float effectiveGlow = live ? std::max(0.85f, glow) : glow;
    gpu_.DrawToggleSwitch(track, slide, effectiveGlow, ColSignal(), ColSignalHi(), hov, alpha);
    Hit(Rc(x - 6.0f, y - 8.0f, x + 42.0f, y + 28.0f), action);
}

void UiRenderer::KeyChip(const D2D1_RECT_F& r, uint16_t vk, UiAction action, bool binding) noexcept {
    const D2D1_RECT_F sr = Anim().ApplySqueeze(r, EncodeUiActionKey(action));
    const bool hov = Hovered(action);
    if (binding) {
        gpu_.FillRadialGlow((sr.left + sr.right) * 0.5f, (sr.top + sr.bottom) * 0.5f,
                            (sr.right - sr.left) * 0.58f, (sr.bottom - sr.top) * 0.70f,
                            ColSignal(), 0.22f, ColSignal(), 0.0f);
        Fill(sr, 7.0f, ColSignal(), 0.14f);
        Stroke(sr, 7.0f, ColSignalHi(), 1.5f);
        Text(L"Press a key\u2026", gpu_.fBodyStrong, sr, ColSignalHi(), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        const bool physHeld = Ui().settings.showLiveKeyIndicators &&
                              (vk != 0U) &&
                              (vk != mask::core::kVkWheelUp) &&
                              (vk != mask::core::kVkWheelDown) &&
                              (Ui().SharedState().physicalKeyState[vk & 0xFFU].load(std::memory_order_relaxed) != 0U);
        Fill(sr, 7.0f, 0x02060EU, 0.96f);
        const D2D1_RECT_F face = Rc(sr.left, sr.top, sr.right, sr.bottom - 3.0f);
        gpu_.FillLinearRect(face, 7.0f, hov ? theme::RaisedHi : theme::Raised, 0.95f, theme::Panel, 0.95f, false);
        if (physHeld) {
            Fill(face, 7.0f, theme::Ok, 0.14f);
        }
        Stroke(face, 7.0f, physHeld ? theme::Ok : (hov ? ColSignal() : theme::LineHi), physHeld ? 1.5f : 1.0f,
               physHeld ? 0.95f : (hov ? 0.85f : 0.88f));
        float textLeft = face.left + 8.0f;
        if (physHeld) {
            const float dotX = face.left + 11.0f;
            const float dotY = (face.top + face.bottom) * 0.5f;
            gpu_.FillRadialGlow(dotX, dotY, 7.5f, 7.5f, theme::Ok, 0.35f, theme::Ok, 0.0f);
            Dot(dotX, dotY, 3.2f, theme::Ok);
            textLeft = face.left + 19.0f;
        }
        Text(NameOf(vk).text, gpu_.fKey, Rc(textLeft, face.top, face.right - 6.0f, face.bottom),
             vk == 0 ? theme::Faint : (physHeld ? theme::White : theme::Text), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    Hit(r, action);
}

float UiRenderer::MiniKey(float x, float cy, const wchar_t* label) noexcept {
    const float w = std::ceil(MeasureWidth(label, gpu_.fMini)) + 12.0f;
    const D2D1_RECT_F r = Rc(x, cy - 9.0f, x + w, cy + 9.0f);
    Fill(r, 4.0f, theme::Raised, 0.88f);
    Stroke(r, 4.0f, theme::LineHi, 1.0f, 0.78f);
    Text(label, gpu_.fMini, r, theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    return x + w;
}

void UiRenderer::NumberField(const D2D1_RECT_F& r, const MacroConfig& slot, Field f, bool steppers,
                             const wchar_t* zeroText) noexcept {
    const auto& u = Ui();
    const int16_t fieldId = static_cast<int16_t>(f);
    const bool editing = (u.editField == f);
    const UiAction focusAction{ ActionKind::FieldFocus, fieldId };
    const bool hov = Hovered(focusAction);

    Fill(r, 6.0f, theme::Inset, 0.90f);
    Stroke(r, 6.0f, editing ? ColSignal() : (hov ? theme::LineHi : theme::Line), editing ? 1.5f : 1.0f);

    D2D1_RECT_F valueRect = r;
    if (steppers) {
        const float bw = 26.0f;
        const D2D1_RECT_F dec = Rc(r.left + 2.0f, r.top + 2.0f, r.left + bw, r.bottom - 2.0f);
        const D2D1_RECT_F inc = Rc(r.right - bw, r.top + 2.0f, r.right - 2.0f, r.bottom - 2.0f);
        const UiAction decA{ ActionKind::FieldDec, fieldId };
        const UiAction incA{ ActionKind::FieldInc, fieldId };
        if (Hovered(decA)) Fill(dec, 5.0f, theme::Raised);
        if (Hovered(incA)) Fill(inc, 5.0f, theme::Raised);
        GlyphMinus(dec, Hovered(decA) ? theme::Text : theme::Muted);
        GlyphPlus(inc, Hovered(incA) ? theme::Text : theme::Muted);
        Hit(dec, decA);
        Hit(inc, incA);
        valueRect = Rc(r.left + bw, r.top, r.right - bw, r.bottom);
    }

    wchar_t text[48]{};
    if (editing) {
        wcscpy_s(text, u.editBuffer);
        const float tw = MeasureWidth(text, gpu_.fNum);
        const float cx = (valueRect.left + valueRect.right) * 0.5f;
        Text(text, gpu_.fNum, valueRect, theme::White, DWRITE_TEXT_ALIGNMENT_CENTER);
        const float caretX = Snap(cx + tw * 0.5f + 1.5f);
        Line(caretX, r.top + 8.0f, caretX, r.bottom - 8.0f, ColSignalHi(), 1.5f);
    } else if (zeroText != nullptr && FieldGet(slot, f) == 0.0) {
        Text(zeroText, gpu_.fNum, valueRect, ColSignalHi(), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        wchar_t num[32]{};
        FormatFieldNumber(slot, f, num, std::size(num));
        std::swprintf(text, std::size(text), L"%ls%ls", num, FieldSuffix(f));
        Text(text, gpu_.fNum, valueRect, theme::Text, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    Hit(valueRect, focusAction);
}

void UiRenderer::GenericField(const D2D1_RECT_F& r, Field f, const wchar_t* displayText, bool steppers,
                              bool leftAligned, const wchar_t* placeholder) noexcept {
    const auto& u = Ui();
    const int16_t fieldId = static_cast<int16_t>(f);
    const bool editing = (u.editField == f);
    const UiAction focusAction{ ActionKind::FieldFocus, fieldId };
    const bool hov = Hovered(focusAction);

    Fill(r, 6.0f, theme::Inset, 0.90f);
    Stroke(r, 6.0f, editing ? ColSignal() : (hov ? theme::LineHi : theme::Line), editing ? 1.5f : 1.0f);

    D2D1_RECT_F valueRect = r;
    if (steppers) {
        const float bw = 24.0f;
        const D2D1_RECT_F dec = Rc(r.left + 2.0f, r.top + 2.0f, r.left + bw, r.bottom - 2.0f);
        const D2D1_RECT_F inc = Rc(r.right - bw, r.top + 2.0f, r.right - 2.0f, r.bottom - 2.0f);
        const UiAction decA{ ActionKind::FieldDec, fieldId };
        const UiAction incA{ ActionKind::FieldInc, fieldId };
        if (Hovered(decA)) Fill(dec, 5.0f, theme::Raised);
        if (Hovered(incA)) Fill(inc, 5.0f, theme::Raised);
        GlyphMinus(dec, Hovered(decA) ? theme::Text : theme::Muted);
        GlyphPlus(inc, Hovered(incA) ? theme::Text : theme::Muted);
        Hit(dec, decA);
        Hit(inc, incA);
        valueRect = Rc(r.left + bw, r.top, r.right - bw, r.bottom);
    }

    IDWriteTextFormat* fmt = leftAligned ? gpu_.fBody : gpu_.fNum;
    const D2D1_RECT_F textRect = leftAligned
        ? Rc(valueRect.left + 8.0f, valueRect.top, valueRect.right - 8.0f, valueRect.bottom)
        : valueRect;
    if (editing) {
        Text(u.editBuffer, fmt, textRect, theme::White,
             leftAligned ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
        const float tw = MeasureWidth(u.editBuffer, fmt);
        const float caretX = leftAligned
            ? Snap(std::min(textRect.right - 2.0f, textRect.left + tw + 1.5f))
            : Snap((valueRect.left + valueRect.right) * 0.5f + tw * 0.5f + 1.5f);
        Line(caretX, r.top + 6.0f, caretX, r.bottom - 6.0f, ColSignalHi(), 1.5f);
    } else if ((displayText == nullptr || displayText[0] == L'\0') && placeholder != nullptr) {
        Text(placeholder, fmt, textRect, theme::Faint,
             leftAligned ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        Text(displayText ? displayText : L"", fmt, textRect, theme::Text,
             leftAligned ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    Hit(valueRect, focusAction);
}

void UiRenderer::RowLabel(float x, float y, float rowHeight, const wchar_t* label) noexcept {
    Text(label, gpu_.fLabel, RcW(x, y, 92.0f, rowHeight), theme::Muted);
}

// =====================================================================================
//  Cosmic Glass Top Bar
// =====================================================================================

void UiRenderer::DrawTopBar(float W, float margin, float h) noexcept {
    const auto& u = Ui();
    const float cy = h * 0.5f;
    float leftX = margin;

    DrawMaskMark(leftX, cy - 16.0f, 32.0f);
    leftX += 40.0f;
    Text(L"MASK", gpu_.fHeading, Rc(leftX, 0.0f, leftX + 56.0f, h), theme::Text);
    leftX += 60.0f;

    const KeyName pauseKey = NameOf(u.settings.pauseHotkeyVk);
    const bool showPauseKey = (u.settings.pauseHotkeyVk != 0U);
    const float keyW = showPauseKey ? (std::ceil(MeasureWidth(pauseKey.text, gpu_.fMini)) + 14.0f) : 0.0f;

    const wchar_t* label = L"Active";
    UINT32 dot = ColSignalHi();
    if (frame_.paused) {
        dot = theme::Warn;
        label = L"Paused";
        if (frame_.safeStopReason == static_cast<uint8_t>(mask::core::SafeStopReason::PanicChord)) {
            label = L"Emergency stop";
            dot = theme::Danger;
        } else if (frame_.safeStopReason == static_cast<uint8_t>(mask::core::SafeStopReason::EngineStall)) {
            label = L"Stopped for safety";
            dot = theme::Danger;
        }
    }
    const UiAction pauseAct{ ActionKind::TogglePause };
    const float pillW = std::max(146.0f, 44.0f + std::ceil(MeasureWidth(label, gpu_.fBodyStrong)) + (showPauseKey ? keyW + 14.0f : 14.0f));
    const D2D1_RECT_F pill = Anim().ApplySqueeze(Rc(leftX, cy - 16.0f, leftX + pillW, cy + 16.0f), EncodeUiActionKey(pauseAct));
    const bool hov = Hovered(pauseAct);
    GlassCard(pill, 16.0f, false, hov, hov ? mask::ui::Rgba(dot, 0.75f) : theme::GlassBorder, dot, 0.16f);
    gpu_.FillRadialGlow(pill.left + 17.0f, cy, 10.0f, 10.0f, dot, frame_.paused ? 0.25f : 0.42f, dot, 0.0f);
    Dot(pill.left + 17.0f, cy, 4.2f, dot);
    Text(label, gpu_.fBodyStrong, Rc(pill.left + 30.0f, pill.top, pill.right - (showPauseKey ? keyW + 10.0f : 10.0f), pill.bottom), theme::Text);
    if (showPauseKey) {
        MiniKey(pill.right - 10.0f - keyW + 2.0f, cy, pauseKey.text);
    }
    Hit(Rc(leftX, cy - 16.0f, leftX + pillW, cy + 16.0f), pauseAct);
    leftX += pillW + 10.0f;

    if (u.AnyRemapEnabled()) {
        const UiAction cycleAct{ ActionKind::CycleTopLayer };
        const UiAction lockAct{ ActionKind::ToggleTopLock };
        const D2D1_RECT_F layerPill = Anim().ApplySqueeze(RcW(leftX, cy - 15.0f, 92.0f, 30.0f), EncodeUiActionKey(cycleAct));
        const bool lHov = Hovered(cycleAct) || Hovered(lockAct);
        const UINT32 borderCol = frame_.activeRemapLayer > 0 ? mask::ui::Rgba(ColSecondary(), 0.78f) : theme::GlassBorder;
        GlassCard(layerPill, 15.0f, false, lHov, borderCol, ColSecondary(), 0.14f);
        wchar_t lTxt[16]{};
        std::swprintf(lTxt, std::size(lTxt), L"%ls", LayerName(frame_.activeRemapLayer));
        Text(lTxt, gpu_.fBodyStrong, Rc(layerPill.left + 12.0f, layerPill.top, layerPill.left + 58.0f, layerPill.bottom),
             frame_.activeRemapLayer > 0 ? ColSignalHi() : theme::Text);
        const UINT32 lockCol = frame_.remapUnlocked ? theme::Ok : theme::Warn;
        gpu_.FillRadialGlow(layerPill.right - 16.0f, cy, 8.0f, 8.0f, lockCol, 0.30f, lockCol, 0.0f);
        Dot(layerPill.right - 16.0f, cy, 4.0f, lockCol);
        Hit(RcW(leftX, cy - 15.0f, 60.0f, 30.0f), cycleAct);
        Hit(RcW(leftX + 60.0f, cy - 15.0f, 32.0f, 30.0f), lockAct);
    }

    float right = W - margin;

    UINT32 stateColor = theme::Faint;
    const wchar_t* stateLabel = L"Virtual HID";
    const wchar_t* actionText = nullptr;
    switch (u.outputState) {
        case OutputState::Starting:      stateColor = theme::Faint;  stateLabel = L"HID Init";     break;
        case OutputState::Ready:         stateColor = theme::Ok;     stateLabel = L"HID Ready";    break;
        case OutputState::NeedsAdmin:    stateColor = theme::Warn;   stateLabel = L"Admin Req";    actionText = L"Restart as admin"; break;
        case OutputState::Unavailable:   stateColor = theme::Danger; stateLabel = L"HID Offline";  actionText = L"Reconnect"; break;
        case OutputState::NotResponding: stateColor = theme::Danger; stateLabel = L"HID Stalled";  actionText = L"Reconnect"; break;
    }

    if (actionText != nullptr) {
        const float bw = std::ceil(MeasureWidth(actionText, gpu_.fSmall)) + 22.0f;
        Button(Rc(right - bw, cy - 15.0f, right, cy + 15.0f), actionText,
               UiAction{ ActionKind::OutputAction }, BtnStyle::Danger, gpu_.fSmall);
        right -= bw + 8.0f;
    }

    {
        const UiAction hwAct{ ActionKind::OpenHardwareTab };
        const float badgeW = std::ceil(MeasureWidth(stateLabel, gpu_.fSmall)) + 34.0f;
        const D2D1_RECT_F badgeR = Anim().ApplySqueeze(Rc(right - badgeW, cy - 15.0f, right, cy + 15.0f), EncodeUiActionKey(hwAct));
        const bool dh = Hovered(hwAct);
        GlassCard(badgeR, 15.0f, false, dh, dh ? mask::ui::Rgba(stateColor, 0.70f) : theme::GlassBorder, stateColor, 0.14f);
        gpu_.FillRadialGlow(badgeR.left + 15.0f, cy, 8.0f, 8.0f, stateColor, 0.34f, stateColor, 0.0f);
        Dot(badgeR.left + 15.0f, cy, 4.0f, stateColor);
        Text(stateLabel, gpu_.fSmall, Rc(badgeR.left + 25.0f, badgeR.top, badgeR.right - 8.0f, badgeR.bottom),
             dh ? theme::Text : theme::Muted);
        Hit(Rc(right - badgeW, cy - 15.0f, right, cy + 15.0f), hwAct);
        right -= badgeW + 8.0f;
    }

    const float stopW = 78.0f;
    Button(Rc(right - stopW, cy - 15.0f, right, cy + 15.0f), L"Stop all",
           UiAction{ ActionKind::StopAll }, BtnStyle::Ghost, gpu_.fBodyStrong);
    right -= stopW + 8.0f;

    const float setW = 88.0f;
    Button(Rc(right - setW, cy - 15.0f, right, cy + 15.0f), L"Settings",
           UiAction{ ActionKind::ToggleSettings },
           u.showSettings ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fBodyStrong);
}

// =====================================================================================
//  Unified Glass Sidebar
// =====================================================================================

void UiRenderer::DrawSidebar(const D2D1_RECT_F& panel) noexcept {
    const auto& u = Ui();
    const auto& a = Anim();

    GlassCard(panel, 12.0f, false, false, theme::GlassBorder, ColSignal(), 0.24f);

    const float x = panel.left + 14.0f;
    const float right = panel.right - 12.0f;

    Text(L"Workspace", gpu_.fHeading, Rc(x, panel.top + 11.0f, right - 80.0f, panel.top + 40.0f), theme::Text);
    Button(Rc(right - 74.0f, panel.top + 11.0f, right, panel.top + 40.0f), L"+ New",
           UiAction{ ActionKind::OpenNewPicker }, BtnStyle::Primary);

    static constexpr const wchar_t* kFilterLabels[5] = { L"All", L"Auto", L"Seq", L"Trig", L"Map" };
    const float filterTop = panel.top + 48.0f;
    const float filterW = (right - x - 12.0f) / 5.0f;
    for (int f = 0; f < 5; ++f) {
        const float fx = x + static_cast<float>(f) * (filterW + 3.0f);
        const D2D1_RECT_F fr = RcW(fx, filterTop, filterW, 24.0f);
        const bool active = (u.sidebarFilter == f);
        Button(fr, kFilterLabels[f], UiAction{ ActionKind::SidebarFilter, static_cast<int16_t>(f) },
               active ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fMini);
    }

    const float listTop = panel.top + 80.0f;
    Line(panel.left + 1.0f, Snap(listTop - 1.0f), panel.right - 1.0f, Snap(listTop - 1.0f), theme::Line, 1.0f, 0.72f);
    listRect_ = Rc(panel.left + 6.0f, listTop, panel.right - 6.0f, panel.bottom - 6.0f);

    std::array<SidebarEntry, 128> entries{};
    const int count = u.BuildSidebarList(entries);
    const float baseRowH = CurrentListRowHeight();

    float contentH = 10.0f;
    for (int idx = 0; idx < count; ++idx) {
        const size_t animSlot = AnimSlotFor(entries[static_cast<size_t>(idx)].kind, entries[static_cast<size_t>(idx)].index);
        const float sc = std::clamp(a.rowScale[animSlot].value, 0.0f, 1.0f);
        contentH += (baseRowH + kListRowGap) * sc;
    }
    const float viewH = listRect_.bottom - listRect_.top;
    listMaxScroll_ = std::max(0.0f, contentH - viewH);
    const float scrollSrc = (a.listScrollSmooth.value > 0.0f || a.listScrollSmooth.target > 0.0f)
        ? a.listScrollSmooth.value : u.listScroll;
    const float scrollY = std::clamp(scrollSrc, 0.0f, listMaxScroll_);

    if (count == 0) {
        Text(L"Nothing here yet.", gpu_.fBodyStrong, Rc(x, listRect_.top + 16.0f, right, listRect_.top + 38.0f), theme::Muted);
        Text(L"Click + New above to create an", gpu_.fSmall,
             Rc(x, listRect_.top + 42.0f, right, listRect_.top + 60.0f), theme::Faint);
        Text(L"autoclicker, sequence, trigger, or remap.", gpu_.fSmall,
             Rc(x, listRect_.top + 60.0f, right, listRect_.top + 78.0f), theme::Faint);
        return;
    }

    std::array<D2D1_RECT_F, 128> rowRects{};
    std::array<float, 128>       rowScales{};
    UINT32 selectedBadgeCol = ColSignal();
    float iy = listRect_.top + 6.0f - scrollY;
    for (int idx = 0; idx < count; ++idx) {
        const auto& e = entries[static_cast<size_t>(idx)];
        const size_t animSlot = AnimSlotFor(e.kind, e.index);
        const float sc = std::clamp(a.rowScale[animSlot].value, 0.0f, 1.0f);
        const float rowH = baseRowH * sc;
        const D2D1_RECT_F rr = Rc(listRect_.left + 2.0f, iy, listRect_.right - 2.0f, iy + rowH);
        rowRects[static_cast<size_t>(idx)] = rr;
        rowScales[static_cast<size_t>(idx)] = sc;
        if (!u.showSettings && u.selectedKind == e.kind && u.selectedIndex == e.index) {
            hasSelectedSidebarRow_ = true;
            selectedSidebarRowY_ = rr.top;
            selectedSidebarRowH_ = rowH;
            selectedSidebarRowRect_ = rr;
            switch (e.kind) {
                case ItemKind::Autoclicker: selectedBadgeCol = ColSignal(); break;
                case ItemKind::Sequence:    selectedBadgeCol = ColSecondary(); break;
                case ItemKind::Trigger:     selectedBadgeCol = theme::Ok; break;
                case ItemKind::Remap:       selectedBadgeCol = theme::Violet; break;
            }
        }
        iy += (baseRowH + kListRowGap) * sc;
    }

    PushClip(listRect_);

    if (!u.showSettings && (hasSelectedSidebarRow_ || a.sidebarIndH.value > 2.0f)) {
        const bool useAnim = (a.sidebarIndH.value > 2.0f);
        const float indY = useAnim ? a.sidebarIndY.value : selectedSidebarRowY_;
        const float indH = useAnim ? a.sidebarIndH.value : selectedSidebarRowH_;
        if (indH > 2.0f) {
            const D2D1_RECT_F indR = Rc(listRect_.left + 2.0f, indY, listRect_.right - 2.0f, indY + indH);
            gpu_.FillLinearRect(indR, 8.0f, ColSignal(), 0.16f, ColSecondary(), 0.06f, true);
            Stroke(indR, 8.0f, ColSignal(), 1.0f, 0.50f);
            const float insetY = std::min(10.0f, indH * 0.24f);
            GlowLine(indR.left + 2.5f, indR.top + insetY, indR.left + 2.5f, indR.bottom - insetY,
                     selectedBadgeCol, 2.4f, 7.5f, 0.95f);
        }
    }

    for (int idx = 0; idx < count; ++idx) {
        const float sc = rowScales[static_cast<size_t>(idx)];
        const D2D1_RECT_F& rr = rowRects[static_cast<size_t>(idx)];
        const float rowH = rr.bottom - rr.top;
        if (rowH >= 2.0f && rr.bottom >= listRect_.top && rr.top <= listRect_.bottom) {
            DrawSidebarEntry(entries[static_cast<size_t>(idx)], rr, sc);
        }
    }
    PopClip();

    if (listMaxScroll_ > 0.5f) {
        const float trackH = viewH - 12.0f;
        const float thumbH = std::max(28.0f, trackH * (viewH / contentH));
        const float t = scrollY / listMaxScroll_;
        Fill(RcW(panel.right - 5.0f, listRect_.top + 6.0f + t * (trackH - thumbH), 3.0f, thumbH), 1.5f, theme::LineHi, 0.80f);
    }
}

void UiRenderer::DrawSidebarEntry(const SidebarEntry& e, const D2D1_RECT_F& r, float alphaScale) noexcept {
    const auto& u = Ui();
    const auto& a = Anim();
    const bool compact = u.settings.compactSidebar;
    const bool selected = !u.showSettings && (u.selectedKind == e.kind && u.selectedIndex == e.index);
    const int animIdx = static_cast<int>(AnimSlotFor(e.kind, e.index));
    const UiAction selectA{ ActionKind::SelectItemBase, static_cast<int16_t>(animIdx) };
    const UiAction toggleA{ ActionKind::ToggleItemBase, static_cast<int16_t>(animIdx) };

    const wchar_t* badge = L"A";
    UINT32 badgeCol = ColSignal();
    const wchar_t* name = L"";
    bool enabled = false;
    bool firing = false;
    wchar_t sub[96]{};
    UINT32 subCol = theme::Muted;
    UINT32 swatchCol = 0;
    bool hasSwatch = false;

    switch (e.kind) {
        case ItemKind::Autoclicker: {
            const auto& s = u.slots[static_cast<size_t>(e.index)];
            badge = L"A";
            badgeCol = ColSignal();
            name = s.name;
            enabled = s.enabled;
            firing = (frame_.firingMask & (1ULL << e.index)) != 0ULL;
            if (s.triggerVk == 0U) {
                wcscpy_s(sub, L"No trigger yet");
                subCol = theme::Warn;
            } else {
                std::swprintf(sub, std::size(sub), L"%ls \u2022 %ls \u2192 %ls",
                              ModeName(s.mode), NameOf(s.triggerVk).text, NameOf(s.outputVk).text);
            }
            break;
        }
        case ItemKind::Sequence: {
            const auto& q = u.sequences[static_cast<size_t>(e.index)];
            badge = L"S";
            badgeCol = ColSecondary();
            name = q.name;
            enabled = q.enabled;
            firing = (frame_.activeSeqMask & (1U << e.index)) != 0U;
            std::swprintf(sub, std::size(sub), L"%u steps \u2022 %ls",
                          static_cast<unsigned>(q.stepCount),
                          q.triggerVk ? NameOf(q.triggerVk).text : L"No trigger");
            break;
        }
        case ItemKind::Trigger: {
            const auto& t = u.triggers[static_cast<size_t>(e.index)];
            badge = L"T";
            badgeCol = theme::Ok;
            name = t.name;
            enabled = t.enabled;
            hasSwatch = true;
            swatchCol = (static_cast<UINT32>(t.r) << 16) | (static_cast<UINT32>(t.g) << 8) | static_cast<UINT32>(t.b);
            const wchar_t* modeLbl = (static_cast<ColorTriggerMode>(t.mode) == ColorTriggerMode::ColorChanged)
                ? L"Change" : L"Color";
            if (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::ClickKey) ||
                t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::HoldKey)) {
                std::swprintf(sub, std::size(sub), L"%ls \u2192 %ls", modeLbl, NameOf(t.actionVk).text);
            } else if (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot)) {
                std::swprintf(sub, std::size(sub), L"%ls \u2192 Clicker #%u", modeLbl, static_cast<unsigned>(t.linkedIndex + 1U));
            } else {
                std::swprintf(sub, std::size(sub), L"%ls \u2192 Sequence #%u", modeLbl, static_cast<unsigned>(t.linkedIndex + 1U));
            }
            break;
        }
        case ItemKind::Remap: {
            const auto& rm = u.remaps[static_cast<size_t>(e.index)];
            badge = L"R";
            badgeCol = theme::Violet;
            name = rm.name;
            enabled = rm.enabled;
            if (rm.sourceVk == 0U) {
                wcscpy_s(sub, L"No source key yet");
                subCol = theme::Warn;
            } else {
                std::swprintf(sub, std::size(sub), L"%ls \u2022 %ls \u2192 %ls",
                              LayerName(rm.layerIndex), NameOf(rm.sourceVk).text,
                              rm.actionKind == static_cast<uint8_t>(RemapLayerAction::RemapKey)
                                  ? NameOf(rm.targetVk).text
                                  : RemapActionName(static_cast<RemapLayerAction>(rm.actionKind)));
            }
            break;
        }
    }

    const bool hov = Hovered(selectA) || Hovered(toggleA);
    if (!selected && hov) {
        Fill(r, 8.0f, theme::Raised, 0.55f * alphaScale);
        Stroke(r, 8.0f, theme::Line, 1.0f, 0.60f * alphaScale);
    }
    Hit(r, selectA);

    const float cy = (r.top + r.bottom) * 0.5f;
    const float badgeX = r.left + 11.0f;
    const D2D1_RECT_F badgeR = RcW(badgeX, cy - 9.0f, 18.0f, 18.0f);
    Fill(badgeR, 5.0f, badgeCol, 0.16f * alphaScale);
    Stroke(badgeR, 5.0f, badgeCol, 1.0f, 0.65f * alphaScale);
    Text(badge, gpu_.fMini, badgeR, badgeCol, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);

    float textLeft = badgeR.right + 8.0f;
    const float textRight = r.right - 50.0f;

    const float pulseT = (animIdx >= 0 && animIdx < static_cast<int>(mask::ui::UiAnimator::kSlotCount))
        ? std::clamp(a.firingPulse[static_cast<size_t>(animIdx)].value, 0.0f, 1.0f)
        : (firing ? 1.0f : 0.0f);
    const float activeGlow = firing ? std::max(0.85f, pulseT) : pulseT;

    if (activeGlow > 0.05f) {
        const float dotY = compact ? cy : r.top + 17.0f;
        gpu_.FillRadialGlow(textLeft + 4.0f, dotY, 8.5f, 8.5f, ColSignalHi(), 0.45f * activeGlow * alphaScale, ColSignal(), 0.0f);
        Dot(textLeft + 4.0f, dotY, 3.6f, ColSignalHi(), alphaScale * std::max(0.35f, activeGlow));
        textLeft += 14.0f;
    } else if (hasSwatch) {
        const float dotY = compact ? cy : r.top + 17.0f;
        Dot(textLeft + 4.0f, dotY, 4.5f, swatchCol, alphaScale);
        textLeft += 13.0f;
    }

    if (compact) {
        Text(name, gpu_.fBodyStrong, Rc(textLeft, r.top + 4.0f, textRight, r.bottom - 4.0f),
             enabled ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);
    } else {
        Text(name, gpu_.fBodyStrong, Rc(textLeft, r.top + 6.0f, textRight, r.top + 28.0f),
             enabled ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);
        Text(sub, gpu_.fSmall, Rc(badgeR.right + 8.0f, r.top + 27.0f, textRight, r.bottom - 5.0f),
             subCol, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);
    }

    Switch(r.right - 44.0f, cy - 10.0f, enabled, toggleA, firing, animIdx, alphaScale);
}

// =====================================================================================
//  Main Editor Glass Router
// =====================================================================================

void UiRenderer::DrawEditor(const D2D1_RECT_F& panel) noexcept {
    const auto& u = Ui();
    const auto& a = Anim();

    GlassCard(panel, 12.0f, false, false, theme::GlassBorder, ColSignal(), 0.24f);

    const float vt = std::clamp(a.viewTransition.value, 0.0f, 1.0f);
    const float slideY = (1.0f - vt) * 10.0f;
    const D2D1_RECT_F viewPanel = Rc(panel.left, panel.top + slideY, panel.right, panel.bottom + slideY);

    PushClip(Rc(panel.left + 1.0f, panel.top + 1.0f, panel.right - 1.0f, panel.bottom - 1.0f));
    if (u.showSettings) {
        DrawSettingsOverlay(viewPanel);
    } else if (!u.HasSelection()) {
        DrawEmptyState(viewPanel);
    } else {
        switch (u.selectedKind) {
            case ItemKind::Autoclicker: DrawAutoclickerEditor(viewPanel); break;
            case ItemKind::Sequence:    DrawSequenceEditor(viewPanel); break;
            case ItemKind::Trigger:     DrawTriggerEditor(viewPanel); break;
            case ItemKind::Remap:       DrawRemapEditor(viewPanel); break;
        }
    }
    PopClip();
}

void UiRenderer::DrawEmptyState(const D2D1_RECT_F& panel) noexcept {
    const float cx = (panel.left + panel.right) * 0.5f;
    const float cy = (panel.top + panel.bottom) * 0.5f - 28.0f;
    gpu_.FillRadialGlow(cx, cy - 10.0f, 180.0f, 70.0f, ColSignal(), 0.14f, ColSignal(), 0.0f);
    Text(L"Your cosmic workspace is ready", gpu_.fTitle, Rc(panel.left, cy - 36.0f, panel.right, cy),
         theme::Text, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(L"Create an autoclicker, 128-step sequence, pixel color trigger, or multi-device remap.", gpu_.fBody,
         Rc(panel.left + 24.0f, cy + 4.0f, panel.right - 24.0f, cy + 28.0f),
         theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    Button(Rc(cx - 78.0f, cy + 44.0f, cx + 78.0f, cy + 78.0f), L"+ New item",
           UiAction{ ActionKind::OpenNewPicker }, BtnStyle::Primary);
}

// =====================================================================================
//  Unrestricted Autoclicker Editor (Hold / Toggle / Burst + Amber Warning + Oscilloscope)
// =====================================================================================

void UiRenderer::DrawAutoclickerEditor(const D2D1_RECT_F& panel) noexcept {
    const auto& u = Ui();
    const int idx = std::clamp(u.selectedIndex, 0, kSlotCount - 1);
    const auto& s = u.slots[static_cast<size_t>(idx)];
    const auto mode = static_cast<MacroMode>(s.mode);
    const bool firing = (frame_.firingMask & (1ULL << idx)) != 0ULL;
    const uint8_t bindTarget = frame_.bindTarget;
    const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Autoclicker, idx));
    const bool unsafeTiming = IsAutoclickerTimingRisky(s);

    const float L = panel.left + 24.0f;
    const float R = panel.right - 24.0f;
    const float labelW = 96.0f;
    const float C = L + labelW;
    float y = panel.top + 14.0f;

    // Header: inline rename | on/off | duplicate | delete
    const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
    Button(delBtn, u.pendingDeleteConfirm ? L"Confirm?" : L"Delete", UiAction{ ActionKind::DeleteItem },
           u.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

    const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
    Button(dupBtn, L"Duplicate", UiAction{ ActionKind::DuplicateItem }, BtnStyle::Ghost);

    const float switchX = dupBtn.left - 16.0f - 36.0f;
    const UiAction toggleAct{ ActionKind::ToggleEnabled };
    Switch(switchX, y + 8.0f, s.enabled, toggleAct, firing, animIdx);
    const wchar_t* stateWord = firing ? L"Running" : (s.enabled ? L"On" : L"Off");
    Text(stateWord, gpu_.fBody, Rc(switchX - 72.0f, y, switchX - 10.0f, y + 36.0f),
         firing ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Hit(Rc(switchX - 72.0f, y, switchX, y + 36.0f), toggleAct);

    const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 82.0f, y + 36.0f);
    const UiAction nameA{ ActionKind::FieldFocus, static_cast<int16_t>(Field::Name) };
    if (u.editField == Field::Name) {
        Fill(nameRect, 6.0f, theme::Inset, 0.92f);
        Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
        Text(u.editBuffer, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
        const float caretX = Snap(L + MeasureWidth(u.editBuffer, gpu_.fTitle) + 1.5f);
        Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignalHi(), 1.5f);
    } else {
        if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised, 0.72f);
        Text(s.name, gpu_.fTitle, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
    }
    Hit(nameRect, nameA);

    wchar_t sentence[200]{};
    DescribeAutoclicker(s, sentence, std::size(sentence));
    const UINT32 subCol = (s.triggerVk == 0U || unsafeTiming) ? theme::Warn : theme::Muted;
    Text(sentence, gpu_.fBody, Rc(L, y + 36.0f, R, y + 54.0f), subCol);
    y += 58.0f;

    // Prominent Amber Warning Banner when CPS > 1,000 or Hold/Release < 0.05 ms (including 0 ms)
    if (unsafeTiming) {
        const D2D1_RECT_F warnR = Rc(L, y, R, y + 32.0f);
        gpu_.FillLinearRect(warnR, 8.0f, theme::Warn, 0.22f, theme::Warn, 0.10f, true);
        Stroke(warnR, 8.0f, theme::Warn, 1.25f, 0.90f);
        const float dotY = (warnR.top + warnR.bottom) * 0.5f;
        gpu_.FillRadialGlow(warnR.left + 15.0f, dotY, 9.0f, 9.0f, theme::Warn, 0.38f, theme::Warn, 0.0f);
        Dot(warnR.left + 15.0f, dotY, 3.8f, theme::Warn);
        Text(L"Warning: CPS over 1,000 or hold/release under 0.05 ms (or 0 ms) can cause lag and unstable behavior.",
             gpu_.fSmall, Rc(warnR.left + 26.0f, warnR.top, warnR.right - 10.0f, warnR.bottom), 0xFDE68AU);
        y += 40.0f;
    }

    if (u.settings.showSignalView) {
        const float sigH = unsafeTiming ? 104.0f : 114.0f;
        DrawSignalView(Rc(L, y, R, y + sigH), s, firing);
        y += sigH + 10.0f;
    }

    constexpr float kRowH   = 32.0f;
    constexpr float kRowGap = 7.0f;

    // Trigger bind
    RowLabel(L, y, kRowH, L"Trigger");
    const D2D1_RECT_F trigChip = RcW(C, y, 170.0f, kRowH);
    KeyChip(trigChip, s.triggerVk, UiAction{ ActionKind::BindTrigger }, bindTarget == 1U);
    DrawBindHint(Rc(trigChip.right + 16.0f, y, R, y + kRowH), bindTarget == 1U, s, idx);
    y += kRowH + kRowGap;

    // Output bind + quick mouse/scroll chips
    RowLabel(L, y, kRowH, L"Output");
    const D2D1_RECT_F outChip = RcW(C, y, 144.0f, kRowH);
    KeyChip(outChip, s.outputVk, UiAction{ ActionKind::BindOutput }, bindTarget == 2U);
    const float qStart = outChip.right + 12.0f;
    float qx = qStart;
    float qy = y + 3.0f;
    bool wrapped = false;
    for (int i = 0; i < kQuickOutputCount; ++i) {
        const float qw = std::ceil(MeasureWidth(kQuickOutputs[i].label, gpu_.fSmall)) + 16.0f;
        if (qx > qStart && qx + qw > R) {
            qx = qStart;
            qy += 28.0f;
            wrapped = true;
        }
        Button(RcW(qx, qy, qw, 26.0f), kQuickOutputs[i].label,
               UiAction{ ActionKind::QuickOut, static_cast<int16_t>(i) },
               s.outputVk == kQuickOutputs[i].vk ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
        qx += qw + 5.0f;
    }
    y += (wrapped ? 56.0f : kRowH) + kRowGap;

    // Mode segmented pills (Hold / Toggle / Burst) + Repeats stepper in Burst
    RowLabel(L, y, kRowH, L"Mode");
    const bool showTimes = (mode == MacroMode::RepeatNTimes);
    const float segW = std::min(R - C - (showTimes ? 148.0f : 0.0f), 324.0f);
    const D2D1_RECT_F seg = RcW(C, y, segW, kRowH);
    Fill(seg, 8.0f, theme::Inset, 0.85f);
    Stroke(seg, 8.0f, theme::Line, 1.0f, 0.85f);
    const float cellW = (segW - 6.0f) / static_cast<float>(kModeCount);
    for (int m = 0; m < kModeCount; ++m) {
        const D2D1_RECT_F cell = RcW(seg.left + 3.0f + cellW * static_cast<float>(m), seg.top + 3.0f, cellW, kRowH - 6.0f);
        const bool sel = (s.mode == static_cast<uint8_t>(m));
        const UiAction modeAct{ ActionKind::Mode, static_cast<int16_t>(m) };
        if (sel) {
            gpu_.FillLinearRect(cell, 6.0f, theme::RaisedHi, 0.95f, theme::Raised, 0.90f, false);
            GlowLine(cell.left + 14.0f, cell.bottom - 1.5f, cell.right - 14.0f, cell.bottom - 1.5f,
                     ColSignalHi(), 2.0f, 5.5f, 0.92f);
        } else if (Hovered(modeAct)) {
            Fill(cell, 6.0f, theme::Raised, 0.65f);
        }
        Text(ModeName(m), sel ? gpu_.fBodyStrong : gpu_.fBody, cell,
             (sel || Hovered(modeAct)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(cell, modeAct);
    }
    if (showTimes) {
        Text(L"Repeats", gpu_.fSmall, RcW(seg.right + 12.0f, y, 50.0f, kRowH), theme::Muted);
        NumberField(RcW(seg.right + 64.0f, y + 1.0f, std::min(120.0f, R - (seg.right + 64.0f)), kRowH - 2.0f),
                    s, Field::Repeat, true);
    }
    Text(ModeDescription(s.mode), gpu_.fSmall, Rc(C, y + kRowH + 2.0f, R, y + kRowH + 17.0f), theme::Faint);
    y += kRowH + 17.0f + kRowGap;

    // CPS, Hold (ms), Release (ms)
    RowLabel(L, y, kRowH, L"CPS & delay");
    NumberField(RcW(C, y, 146.0f, kRowH), s, Field::Rate, true, L"\u221E CPS");
    float fx = C + 146.0f + 14.0f;
    Text(L"Hold", gpu_.fSmall, RcW(fx, y, 32.0f, kRowH), theme::Muted);
    fx += 34.0f;
    NumberField(RcW(fx, y, 96.0f, kRowH), s, Field::Down, false);
    fx += 96.0f + 14.0f;
    Text(L"Release", gpu_.fSmall, RcW(fx, y, 48.0f, kRowH), theme::Muted);
    fx += 50.0f;
    NumberField(RcW(fx, y, std::min(96.0f, std::max(72.0f, R - fx)), kRowH), s, Field::Up, false);
    y += kRowH + kRowGap;

    // Duty cycle slider + field
    RowLabel(L, y, kRowH, L"Duty cycle");
    const float sliderW = std::min(280.0f, R - C - 140.0f);
    const float trackY = Snap(y + kRowH * 0.5f);
    dutyTrack_ = Rc(C + 8.0f, trackY - 12.0f, C + sliderW - 8.0f, trackY + 12.0f);
    const double dutyPct = DutyPercent(s);
    const float frac = static_cast<float>(std::clamp(dutyPct / 100.0, 0.0, 1.0));
    const float thumbX = dutyTrack_.left + frac * (dutyTrack_.right - dutyTrack_.left);
    const UiAction dutyAct{ ActionKind::DutySlider };
    const bool sliderHot = u.draggingDuty || (u.hoveredAction == dutyAct);
    Fill(Rc(dutyTrack_.left, trackY - 3.0f, dutyTrack_.right, trackY + 3.0f), 3.0f, theme::Inset);
    Stroke(Rc(dutyTrack_.left, trackY - 3.0f, dutyTrack_.right, trackY + 3.0f), 3.0f, theme::Line);
    if (thumbX > dutyTrack_.left + 1.0f) {
        gpu_.FillLinearRect(Rc(dutyTrack_.left, trackY - 3.0f, thumbX, trackY + 3.0f), 3.0f,
                            ColSignal(), 0.95f, ColSignalHi(), 0.95f, true);
    }
    if (sliderHot) {
        gpu_.FillRadialGlow(thumbX, trackY, 14.0f, 14.0f, ColSignal(), 0.30f, ColSignal(), 0.0f);
    }
    Dot(thumbX, trackY, 7.5f, ColSignal());
    Dot(thumbX, trackY, 3.0f, theme::IceCore);
    Hit(Rc(C, y, C + sliderW, y + kRowH), dutyAct);

    NumberField(RcW(C + sliderW + 14.0f, y, 122.0f, kRowH), s, Field::Duty, true);
    const wchar_t* dutyHint = (PeriodMs(s) <= 0.0)
        ? L"Both hold and release are 0 ms (50% nominal duty)."
        : L"Hold vs release share per click cycle (0 ms = zero delay).";
    Text(dutyHint, gpu_.fSmall, Rc(C, y + kRowH + 1.0f, R, y + kRowH + 16.0f), theme::Faint);
    y += kRowH + 16.0f + kRowGap;

    // Optional Jitter (Exact / +-5% / +-10% / +-20% / +-35%)
    RowLabel(L, y, kRowH, L"Jitter");
    const uint8_t curHum = mask::core::HumanizePercent(s);
    float hx = C;
    for (int i = 0; i < kHumanizeCount; ++i) {
        const float hw = std::ceil(MeasureWidth(kHumanizePresets[i].label, gpu_.fSmall)) + 18.0f;
        Button(RcW(hx, y + 3.0f, hw, 26.0f), kHumanizePresets[i].label,
               UiAction{ ActionKind::HumanizeBase, static_cast<int16_t>(i) },
               curHum == kHumanizePresets[i].pct ? BtnStyle::ChipOn : BtnStyle::Chip, gpu_.fSmall);
        hx += hw + 5.0f;
    }
    if (hx + 110.0f < R) {
        const wchar_t* humHint = (curHum == 0U)
            ? L"Exact hardware timing"
            : L"Varies hold and release per click";
        Text(humHint, gpu_.fSmall, Rc(hx + 8.0f, y, R, y + kRowH), theme::Faint);
    }
    y += kRowH + kRowGap;

    // Optional subtle mouse move delta
    RowLabel(L, y, kRowH, L"Move pointer");
    NumberField(RcW(C, y, 118.0f, kRowH), s, Field::MoveX, true);
    Text(L"right", gpu_.fSmall, RcW(C + 124.0f, y, 36.0f, kRowH), theme::Faint);
    NumberField(RcW(C + 166.0f, y, 118.0f, kRowH), s, Field::MoveY, true);
    Text(L"down, per click", gpu_.fSmall, RcW(C + 290.0f, y, R - C - 290.0f, kRowH), theme::Faint);
}

void UiRenderer::DrawBindHint(const D2D1_RECT_F& r, bool binding, const MacroConfig& s, int idx) noexcept {
    const auto& u = Ui();
    if (binding) {
        Text(L"Press any key or mouse button. Esc cancels.", gpu_.fSmall, r, ColSignalHi());
        return;
    }
    if (u.settings.pauseHotkeyVk != 0U && s.triggerVk == u.settings.pauseHotkeyVk) {
        wchar_t msg[96]{};
        std::swprintf(msg, std::size(msg), L"%ls is the pause shortcut, so it can't start an autoclicker.",
                      NameOf(u.settings.pauseHotkeyVk).text);
        Text(msg, gpu_.fSmall, r, theme::Warn);
        return;
    }
    if (s.triggerVk != 0U) {
        for (int j = 0; j < kSlotCount; ++j) {
            if (j == idx || !u.slots[static_cast<size_t>(j)].enabled) continue;
            if (u.slots[static_cast<size_t>(j)].triggerVk == s.triggerVk) {
                wchar_t msg[96]{};
                std::swprintf(msg, std::size(msg), L"Also starts \u201C%ls\u201D", u.slots[static_cast<size_t>(j)].name);
                Text(msg, gpu_.fSmall, r, theme::Warn);
                return;
            }
        }
    }
    if (s.triggerVk == 0U) {
        Text(L"Click to bind", gpu_.fSmall, r, theme::Faint);
    }
}

// =====================================================================================
//  Glowing Cosmic Oscilloscope Signal View
// =====================================================================================

void UiRenderer::Trace(const D2D1_RECT_F& lane, const Pulse* pulses, int count, float fillAlpha,
                       float coreWidth, float glowWidth, bool labels) noexcept {
    const float top = Snap(lane.top);
    const float base = Snap(lane.bottom);
    const UINT32 lowColor = count > 0 ? pulses[0].color : theme::Faint;
    float cursor = lane.left;

    for (int i = 0; i < count; ++i) {
        const Pulse& p = pulses[i];
        const float x0 = Snap(std::max(p.x0, lane.left - 8.0f));
        const float x1 = Snap(std::min(p.x1, lane.right + 8.0f));
        if (x1 <= x0 || x0 > lane.right) continue;
        const bool joinsPrev = (i > 0 && std::fabs(p.x0 - pulses[i - 1].x1) < 0.75f);
        const bool joinsNext = (i + 1 < count && std::fabs(pulses[i + 1].x0 - p.x1) < 0.75f);

        gpu_.FillLinearRect(Rc(x0, top, x1, base), 0.0f, p.color, fillAlpha, p.color, fillAlpha * 0.14f, false);
        if (!joinsPrev) {
            Line(cursor, base, x0, base, lowColor, coreWidth * 0.85f, 0.36f);
            GlowLine(x0, base, x0, top, p.color, coreWidth, glowWidth, 0.96f);
        } else {
            Line(x0, top + 4.0f, x0, base - 3.0f, theme::Inset, 1.5f);
        }
        GlowLine(x0, top, x1, top, p.color, coreWidth, glowWidth, 0.98f);
        if (!joinsNext) {
            GlowLine(x1, top, x1, base, p.color, coreWidth, glowWidth, 0.96f);
        }
        cursor = x1;

        if (labels && p.label[0] != L'\0') {
            const float tw = MeasureWidth(p.label, gpu_.fAxis);
            if (tw + 8.0f <= (x1 - x0)) {
                Text(p.label, gpu_.fAxis, Rc(x0, top, x1, base), theme::Text, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }
    if (cursor < lane.right) {
        Line(cursor, base, lane.right, base, lowColor, coreWidth * 0.85f, 0.36f);
    }
}

void UiRenderer::LaneLabel(const D2D1_RECT_F& view, const D2D1_RECT_F& lane, float gutterRight,
                           const wchar_t* text, UINT32 color) noexcept {
    Text(text, gpu_.fSmall, Rc(view.left + 14.0f, lane.top - 4.0f, gutterRight - 10.0f, lane.bottom + 4.0f),
         color, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void UiRenderer::Tick(float x, float top, float bottom, const wchar_t* label, float labelRight, float maxLabelWidth) noexcept {
    const float sx = Snap(x);
    Line(sx, top, sx, bottom, theme::Line, 1.0f, 0.55f);
    if (label != nullptr && label[0] != L'\0') {
        Text(label, gpu_.fAxis, Rc(sx - 3.0f, bottom + 2.0f, std::min(sx + maxLabelWidth, labelRight), bottom + 17.0f), theme::Faint);
    }
}

void UiRenderer::DrawSignalView(const D2D1_RECT_F& view, const MacroConfig& s, bool firing) noexcept {
    GlassCard(view, 9.0f, true, firing,
              firing ? mask::ui::Rgba(ColSignal(), 0.78f) : theme::GlassBorder,
              ColSignal(), 0.16f);

    const auto mode = static_cast<MacroMode>(s.mode);
    const UINT32 ch1 = !s.enabled ? theme::Faint : (firing ? ColSignalHi() : ColSignal());
    const UINT32 trig = theme::Muted;
    const float outAlpha = !s.enabled ? 0.12f : (firing ? 0.38f : 0.22f);
    const float outWidth = firing ? 2.1f : 1.7f;
    const float outGlow  = !s.enabled ? 0.0f : (firing ? 8.0f : 5.5f);

    const float gutter = view.left + 104.0f;
    const float plotL = gutter;
    const float plotR = view.right - 18.0f;
    const float plotW = std::max(40.0f, plotR - plotL);
    const float viewH = view.bottom - view.top;

    PushClip(Rc(view.left + 1.0f, view.top + 1.0f, view.right - 1.0f, view.bottom - 1.0f));

    for (int g = 1; g <= 3; ++g) {
        const float gy = Snap(view.top + viewH * (static_cast<float>(g) * 0.25f));
        Line(plotL, gy, plotR, gy, theme::Line, 1.0f, 0.24f);
    }

    const D2D1_RECT_F laneT = Rc(plotL, view.top + 10.0f, plotR, view.top + 28.0f);
    const D2D1_RECT_F laneO = Rc(plotL, view.top + 40.0f, plotR, view.bottom - 26.0f);
    const float tickTop     = view.top + 8.0f;
    const float tickBottom  = view.bottom - 21.0f;

    LaneLabel(view, laneT, gutter, s.triggerVk != 0U ? NameOf(s.triggerVk).text : L"Trigger", theme::Faint);

    const double downMs   = DownMs(s);
    const double upMs     = UpMs(s);
    const double periodMs = PeriodMs(s);
    const float pxPerCycle = plotW / 5.6f;
    const float x0 = plotL + 0.32f * pxPerCycle;

    float visDutyFrac = 0.5f;
    if (periodMs > 0.0) {
        const float rawFrac = static_cast<float>(downMs / periodMs);
        if (upMs <= 0.0) {
            visDutyFrac = 0.90f;
        } else if (downMs <= 0.0) {
            visDutyFrac = 0.08f;
        } else {
            visDutyFrac = std::clamp(rawFrac, 0.06f, 0.94f);
        }
    }

    const float stopAt = 4.25f;
    auto X = [&](float cycles) noexcept { return x0 + cycles * pxPerCycle; };

    const bool zeroPeriod = !(periodMs > 0.0);
    const bool seconds = !zeroPeriod && (periodMs * 5.0 >= 1000.0);
    for (int k = 0; k <= 5; ++k) {
        wchar_t label[40]{}, num[24]{};
        if (zeroPeriod) {
            wcscpy_s(label, k == 5 ? L"0 ms" : L"0");
        } else {
            const double tMs = periodMs * static_cast<double>(k);
            if (seconds) FormatNumber(tMs / 1000.0, 2, num, std::size(num));
            else         FormatMs(tMs, num, std::size(num));
            if (k == 5) std::swprintf(label, std::size(label), L"%ls %ls", num, seconds ? L"s" : L"ms");
            else        wcscpy_s(label, num);
        }
        Tick(X(static_cast<float>(k)), tickTop, tickBottom, label, plotR + 14.0f);
    }

    Pulse trigger[2]{};
    int triggerCount = 1;
    Pulse out[8]{};
    int n = 0;
    const float tapW = std::max(8.0f, 0.16f * pxPerCycle);

    if (mode == MacroMode::HoldToLoop) {
        trigger[0] = Pulse{ X(0.0f), X(stopAt), trig, {} };
        for (int k = 0; k < 5 && static_cast<float>(k) < stopAt; ++k) {
            const float pEnd = std::min(static_cast<float>(k) + visDutyFrac, stopAt);
            out[n++] = Pulse{ X(static_cast<float>(k)), std::max(X(static_cast<float>(k)) + 2.0f, X(pEnd)), ch1, {} };
        }
    } else if (mode == MacroMode::InfiniteToggleLoop) {
        trigger[0] = Pulse{ X(0.0f), X(0.0f) + tapW, trig, {} };
        trigger[1] = Pulse{ X(stopAt), X(stopAt) + tapW, trig, {} };
        triggerCount = 2;
        for (int k = 0; k < 5 && static_cast<float>(k) < stopAt; ++k) {
            out[n++] = Pulse{ X(static_cast<float>(k)), X(static_cast<float>(k) + visDutyFrac), ch1, {} };
        }
    } else {
        trigger[0] = Pulse{ X(0.0f), X(0.0f) + tapW, trig, {} };
        const int shown = static_cast<int>(std::min<uint32_t>(s.repeatCount, 5U));
        for (int k = 0; k < shown; ++k) {
            out[n++] = Pulse{ X(static_cast<float>(k)), X(static_cast<float>(k) + visDutyFrac), ch1, {} };
        }
    }

    Trace(laneT, trigger, triggerCount, 0.12f, 1.25f, 0.0f, false);
    Trace(laneO, out, n, outAlpha, outWidth, outGlow, false);

    const uint8_t hum = mask::core::HumanizePercent(s);
    if (hum > 0U && !zeroPeriod) {
        const float jitterPx = std::clamp(pxPerCycle * (static_cast<float>(hum) / 100.0f) * 0.45f, 2.0f, 14.0f);
        const float jTop = Snap(laneO.top);
        const float jBot = Snap(laneO.bottom);
        for (int i = 0; i < n; ++i) {
            if (i > 0) {
                const float rx = Snap(std::max(out[i].x0, laneO.left));
                if (rx >= laneO.left && rx <= laneO.right) {
                    Fill(Rc(rx - jitterPx, jTop, rx + jitterPx, jBot), 0.0f, ch1, 0.12f);
                    Line(Snap(rx - jitterPx), jTop + 3.0f, Snap(rx - jitterPx), jBot - 3.0f, ch1, 1.0f, 0.42f);
                    Line(Snap(rx + jitterPx), jTop + 3.0f, Snap(rx + jitterPx), jBot - 3.0f, ch1, 1.0f, 0.42f);
                }
            }
            const float fx = Snap(std::min(out[i].x1, laneO.right));
            if (fx >= laneO.left && fx <= laneO.right) {
                Fill(Rc(fx - jitterPx, jTop, fx + jitterPx, jBot), 0.0f, ch1, 0.12f);
                Line(Snap(fx - jitterPx), jTop + 3.0f, Snap(fx - jitterPx), jBot - 3.0f, ch1, 1.0f, 0.42f);
                Line(Snap(fx + jitterPx), jTop + 3.0f, Snap(fx + jitterPx), jBot - 3.0f, ch1, 1.0f, 0.42f);
            }
        }
    }

    LaneLabel(view, laneO, gutter, NameOf(s.outputVk).text, theme::Muted);

    if (mode == MacroMode::RepeatNTimes && s.repeatCount > 5U) {
        wchar_t more[64]{};
        if (hum > 0U) {
            std::swprintf(more, std::size(more), L"%u clicks \u2022 \u00B1%u%%",
                          static_cast<unsigned>(s.repeatCount), static_cast<unsigned>(hum));
        } else {
            std::swprintf(more, std::size(more), L"%u clicks total", static_cast<unsigned>(s.repeatCount));
        }
        Text(more, gpu_.fAxis, Rc(plotR - 160.0f, laneT.top, plotR, laneT.bottom), theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (zeroPeriod) {
        Text(L"0 ms period (\u221E CPS)", gpu_.fAxis, Rc(plotR - 150.0f, laneT.top, plotR, laneT.bottom),
             theme::Warn, DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (hum > 0U) {
        wchar_t humTag[32]{};
        std::swprintf(humTag, std::size(humTag), L"\u00B1%u%% jitter", static_cast<unsigned>(hum));
        Text(humTag, gpu_.fAxis, Rc(plotR - 110.0f, laneT.top, plotR, laneT.bottom), theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    PopClip();
}

// =====================================================================================
//  Status Bar
// =====================================================================================

void UiRenderer::DrawStatusBar(const D2D1_RECT_F& r) noexcept {
    const auto& u = Ui();
    const auto& a = Anim();
    const float cy = Snap((r.top + r.bottom) * 0.5f) - 0.5f;

    wchar_t bankBuf[24]{};
    std::swprintf(bankBuf, std::size(bankBuf), L"Bank %u", static_cast<unsigned>(u.settings.activeProfileBank + 1U));
    const wchar_t* saveText = L"All changes saved";
    UINT32 saveColor = theme::Faint;
    if (u.saveState == SaveState::Pending) {
        saveText = L"Saving\u2026";
        saveColor = ColSignalHi();
    } else if (u.saveState == SaveState::Failed) {
        saveText = L"Couldn't save";
        saveColor = theme::Danger;
    }
    wchar_t rightInfo[64]{};
    std::swprintf(rightInfo, std::size(rightInfo), L"%ls \u2022 %ls", bankBuf, saveText);
    const float saveW = std::ceil(MeasureWidth(rightInfo, gpu_.fSmall));
    Text(rightInfo, gpu_.fSmall, Rc(r.right - saveW - 4.0f, r.top, r.right, r.bottom), saveColor, DWRITE_TEXT_ALIGNMENT_TRAILING);

    float hintsLeft = r.right - saveW - 24.0f;
    if (u.settings.panicChordEnabled) {
        const wchar_t* stopLabel = L"emergency stop";
        const float hintsW = 3.0f * 12.0f + MeasureWidth(L"Left Ctrl", gpu_.fMini) + MeasureWidth(L"Right Ctrl", gpu_.fMini) +
                             MeasureWidth(L"Esc", gpu_.fMini) + 2.0f * 4.0f + 8.0f + MeasureWidth(stopLabel, gpu_.fSmall);
        float hx = r.right - saveW - 24.0f - hintsW;
        hintsLeft = hx;
        if (hx > r.left + 320.0f) {
            hx = MiniKey(hx, cy, L"Left Ctrl") + 4.0f;
            hx = MiniKey(hx, cy, L"Right Ctrl") + 4.0f;
            hx = MiniKey(hx, cy, L"Esc") + 8.0f;
            Text(stopLabel, gpu_.fSmall, Rc(hx, r.top, hx + 140.0f, r.bottom), theme::Faint);
        }
    }

    const float ts = std::clamp(a.toastSlide.value, 0.0f, 1.0f);
    if (u.toast[0] != L'\0' && ts > 0.01f) {
        const float slideY = (1.0f - ts) * 10.0f;
        const float maxRight = std::max(r.left + 200.0f, hintsLeft - 18.0f);
        float actionW = 0.0f;
        if (!u.toastAction.IsNone()) {
            actionW = std::ceil(MeasureWidth(u.toastActionLabel, gpu_.fBodyStrong)) + 22.0f;
        }
        const float tw = std::min(std::ceil(MeasureWidth(u.toast, gpu_.fBody)) + 2.0f, maxRight - r.left - 4.0f - actionW - 10.0f);
        Text(u.toast, gpu_.fBody, Rc(r.left + 4.0f, r.top + slideY, r.left + 4.0f + tw, r.bottom + slideY), theme::Text,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, ts);
        if (!u.toastAction.IsNone()) {
            const D2D1_RECT_F b = RcW(r.left + 4.0f + tw + 10.0f, cy - 13.0f + slideY, actionW, 26.0f);
            Button(b, u.toastActionLabel, u.toastAction, BtnStyle::Secondary, gpu_.fSmall, ts);
        }
    }
}

} // namespace mask::app
