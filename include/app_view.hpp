#pragma once

// GPU-accelerated Cosmic Blue/Black Glassmorphic UI Renderer for Mask.
//
// Pipeline:
// - Backed by `mask::ui::GpuCanvas` (Direct3D 11 + DXGI Flip-Model + Direct2D 1.1 + DirectWrite
//   + live Cosmic Nebula & Starfield HLSL shader) and `mask::ui::UiAnimator`.
// - Zero global singletons: Render(const AppModel&, const FrameState&, const UiAnimator&) receives
//   all model, engine frame telemetry, and animation state explicitly per frame.

#include "app_model.hpp"
#include "ui_gpu_renderer.hpp"
#include "ui_anim.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <vector>

namespace mask::app {

// =====================================================================================
//  Cosmic Blue / Black Theme Palette (0xRRGGBB & 0xRRGGBBAA)
// =====================================================================================

namespace theme {
inline constexpr UINT32 Window   = 0x02050BU; // Deep cosmic void black
inline constexpr UINT32 Panel    = 0x07101FU; // Obsidian glass base
inline constexpr UINT32 Raised   = 0x0D1B34U; // Elevated sapphire surface
inline constexpr UINT32 RaisedHi = 0x152B50U; // Hovered sapphire surface
inline constexpr UINT32 Inset    = 0x040914U; // Deep obsidian well
inline constexpr UINT32 Line     = 0x183862U; // Crisp sapphire border
inline constexpr UINT32 LineHi   = 0x2A5EA0U; // Illuminated sapphire border
inline constexpr UINT32 Text     = 0xE8F4FFU; // Ice-blue primary text
inline constexpr UINT32 Muted    = 0x88A4C7U; // Slate blue secondary text
inline constexpr UINT32 Faint    = 0x4E698DU; // Muted cosmos tertiary text
inline constexpr UINT32 Signal   = 0x38BDF8U; // Electric cosmic cyan fallback
inline constexpr UINT32 SignalHi = 0x67E8F9U; // Luminous cyan highlight
inline constexpr UINT32 OnSignal = 0x03111EU; // Deep abyssal text on cyan
inline constexpr UINT32 Cyan     = 0x22D3EEU; // Plasma cyan
inline constexpr UINT32 IceCore  = 0xE0F7FFU; // Incandescent starlight core
inline constexpr UINT32 Violet   = 0x818CF8U; // Nebula violet
inline constexpr UINT32 Danger   = 0xF43F5EU; // Cosmic crimson alert
inline constexpr UINT32 Warn     = 0xF59E0BU; // Stellar amber warn
inline constexpr UINT32 Ok       = 0x34D399U; // Aurora emerald-mint
inline constexpr UINT32 White    = 0xFFFFFFU;

// Translucent obsidian glass surface gradients (0xRRGGBBAA)
inline constexpr UINT32 GlassTop      = 0x060D1AE0U;
inline constexpr UINT32 GlassBot      = 0x0B162CD4U;
inline constexpr UINT32 GlassInnerTop = 0x040914D2U;
inline constexpr UINT32 GlassInnerBot = 0x081326C8U;
inline constexpr UINT32 GlassBorder   = 0x183862E8U;
inline constexpr UINT32 GlassBorderHi = 0x2A5EA0F2U;
inline constexpr UINT32 GlassModalTop = 0x060F1EF2U;
inline constexpr UINT32 GlassModalBot = 0x0B1830F0U;

[[nodiscard]] constexpr COLORREF ToColorRef(UINT32 rgb) noexcept {
    return RGB((rgb >> 16) & 0xFFU, (rgb >> 8) & 0xFFU, rgb & 0xFFU);
}
} // namespace theme

// =====================================================================================
//  UiAction <-> UiAnimator integer key bridge
// =====================================================================================

[[nodiscard]] constexpr int EncodeUiActionKey(UiAction a) noexcept {
    if (a.kind == ActionKind::None) {
        return 0;
    }
    return (static_cast<int>(static_cast<uint16_t>(a.kind)) << 16) |
           static_cast<int>(static_cast<uint16_t>(a.index));
}

[[nodiscard]] constexpr UiAction DecodeUiActionKey(int key) noexcept {
    if (key == 0) {
        return UiAction{};
    }
    const uint32_t u = static_cast<uint32_t>(key);
    return UiAction{
        static_cast<ActionKind>(static_cast<uint16_t>(u >> 16)),
        static_cast<int16_t>(static_cast<uint16_t>(u & 0xFFFFU))
    };
}

// =====================================================================================
//  UI Presets & Frame Snapshot
// =====================================================================================

struct QuickOutputPreset {
    const wchar_t* label;
    uint16_t vk;
};
inline constexpr int kQuickOutputCount = 7;
inline constexpr QuickOutputPreset kQuickOutputs[kQuickOutputCount] = {
    { L"Left",        VK_LBUTTON               },
    { L"Right",       VK_RBUTTON               },
    { L"Middle",      VK_MBUTTON               },
    { L"Mouse 4",     VK_XBUTTON1              },
    { L"Mouse 5",     VK_XBUTTON2              },
    { L"Scroll up",   mask::core::kVkWheelUp   },
    { L"Scroll down", mask::core::kVkWheelDown },
};

struct TriggerColorPreset {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const wchar_t* name;
};
inline constexpr int kTriggerColorPresetCount = 6;
inline constexpr TriggerColorPreset kTriggerColorPresets[kTriggerColorPresetCount] = {
    { 255, 60,  60,  L"Red"    },
    { 245, 215, 55,  L"Yellow" },
    { 185, 85,  255, L"Purple" },
    { 52,  211, 153, L"Mint"   },
    { 56,  189, 248, L"Cyan"   },
    { 240, 249, 255, L"Ice"    },
};

struct HumanizePreset {
    const wchar_t* label;
    uint8_t        pct;
};
inline constexpr int kHumanizeCount = 5;
inline constexpr HumanizePreset kHumanizePresets[kHumanizeCount] = {
    { L"Exact",     0  },
    { L"\u00B15%",  5  },
    { L"\u00B110%", 10 },
    { L"\u00B120%", 20 },
    { L"\u00B135%", 35 },
};

struct HardwareProfilePreset {
    const wchar_t* label;
    const wchar_t* id;
};
inline constexpr int kMouseProfileCount = 4;
inline constexpr HardwareProfilePreset kMouseProfiles[kMouseProfileCount] = {
    { L"Logitech G502 HERO",      L"logitech-g502-hero" },
    { L"Razer DeathAdder V3 Pro", L"razer-deathadder-v3-pro" },
    { L"Zowie EC2-CW",            L"zowie-ec2-cw" },
    { L"G Pro X Superlight",      L"logitech-g-pro-x-superlight" },
};
inline constexpr int kKbProfileCount = 4;
inline constexpr HardwareProfilePreset kKbProfiles[kKbProfileCount] = {
    { L"Keychron Q6",             L"keychron-q6" },
    { L"Wooting 60HE",            L"wooting-60he" },
    { L"Huntsman V3 Pro",         L"razer-huntsman-v3-pro" },
    { L"Corsair K70 Pro",         L"corsair-k70-rgb-pro" },
};

/// Everything the renderer needs to know about the engines for one frame.
struct FrameState {
    uint64_t                                              firingMask{0};
    uint16_t                                              activeSeqMask{0};
    std::array<uint16_t, mask::core::kMaxComplexSequences> activeSeqStep{};
    uint8_t                                               activeRemapLayer{0};
    bool                                                  remapUnlocked{true};
    bool                                                  paused{false};
    bool                                                  windowFocused{true};
    uint8_t                                               bindTarget{0};
    uint8_t                                               safeStopReason{0};
    mask::core::TelemetrySnapshot                         snap{};
    mask::triggers::ColorTriggerPreviewSnapshot           triggerPreview{};
};

// =====================================================================================
//  Renderer
// =====================================================================================

class UiRenderer {
public:
    static constexpr float kListRowHeight        = 54.0f;
    static constexpr float kListRowHeightCompact = 42.0f;
    static constexpr float kListRowGap           = 3.0f;
    static constexpr float kSeqStepRowH          = 38.0f;

    UiRenderer() noexcept = default;
    ~UiRenderer() noexcept = default;

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    bool Initialize(HWND hwnd) noexcept;
    void Shutdown() noexcept;
    void Resize(UINT pixelW, UINT pixelH) noexcept;
    void OnResize(UINT widthPx, UINT heightPx) noexcept;
    void SetDpi(float dpi) noexcept;
    void OnDpiChanged(UINT dpi) noexcept;
    void TrimGpu() noexcept;
    void Discard() noexcept;

    [[nodiscard]] float Scale() const noexcept { return dpi_ / 96.0f; }
    [[nodiscard]] float CurrentListRowHeight() const noexcept {
        return lastCompactSidebar_ ? kListRowHeightCompact : kListRowHeight;
    }

    /// Hit-test in logical DIP coordinates.
    [[nodiscard]] UiAction HitTest(float dipX, float dipY) const noexcept;
    /// Hit-test in physical client pixels.
    [[nodiscard]] UiAction HitTestPx(int xPx, int yPx) const noexcept;

    [[nodiscard]] bool PointInList(float dipX, float dipY) const noexcept;
    [[nodiscard]] bool PointInListPx(int xPx, int yPx) const noexcept;
    [[nodiscard]] bool PointInSeqList(float dipX, float dipY) const noexcept;
    [[nodiscard]] bool PointInSeqListPx(int xPx, int yPx) const noexcept;

    [[nodiscard]] float ListMaxScroll() const noexcept { return listMaxScroll_; }
    [[nodiscard]] float ListViewHeight() const noexcept;
    [[nodiscard]] float SeqStepMaxScroll() const noexcept { return seqStepMaxScroll_; }
    [[nodiscard]] float SeqStepViewHeight() const noexcept;

    bool GetSettingsTabRect(int tabIndex, float& outX, float& outW) const noexcept;
    bool GetSettingsTabRect(int tabIndex, D2D1_RECT_F& outRect) const noexcept;
    void QuerySettingsTabRect(int tabIndex, float& outX, float& outW) const noexcept;

    bool GetSelectedSidebarRowRect(float& outY, float& outH) const noexcept;
    bool GetSelectedSidebarRowRect(D2D1_RECT_F& outRect) const noexcept;

    [[nodiscard]] double DutyFromSliderX(float dipX) const noexcept;
    [[nodiscard]] double DutyFromSliderXPx(int xPx) const noexcept;
    [[nodiscard]] int TriggerToleranceFromX(float dipX) const noexcept;
    [[nodiscard]] int TriggerToleranceFromXPx(int xPx) const noexcept;

    void Render(const AppModel& ui, const FrameState& frame, const mask::ui::UiAnimator& anim) noexcept;

private:
    struct HitBox {
        D2D1_RECT_F rect{};
        UiAction    action{};
    };

    enum class BtnStyle { Secondary, Primary, Ghost, Danger, Chip, ChipOn };

    struct Pulse {
        float   x0{0.0f};
        float   x1{0.0f};
        UINT32  color{0};
        wchar_t label[16]{};
    };

    static D2D1_RECT_F Rc(float l, float t, float r, float b) noexcept;
    static D2D1_RECT_F RcW(float x, float y, float w, float h) noexcept;
    static float Snap(float v) noexcept;

    [[nodiscard]] const AppModel& Ui() const noexcept { return *ui_; }
    [[nodiscard]] const mask::ui::UiAnimator& Anim() const noexcept { return *anim_; }

    [[nodiscard]] UINT32 ColSignal()    const noexcept { return accent_.signal; }
    [[nodiscard]] UINT32 ColSignalHi()  const noexcept { return accent_.signalHi; }
    [[nodiscard]] UINT32 ColOnSignal()  const noexcept { return accent_.onSignal; }
    [[nodiscard]] UINT32 ColSecondary() const noexcept { return accent_.secondary; }

    // Primitives (delegating to GpuCanvas)
    void Fill(const D2D1_RECT_F& r, float radius, UINT32 rgb, float alpha = 1.0f) noexcept;
    void Stroke(const D2D1_RECT_F& r, float radius, UINT32 rgb, float width = 1.0f, float alpha = 1.0f) noexcept;
    void Line(float x0, float y0, float x1, float y1, UINT32 rgb, float width = 1.0f, float alpha = 1.0f) noexcept;
    void GlowLine(float x0, float y0, float x1, float y1, UINT32 rgb, float coreW = 1.8f, float glowW = 6.5f, float alpha = 1.0f) noexcept;
    void Dot(float cx, float cy, float radius, UINT32 rgb, float alpha = 1.0f) noexcept;
    void GlassCard(const D2D1_RECT_F& r, float radius, bool inner = false, bool hoverHighlight = false,
                   UINT32 borderRgba = 0U, UINT32 glowRgb = 0U, float specAlpha = 0.22f) noexcept;
    void Text(const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, UINT32 rgb,
              DWRITE_TEXT_ALIGNMENT h = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
              float alpha = 1.0f) noexcept;
    [[nodiscard]] float MeasureWidth(const wchar_t* text, IDWriteTextFormat* fmt) const noexcept;

    void DrawMaskMark(float x, float y, float sz) noexcept;
    void ArrowRight(float x, float cy, float length, UINT32 rgb, float width = 1.4f, bool glow = false) noexcept;
    void GlyphMinus(const D2D1_RECT_F& r, UINT32 rgb) noexcept;
    void GlyphPlus(const D2D1_RECT_F& r, UINT32 rgb) noexcept;
    void Hit(const D2D1_RECT_F& r, UiAction action) noexcept;
    [[nodiscard]] bool Hovered(UiAction action) const noexcept;
    void PushClip(const D2D1_RECT_F& r) noexcept;
    void PopClip() noexcept;

    // Animated Cosmic Glass Widgets
    void Button(const D2D1_RECT_F& r, const wchar_t* label, UiAction action, BtnStyle style,
                IDWriteTextFormat* fmt = nullptr, float alpha = 1.0f) noexcept;
    void Switch(float x, float y, bool on, UiAction action, bool live = false, int animIdx = -1, float alpha = 1.0f) noexcept;
    void KeyChip(const D2D1_RECT_F& r, uint16_t vk, UiAction action, bool binding) noexcept;
    float MiniKey(float x, float cy, const wchar_t* label) noexcept;
    void NumberField(const D2D1_RECT_F& r, const MacroConfig& slot, Field f, bool steppers,
                     const wchar_t* zeroText = nullptr) noexcept;
    void GenericField(const D2D1_RECT_F& r, Field f, const wchar_t* displayText, bool steppers,
                      bool leftAligned = false, const wchar_t* placeholder = nullptr) noexcept;
    void RowLabel(float x, float y, float rowHeight, const wchar_t* label) noexcept;

    // Core Chrome & Autoclicker Editor (src/app_view.cpp)
    void DrawTopBar(float W, float margin, float h) noexcept;
    void DrawSidebar(const D2D1_RECT_F& panel) noexcept;
    void DrawSidebarEntry(const SidebarEntry& e, const D2D1_RECT_F& r, float alphaScale) noexcept;
    void DrawEditor(const D2D1_RECT_F& panel) noexcept;
    void DrawEmptyState(const D2D1_RECT_F& panel) noexcept;
    void DrawAutoclickerEditor(const D2D1_RECT_F& panel) noexcept;
    void DrawBindHint(const D2D1_RECT_F& r, bool binding, const MacroConfig& s, int idx) noexcept;
    void DrawSignalView(const D2D1_RECT_F& view, const MacroConfig& s, bool firing) noexcept;
    void Trace(const D2D1_RECT_F& lane, const Pulse* pulses, int count, float fillAlpha,
               float coreWidth, float glowWidth, bool labels) noexcept;
    void LaneLabel(const D2D1_RECT_F& view, const D2D1_RECT_F& lane, float gutterRight, const wchar_t* text, UINT32 color) noexcept;
    void Tick(float x, float top, float bottom, const wchar_t* label, float labelRight, float maxLabelWidth = 90.0f) noexcept;
    void DrawStatusBar(const D2D1_RECT_F& r) noexcept;

    // Secondary Editors & Overlays (src/app_view_editors.cpp)
    void DrawSequenceEditor(const D2D1_RECT_F& panel) noexcept;
    void DrawStepKindPopover(float W, float H) noexcept;
    void DrawRemapEditor(const D2D1_RECT_F& panel) noexcept;
    void DrawTriggerEditor(const D2D1_RECT_F& panel) noexcept;
    void DrawSettingsOverlay(const D2D1_RECT_F& panel) noexcept;
    void DrawNewPickerModal(float W, float H, float openT) noexcept;

    HWND                        hwnd_{nullptr};
    float                       dpi_{96.0f};
    UINT                        pixelW_{1160U};
    UINT                        pixelH_{760U};
    mask::ui::GpuCanvas         gpu_{};
    mask::ui::detail::ComPtr<ID2D1PathGeometry> shieldGeo_{};

    const AppModel*             ui_{nullptr};
    const mask::ui::UiAnimator* anim_{nullptr};
    FrameState                  frame_{};
    mask::ui::AccentPalette     accent_{};
    bool                        lastCompactSidebar_{false};

    std::vector<HitBox>         hitBoxes_{};
    D2D1_RECT_F                 listRect_{};
    float                       listMaxScroll_{0.0f};
    bool                        hasSelectedSidebarRow_{false};
    float                       selectedSidebarRowY_{0.0f};
    float                       selectedSidebarRowH_{0.0f};
    D2D1_RECT_F                 selectedSidebarRowRect_{};
    D2D1_RECT_F                 seqListRect_{};
    float                       seqStepMaxScroll_{0.0f};
    D2D1_RECT_F                 seqStepKindBtnRect_{};
    D2D1_RECT_F                 dutyTrack_{};
    D2D1_RECT_F                 trigTolTrack_{};
    std::array<D2D1_RECT_F, 5>  settingsTabRects_{};
};

} // namespace mask::app
