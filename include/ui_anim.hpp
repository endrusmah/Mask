#pragma once

// Direct2D / D3D11 Animation, Cosmic Palette & Micro-Interaction Engine for Mask.
//
// Design goals:
// - 0% idle CPU/GPU: every SpringValue and SmoothValue snaps to its target within rest epsilon
//   (0.001f) and IsAnimating(windowFocused) returns false once transient animations settle whenever
//   unfocused, minimized, or in Reduced/Off AnimationQuality.
// - Frame-rate independent: dt is clamped to <= 0.033s and springs sub-step safely so frame hitches
//   never explode or overshoot erratically.
// - Quality-aware:
//   - Full (0): Smooth ~60fps ambient cosmic drift/starlight & cursor spotlight while focused +
//               springs, squeeze, firing pulses, and click ripples. Frozen immediately when unfocused.
//   - Reduced (1): 2x snappier functional springs/scrolls/tabs; sleeps immediately at rest;
//                  no ambient loop, squeeze, cursor aura, or decorative click ripples.
//   - Off (2): Instant snap; IsAnimating() always false.

#include "app_settings.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mask::ui {

inline constexpr float kAnimRestEpsilon = 0.001f;
inline constexpr float kAnimMaxDtSec    = 0.033f;

// =====================================================================================
//  Cosmic Blue / Black Accent Theme Palettes (0..4 aligned with app_settings.hpp)
//  0 = CosmicCyan [default], 1 = DeepSapphire, 2 = IceStarlight,
//  3 = NebulaViolet, 4 = AuroraTeal
// =====================================================================================

struct AccentPalette {
    UINT32 signal{0x38BDF8U};
    UINT32 signalHi{0x7DD3FCU};
    UINT32 onSignal{0x03111EU};
    UINT32 secondary{0x22D3EEU};
};

[[nodiscard]] constexpr AccentPalette GetAccentPalette(uint8_t preset) noexcept {
    switch (static_cast<mask::settings::AccentPreset>(preset)) {
        case mask::settings::AccentPreset::DeepSapphire:
            return AccentPalette{ 0x3B82F6U, 0x60A5FAU, 0x031024U, 0x38BDF8U };
        case mask::settings::AccentPreset::IceStarlight:
            return AccentPalette{ 0x67E8F9U, 0xE0F7FFU, 0x02131BU, 0x38BDF8U };
        case mask::settings::AccentPreset::NebulaViolet:
            return AccentPalette{ 0x818CF8U, 0xA5B4FCU, 0x090C24U, 0x38BDF8U };
        case mask::settings::AccentPreset::AuroraTeal:
            return AccentPalette{ 0x2DD4BFU, 0x5EEAD4U, 0x031A17U, 0x38BDF8U };
        case mask::settings::AccentPreset::CosmicCyan:
        default:
            return AccentPalette{ 0x38BDF8U, 0x7DD3FCU, 0x03111EU, 0x22D3EEU };
    }
}

[[nodiscard]] constexpr const wchar_t* GetAccentName(uint8_t preset) noexcept {
    switch (static_cast<mask::settings::AccentPreset>(preset)) {
        case mask::settings::AccentPreset::DeepSapphire: return L"Deep Sapphire";
        case mask::settings::AccentPreset::IceStarlight: return L"Ice Starlight";
        case mask::settings::AccentPreset::NebulaViolet: return L"Nebula Violet";
        case mask::settings::AccentPreset::AuroraTeal:   return L"Aurora Teal";
        case mask::settings::AccentPreset::CosmicCyan:
        default:                                         return L"Cosmic Cyan";
    }
}

[[nodiscard]] inline UINT32 LerpRgb(UINT32 a, UINT32 b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const float ar = static_cast<float>((a >> 16) & 0xFFU);
    const float ag = static_cast<float>((a >> 8) & 0xFFU);
    const float ab = static_cast<float>(a & 0xFFU);
    const float br = static_cast<float>((b >> 16) & 0xFFU);
    const float bg = static_cast<float>((b >> 8) & 0xFFU);
    const float bb = static_cast<float>(b & 0xFFU);
    const UINT32 r  = static_cast<UINT32>(std::clamp(ar + (br - ar) * t + 0.5f, 0.0f, 255.0f));
    const UINT32 g  = static_cast<UINT32>(std::clamp(ag + (bg - ag) * t + 0.5f, 0.0f, 255.0f));
    const UINT32 bl = static_cast<UINT32>(std::clamp(ab + (bb - ab) * t + 0.5f, 0.0f, 255.0f));
    return (r << 16) | (g << 8) | bl;
}

/// Packs 0xRRGGBB + alpha [0..1] into 0xRRGGBBAA.
[[nodiscard]] inline UINT32 Rgba(UINT32 rgb, float alpha) noexcept {
    const float clamped = std::clamp(alpha, 0.0f, 1.0f);
    const UINT32 a = static_cast<UINT32>(clamped * 255.0f + 0.5f) & 0xFFU;
    return ((rgb & 0xFFFFFFU) << 8) | a;
}

/// Packs 0xRRGGBB + alpha byte [0..255] into 0xRRGGBBAA.
[[nodiscard]] constexpr UINT32 RgbaByte(UINT32 rgb, uint8_t alphaByte) noexcept {
    return ((rgb & 0xFFFFFFU) << 8) | static_cast<UINT32>(alphaByte);
}

/// Unpacks 0xRRGGBBAA into a D2D1_COLOR_F.
[[nodiscard]] inline D2D1_COLOR_F ColorFromRgba(UINT32 rgba, float alphaMul = 1.0f) noexcept {
    const float r = static_cast<float>((rgba >> 24) & 0xFFU) / 255.0f;
    const float g = static_cast<float>((rgba >> 16) & 0xFFU) / 255.0f;
    const float b = static_cast<float>((rgba >> 8) & 0xFFU) / 255.0f;
    const float a = (static_cast<float>(rgba & 0xFFU) / 255.0f) * std::clamp(alphaMul, 0.0f, 1.0f);
    return D2D1::ColorF(r, g, b, a);
}

// =====================================================================================
//  Spring & Exponential Smoothing Primitives
// =====================================================================================

struct SpringValue {
    float value{0.0f};
    float target{0.0f};
    float velocity{0.0f};

    constexpr void Snap(float v) noexcept {
        value = v;
        target = v;
        velocity = 0.0f;
    }

    constexpr void SetTarget(float t) noexcept {
        target = t;
    }

    [[nodiscard]] bool IsAtRest(float eps = kAnimRestEpsilon) const noexcept {
        return std::fabs(value - target) <= eps && std::fabs(velocity) <= eps;
    }

    bool Step(float dt, float stiffness = 360.0f, float damping = 28.0f) noexcept;
};

struct SmoothValue {
    float value{0.0f};
    float target{0.0f};

    constexpr void Snap(float v) noexcept {
        value = v;
        target = v;
    }

    constexpr void SetTarget(float t) noexcept {
        target = t;
    }

    [[nodiscard]] bool IsAtRest(float eps = kAnimRestEpsilon) const noexcept {
        return std::fabs(value - target) <= eps;
    }

    bool Step(float dt, float speed = 14.0f) noexcept;
};

// =====================================================================================
//  Click Ripple Primitive
// =====================================================================================

struct ClickRipple {
    float  x{0.0f};
    float  y{0.0f};
    float  progress{0.0f};
    UINT32 rgb{0x38BDF8U};
    bool   active{false};
};

// =====================================================================================
//  UiAnimator
// =====================================================================================

class UiAnimator {
public:
    static constexpr size_t kMacroOffset       = 0;
    static constexpr size_t kAutoclickerOffset = kMacroOffset;
    static constexpr size_t kSequenceOffset    = kMacroOffset + mask::core::kMaxMacroSlots;
    static constexpr size_t kTriggerOffset     = kSequenceOffset + mask::core::kMaxComplexSequences;
    static constexpr size_t kRemapOffset       = kTriggerOffset + mask::core::kMaxColorTriggers;
    static constexpr size_t kSlotAnimTotal     = kRemapOffset + mask::core::kMaxRemapRules;
    static constexpr size_t kSlotCount         = kSlotAnimTotal;
    static constexpr size_t kSectionCount      = 8;
    static constexpr size_t kRippleCount       = 6;

    static_assert(kSlotAnimTotal == mask::core::kMaxMacroSlots +
                                    mask::core::kMaxComplexSequences +
                                    mask::core::kMaxColorTriggers +
                                    mask::core::kMaxRemapRules,
                  "kSlotAnimTotal must cover Autoclicker/Macro, Sequence, Trigger, and Remap slots.");

    uint8_t quality{static_cast<uint8_t>(mask::settings::AnimationQuality::Full)};

    // App open entrance (0 -> 1 drives fade-in + subtle slide-up)
    SmoothValue appOpen{0.0f, 1.0f};

    // Button / control squeeze feedback
    int         pressedAction{0};
    bool        pointerDown{false};
    SpringValue pressScale{1.0f, 1.0f, 0.0f};

    // Per-item toggle slides (0=off, 1=on), glows, row scales, and live firing pulse glows
    std::array<SpringValue, kSlotAnimTotal> toggleSlide{};
    std::array<SmoothValue, kSlotAnimTotal> toggleGlow{};
    std::array<SmoothValue, kSlotAnimTotal> rowScale{};
    std::array<SmoothValue, kSlotAnimTotal> firingPulse{};
    int                                     pendingDeleteId{-1};

    // Extension / drawer / popover / tab / toast animations
    SpringValue                            newPickerOpen{0.0f, 0.0f, 0.0f};
    SpringValue                            settingsOpen{0.0f, 0.0f, 0.0f};
    SmoothValue                            viewTransition{1.0f, 1.0f};
    SpringValue                            settingsTabX{0.0f, 0.0f, 0.0f};
    SpringValue                            settingsTabW{80.0f, 80.0f, 0.0f};
    std::array<SmoothValue, kSectionCount> sectionExpand{};
    SmoothValue                            toastSlide{0.0f, 0.0f};

    // Cosmic Interactive Animations
    float                                 ambientPhase{0.0f}; // Incremented ONLY when focused && active && quality == Full
    SpringValue                           sidebarIndY{0.0f, 0.0f, 0.0f};
    SpringValue                           sidebarIndH{0.0f, 0.0f, 0.0f};
    SmoothValue                           listScrollSmooth{0.0f, 0.0f};
    SmoothValue                           seqScrollSmooth{0.0f, 0.0f};
    SmoothValue                           cursorX{0.0f, 0.0f};
    SmoothValue                           cursorY{0.0f, 0.0f};
    SmoothValue                           cursorInside{0.0f, 0.0f};
    std::array<ClickRipple, kRippleCount> clickRipples{};

    UiAnimator() noexcept;

    [[nodiscard]] static double NowSeconds() noexcept;

    void SetQuality(uint8_t q) noexcept;
    void TriggerAppOpen() noexcept;

    [[nodiscard]] float AppOpenOffsetY(float maxSlidePx = 14.0f) const noexcept;
    [[nodiscard]] float AppOpenAlpha() const noexcept;
    [[nodiscard]] float PressAmount() const noexcept;

    void OnPointerDown(int action) noexcept;
    void OnPointerUp() noexcept;
    [[nodiscard]] D2D1_RECT_F ApplySqueeze(const D2D1_RECT_F& r, int action) const noexcept;

    void SetToggle(size_t slotIndex, bool on, bool snap = false) noexcept;
    void SetToggleGlow(size_t slotIndex, float glow, bool snap = false) noexcept;
    void SetFiring(size_t slotIndex, bool firing, bool snap = false) noexcept;

    void TriggerRowAdd(size_t slotIndex) noexcept;
    void TriggerRowDelete(int deleteId, size_t slotIndex) noexcept;
    [[nodiscard]] bool ConsumeFinishedDelete(int& outId) noexcept;

    void SetNewPickerOpen(bool open, bool snap = false) noexcept;
    void SetSettingsOpen(bool open, bool snap = false) noexcept;
    void TriggerViewTransition() noexcept;
    void SetSettingsTab(float x, float w, bool snap = false) noexcept;
    void SetSidebarIndicator(float y, float h, bool snap = false) noexcept;

    void SetListScrollTarget(float scrollY, bool snap = false) noexcept;
    void SetSeqScrollTarget(float scrollY, bool snap = false) noexcept;
    void ClampScrollTargets(float maxListScroll, float maxSeqScroll) noexcept;

    void SetCursorPosition(float dipX, float dipY, bool inside, bool snap = false) noexcept;
    void TriggerRipple(float dipX, float dipY, UINT32 rgb) noexcept;

    void SetSectionExpanded(size_t index, bool expanded, bool snap = false) noexcept;
    void SetToastVisible(bool visible, bool snap = false) noexcept;

    void SnapAll() noexcept;

    /// Advances active springs, smooth values, click ripples, and (when focused && active && Full)
    /// the cosmic ambient phase. Returns true while rendering should continue animating.
    bool Tick(double nowSec, bool windowFocused = true, bool windowActive = true) noexcept;

    /// Returns true ONLY while transient springs/ripples are moving OR when
    /// (windowFocused && windowActive && quality == Full) for ambient cosmic starlight/nebula drift.
    [[nodiscard]] bool IsAnimating(bool windowFocused = true, bool windowActive = true) const noexcept;
    [[nodiscard]] bool HasTransientMotion() const noexcept;

private:
    double lastTickSec_{0.0};
    bool   animating_{false};
    size_t pendingDeleteSlot_{kSlotAnimTotal};
};

} // namespace mask::ui
