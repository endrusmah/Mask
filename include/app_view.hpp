#pragma once

// Direct2D / DirectWrite renderer for the Mask suite window.
//
// Visual language: precision test instrument meets modern pro-grade peripheral suite.
// Features dynamic AccentPalette theming, UiAnimator springs/squeezes/slides/row-collapse,
// decluttered top bar with custom Mask emblem, unified filterable sidebar, animated `+ New`
// creation picker modal, Macro oscilloscope editor, 128-step Complex Sequence editor,
// Multi-Device/Monitor Remap editor, Color Trigger summary card, and 5-tab Settings menu.

#include "app_model.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <initializer_list>
#include <vector>

namespace mask::app {

// =====================================================================================
//  Theme (0xRRGGBB) - base slate surfaces; accents adapt live from AccentPalette
// =====================================================================================

namespace theme {
inline constexpr UINT32 Window   = 0x141821;
inline constexpr UINT32 Panel    = 0x1A1F2A;
inline constexpr UINT32 Raised   = 0x232A38;
inline constexpr UINT32 RaisedHi = 0x2D3546;
inline constexpr UINT32 Inset    = 0x0F131A;
inline constexpr UINT32 Line     = 0x2A3141;
inline constexpr UINT32 LineHi   = 0x3D475B;
inline constexpr UINT32 Text     = 0xE9EBF0;
inline constexpr UINT32 Muted    = 0x9DA5B4;
inline constexpr UINT32 Faint    = 0x6B7386;
inline constexpr UINT32 Signal   = 0xF2C94C; // default Amber fallback
inline constexpr UINT32 SignalHi = 0xFFDD75;
inline constexpr UINT32 OnSignal = 0x1E1800;
inline constexpr UINT32 Cyan     = 0x5CCFE6;
inline constexpr UINT32 Violet   = 0xA882FF;
inline constexpr UINT32 Danger   = 0xF0625D;
inline constexpr UINT32 Warn     = 0xF59E4B;
inline constexpr UINT32 Ok       = 0x6DD49B;
inline constexpr UINT32 White    = 0xFFFFFF;

[[nodiscard]] constexpr COLORREF ToColorRef(UINT32 rgb) noexcept {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
} // namespace theme

// =====================================================================================
//  UI actions (hit-test ids)
// =====================================================================================

namespace act {
inline constexpr int None                  = 0;
inline constexpr int TogglePause           = 1;
inline constexpr int StopAll               = 2;
inline constexpr int OpenNewPicker         = 3;
inline constexpr int CloseNewPicker        = 4;
inline constexpr int CreateMacro           = 5;
inline constexpr int CreateSequence        = 6;
inline constexpr int CreateTrigger         = 7;
inline constexpr int CreateRemap           = 8;
inline constexpr int ToggleSettings        = 9;
inline constexpr int OutputAction          = 10;
inline constexpr int ToastAction           = 11;
inline constexpr int DutySlider            = 12;
inline constexpr int DuplicateItem         = 13;
inline constexpr int DeleteItem            = 14;
inline constexpr int ToggleEnabled         = 15;
inline constexpr int BindTrigger           = 16;
inline constexpr int BindOutput            = 17;
inline constexpr int StepAdd               = 18;
inline constexpr int StepRemove            = 19;
inline constexpr int OpenHardwareTab       = 20;
inline constexpr int CycleTopLayer         = 21;
inline constexpr int ToggleTopLock         = 22;
inline constexpr int SeqStepAdd            = 25;
inline constexpr int SeqStepInsert         = 26;
inline constexpr int SeqStepDup            = 27;
inline constexpr int SeqStepUp             = 28;
inline constexpr int SeqStepDown           = 29;
inline constexpr int SeqStepDel            = 30;
inline constexpr int SeqBindTrigger        = 31;
inline constexpr int SeqStepBindKey        = 32;
inline constexpr int SeqStepPickColor      = 33;
inline constexpr int RemapBindSource       = 34;
inline constexpr int RemapBindTarget       = 35;
inline constexpr int RemapCycleDevice      = 36;
inline constexpr int RemapRefreshDevices   = 37;
inline constexpr int RemapToggleSuppress   = 38;
inline constexpr int RemapToggleUnlockLive = 39;
inline constexpr int SetBindPauseVk        = 40;
inline constexpr int SetClearPauseVk       = 41;
inline constexpr int SetResetPauseVk       = 42;
inline constexpr int SetBindStopAllVk      = 43;
inline constexpr int SetClearStopAllVk     = 44;
inline constexpr int SetOpenDataFolder     = 45;
inline constexpr int SetResetAll           = 46;
inline constexpr int SetRefreshHw          = 47;
inline constexpr int NewPickerBackdrop     = 48;
inline constexpr int ModalSurface          = 49;
inline constexpr int SeqToggleStepKindMenu = 50;
inline constexpr int SeqCycleHumanize      = 51;
inline constexpr int TrigPickPixel         = 52;
inline constexpr int TrigLockCenter        = 53;
inline constexpr int TrigBindArmVk         = 54;
inline constexpr int TrigClearArmVk        = 55;
inline constexpr int TrigBindActionVk      = 56;
inline constexpr int TrigSliderTol         = 60;
inline constexpr int PopoverBackdrop       = 61;

inline constexpr int SidebarFilter         = 100; // + 0..4
inline constexpr int SettingsTab           = 110; // + 0..4

inline constexpr int SetToggleStartWin     = 200;
inline constexpr int SetToggleStartMin     = 201;
inline constexpr int SetToggleMinTray      = 202;
inline constexpr int SetToggleTopmost      = 203;
inline constexpr int SetToggleBounds       = 204;
inline constexpr int SetToggleConfirmDel   = 205;
inline constexpr int SetToggleCompact      = 206;
inline constexpr int SetToggleSignal       = 207;
inline constexpr int SetToggleLiveKeys     = 208;
inline constexpr int SetToggleLowRes       = 209;
inline constexpr int SetToggleTrimMem      = 210;
inline constexpr int SetTogglePanic        = 211;
inline constexpr int SetToggleSessionLock  = 212;
inline constexpr int SetToggleSuppressType = 213;
inline constexpr int SetToggleSound        = 214;
inline constexpr int SetAnimQuality        = 220; // + 0..2
inline constexpr int SetAccent             = 230; // + 0..4
inline constexpr int SetPowerMode          = 240; // + 0..2
inline constexpr int SetPollHz             = 250; // + 0..2
inline constexpr int SetSpinPolicy         = 260; // + 0..2
inline constexpr int SetProfileBank        = 270; // + 0..2

inline constexpr int SeqTriggerMode        = 300; // + 0..2
inline constexpr int SeqStepKindBase       = 320; // + 0..13
inline constexpr int SeqStepLayer          = 340; // + 0..3
inline constexpr int SeqStepQuickWait      = 350; // + 0..3
inline constexpr int SeqStepQuickScroll    = 360; // + 0..1
inline constexpr int RemapLiveLayer        = 370; // + 0..3
inline constexpr int RemapRuleLayer        = 380; // + 0..3
inline constexpr int RemapTargetLayer      = 390; // + 0..7
inline constexpr int RemapActionKind       = 400; // + 0..5
inline constexpr int RemapUnlockPolicy     = 410; // + 0..2
inline constexpr int RemapDevKind          = 420; // + 0..2
inline constexpr int RemapMonitor          = 430; // + 0..8 (0=Any, 1..8=Mon 0..7)
inline constexpr int TrigMode              = 450; // + 0..1
inline constexpr int TrigRadius            = 460; // + 0..3
inline constexpr int TrigMonitor           = 470; // + 0..7
inline constexpr int TrigArmMode           = 480; // + 0..2
inline constexpr int TrigActionKind        = 490; // + 0..3
inline constexpr int TrigQuickOut          = 500; // + 0..3
inline constexpr int TrigColorPreset       = 510; // + 0..5

inline constexpr int SelectMacro           = 1000; // + 0..63
inline constexpr int SelectSeq             = 1064; // + 0..15
inline constexpr int SelectTrigger         = 1080; // + 0..7
inline constexpr int SelectRemap           = 1088; // + 0..31
inline constexpr int ToggleMacro           = 1120; // + 0..63
inline constexpr int ToggleSeq             = 1184; // + 0..15
inline constexpr int ToggleTrigger         = 1200; // + 0..7
inline constexpr int ToggleRemap           = 1208; // + 0..31
inline constexpr int BindStep              = 1250; // + 0..7
inline constexpr int Mode                  = 1300; // + 0..4
inline constexpr int QuickOut              = 1400; // + 0..6
inline constexpr int HumanizeBase          = 1500; // + 0..3
inline constexpr int SelectMouseProfile    = 1600; // + 0..3
inline constexpr int SelectKbProfile       = 1700; // + 0..3
inline constexpr int SelectSeqStep         = 1800; // + 0..127

inline constexpr int FieldFocus            = 2000; // + Field (0..63)
inline constexpr int FieldDec              = 2100; // + Field (0..63)
inline constexpr int FieldInc              = 2200; // + Field (0..63)

[[nodiscard]] constexpr bool InRange(int a, int base, int count) noexcept { return a >= base && a < base + count; }
} // namespace act

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
    { 75,  215, 115, L"Green"  },
    { 70,  210, 235, L"Cyan"   },
    { 245, 245, 245, L"White"  },
};

struct HumanizePreset {
    const wchar_t* label;
    uint8_t        pct;
};
inline constexpr int kHumanizeCount = 4;
inline constexpr HumanizePreset kHumanizePresets[kHumanizeCount] = {
    { L"Exact",     0  },
    { L"\u00B15%",  5  },
    { L"\u00B112%", 12 },
    { L"\u00B125%", 25 },
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
    static constexpr float kListRowGap           = 2.0f;
    static constexpr float kSeqStepRowH          = 38.0f;

    bool Initialize(HWND hwnd) noexcept {
        hwnd_ = hwnd;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_)) || d2dFactory_ == nullptr) {
            return false;
        }
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dwriteFactory_))) || dwriteFactory_ == nullptr) {
            return false;
        }
        dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
        if (dpi_ <= 0.0f) dpi_ = 96.0f;
        hitBoxes_.reserve(384);
        CreateTextFormats();
        return EnsureRenderTarget();
    }

    void Shutdown() noexcept {
        DiscardDeviceResources();
        for (IDWriteTextFormat** f : AllFormats()) {
            if (*f) { (*f)->Release(); *f = nullptr; }
        }
        if (dwriteFactory_) { dwriteFactory_->Release(); dwriteFactory_ = nullptr; }
        if (d2dFactory_)    { d2dFactory_->Release();    d2dFactory_ = nullptr; }
    }

    void OnResize(UINT widthPx, UINT heightPx) noexcept {
        if (rt_ != nullptr && widthPx > 0 && heightPx > 0) {
            rt_->Resize(D2D1::SizeU(widthPx, heightPx));
        }
    }

    void OnDpiChanged(UINT dpi) noexcept {
        dpi_ = static_cast<float>(dpi);
        if (rt_ != nullptr) {
            rt_->SetDpi(dpi_, dpi_);
        }
    }

    [[nodiscard]] float Scale() const noexcept { return dpi_ / 96.0f; }

    [[nodiscard]] float CurrentListRowHeight() const noexcept {
        return g_Ui.settings.compactSidebar ? kListRowHeightCompact : kListRowHeight;
    }

    /// Hit-test in physical client pixels.
    [[nodiscard]] int HitTest(int xPx, int yPx) const noexcept {
        const float x = static_cast<float>(xPx) / Scale();
        const float y = static_cast<float>(yPx) / Scale();
        for (auto it = hitBoxes_.rbegin(); it != hitBoxes_.rend(); ++it) {
            if (x >= it->rect.left && x <= it->rect.right && y >= it->rect.top && y <= it->rect.bottom) {
                return it->action;
            }
        }
        return act::None;
    }

    [[nodiscard]] bool PointInList(int xPx, int yPx) const noexcept {
        const float x = static_cast<float>(xPx) / Scale();
        const float y = static_cast<float>(yPx) / Scale();
        return x >= listRect_.left && x <= listRect_.right && y >= listRect_.top && y <= listRect_.bottom;
    }

    [[nodiscard]] bool PointInSeqList(int xPx, int yPx) const noexcept {
        const float x = static_cast<float>(xPx) / Scale();
        const float y = static_cast<float>(yPx) / Scale();
        return x >= seqListRect_.left && x <= seqListRect_.right && y >= seqListRect_.top && y <= seqListRect_.bottom;
    }

    [[nodiscard]] float ListMaxScroll() const noexcept { return listMaxScroll_; }
    [[nodiscard]] float ListViewHeight() const noexcept { return std::max(CurrentListRowHeight(), listRect_.bottom - listRect_.top); }
    [[nodiscard]] float SeqStepMaxScroll() const noexcept { return seqStepMaxScroll_; }
    [[nodiscard]] float SeqStepViewHeight() const noexcept { return std::max(kSeqStepRowH, seqListRect_.bottom - seqListRect_.top); }

    /// Computes target X/W of a Settings tab pill in DIPs for smooth animated indicator transitions.
    void QuerySettingsTabRect(int tabIndex, float& outX, float& outW) const noexcept {
        const int idx = std::clamp(tabIndex, 0, 4);
        outX = settingsTabRects_[idx].left;
        outW = std::max(40.0f, settingsTabRects_[idx].right - settingsTabRects_[idx].left);
    }

    [[nodiscard]] double DutyFromSliderX(int xPx) const noexcept {
        const float x = static_cast<float>(xPx) / Scale();
        const float span = std::max(1.0f, dutyTrack_.right - dutyTrack_.left);
        const float t = std::clamp((x - dutyTrack_.left) / span, 0.0f, 1.0f);
        if (t >= 0.985f) {
            return 100.0;
        }
        return std::clamp(std::round(1.0 + static_cast<double>(t) * 99.0), 1.0, 99.0);
    }

    [[nodiscard]] int TriggerToleranceFromX(int xPx) const noexcept {
        const float x = static_cast<float>(xPx) / Scale();
        const float span = std::max(1.0f, trigTolTrack_.right - trigTolTrack_.left);
        const float t = std::clamp((x - trigTolTrack_.left) / span, 0.0f, 1.0f);
        return std::clamp(static_cast<int>(std::lround(t * 150.0f)), 0, 150);
    }

    void Render(const FrameState& frame) noexcept {
        if (!EnsureRenderTarget()) {
            return;
        }
        hitBoxes_.clear();
        frame_ = frame;
        accent_ = mask::ui::GetAccentPalette(g_Ui.settings.accentPreset);
        seqListRect_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);
        seqStepKindBtnRect_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);
        trigTolTrack_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);
        seqStepMaxScroll_ = 0.0f;

        rt_->BeginDraw();
        rt_->SetTransform(D2D1::Matrix3x2F::Identity());
        rt_->Clear(D2D1::ColorF(theme::Window));

        const D2D1_SIZE_F size = rt_->GetSize();
        const float W = size.width;
        const float H = size.height;

        const float openAlpha = g_Animator.AppOpenAlpha();
        const float openY     = g_Animator.AppOpenOffsetY(14.0f);
        if (openY > 0.05f) {
            rt_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, openY));
        }

        constexpr float kMargin  = 14.0f;
        constexpr float kTopBarH = 56.0f;
        constexpr float kStatusH = 40.0f;
        const float sidebarW = g_Ui.settings.compactSidebar ? 248.0f : 280.0f;

        DrawTopBar(W, kMargin, kTopBarH);
        DrawSidebar(Rc(kMargin, kTopBarH, kMargin + sidebarW, H - kStatusH));
        DrawEditor(Rc(kMargin + sidebarW + 10.0f, kTopBarH, W - kMargin, H - kStatusH));
        DrawStatusBar(Rc(kMargin, H - kStatusH, W - kMargin, H));

        rt_->SetTransform(D2D1::Matrix3x2F::Identity());

        // Step-kind selector popover overlay (Sequence editor)
        if (g_Ui.showStepKindMenu && !g_Ui.showSettings && g_Ui.selectedKind == ItemKind::Sequence && !g_Ui.showNewPicker) {
            DrawStepKindPopover(W, H);
        }

        // App-open entrance veil fade
        if (openAlpha < 0.995f) {
            Fill(Rc(0.0f, 0.0f, W, H), 0.0f, theme::Window, 1.0f - openAlpha);
        }

        // Animated "+ New" creation picker modal overlay
        const float modalT = g_Animator.newPickerOpen.value;
        if (g_Ui.showNewPicker || modalT > 0.005f) {
            DrawNewPickerModal(W, H, std::clamp(modalT, 0.0f, 1.0f));
        }

        if (rt_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
            DiscardDeviceResources();
        }
    }

private:
    struct HitBox {
        D2D1_RECT_F rect{};
        int         action{0};
    };

    enum class BtnStyle { Secondary, Primary, Ghost, Danger, Chip, ChipOn };

    struct Pulse {
        float   x0{0.0f};
        float   x1{0.0f};
        UINT32  color{0};
        wchar_t label[16]{};
    };

    static D2D1_RECT_F Rc(float l, float t, float r, float b) noexcept { return D2D1::RectF(l, t, r, b); }
    static D2D1_RECT_F RcW(float x, float y, float w, float h) noexcept { return D2D1::RectF(x, y, x + w, y + h); }
    static float Snap(float v) noexcept { return std::floor(v) + 0.5f; }

    [[nodiscard]] UINT32 ColSignal()    const noexcept { return accent_.signal; }
    [[nodiscard]] UINT32 ColSignalHi()  const noexcept { return accent_.signalHi; }
    [[nodiscard]] UINT32 ColOnSignal()  const noexcept { return accent_.onSignal; }
    [[nodiscard]] UINT32 ColSecondary() const noexcept { return accent_.secondary; }

    // Out-of-line editors & overlays implemented in include/app_view_editors.hpp
    inline void DrawComplexSequenceEditor(const D2D1_RECT_F& panel) noexcept;
    inline void DrawStepKindPopover(float W, float H) noexcept;
    inline void DrawRemapEditor(const D2D1_RECT_F& panel) noexcept;
    inline void DrawTriggerEditor(const D2D1_RECT_F& panel) noexcept;
    inline void DrawSettingsView(const D2D1_RECT_F& panel) noexcept;
    inline void DrawNewPickerModal(float W, float H, float openT) noexcept;

    // ---------------------------------------------------------------------------------
    //  Resources
    // ---------------------------------------------------------------------------------

    std::array<IDWriteTextFormat**, 10> AllFormats() noexcept {
        return { &fTitle_, &fHeading_, &fBody_, &fBodyStrong_, &fSmall_, &fLabel_, &fNum_, &fKey_, &fAxis_, &fMini_ };
    }

    bool FontExists(const wchar_t* family) noexcept {
        IDWriteFontCollection* collection = nullptr;
        if (FAILED(dwriteFactory_->GetSystemFontCollection(&collection, FALSE)) || collection == nullptr) {
            return false;
        }
        UINT32 index = 0;
        BOOL exists = FALSE;
        collection->FindFamilyName(family, &index, &exists);
        collection->Release();
        return exists != FALSE;
    }

    IDWriteTextFormat* MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                  DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL) noexcept {
        IDWriteTextFormat* fmt = nullptr;
        dwriteFactory_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, stretch, size, L"en-us", &fmt);
        if (fmt != nullptr) {
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            IDWriteInlineObject* ellipsis = nullptr;
            if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(fmt, &ellipsis)) && ellipsis != nullptr) {
                fmt->SetTrimming(&trimming, ellipsis);
                ellipsis->Release();
            }
        }
        return fmt;
    }

    void CreateTextFormats() noexcept {
        const wchar_t* ui = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        const bool hasDin = FontExists(L"Bahnschrift");
        const wchar_t* din = hasDin ? L"Bahnschrift" : ui;
        const DWRITE_FONT_STRETCH dinStretch = hasDin ? DWRITE_FONT_STRETCH_SEMI_CONDENSED : DWRITE_FONT_STRETCH_NORMAL;

        fTitle_      = MakeFormat(din, 23.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, dinStretch);
        fHeading_    = MakeFormat(din, 15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, dinStretch);
        fNum_        = MakeFormat(din, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, dinStretch);
        fKey_        = MakeFormat(din, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, dinStretch);
        fAxis_       = MakeFormat(din, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, dinStretch);
        fBody_       = MakeFormat(ui, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
        fBodyStrong_ = MakeFormat(ui, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        fLabel_      = MakeFormat(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        fSmall_      = MakeFormat(ui, 11.8f, DWRITE_FONT_WEIGHT_NORMAL);
        fMini_       = MakeFormat(ui, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    bool EnsureRenderTarget() noexcept {
        if (rt_ != nullptr && brush_ != nullptr) {
            return true;
        }
        if (rt_ != nullptr) {
            DiscardDeviceResources();
        }
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left)),
            static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top)));

        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi_, dpi_);
        if (FAILED(d2dFactory_->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_NONE), &rt_)) ||
            rt_ == nullptr) {
            return false;
        }
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
        if (FAILED(rt_->CreateSolidColorBrush(D2D1::ColorF(theme::White), &brush_)) || brush_ == nullptr) {
            DiscardDeviceResources();
            return false;
        }
        return true;
    }

    void DiscardDeviceResources() noexcept {
        if (brush_) { brush_->Release(); brush_ = nullptr; }
        if (rt_)    { rt_->Release();    rt_ = nullptr; }
    }

    // ---------------------------------------------------------------------------------
    //  Primitives
    // ---------------------------------------------------------------------------------

    void Fill(const D2D1_RECT_F& r, float radius, UINT32 rgb, float alpha = 1.0f) noexcept {
        brush_->SetColor(D2D1::ColorF(rgb, alpha));
        if (radius <= 0.0f) {
            rt_->FillRectangle(r, brush_);
        } else {
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_);
        }
    }

    void Stroke(const D2D1_RECT_F& r, float radius, UINT32 rgb, float width = 1.0f, float alpha = 1.0f) noexcept {
        const float h = width * 0.5f;
        const D2D1_RECT_F ri = Rc(r.left + h, r.top + h, r.right - h, r.bottom - h);
        brush_->SetColor(D2D1::ColorF(rgb, alpha));
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(ri, std::max(0.0f, radius - h), std::max(0.0f, radius - h)), brush_, width);
    }

    void Line(float x0, float y0, float x1, float y1, UINT32 rgb, float width = 1.0f, float alpha = 1.0f) noexcept {
        brush_->SetColor(D2D1::ColorF(rgb, alpha));
        rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_, width);
    }

    void Dot(float cx, float cy, float radius, UINT32 rgb, float alpha = 1.0f) noexcept {
        brush_->SetColor(D2D1::ColorF(rgb, alpha));
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_);
    }

    void Text(const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, UINT32 rgb,
              DWRITE_TEXT_ALIGNMENT h = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
              float alpha = 1.0f) noexcept {
        if (fmt == nullptr || text == nullptr || r.right <= r.left) return;
        fmt->SetTextAlignment(h);
        fmt->SetParagraphAlignment(v);
        brush_->SetColor(D2D1::ColorF(rgb, alpha));
        rt_->DrawText(text, static_cast<UINT32>(std::wcslen(text)), fmt, r, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    float MeasureWidth(const wchar_t* text, IDWriteTextFormat* fmt) noexcept {
        if (fmt == nullptr || text == nullptr || text[0] == L'\0') return 0.0f;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(dwriteFactory_->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)), fmt,
                                                    4096.0f, 64.0f, &layout)) || layout == nullptr) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        return m.widthIncludingTrailingWhitespace;
    }

    /// Custom crisp vector Mask shield/visor mark with glowing accent pulse trace.
    void DrawMaskMark(float x, float y, float sz) noexcept {
        const D2D1_RECT_F tile = RcW(x, y, sz, sz);
        Fill(tile, sz * 0.24f, theme::Panel);
        Stroke(tile, sz * 0.24f, ColSignal(), 1.2f, 0.72f);

        // Faceted visor band inside the shield tile
        const D2D1_RECT_F visor = Rc(x + sz * 0.16f, y + sz * 0.26f, x + sz * 0.84f, y + sz * 0.74f);
        Fill(visor, 4.0f, theme::Inset);
        Stroke(visor, 4.0f, theme::LineHi, 1.0f, 0.7f);

        // Square-wave pulse trace across the visor
        const float px[6] = {
            x + sz * 0.22f, x + sz * 0.36f, x + sz * 0.36f,
            x + sz * 0.58f, x + sz * 0.58f, x + sz * 0.78f
        };
        const float py[6] = {
            y + sz * 0.60f, y + sz * 0.60f, y + sz * 0.38f,
            y + sz * 0.38f, y + sz * 0.60f, y + sz * 0.60f
        };
        for (int i = 0; i < 5; ++i) {
            Line(px[i], py[i], px[i + 1], py[i + 1], ColSignalHi(), 1.9f);
        }
    }

    void ArrowRight(float x, float cy, float length, UINT32 rgb, float width = 1.4f) noexcept {
        Line(x, cy, x + length, cy, rgb, width);
        Line(x + length - 4.0f, cy - 3.5f, x + length, cy, rgb, width);
        Line(x + length - 4.0f, cy + 3.5f, x + length, cy, rgb, width);
    }

    void ArrowBoth(float x, float cy, float length, UINT32 rgb, float width = 1.4f) noexcept {
        ArrowRight(x, cy, length, rgb, width);
        Line(x + 4.0f, cy - 3.5f, x, cy, rgb, width);
        Line(x + 4.0f, cy + 3.5f, x, cy, rgb, width);
    }

    void GlyphMinus(const D2D1_RECT_F& r, UINT32 rgb) noexcept {
        const float cx = (r.left + r.right) * 0.5f;
        const float cy = Snap((r.top + r.bottom) * 0.5f);
        Line(cx - 4.5f, cy, cx + 4.5f, cy, rgb, 1.5f);
    }

    void GlyphPlus(const D2D1_RECT_F& r, UINT32 rgb) noexcept {
        const float cx = Snap((r.left + r.right) * 0.5f);
        const float cy = Snap((r.top + r.bottom) * 0.5f);
        Line(cx - 4.5f, cy, cx + 4.5f, cy, rgb, 1.5f);
        Line(cx, cy - 4.5f, cx, cy + 4.5f, rgb, 1.5f);
    }

    void Hit(const D2D1_RECT_F& r, int action) noexcept {
        if (action == act::None) return;
        D2D1_RECT_F clipped = r;
        if (clipActive_) {
            clipped.left   = std::max(clipped.left, clip_.left);
            clipped.top    = std::max(clipped.top, clip_.top);
            clipped.right  = std::min(clipped.right, clip_.right);
            clipped.bottom = std::min(clipped.bottom, clip_.bottom);
            if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) return;
        }
        hitBoxes_.push_back(HitBox{ clipped, action });
    }

    [[nodiscard]] bool Hovered(int action) const noexcept {
        return action != act::None && g_Ui.hoveredAction == action && !g_Ui.draggingDuty && g_Ui.draggingTriggerSlider == 0;
    }

    void PushClip(const D2D1_RECT_F& r) noexcept {
        rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        clip_ = r;
        clipActive_ = true;
    }

    void PopClip() noexcept {
        rt_->PopAxisAlignedClip();
        clipActive_ = false;
    }

    // ---------------------------------------------------------------------------------
    //  Animated Widgets
    // ---------------------------------------------------------------------------------

    void Button(const D2D1_RECT_F& r, const wchar_t* label, int action, BtnStyle style,
                IDWriteTextFormat* fmt = nullptr, float alpha = 1.0f) noexcept {
        const D2D1_RECT_F sr = g_Animator.ApplySqueeze(r, action);
        const bool hov = Hovered(action);
        const bool chip = (style == BtnStyle::Chip || style == BtnStyle::ChipOn);
        const float radius = chip ? (sr.bottom - sr.top) * 0.5f : 6.0f;
        UINT32 text = theme::Text;
        switch (style) {
            case BtnStyle::Primary:
                Fill(sr, radius, hov ? ColSignalHi() : ColSignal(), alpha);
                text = ColOnSignal();
                break;
            case BtnStyle::Ghost:
                if (hov) Fill(sr, radius, theme::Raised, alpha);
                text = hov ? theme::Text : theme::Muted;
                break;
            case BtnStyle::Danger:
                if (hov) Fill(sr, radius, theme::Danger, 0.16f * alpha);
                Stroke(sr, radius, theme::Danger, 1.0f, (hov ? 0.9f : 0.48f) * alpha);
                text = theme::Danger;
                break;
            case BtnStyle::ChipOn:
                Fill(sr, radius, ColSignal(), 0.15f * alpha);
                Stroke(sr, radius, ColSignal(), 1.0f, 0.88f * alpha);
                text = ColSignalHi();
                break;
            case BtnStyle::Chip:
                if (hov) Fill(sr, radius, theme::Raised, alpha);
                Stroke(sr, radius, hov ? theme::LineHi : theme::Line, 1.0f, alpha);
                text = hov ? theme::Text : theme::Muted;
                break;
            case BtnStyle::Secondary:
            default:
                Fill(sr, radius, hov ? theme::RaisedHi : theme::Raised, alpha);
                Stroke(sr, radius, hov ? theme::LineHi : theme::Line, 1.0f, alpha);
                break;
        }
        Text(label, fmt ? fmt : fBodyStrong_, sr, text, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alpha);
        Hit(r, action);
    }

    void Switch(float x, float y, bool on, int action, bool live = false, int animIdx = -1, float alpha = 1.0f) noexcept {
        const float slide = (animIdx >= 0 && animIdx < static_cast<int>(mask::ui::UiAnimator::kSlotCount))
            ? std::clamp(g_Animator.toggleSlide[static_cast<size_t>(animIdx)].value, 0.0f, 1.0f)
            : (on ? 1.0f : 0.0f);
        const float glow = (animIdx >= 0 && animIdx < static_cast<int>(mask::ui::UiAnimator::kSlotCount))
            ? std::clamp(g_Animator.toggleGlow[static_cast<size_t>(animIdx)].value, 0.0f, 1.0f)
            : (on ? 1.0f : 0.0f);

        const D2D1_RECT_F track = g_Animator.ApplySqueeze(RcW(x, y, 34.0f, 20.0f), action);
        const bool hov = Hovered(action);
        const UINT32 offFill = hov ? theme::RaisedHi : theme::Raised;
        const UINT32 onFill  = (live || hov) ? ColSignalHi() : ColSignal();
        const UINT32 trackCol = mask::ui::LerpRgb(offFill, onFill, slide);

        if (live || glow > 0.25f) {
            const float glowA = (live ? 0.22f : 0.10f * glow) * alpha;
            Fill(Rc(track.left - 2.0f, track.top - 2.0f, track.right + 2.0f, track.bottom + 2.0f), 12.0f, ColSignal(), glowA);
        }
        Fill(track, 10.0f, trackCol, alpha);
        if (slide < 0.85f) {
            Stroke(track, 10.0f, hov ? theme::Faint : theme::LineHi, 1.0f, (1.0f - slide) * alpha);
        }
        const float thumbX = track.left + 10.0f + slide * 14.0f;
        const float thumbY = (track.top + track.bottom) * 0.5f;
        const float thumbR = 5.0f + slide * 1.5f;
        const UINT32 thumbCol = mask::ui::LerpRgb(hov ? theme::Text : theme::Muted, ColOnSignal(), slide);
        Dot(thumbX, thumbY, thumbR, thumbCol, alpha);
        Hit(Rc(x - 6.0f, y - 8.0f, x + 40.0f, y + 28.0f), action);
    }

    void KeyChip(const D2D1_RECT_F& r, uint16_t vk, int action, bool binding) noexcept {
        const D2D1_RECT_F sr = g_Animator.ApplySqueeze(r, action);
        const bool hov = Hovered(action);
        if (binding) {
            Fill(sr, 7.0f, ColSignal(), 0.12f);
            Stroke(sr, 7.0f, ColSignal(), 1.5f);
            Text(L"Press a key\u2026", fBodyStrong_, sr, ColSignalHi(), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            const bool physHeld = g_Ui.settings.showLiveKeyIndicators &&
                                  (vk != 0U) &&
                                  (vk != mask::core::kVkWheelUp) &&
                                  (vk != mask::core::kVkWheelDown) &&
                                  (g_SharedState.physicalKeyState[vk & 0xFFU].load(std::memory_order_relaxed) != 0U);
            Fill(sr, 7.0f, 0x0B0E14);
            const D2D1_RECT_F face = Rc(sr.left, sr.top, sr.right, sr.bottom - 3.0f);
            Fill(face, 7.0f, hov ? theme::RaisedHi : theme::Raised);
            if (physHeld) {
                Fill(face, 7.0f, theme::Ok, 0.12f);
            }
            Stroke(face, 7.0f, physHeld ? theme::Ok : (hov ? theme::Faint : theme::LineHi), physHeld ? 1.5f : 1.0f);
            float textLeft = face.left + 8.0f;
            if (physHeld) {
                const float dotX = face.left + 11.0f;
                const float dotY = (face.top + face.bottom) * 0.5f;
                Dot(dotX, dotY, 6.0f, theme::Ok, 0.24f);
                Dot(dotX, dotY, 3.2f, theme::Ok);
                textLeft = face.left + 19.0f;
            }
            Text(NameOf(vk).text, fKey_, Rc(textLeft, face.top, face.right - 6.0f, face.bottom),
                 vk == 0 ? theme::Faint : (physHeld ? theme::White : theme::Text), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        Hit(r, action);
    }

    float MiniKey(float x, float cy, const wchar_t* label) noexcept {
        const float w = std::ceil(MeasureWidth(label, fMini_)) + 12.0f;
        const D2D1_RECT_F r = Rc(x, cy - 9.0f, x + w, cy + 9.0f);
        Fill(r, 4.0f, theme::Raised);
        Stroke(r, 4.0f, theme::LineHi);
        Text(label, fMini_, r, theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        return x + w;
    }

    void NumberField(const D2D1_RECT_F& r, const MacroConfig& slot, Field f, bool steppers,
                     const wchar_t* zeroText = nullptr) noexcept {
        const int fieldId = static_cast<int>(f);
        const bool editing = (g_Ui.editField == f);
        const int focusAction = act::FieldFocus + fieldId;
        const bool hov = Hovered(focusAction);

        Fill(r, 6.0f, theme::Inset);
        Stroke(r, 6.0f, editing ? ColSignal() : (hov ? theme::LineHi : theme::Line), editing ? 1.5f : 1.0f);

        D2D1_RECT_F valueRect = r;
        if (steppers) {
            const float bw = 28.0f;
            const D2D1_RECT_F dec = Rc(r.left + 2.0f, r.top + 2.0f, r.left + bw, r.bottom - 2.0f);
            const D2D1_RECT_F inc = Rc(r.right - bw, r.top + 2.0f, r.right - 2.0f, r.bottom - 2.0f);
            const int decA = act::FieldDec + fieldId;
            const int incA = act::FieldInc + fieldId;
            if (Hovered(decA)) Fill(dec, 5.0f, theme::Raised);
            if (Hovered(incA)) Fill(inc, 5.0f, theme::Raised);
            GlyphMinus(dec, Hovered(decA) ? theme::Text : theme::Faint);
            GlyphPlus(inc, Hovered(incA) ? theme::Text : theme::Faint);
            Hit(dec, decA);
            Hit(inc, incA);
            valueRect = Rc(r.left + bw, r.top, r.right - bw, r.bottom);
        }

        wchar_t text[48]{};
        if (editing) {
            wcscpy_s(text, g_Ui.editBuffer);
            const float tw = MeasureWidth(text, fNum_);
            const float cx = (valueRect.left + valueRect.right) * 0.5f;
            Text(text, fNum_, valueRect, theme::White, DWRITE_TEXT_ALIGNMENT_CENTER);
            const float caretX = Snap(cx + tw * 0.5f + 1.5f);
            Line(caretX, r.top + 8.0f, caretX, r.bottom - 8.0f, ColSignal(), 1.5f);
        } else if (zeroText != nullptr && FieldGet(slot, f) == 0.0) {
            Text(zeroText, fNum_, valueRect, ColSignalHi(), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            wchar_t num[32]{};
            FormatFieldNumber(slot, f, num, std::size(num));
            std::swprintf(text, std::size(text), L"%ls%ls", num, FieldSuffix(f));
            Text(text, fNum_, valueRect, theme::Text, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        Hit(valueRect, focusAction);
    }

    /// Reusable editable numeric/text field for Sequence Inspector, Remap, and Settings.
    void GenericField(const D2D1_RECT_F& r, Field f, const wchar_t* displayText, bool steppers,
                      bool leftAligned = false, const wchar_t* placeholder = nullptr) noexcept {
        const int fieldId = static_cast<int>(f);
        const bool editing = (g_Ui.editField == f);
        const int focusAction = act::FieldFocus + fieldId;
        const bool hov = Hovered(focusAction);

        Fill(r, 6.0f, theme::Inset);
        Stroke(r, 6.0f, editing ? ColSignal() : (hov ? theme::LineHi : theme::Line), editing ? 1.5f : 1.0f);

        D2D1_RECT_F valueRect = r;
        if (steppers) {
            const float bw = 24.0f;
            const D2D1_RECT_F dec = Rc(r.left + 2.0f, r.top + 2.0f, r.left + bw, r.bottom - 2.0f);
            const D2D1_RECT_F inc = Rc(r.right - bw, r.top + 2.0f, r.right - 2.0f, r.bottom - 2.0f);
            const int decA = act::FieldDec + fieldId;
            const int incA = act::FieldInc + fieldId;
            if (Hovered(decA)) Fill(dec, 5.0f, theme::Raised);
            if (Hovered(incA)) Fill(inc, 5.0f, theme::Raised);
            GlyphMinus(dec, Hovered(decA) ? theme::Text : theme::Faint);
            GlyphPlus(inc, Hovered(incA) ? theme::Text : theme::Faint);
            Hit(dec, decA);
            Hit(inc, incA);
            valueRect = Rc(r.left + bw, r.top, r.right - bw, r.bottom);
        }

        IDWriteTextFormat* fmt = leftAligned ? fBody_ : fNum_;
        const D2D1_RECT_F textRect = leftAligned ? Rc(valueRect.left + 8.0f, valueRect.top, valueRect.right - 8.0f, valueRect.bottom) : valueRect;
        if (editing) {
            Text(g_Ui.editBuffer, fmt, textRect, theme::White,
                 leftAligned ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
            const float tw = MeasureWidth(g_Ui.editBuffer, fmt);
            const float caretX = leftAligned
                ? Snap(std::min(textRect.right - 2.0f, textRect.left + tw + 1.5f))
                : Snap((valueRect.left + valueRect.right) * 0.5f + tw * 0.5f + 1.5f);
            Line(caretX, r.top + 6.0f, caretX, r.bottom - 6.0f, ColSignal(), 1.5f);
        } else if ((displayText == nullptr || displayText[0] == L'\0') && placeholder != nullptr) {
            Text(placeholder, fmt, textRect, theme::Faint,
                 leftAligned ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            Text(displayText ? displayText : L"", fmt, textRect, theme::Text,
                 leftAligned ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        Hit(valueRect, focusAction);
    }

    void RowLabel(float x, float y, float rowHeight, const wchar_t* label) noexcept {
        Text(label, fLabel_, RcW(x, y, 88.0f, rowHeight), theme::Muted);
    }

    // ---------------------------------------------------------------------------------
    //  Decluttered Top Bar
    // ---------------------------------------------------------------------------------

    void DrawTopBar(float W, float margin, float h) noexcept {
        const float cy = h * 0.5f;
        float leftX = margin;

        // 1. Custom crisp Mask emblem + brand wordmark
        DrawMaskMark(leftX, cy - 15.0f, 30.0f);
        leftX += 38.0f;
        Text(L"MASK", fHeading_, Rc(leftX, 0.0f, leftX + 54.0f, h), theme::Text);
        leftX += 56.0f;

        // 2. Master Macros on / Paused pill with dynamic pause hotkey
        const KeyName pauseKey = NameOf(g_Ui.settings.pauseHotkeyVk);
        const bool showPauseKey = (g_Ui.settings.pauseHotkeyVk != 0U);
        const float keyW = showPauseKey ? (std::ceil(MeasureWidth(pauseKey.text, fMini_)) + 14.0f) : 0.0f;

        const wchar_t* label = L"Macros on";
        UINT32 dot = ColSignal();
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
        const float pillW = std::max(168.0f, 44.0f + std::ceil(MeasureWidth(label, fBodyStrong_)) + (showPauseKey ? keyW + 14.0f : 14.0f));
        const D2D1_RECT_F pill = g_Animator.ApplySqueeze(Rc(leftX, cy - 16.0f, leftX + pillW, cy + 16.0f), act::TogglePause);
        const bool hov = Hovered(act::TogglePause);
        Fill(pill, 16.0f, hov ? theme::Raised : theme::Panel);
        Stroke(pill, 16.0f, hov ? theme::LineHi : theme::Line);
        Dot(pill.left + 17.0f, cy, 4.2f, dot);
        Dot(pill.left + 17.0f, cy, 8.0f, dot, 0.16f);
        Text(label, fBodyStrong_, Rc(pill.left + 31.0f, pill.top, pill.right - (showPauseKey ? keyW + 10.0f : 10.0f), pill.bottom), theme::Text);
        if (showPauseKey) {
            MiniKey(pill.right - 10.0f - keyW + 2.0f, cy, pauseKey.text);
        }
        Hit(Rc(leftX, cy - 16.0f, leftX + pillW, cy + 16.0f), act::TogglePause);
        leftX += pillW + 10.0f;

        // 3. Compact active Remap Layer & Lock badge (visible whenever any remap rule is enabled)
        if (g_Ui.AnyRemapEnabled()) {
            const D2D1_RECT_F layerPill = g_Animator.ApplySqueeze(RcW(leftX, cy - 14.0f, 86.0f, 28.0f), act::CycleTopLayer);
            const bool lHov = Hovered(act::CycleTopLayer);
            Fill(layerPill, 14.0f, lHov ? theme::Raised : theme::Panel);
            Stroke(layerPill, 14.0f, frame_.activeRemapLayer > 0 ? ColSecondary() : theme::Line);
            wchar_t lTxt[16]{};
            std::swprintf(lTxt, std::size(lTxt), L"%ls", LayerName(frame_.activeRemapLayer));
            Text(lTxt, fBodyStrong_, Rc(layerPill.left + 12.0f, layerPill.top, layerPill.left + 52.0f, layerPill.bottom),
                 frame_.activeRemapLayer > 0 ? ColSecondary() : theme::Text);
            Dot(layerPill.right - 16.0f, cy, 4.0f, frame_.remapUnlocked ? theme::Ok : theme::Warn);
            Hit(RcW(leftX, cy - 14.0f, 56.0f, 28.0f), act::CycleTopLayer);
            Hit(RcW(leftX + 56.0f, cy - 14.0f, 30.0f, 28.0f), act::ToggleTopLock);
            leftX += 94.0f;
        }

        // ---- Right side: Virtual device status dot (+ compact action ONLY when unhealthy), Stop all, Settings ----
        float right = W - margin;

        // Virtual device compact status indicator (clicking opens Settings -> Hardware)
        UINT32 stateColor = theme::Faint;
        const wchar_t* actionText = nullptr;
        switch (g_Ui.outputState) {
            case OutputState::Starting:      stateColor = theme::Faint;  break;
            case OutputState::Ready:         stateColor = theme::Ok;     break;
            case OutputState::NeedsAdmin:    stateColor = theme::Warn;   actionText = L"Restart as admin"; break;
            case OutputState::Unavailable:   stateColor = theme::Danger; actionText = L"Reconnect"; break;
            case OutputState::NotResponding: stateColor = theme::Danger; actionText = L"Reconnect"; break;
        }

        if (actionText != nullptr) {
            const float bw = std::ceil(MeasureWidth(actionText, fSmall_)) + 22.0f;
            Button(Rc(right - bw, cy - 15.0f, right, cy + 15.0f), actionText, act::OutputAction, BtnStyle::Danger, fSmall_);
            right -= bw + 8.0f;
        }

        // Clean virtual device status dot pill (opens Settings -> Hardware)
        {
            const D2D1_RECT_F dotPill = g_Animator.ApplySqueeze(Rc(right - 32.0f, cy - 15.0f, right, cy + 15.0f), act::OpenHardwareTab);
            const bool dh = Hovered(act::OpenHardwareTab);
            Fill(dotPill, 15.0f, dh ? theme::Raised : theme::Panel);
            Stroke(dotPill, 15.0f, dh ? theme::LineHi : theme::Line);
            const float dcx = (dotPill.left + dotPill.right) * 0.5f;
            Dot(dcx, cy, 8.0f, stateColor, 0.18f);
            Dot(dcx, cy, 4.2f, stateColor);
            Hit(Rc(right - 32.0f, cy - 15.0f, right, cy + 15.0f), act::OpenHardwareTab);
            right -= 32.0f + 8.0f;
        }

        // Stop all ghost button
        const float stopW = 76.0f;
        Button(Rc(right - stopW, cy - 15.0f, right, cy + 15.0f), L"Stop all", act::StopAll, BtnStyle::Ghost, fBodyStrong_);
        right -= stopW + 8.0f;

        // Compact Settings pill button
        const float setW = 86.0f;
        Button(Rc(right - setW, cy - 15.0f, right, cy + 15.0f), L"Settings", act::ToggleSettings,
               g_Ui.showSettings ? BtnStyle::ChipOn : BtnStyle::Chip, fBodyStrong_);
    }

    // ---------------------------------------------------------------------------------
    //  Organized Unified Sidebar
    // ---------------------------------------------------------------------------------

    void DrawSidebar(const D2D1_RECT_F& panel) noexcept {
        Fill(panel, 10.0f, theme::Panel);
        Stroke(panel, 10.0f, theme::Line);

        const float x = panel.left + 14.0f;
        const float right = panel.right - 12.0f;

        Text(L"Workspace", fHeading_, Rc(x, panel.top + 11.0f, right - 80.0f, panel.top + 40.0f), theme::Text);
        Button(Rc(right - 74.0f, panel.top + 11.0f, right, panel.top + 40.0f), L"+ New", act::OpenNewPicker, BtnStyle::Primary);

        // Compact filter pills: All | Mac | Seq | Trig | Map
        static constexpr const wchar_t* kFilterLabels[5] = { L"All", L"Mac", L"Seq", L"Trig", L"Map" };
        const float filterTop = panel.top + 48.0f;
        const float filterW = (right - x - 12.0f) / 5.0f;
        for (int f = 0; f < 5; ++f) {
            const float fx = x + static_cast<float>(f) * (filterW + 3.0f);
            const D2D1_RECT_F fr = RcW(fx, filterTop, filterW, 24.0f);
            const bool active = (g_Ui.sidebarFilter == f);
            Button(fr, kFilterLabels[f], act::SidebarFilter + f, active ? BtnStyle::ChipOn : BtnStyle::Chip, fMini_);
        }

        const float listTop = panel.top + 80.0f;
        Line(panel.left + 1.0f, Snap(listTop - 1.0f), panel.right - 1.0f, Snap(listTop - 1.0f), theme::Line);
        listRect_ = Rc(panel.left + 6.0f, listTop, panel.right - 6.0f, panel.bottom - 6.0f);

        std::array<SidebarEntry, 128> entries{};
        const int count = g_Ui.BuildSidebarList(entries);
        const float baseRowH = CurrentListRowHeight();

        // Compute dynamic content height accounting for animated row add/delete scales
        float contentH = 10.0f;
        for (int idx = 0; idx < count; ++idx) {
            const size_t animSlot = AnimSlotFor(entries[idx].kind, entries[idx].index);
            const float sc = std::clamp(g_Animator.rowScale[animSlot].value, 0.0f, 1.0f);
            contentH += (baseRowH + kListRowGap) * sc;
        }
        const float viewH = listRect_.bottom - listRect_.top;
        listMaxScroll_ = std::max(0.0f, contentH - viewH);
        g_Ui.listScroll = std::clamp(g_Ui.listScroll, 0.0f, listMaxScroll_);

        if (count == 0) {
            Text(L"Nothing here yet.", fBody_, Rc(x, listRect_.top + 16.0f, right, listRect_.top + 38.0f), theme::Faint);
            Text(L"Click + New to create a macro, sequence, trigger, or remap.", fSmall_,
                 Rc(x, listRect_.top + 40.0f, right, listRect_.top + 64.0f), theme::Faint);
            return;
        }

        PushClip(listRect_);
        float iy = listRect_.top + 6.0f - g_Ui.listScroll;
        for (int idx = 0; idx < count; ++idx) {
            const auto& e = entries[idx];
            const size_t animSlot = AnimSlotFor(e.kind, e.index);
            const float sc = std::clamp(g_Animator.rowScale[animSlot].value, 0.0f, 1.0f);
            const float rowH = baseRowH * sc;
            if (rowH >= 2.0f && iy + rowH >= listRect_.top && iy <= listRect_.bottom) {
                DrawSidebarEntry(e, Rc(listRect_.left, iy, listRect_.right, iy + rowH), sc);
            }
            iy += (baseRowH + kListRowGap) * sc;
        }
        PopClip();

        if (listMaxScroll_ > 0.5f) {
            const float trackH = viewH - 12.0f;
            const float thumbH = std::max(28.0f, trackH * (viewH / contentH));
            const float t = g_Ui.listScroll / listMaxScroll_;
            Fill(RcW(panel.right - 5.0f, listRect_.top + 6.0f + t * (trackH - thumbH), 3.0f, thumbH), 1.5f, theme::LineHi);
        }
    }

    void DrawSidebarEntry(const SidebarEntry& e, const D2D1_RECT_F& r, float alphaScale) noexcept {
        const bool compact = g_Ui.settings.compactSidebar;
        const bool selected = !g_Ui.showSettings && (g_Ui.selectedKind == e.kind && g_Ui.selectedIndex == e.index);
        const int animIdx = static_cast<int>(AnimSlotFor(e.kind, e.index));

        int selectA = act::None;
        int toggleA = act::None;
        const wchar_t* badge = L"M";
        UINT32 badgeCol = ColSignal();
        const wchar_t* name = L"";
        bool enabled = false;
        bool firing = false;
        wchar_t sub[96]{};
        UINT32 subCol = theme::Muted;
        UINT32 swatchCol = 0;
        bool hasSwatch = false;

        switch (e.kind) {
            case ItemKind::Macro: {
                const auto& s = g_Ui.slots[e.index];
                selectA = act::SelectMacro + e.index;
                toggleA = act::ToggleMacro + e.index;
                badge = L"M";
                badgeCol = ColSignal();
                name = s.name;
                enabled = s.enabled;
                firing = (frame_.firingMask & (1ULL << e.index)) != 0ULL;
                if (s.triggerVk == 0) {
                    wcscpy_s(sub, L"No trigger yet");
                    subCol = theme::Warn;
                } else if (static_cast<MacroMode>(s.mode) == MacroMode::Sequence) {
                    std::swprintf(sub, std::size(sub), L"%ls \u2022 %ls \u2192 %u steps",
                                  ModeName(s.mode), NameOf(s.triggerVk).text, static_cast<unsigned>(s.stepCount));
                } else {
                    std::swprintf(sub, std::size(sub), L"%ls \u2022 %ls %ls %ls",
                                  ModeName(s.mode), NameOf(s.triggerVk).text,
                                  static_cast<MacroMode>(s.mode) == MacroMode::SocdPair ? L"\u2194" : L"\u2192",
                                  NameOf(s.outputVk).text);
                }
                break;
            }
            case ItemKind::Sequence: {
                const auto& q = g_Ui.sequences[e.index];
                selectA = act::SelectSeq + e.index;
                toggleA = act::ToggleSeq + e.index;
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
                const auto& t = g_Ui.triggers[e.index];
                selectA = act::SelectTrigger + e.index;
                toggleA = act::ToggleTrigger + e.index;
                badge = L"T";
                badgeCol = theme::Ok;
                name = t.name;
                enabled = t.enabled;
                hasSwatch = true;
                swatchCol = (static_cast<UINT32>(t.r) << 16) | (static_cast<UINT32>(t.g) << 8) | static_cast<UINT32>(t.b);
                const wchar_t* modeLbl = (t.mode == static_cast<uint8_t>(ColorTriggerMode::ColorChanged))
                    ? L"On change" : L"Color match";
                if (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::ClickKey) ||
                    t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::HoldKey)) {
                    std::swprintf(sub, std::size(sub), L"%ls \u2192 %ls", modeLbl, NameOf(t.actionVk).text);
                } else if (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot)) {
                    std::swprintf(sub, std::size(sub), L"%ls \u2192 Macro #%u", modeLbl, static_cast<unsigned>(t.linkedIndex + 1U));
                } else {
                    std::swprintf(sub, std::size(sub), L"%ls \u2192 Sequence #%u", modeLbl, static_cast<unsigned>(t.linkedIndex + 1U));
                }
                break;
            }
            case ItemKind::Remap: {
                const auto& rm = g_Ui.remaps[e.index];
                selectA = act::SelectRemap + e.index;
                toggleA = act::ToggleRemap + e.index;
                badge = L"R";
                badgeCol = theme::Violet;
                name = rm.name;
                enabled = rm.enabled;
                if (rm.sourceVk == 0) {
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
        if (selected) {
            Fill(r, 7.0f, theme::Raised, alphaScale);
            Fill(Rc(r.left, r.top + 10.0f * alphaScale, r.left + 3.0f, r.bottom - 10.0f * alphaScale), 1.5f, badgeCol, alphaScale);
        } else if (hov) {
            Fill(r, 7.0f, theme::Raised, 0.55f * alphaScale);
        }
        Hit(r, selectA);

        const float cy = (r.top + r.bottom) * 0.5f;
        const float badgeX = r.left + 10.0f;
        const D2D1_RECT_F badgeR = RcW(badgeX, cy - 9.0f, 18.0f, 18.0f);
        Fill(badgeR, 5.0f, badgeCol, 0.16f * alphaScale);
        Stroke(badgeR, 5.0f, badgeCol, 1.0f, 0.65f * alphaScale);
        Text(badge, fMini_, badgeR, badgeCol, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);

        float textLeft = badgeR.right + 8.0f;
        const float textRight = r.right - 50.0f;

        if (firing) {
            Dot(textLeft + 4.0f, compact ? cy : r.top + 17.0f, 3.8f, ColSignalHi(), alphaScale);
            Dot(textLeft + 4.0f, compact ? cy : r.top + 17.0f, 7.2f, ColSignalHi(), 0.22f * alphaScale);
            textLeft += 14.0f;
        } else if (hasSwatch) {
            Dot(textLeft + 4.0f, compact ? cy : r.top + 17.0f, 4.5f, swatchCol, alphaScale);
            textLeft += 13.0f;
        }

        if (compact) {
            Text(name, fBodyStrong_, Rc(textLeft, r.top + 4.0f, textRight, r.bottom - 4.0f),
                 enabled ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);
        } else {
            Text(name, fBodyStrong_, Rc(textLeft, r.top + 6.0f, textRight, r.top + 28.0f),
                 enabled ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);
            Text(sub, fSmall_, Rc(badgeR.right + 8.0f, r.top + 27.0f, textRight, r.bottom - 5.0f),
                 subCol, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, alphaScale);
        }

        Switch(r.right - 42.0f, cy - 10.0f, enabled, toggleA, firing, animIdx, alphaScale);
    }

    // ---------------------------------------------------------------------------------
    //  Main Editor Router
    // ---------------------------------------------------------------------------------

    void DrawEditor(const D2D1_RECT_F& panel) noexcept {
        Fill(panel, 10.0f, theme::Panel);
        Stroke(panel, 10.0f, theme::Line);
        dutyTrack_ = Rc(0.0f, 0.0f, 0.0f, 0.0f);

        const float vt = std::clamp(g_Animator.viewTransition.value, 0.0f, 1.0f);
        const float slideY = (1.0f - vt) * 10.0f;
        const D2D1_RECT_F viewPanel = Rc(panel.left, panel.top + slideY, panel.right, panel.bottom + slideY);

        PushClip(Rc(panel.left + 1.0f, panel.top + 1.0f, panel.right - 1.0f, panel.bottom - 1.0f));
        if (g_Ui.showSettings) {
            DrawSettingsView(viewPanel);
        } else if (!g_Ui.HasSelection()) {
            DrawEmptyState(viewPanel);
        } else {
            switch (g_Ui.selectedKind) {
                case ItemKind::Macro:    DrawMacroEditor(viewPanel); break;
                case ItemKind::Sequence: DrawComplexSequenceEditor(viewPanel); break;
                case ItemKind::Trigger:  DrawTriggerEditor(viewPanel); break;
                case ItemKind::Remap:    DrawRemapEditor(viewPanel); break;
            }
        }
        PopClip();
    }

    void DrawEmptyState(const D2D1_RECT_F& panel) noexcept {
        const float cx = (panel.left + panel.right) * 0.5f;
        const float cy = (panel.top + panel.bottom) * 0.5f - 30.0f;
        Text(L"Your workspace is ready", fTitle_, Rc(panel.left, cy - 34.0f, panel.right, cy), theme::Text, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"Create a macro, 128-step complex sequence, pixel color trigger, or multi-device remap.", fBody_,
             Rc(panel.left + 24.0f, cy + 4.0f, panel.right - 24.0f, cy + 28.0f), theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        Button(Rc(cx - 76.0f, cy + 44.0f, cx + 76.0f, cy + 78.0f), L"+ New item", act::OpenNewPicker, BtnStyle::Primary);
    }

    // ---------------------------------------------------------------------------------
    //  Macro Editor (with Bahnschrift Signal Oscilloscope)
    // ---------------------------------------------------------------------------------

    void DrawMacroEditor(const D2D1_RECT_F& panel) noexcept {
        const int idx = g_Ui.selectedIndex;
        const auto& s = g_Ui.slots[idx];
        const auto mode = static_cast<MacroMode>(s.mode);
        const bool firing = (frame_.firingMask & (1ULL << idx)) != 0ULL;
        const uint8_t bindTarget = frame_.bindTarget;
        const int animIdx = static_cast<int>(AnimSlotFor(ItemKind::Macro, idx));

        const float L = panel.left + 24.0f;
        const float R = panel.right - 24.0f;
        const float labelW = 96.0f;
        const float C = L + labelW;
        float y = panel.top + 14.0f;

        // Header: inline rename | on/off | duplicate | delete
        const D2D1_RECT_F delBtn = Rc(R - 80.0f, y + 3.0f, R, y + 33.0f);
        Button(delBtn, g_Ui.pendingDeleteConfirm ? L"Confirm?" : L"Delete", act::DeleteItem,
               g_Ui.pendingDeleteConfirm ? BtnStyle::Danger : BtnStyle::Ghost);

        const D2D1_RECT_F dupBtn = Rc(delBtn.left - 8.0f - 84.0f, y + 3.0f, delBtn.left - 8.0f, y + 33.0f);
        Button(dupBtn, L"Duplicate", act::DuplicateItem, BtnStyle::Ghost);

        const float switchX = dupBtn.left - 16.0f - 34.0f;
        Switch(switchX, y + 8.0f, s.enabled, act::ToggleEnabled, firing, animIdx);
        const wchar_t* stateWord = firing ? L"Running" : (s.enabled ? L"On" : L"Off");
        Text(stateWord, fBody_, Rc(switchX - 72.0f, y, switchX - 10.0f, y + 36.0f),
             firing ? ColSignalHi() : theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Hit(Rc(switchX - 72.0f, y, switchX, y + 36.0f), act::ToggleEnabled);

        const D2D1_RECT_F nameRect = Rc(L - 8.0f, y, switchX - 82.0f, y + 36.0f);
        const int nameA = act::FieldFocus + static_cast<int>(Field::Name);
        if (g_Ui.editField == Field::Name) {
            Fill(nameRect, 6.0f, theme::Inset);
            Stroke(nameRect, 6.0f, ColSignal(), 1.5f);
            Text(g_Ui.editBuffer, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::White);
            const float caretX = Snap(L + MeasureWidth(g_Ui.editBuffer, fTitle_) + 1.5f);
            Line(caretX, y + 8.0f, caretX, y + 28.0f, ColSignal(), 1.5f);
        } else {
            if (Hovered(nameA)) Fill(nameRect, 6.0f, theme::Raised);
            Text(s.name, fTitle_, Rc(L, y, nameRect.right - 8.0f, y + 36.0f), theme::Text);
        }
        Hit(nameRect, nameA);

        wchar_t sentence[200]{};
        DescribeMacro(s, sentence, std::size(sentence));
        Text(sentence, fBody_, Rc(L, y + 36.0f, R, y + 56.0f), s.triggerVk == 0 ? theme::Warn : theme::Muted);
        y += 60.0f;

        if (g_Ui.settings.showSignalView) {
            DrawSignalView(Rc(L, y, R, y + 124.0f), s, firing);
            y += 124.0f + 14.0f;
        }

        constexpr float kRowH = 34.0f;
        constexpr float kRowGap = 10.0f;

        if (mode == MacroMode::SocdPair) {
            RowLabel(L, y, kRowH, L"Keys");
            const D2D1_RECT_F a = RcW(C, y, 150.0f, kRowH);
            const D2D1_RECT_F b = RcW(C + 190.0f, y, 150.0f, kRowH);
            KeyChip(a, s.triggerVk, act::BindTrigger, bindTarget == 1U);
            ArrowBoth(a.right + 11.0f, Snap(y + kRowH * 0.5f - 1.0f), 18.0f, theme::Faint);
            KeyChip(b, s.outputVk, act::BindOutput, bindTarget == 2U);
            DrawBindHint(Rc(b.right + 16.0f, y, R, y + kRowH), bindTarget == 1U || bindTarget == 2U, s, idx);
        } else {
            RowLabel(L, y, kRowH, L"Trigger");
            const D2D1_RECT_F chip = RcW(C, y, 170.0f, kRowH);
            KeyChip(chip, s.triggerVk, act::BindTrigger, bindTarget == 1U);
            DrawBindHint(Rc(chip.right + 16.0f, y, R, y + kRowH), bindTarget == 1U, s, idx);
        }
        y += kRowH + kRowGap;

        // Mode segmented control
        RowLabel(L, y, kRowH, L"Mode");
        const bool showTimes = (mode == MacroMode::RepeatNTimes);
        const float segW = std::min(R - C - (showTimes ? 136.0f : 0.0f), 430.0f);
        const D2D1_RECT_F seg = RcW(C, y, segW, kRowH);
        Fill(seg, 8.0f, theme::Inset);
        Stroke(seg, 8.0f, theme::Line);
        const float cellW = (segW - 6.0f) / static_cast<float>(kModeCount);
        for (int m = 0; m < kModeCount; ++m) {
            const D2D1_RECT_F cell = RcW(seg.left + 3.0f + cellW * static_cast<float>(m), seg.top + 3.0f, cellW, kRowH - 6.0f);
            const bool sel = (s.mode == m);
            const int a = act::Mode + m;
            if (sel) {
                Fill(cell, 6.0f, theme::RaisedHi);
                Fill(Rc(cell.left + 14.0f, cell.bottom - 2.5f, cell.right - 14.0f, cell.bottom - 0.5f), 1.0f, ColSignal());
            } else if (Hovered(a)) {
                Fill(cell, 6.0f, theme::Raised);
            }
            Text(ModeName(m), sel ? fBodyStrong_ : fBody_, cell,
                 (sel || Hovered(a)) ? theme::Text : theme::Muted, DWRITE_TEXT_ALIGNMENT_CENTER);
            Hit(cell, a);
        }
        if (showTimes) {
            NumberField(RcW(seg.right + 14.0f, y + 1.0f, 122.0f, kRowH - 2.0f), s, Field::Repeat, true);
        }
        Text(ModeDescription(s.mode), fSmall_, Rc(C, y + kRowH + 4.0f, R, y + kRowH + 20.0f), theme::Faint);
        y += kRowH + 20.0f + kRowGap;

        if (IsTimedMode(s.mode)) {
            DrawTimedSettings(L, C, R, y, s, bindTarget);
        } else if (mode == MacroMode::Sequence) {
            DrawSequenceSettings(L, R, y, panel.bottom - 8.0f, s, bindTarget);
        }
    }

    void DrawBindHint(const D2D1_RECT_F& r, bool binding, const MacroConfig& s, int idx) noexcept {
        if (binding) {
            Text(L"Press any key or mouse button. Esc cancels.", fSmall_, r, ColSignalHi());
            return;
        }
        if (g_Ui.settings.pauseHotkeyVk != 0U && s.triggerVk == g_Ui.settings.pauseHotkeyVk) {
            wchar_t msg[96]{};
            std::swprintf(msg, std::size(msg), L"%ls is the pause shortcut, so it can't start a macro.",
                          NameOf(g_Ui.settings.pauseHotkeyVk).text);
            Text(msg, fSmall_, r, theme::Warn);
            return;
        }
        if (s.triggerVk != 0) {
            for (int j = 0; j < kSlotCount; ++j) {
                if (j == idx || !g_Ui.slots[j].enabled) continue;
                if (g_Ui.slots[j].triggerVk == s.triggerVk) {
                    wchar_t msg[96]{};
                    std::swprintf(msg, std::size(msg), L"Also starts \u201C%ls\u201D", g_Ui.slots[j].name);
                    Text(msg, fSmall_, r, theme::Warn);
                    return;
                }
            }
        }
        if (s.triggerVk == 0) {
            Text(L"Click to bind", fSmall_, r, theme::Faint);
        }
    }

    void DrawTimedSettings(float L, float C, float R, float y, const MacroConfig& s, uint8_t bindTarget) noexcept {
        constexpr float kRowH = 34.0f;
        constexpr float kRowGap = 10.0f;

        RowLabel(L, y, kRowH, L"Output");
        const D2D1_RECT_F chip = RcW(C, y, 144.0f, kRowH);
        KeyChip(chip, s.outputVk, act::BindOutput, bindTarget == 2U);
        const float qStart = chip.right + 12.0f;
        float qx = qStart;
        float qy = y + 4.0f;
        bool wrapped = false;
        for (int i = 0; i < kQuickOutputCount; ++i) {
            const float qw = std::ceil(MeasureWidth(kQuickOutputs[i].label, fSmall_)) + 16.0f;
            if (qx > qStart && qx + qw > R) {
                qx = qStart;
                qy += 30.0f;
                wrapped = true;
            }
            Button(RcW(qx, qy, qw, 26.0f), kQuickOutputs[i].label, act::QuickOut + i,
                   s.outputVk == kQuickOutputs[i].vk ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            qx += qw + 5.0f;
        }
        y += (wrapped ? 60.0f : kRowH) + kRowGap;

        RowLabel(L, y, kRowH, L"Speed");
        NumberField(RcW(C, y, 150.0f, kRowH), s, Field::Rate, true);
        float fx = C + 150.0f + 22.0f;
        NumberField(RcW(fx, y, 90.0f, kRowH), s, Field::Down, false);
        Text(L"down", fSmall_, RcW(fx + 95.0f, y, 40.0f, kRowH), theme::Faint);
        fx += 95.0f + 42.0f;
        NumberField(RcW(fx, y, 90.0f, kRowH), s, Field::Up, false, L"never");
        Text(L"up", fSmall_, RcW(fx + 95.0f, y, 30.0f, kRowH), theme::Faint);
        y += kRowH + kRowGap;

        RowLabel(L, y, kRowH, L"Duty cycle");
        const float sliderW = std::min(280.0f, R - C - 146.0f);
        const float trackY = Snap(y + kRowH * 0.5f);
        dutyTrack_ = Rc(C + 8.0f, trackY - 12.0f, C + sliderW - 8.0f, trackY + 12.0f);
        const bool full = IsFullDuty(s);
        const float frac = full ? 1.0f : static_cast<float>(std::clamp((DutyPercent(s) - 1.0) / 99.0, 0.0, 0.985));
        const float thumbX = dutyTrack_.left + frac * (dutyTrack_.right - dutyTrack_.left);
        const bool sliderHot = g_Ui.draggingDuty || (g_Ui.hoveredAction == act::DutySlider);
        Fill(Rc(dutyTrack_.left, trackY - 3.0f, dutyTrack_.right, trackY + 3.0f), 3.0f, theme::Inset);
        Stroke(Rc(dutyTrack_.left, trackY - 3.0f, dutyTrack_.right, trackY + 3.0f), 3.0f, theme::Line);
        Fill(Rc(dutyTrack_.left, trackY - 3.0f, thumbX, trackY + 3.0f), 3.0f, full ? ColSignalHi() : ColSignal());
        Line(Snap(dutyTrack_.right), trackY - 8.0f, Snap(dutyTrack_.right), trackY + 8.0f, full ? ColSignalHi() : theme::LineHi, 2.0f);
        if (sliderHot) Dot(thumbX, trackY, 13.0f, ColSignal(), 0.18f);
        Dot(thumbX, trackY, 8.0f, full ? ColSignalHi() : ColSignal());
        Dot(thumbX, trackY, 3.0f, ColOnSignal());
        Hit(Rc(C, y, C + sliderW, y + kRowH), act::DutySlider);

        NumberField(RcW(C + sliderW + 14.0f, y, 124.0f, kRowH), s, Field::Duty, true);
        if (full) {
            Text(L"Held continuously until stopped.", fSmall_,
                 Rc(C, y + kRowH + 3.0f, R, y + kRowH + 20.0f), ColSignalHi());
        } else {
            Text(L"Active time per cycle (100% holds continuously).", fSmall_,
                 Rc(C, y + kRowH + 3.0f, R, y + kRowH + 20.0f), theme::Faint);
        }
        y += kRowH + 20.0f + kRowGap;

        RowLabel(L, y, kRowH, L"Move pointer");
        NumberField(RcW(C, y, 120.0f, kRowH), s, Field::MoveX, true);
        Text(L"right", fSmall_, RcW(C + 126.0f, y, 36.0f, kRowH), theme::Faint);
        NumberField(RcW(C + 168.0f, y, 120.0f, kRowH), s, Field::MoveY, true);
        Text(full ? L"down, every cycle" : L"down, with every press", fSmall_, RcW(C + 294.0f, y, R - C - 294.0f, kRowH), theme::Faint);
        y += kRowH + kRowGap;

        RowLabel(L, y, kRowH, L"Timing");
        const uint8_t curHum = mask::core::HumanizePercent(s);
        float hx = C;
        for (int i = 0; i < kHumanizeCount; ++i) {
            const float hw = std::ceil(MeasureWidth(kHumanizePresets[i].label, fSmall_)) + 20.0f;
            Button(RcW(hx, y + 4.0f, hw, 26.0f), kHumanizePresets[i].label, act::HumanizeBase + i,
                   curHum == kHumanizePresets[i].pct ? BtnStyle::ChipOn : BtnStyle::Chip, fSmall_);
            hx += hw + 6.0f;
        }
        if (hx + 120.0f < R) {
            const wchar_t* humHint = (curHum == 0U)
                ? L"Locked to exact hardware interval"
                : (full && static_cast<MacroMode>(s.mode) != MacroMode::RepeatNTimes
                       ? L"Variance applies when duty < 100%"
                       : L"Varies each press and release interval");
            Text(humHint, fSmall_, Rc(hx + 10.0f, y, R, y + kRowH), theme::Faint);
        }
    }

    void DrawSequenceSettings(float L, float R, float y, float bottom, const MacroConfig& s, uint8_t bindTarget) noexcept {
        const float inner = R - L;
        Text(L"Steps", fLabel_, RcW(L, y, 88.0f, 28.0f), theme::Muted);

        const bool canAdd = s.stepCount < kStepCount;
        const bool canRemove = s.stepCount > 1;
        float bx = R;
        if (canAdd) {
            Button(Rc(bx - 92.0f, y, bx, y + 28.0f), L"Add step", act::StepAdd, BtnStyle::Secondary, fSmall_);
            bx -= 98.0f;
        }
        if (canRemove) {
            Button(Rc(bx - 100.0f, y, bx, y + 28.0f), L"Remove last", act::StepRemove, BtnStyle::Ghost, fSmall_);
        }
        y += 34.0f;

        const float colGap = 28.0f;
        const float colW = (inner - colGap) * 0.5f;
        const float numW = 22.0f;
        const float rest = colW - numW - 8.0f - 16.0f;
        const float keyW = rest * 0.38f;
        const float fieldW = rest * 0.31f;

        const int columns = (s.stepCount > 4) ? 2 : 1;
        for (int c = 0; c < columns; ++c) {
            const float cx = L + static_cast<float>(c) * (colW + colGap) + numW + 8.0f;
            Text(L"Key", fSmall_, RcW(cx, y, keyW, 16.0f), theme::Faint);
            Text(L"Hold for", fSmall_, RcW(cx + keyW + 8.0f, y, fieldW, 16.0f), theme::Faint);
            Text(L"Then wait", fSmall_, RcW(cx + keyW + 16.0f + fieldW, y, fieldW, 16.0f), theme::Faint);
        }
        y += 20.0f;

        for (int st = 0; st < static_cast<int>(s.stepCount); ++st) {
            const int col = st / 4;
            const int row = st % 4;
            const float cx = L + static_cast<float>(col) * (colW + colGap);
            const float ry = y + static_cast<float>(row) * 38.0f;

            wchar_t num[8]{};
            std::swprintf(num, std::size(num), L"%d", st + 1);
            Text(num, fNum_, RcW(cx, ry, numW, 34.0f), theme::Faint, DWRITE_TEXT_ALIGNMENT_CENTER);

            float x = cx + numW + 8.0f;
            KeyChip(RcW(x, ry, keyW, 34.0f), s.steps[st].outputVk, act::BindStep + st, bindTarget == static_cast<uint8_t>(3 + st));
            x += keyW + 8.0f;
            NumberField(RcW(x, ry, fieldW, 34.0f), s, StepDownField(st), false);
            x += fieldW + 8.0f;
            NumberField(RcW(x, ry, fieldW, 34.0f), s, StepUpField(st), false, L"no gap");
        }

        const float hintY = y + 4.0f * 38.0f + 4.0f;
        if (hintY + 18.0f <= bottom) {
            Text(L"Scroll or click numbers to edit.",
                 fSmall_, Rc(L, hintY, R, hintY + 18.0f), theme::Faint);
        }
    }

    // ---------------------------------------------------------------------------------
    //  Signal view (dynamic accent traces)
    // ---------------------------------------------------------------------------------

    void Trace(const D2D1_RECT_F& lane, const Pulse* pulses, int count, float fillAlpha, float width, bool labels) noexcept {
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

            Fill(Rc(x0, top, x1, base), 0.0f, p.color, fillAlpha);
            if (!joinsPrev) {
                Line(cursor, base, x0, base, lowColor, width, 0.55f);
                Line(x0, base, x0, top, p.color, width);
            } else {
                Line(x0, top + 4.0f, x0, base - 3.0f, theme::Inset, 1.5f);
            }
            Line(x0, top, x1, top, p.color, width);
            if (!joinsNext) {
                Line(x1, top, x1, base, p.color, width);
            }
            cursor = x1;

            if (labels && p.label[0] != L'\0') {
                const float tw = MeasureWidth(p.label, fAxis_);
                if (tw + 8.0f <= (x1 - x0)) {
                    Text(p.label, fAxis_, Rc(x0, top, x1, base), theme::Text, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
        }
        if (cursor < lane.right) {
            Line(cursor, base, lane.right, base, lowColor, width, 0.55f);
        }
    }

    void LaneLabel(const D2D1_RECT_F& view, const D2D1_RECT_F& lane, float gutterRight, const wchar_t* text, UINT32 color) noexcept {
        Text(text, fSmall_, Rc(view.left + 14.0f, lane.top - 4.0f, gutterRight - 10.0f, lane.bottom + 4.0f), color,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    void Tick(float x, float top, float bottom, const wchar_t* label, float labelRight, float maxLabelWidth = 90.0f) noexcept {
        const float sx = Snap(x);
        Line(sx, top, sx, bottom, theme::Line, 1.0f);
        if (label != nullptr && label[0] != L'\0') {
            Text(label, fAxis_, Rc(sx - 3.0f, bottom + 3.0f, std::min(sx + maxLabelWidth, labelRight), bottom + 19.0f), theme::Faint);
        }
    }

    void DrawSignalView(const D2D1_RECT_F& view, const MacroConfig& s, bool firing) noexcept {
        Fill(view, 8.0f, theme::Inset);
        Stroke(view, 8.0f, theme::Line);

        const auto mode = static_cast<MacroMode>(s.mode);
        const UINT32 ch1 = !s.enabled ? 0x7C8597 : (firing ? ColSignalHi() : ColSignal());
        const UINT32 ch1Dim = !s.enabled ? 0x5A6272 : mask::ui::LerpRgb(theme::Inset, ColSignal(), 0.55f);
        const UINT32 ch2 = !s.enabled ? 0x5F6879 : ColSecondary();
        const UINT32 trig = 0x8B93A4;
        const float outAlpha = !s.enabled ? 0.10f : (firing ? 0.34f : 0.16f);
        const float outWidth = firing ? 2.0f : 1.5f;

        const float gutter = view.left + 104.0f;
        const float plotL = gutter;
        const float plotR = view.right - 18.0f;
        const float plotW = std::max(40.0f, plotR - plotL);

        PushClip(Rc(view.left + 1.0f, view.top + 1.0f, view.right - 1.0f, view.bottom - 1.0f));

        if (mode == MacroMode::SocdPair) {
            const D2D1_RECT_F laneA = Rc(plotL, view.top + 12.0f, plotR, view.top + 30.0f);
            const D2D1_RECT_F laneB = Rc(plotL, view.top + 40.0f, plotR, view.top + 58.0f);
            const D2D1_RECT_F laneS = Rc(plotL, view.top + 74.0f, plotR, view.top + 104.0f);
            const float u = plotW / 10.0f;
            const KeyName a = NameOf(s.triggerVk);
            const KeyName b = NameOf(s.outputVk);

            for (float t : { 1.0f, 3.6f, 6.2f, 8.6f }) {
                Tick(plotL + t * u, view.top + 8.0f, view.top + 112.0f, nullptr, plotR);
            }
            Pulse pa{ plotL + 1.0f * u, plotL + 8.6f * u, trig, {} };
            Pulse pb{ plotL + 3.6f * u, plotL + 6.2f * u, trig, {} };
            Trace(laneA, &pa, 1, 0.10f, 1.25f, false);
            Trace(laneB, &pb, 1, 0.10f, 1.25f, false);

            Pulse sent[3]{};
            sent[0] = Pulse{ plotL + 1.0f * u, plotL + 3.6f * u, ch1, {} };
            sent[1] = Pulse{ plotL + 3.6f * u, plotL + 6.2f * u, ch2, {} };
            sent[2] = Pulse{ plotL + 6.2f * u, plotL + 8.6f * u, ch1, {} };
            wcsncpy_s(sent[0].label, a.text, _TRUNCATE);
            wcsncpy_s(sent[1].label, b.text, _TRUNCATE);
            wcsncpy_s(sent[2].label, a.text, _TRUNCATE);
            Trace(laneS, sent, 3, outAlpha + 0.08f, outWidth, true);

            wchar_t la[40]{}, lb[40]{};
            std::swprintf(la, std::size(la), L"%ls held", a.text);
            std::swprintf(lb, std::size(lb), L"%ls held", b.text);
            LaneLabel(view, laneA, gutter, la, theme::Faint);
            LaneLabel(view, laneB, gutter, lb, theme::Faint);
            LaneLabel(view, laneS, gutter, L"Sent", theme::Muted);
            PopClip();
            return;
        }

        const D2D1_RECT_F laneT = Rc(plotL, view.top + 14.0f, plotR, view.top + 34.0f);
        const D2D1_RECT_F laneO = Rc(plotL, view.top + 50.0f, plotR, view.top + 90.0f);
        const float tickTop = view.top + 10.0f;
        const float tickBottom = view.top + 96.0f;

        LaneLabel(view, laneT, gutter, s.triggerVk != 0 ? NameOf(s.triggerVk).text : L"Trigger", theme::Faint);

        if (mode == MacroMode::Sequence) {
            const int count = std::clamp<int>(s.stepCount, 1, kStepCount);
            const double cycleMs = std::max(SequenceCycleMs(s), 0.1);
            const float pxPerMs = plotW / static_cast<float>(cycleMs * 1.30);
            const float x0 = plotL + 12.0f;

            wchar_t cycleText[48]{}, dur[32]{};
            FormatDuration(cycleMs, dur, std::size(dur));
            std::swprintf(cycleText, std::size(cycleText), L"%ls, then repeats", dur);
            Tick(x0, tickTop, tickBottom, L"0", plotR);
            Tick(x0 + static_cast<float>(cycleMs) * pxPerMs, tickTop, tickBottom, cycleText, plotR + 14.0f, 200.0f);

            Pulse tap{ x0, x0 + 9.0f, trig, {} };
            Trace(laneT, &tap, 1, 0.12f, 1.25f, false);

            Pulse pulses[kStepCount * 2]{};
            int n = 0;
            double t = 0.0;
            for (int rep = 0; rep < 2; ++rep) {
                for (int i = 0; i < count; ++i) {
                    const double down = static_cast<double>(s.steps[i].dwellDurationNs) / 1e6;
                    const double gap = static_cast<double>(s.steps[i].recoveryDurationNs) / 1e6;
                    Pulse& p = pulses[n++];
                    p.x0 = x0 + static_cast<float>(t) * pxPerMs;
                    p.x1 = std::max(p.x0 + 2.0f, x0 + static_cast<float>(t + down) * pxPerMs);
                    p.color = (rep == 0) ? ch1 : ch1Dim;
                    wcsncpy_s(p.label, NameOf(s.steps[i].outputVk).text, _TRUNCATE);
                    t += down + gap;
                }
            }
            Trace(laneO, pulses, n, outAlpha, outWidth, true);
            LaneLabel(view, laneO, gutter, L"Keys sent", theme::Muted);
            PopClip();
            return;
        }

        const double downMs = DownMs(s);
        const double periodMs = std::max(PeriodMs(s), kMinPhaseMs);
        const bool full = IsFullDuty(s);
        const float pxPerCycle = plotW / 5.6f;
        const float x0 = plotL + 0.32f * pxPerCycle;
        const float dutyFrac = full ? 1.0f : static_cast<float>(downMs / periodMs);
        const float stopAt = 4.25f;
        auto X = [&](float cycles) noexcept { return x0 + cycles * pxPerCycle; };

        const bool seconds = periodMs * 5.0 >= 1000.0;
        for (int k = 0; k <= 5; ++k) {
            wchar_t label[40]{}, num[24]{};
            const double tMs = periodMs * k;
            if (seconds) FormatNumber(tMs / 1000.0, 2, num, std::size(num));
            else         FormatMs(tMs, num, std::size(num));
            if (k == 5) std::swprintf(label, std::size(label), L"%ls %ls", num, seconds ? L"s" : L"ms");
            else        wcscpy_s(label, num);
            Tick(X(static_cast<float>(k)), tickTop, tickBottom, label, plotR + 14.0f);
        }

        Pulse trigger[2]{};
        int triggerCount = 1;
        Pulse out[8]{};
        int n = 0;
        const float tapW = std::max(8.0f, 0.16f * pxPerCycle);

        if (mode == MacroMode::HoldToLoop) {
            trigger[0] = Pulse{ X(0.0f), X(stopAt), trig, {} };
            if (full) out[n++] = Pulse{ X(0.0f), X(stopAt), ch1, {} };
            else {
                for (int k = 0; k < 5 && static_cast<float>(k) < stopAt; ++k) {
                    out[n++] = Pulse{ X(static_cast<float>(k)), X(std::min(static_cast<float>(k) + dutyFrac, stopAt)), ch1, {} };
                }
            }
        } else if (mode == MacroMode::InfiniteToggleLoop) {
            trigger[0] = Pulse{ X(0.0f), X(0.0f) + tapW, trig, {} };
            trigger[1] = Pulse{ X(stopAt), X(stopAt) + tapW, trig, {} };
            triggerCount = 2;
            if (full) out[n++] = Pulse{ X(0.0f), X(stopAt), ch1, {} };
            else {
                for (int k = 0; k < 5 && static_cast<float>(k) < stopAt; ++k) {
                    out[n++] = Pulse{ X(static_cast<float>(k)), X(static_cast<float>(k) + dutyFrac), ch1, {} };
                }
            }
        } else {
            trigger[0] = Pulse{ X(0.0f), X(0.0f) + tapW, trig, {} };
            const int shown = static_cast<int>(std::min<uint32_t>(s.repeatCount, 5U));
            if (full) {
                const float end = (s.repeatCount > 5U) ? 6.0f : static_cast<float>(shown);
                out[n++] = Pulse{ X(0.0f), X(end), ch1, {} };
            } else {
                for (int k = 0; k < shown; ++k) {
                    out[n++] = Pulse{ X(static_cast<float>(k)), X(static_cast<float>(k) + dutyFrac), ch1, {} };
                }
            }
        }

        Trace(laneT, trigger, triggerCount, 0.12f, 1.25f, false);
        Trace(laneO, out, n, outAlpha, outWidth, false);

        const uint8_t hum = mask::core::HumanizePercent(s);
        if (hum > 0U && !full) {
            const float jitterPx = std::clamp(pxPerCycle * (static_cast<float>(hum) / 100.0f) * 0.45f, 2.0f, 14.0f);
            const float jTop = Snap(laneO.top);
            const float jBot = Snap(laneO.bottom);
            for (int i = 0; i < n; ++i) {
                if (i > 0) {
                    const float rx = Snap(std::max(out[i].x0, laneO.left));
                    if (rx >= laneO.left && rx <= laneO.right) {
                        Fill(Rc(rx - jitterPx, jTop, rx + jitterPx, jBot), 0.0f, ch1, 0.12f);
                        Line(Snap(rx - jitterPx), jTop + 3.0f, Snap(rx - jitterPx), jBot - 3.0f, ch1, 1.0f, 0.40f);
                        Line(Snap(rx + jitterPx), jTop + 3.0f, Snap(rx + jitterPx), jBot - 3.0f, ch1, 1.0f, 0.40f);
                    }
                }
                const float fx = Snap(std::min(out[i].x1, laneO.right));
                if (fx >= laneO.left && fx <= laneO.right &&
                    !(mode == MacroMode::HoldToLoop && std::fabs(out[i].x1 - X(stopAt)) < 0.5f)) {
                    Fill(Rc(fx - jitterPx, jTop, fx + jitterPx, jBot), 0.0f, ch1, 0.12f);
                    Line(Snap(fx - jitterPx), jTop + 3.0f, Snap(fx - jitterPx), jBot - 3.0f, ch1, 1.0f, 0.40f);
                    Line(Snap(fx + jitterPx), jTop + 3.0f, Snap(fx + jitterPx), jBot - 3.0f, ch1, 1.0f, 0.40f);
                }
            }
        }

        LaneLabel(view, laneO, gutter, NameOf(s.outputVk).text, theme::Muted);

        if (mode == MacroMode::RepeatNTimes && s.repeatCount > 5U) {
            wchar_t more[64]{};
            if (full) {
                wchar_t total[32]{};
                FormatDuration(downMs * static_cast<double>(s.repeatCount), total, std::size(total));
                std::swprintf(more, std::size(more), L"%ls total", total);
            } else if (hum > 0U) {
                std::swprintf(more, std::size(more), L"%u presses \u2022 \u00B1%u%%",
                              static_cast<unsigned>(s.repeatCount), static_cast<unsigned>(hum));
            } else {
                std::swprintf(more, std::size(more), L"%u presses in total", static_cast<unsigned>(s.repeatCount));
            }
            Text(more, fAxis_, Rc(plotR - 160.0f, laneT.top, plotR, laneT.bottom), theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (hum > 0U && !full) {
            wchar_t humTag[32]{};
            std::swprintf(humTag, std::size(humTag), L"\u00B1%u%% jitter", static_cast<unsigned>(hum));
            Text(humTag, fAxis_, Rc(plotR - 110.0f, laneT.top, plotR, laneT.bottom), theme::Muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        PopClip();
    }

    // ---------------------------------------------------------------------------------
    //  Status bar (with animated toast slide & profile bank badge)
    // ---------------------------------------------------------------------------------

    void DrawStatusBar(const D2D1_RECT_F& r) noexcept {
        const float cy = Snap((r.top + r.bottom) * 0.5f) - 0.5f;

        // Right: Bank badge + autosave state + panic chord reminder
        wchar_t bankBuf[24]{};
        std::swprintf(bankBuf, std::size(bankBuf), L"Bank %u", static_cast<unsigned>(g_Ui.settings.activeProfileBank + 1U));
        const wchar_t* saveText = L"All changes saved";
        UINT32 saveColor = theme::Faint;
        if (g_Ui.saveState == SaveState::Pending) {
            saveText = L"Saving\u2026";
        } else if (g_Ui.saveState == SaveState::Failed) {
            saveText = L"Couldn't save";
            saveColor = theme::Danger;
        }
        wchar_t rightInfo[64]{};
        std::swprintf(rightInfo, std::size(rightInfo), L"%ls \u2022 %ls", bankBuf, saveText);
        const float saveW = std::ceil(MeasureWidth(rightInfo, fSmall_));
        Text(rightInfo, fSmall_, Rc(r.right - saveW - 4.0f, r.top, r.right, r.bottom), saveColor, DWRITE_TEXT_ALIGNMENT_TRAILING);

        float hintsLeft = r.right - saveW - 24.0f;
        if (g_Ui.settings.panicChordEnabled) {
            const wchar_t* stopLabel = L"emergency stop";
            const float hintsW = 3.0f * 12.0f + MeasureWidth(L"Left Ctrl", fMini_) + MeasureWidth(L"Right Ctrl", fMini_) +
                                 MeasureWidth(L"Esc", fMini_) + 2.0f * 4.0f + 8.0f + MeasureWidth(stopLabel, fSmall_);
            float hx = r.right - saveW - 24.0f - hintsW;
            hintsLeft = hx;
            if (hx > r.left + 320.0f) {
                hx = MiniKey(hx, cy, L"Left Ctrl") + 4.0f;
                hx = MiniKey(hx, cy, L"Right Ctrl") + 4.0f;
                hx = MiniKey(hx, cy, L"Esc") + 8.0f;
                Text(stopLabel, fSmall_, Rc(hx, r.top, hx + 140.0f, r.bottom), theme::Faint);
            }
        }

        // Left: Animated sliding toast message
        const float ts = std::clamp(g_Animator.toastSlide.value, 0.0f, 1.0f);
        if (g_Ui.toast[0] != L'\0' && ts > 0.01f) {
            const float slideY = (1.0f - ts) * 10.0f;
            const float maxRight = std::max(r.left + 200.0f, hintsLeft - 18.0f);
            float actionW = 0.0f;
            if (g_Ui.toastAction != 0) {
                actionW = std::ceil(MeasureWidth(g_Ui.toastActionLabel, fBodyStrong_)) + 22.0f;
            }
            const float tw = std::min(std::ceil(MeasureWidth(g_Ui.toast, fBody_)) + 2.0f, maxRight - r.left - 4.0f - actionW - 10.0f);
            Text(g_Ui.toast, fBody_, Rc(r.left + 4.0f, r.top + slideY, r.left + 4.0f + tw, r.bottom + slideY), theme::Text,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, ts);
            if (g_Ui.toastAction != 0) {
                const D2D1_RECT_F b = RcW(r.left + 4.0f + tw + 10.0f, cy - 13.0f + slideY, actionW, 26.0f);
                Button(b, g_Ui.toastActionLabel, act::ToastAction, BtnStyle::Secondary, fSmall_, ts);
            }
        }
    }

    // ---------------------------------------------------------------------------------

    HWND                   hwnd_{nullptr};
    float                  dpi_{96.0f};
    ID2D1Factory*          d2dFactory_{nullptr};
    IDWriteFactory*        dwriteFactory_{nullptr};
    ID2D1HwndRenderTarget* rt_{nullptr};
    ID2D1SolidColorBrush*  brush_{nullptr};
    FrameState             frame_{};
    mask::ui::AccentPalette accent_{};

    IDWriteTextFormat* fTitle_{nullptr};
    IDWriteTextFormat* fHeading_{nullptr};
    IDWriteTextFormat* fBody_{nullptr};
    IDWriteTextFormat* fBodyStrong_{nullptr};
    IDWriteTextFormat* fSmall_{nullptr};
    IDWriteTextFormat* fLabel_{nullptr};
    IDWriteTextFormat* fNum_{nullptr};
    IDWriteTextFormat* fKey_{nullptr};
    IDWriteTextFormat* fAxis_{nullptr};
    IDWriteTextFormat* fMini_{nullptr};

    std::vector<HitBox>        hitBoxes_{};
    D2D1_RECT_F                clip_{};
    bool                       clipActive_{false};
    D2D1_RECT_F                listRect_{};
    float                      listMaxScroll_{0.0f};
    D2D1_RECT_F                seqListRect_{};
    float                      seqStepMaxScroll_{0.0f};
    D2D1_RECT_F                seqStepKindBtnRect_{};
    D2D1_RECT_F                dutyTrack_{};
    D2D1_RECT_F                trigTolTrack_{};
    std::array<D2D1_RECT_F, 5> settingsTabRects_{};
};

} // namespace mask::app

#include "app_view_editors.hpp"

namespace mask::app {
inline UiRenderer g_Renderer{};
} // namespace mask::app
