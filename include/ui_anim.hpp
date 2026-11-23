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

    bool Step(float dt, float stiffness = 360.0f, float damping = 28.0f) noexcept {
        if (std::fabs(value - target) <= kAnimRestEpsilon && std::fabs(velocity) <= kAnimRestEpsilon) {
            value = target;
            velocity = 0.0f;
            return false;
        }
        dt = std::clamp(dt, 0.0f, kAnimMaxDtSec);
        if (dt <= 0.0f) {
            return true;
        }

        // Sub-step at <= 8.33ms so high-stiffness springs stay unconditionally stable across hitches.
        constexpr float kMaxSubDt = 0.008333f;
        float remaining = dt;
        while (remaining > 0.0f) {
            const float step = std::min(remaining, kMaxSubDt);
            const float accel = -stiffness * (value - target) - damping * velocity;
            velocity += accel * step;
            value += velocity * step;
            remaining -= step;
        }

        if (std::fabs(value - target) <= kAnimRestEpsilon && std::fabs(velocity) <= kAnimRestEpsilon) {
            value = target;
            velocity = 0.0f;
            return false;
        }
        return true;
    }
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

    bool Step(float dt, float speed = 14.0f) noexcept {
        if (std::fabs(value - target) <= kAnimRestEpsilon) {
            value = target;
            return false;
        }
        dt = std::clamp(dt, 0.0f, kAnimMaxDtSec);
        if (dt <= 0.0f) {
            return true;
        }

        const float alpha = 1.0f - std::exp(-speed * dt);
        value += (target - value) * alpha;

        if (std::fabs(value - target) <= kAnimRestEpsilon) {
            value = target;
            return false;
        }
        return true;
    }
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
    static constexpr size_t kSlotCount      = 128; // 0..63 Macros, 64..79 Sequences, 80..87 Triggers, 88..119 Remaps
    static constexpr size_t kMacroOffset    = 0;
    static constexpr size_t kSequenceOffset = 64;
    static constexpr size_t kTriggerOffset  = 80;
    static constexpr size_t kRemapOffset    = 88;
    static constexpr size_t kSectionCount   = 8;
    static constexpr size_t kRippleCount    = 6;

    uint8_t quality{static_cast<uint8_t>(mask::settings::AnimationQuality::Full)};

    // App open entrance (0 -> 1 drives fade-in + subtle slide-up)
    SmoothValue appOpen{0.0f, 1.0f};

    // Button / control squeeze feedback
    int         pressedAction{0};
    bool        pointerDown{false};
    SpringValue pressScale{1.0f, 1.0f, 0.0f};

    // Per-item toggle slides (0=off, 1=on), glows, row scales, and live firing pulse glows
    std::array<SpringValue, kSlotCount> toggleSlide{};
    std::array<SmoothValue, kSlotCount> toggleGlow{};
    std::array<SmoothValue, kSlotCount> rowScale{};
    std::array<SmoothValue, kSlotCount> firingPulse{};
    int                                 pendingDeleteId{-1};

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

    UiAnimator() noexcept {
        for (auto& r : rowScale) {
            r.Snap(1.0f);
        }
        for (auto& s : sectionExpand) {
            s.Snap(0.0f);
        }
    }

    [[nodiscard]] static double NowSeconds() noexcept {
        static const double kInvFreq = []() noexcept -> double {
            LARGE_INTEGER freq{};
            QueryPerformanceFrequency(&freq);
            return (freq.QuadPart > 0) ? (1.0 / static_cast<double>(freq.QuadPart)) : 0.0;
        }();
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) * kInvFreq;
    }

    void SetQuality(uint8_t q) noexcept {
        quality = (q <= static_cast<uint8_t>(mask::settings::AnimationQuality::Off))
            ? q
            : static_cast<uint8_t>(mask::settings::AnimationQuality::Full);
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            SnapAll();
        } else if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Reduced)) {
            pressScale.Snap(1.0f);
            pressedAction = 0;
            pointerDown = false;
            cursorInside.Snap(0.0f);
            cursorX.Snap(cursorX.target);
            cursorY.Snap(cursorY.target);
            for (auto& rp : clickRipples) {
                rp.active = false;
                rp.progress = 1.0f;
            }
        }
    }

    void TriggerAppOpen() noexcept {
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            appOpen.Snap(1.0f);
            return;
        }
        appOpen.Snap(0.0f);
        appOpen.SetTarget(1.0f);
        animating_ = true;
    }

    [[nodiscard]] float AppOpenOffsetY(float maxSlidePx = 14.0f) const noexcept {
        const float t = std::clamp(appOpen.value, 0.0f, 1.0f);
        const float inv = 1.0f - t;
        return inv * inv * maxSlidePx;
    }

    [[nodiscard]] float AppOpenAlpha() const noexcept {
        return std::clamp(appOpen.value, 0.0f, 1.0f);
    }

    [[nodiscard]] float PressAmount() const noexcept {
        return std::clamp((1.0f - pressScale.value) / 0.045f, 0.0f, 1.0f);
    }

    void OnPointerDown(int action) noexcept {
        if (action == 0) {
            return;
        }
        pressedAction = action;
        pointerDown = true;
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Full)) {
            pressScale.SetTarget(0.955f);
            animating_ = true;
        } else {
            pressScale.Snap(1.0f);
        }
    }

    void OnPointerUp() noexcept {
        pointerDown = false;
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Full)) {
            pressScale.SetTarget(1.0f);
            if (!pressScale.IsAtRest()) {
                animating_ = true;
            } else {
                pressedAction = 0;
            }
        } else {
            pressScale.Snap(1.0f);
            pressedAction = 0;
        }
    }

    [[nodiscard]] D2D1_RECT_F ApplySqueeze(const D2D1_RECT_F& r, int action) const noexcept {
        if (quality != static_cast<uint8_t>(mask::settings::AnimationQuality::Full) ||
            action == 0 || action != pressedAction) {
            return r;
        }
        const float s = std::clamp(pressScale.value, 0.90f, 1.02f);
        if (std::fabs(s - 1.0f) <= 0.0005f) {
            return r;
        }
        const float cx = (r.left + r.right) * 0.5f;
        const float cy = (r.top + r.bottom) * 0.5f;
        const float hw = (r.right - r.left) * 0.5f * s;
        const float hh = (r.bottom - r.top) * 0.5f * s;
        return D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
    }

    void SetToggle(size_t slotIndex, bool on, bool snap = false) noexcept {
        if (slotIndex >= kSlotCount) {
            return;
        }
        const float target = on ? 1.0f : 0.0f;
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            toggleSlide[slotIndex].Snap(target);
            toggleGlow[slotIndex].Snap(target);
        } else {
            toggleSlide[slotIndex].SetTarget(target);
            toggleGlow[slotIndex].SetTarget(target);
            if (!toggleSlide[slotIndex].IsAtRest() || !toggleGlow[slotIndex].IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void SetToggleGlow(size_t slotIndex, float glow, bool snap = false) noexcept {
        if (slotIndex >= kSlotCount) {
            return;
        }
        const float clamped = std::clamp(glow, 0.0f, 1.0f);
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            toggleGlow[slotIndex].Snap(clamped);
        } else {
            toggleGlow[slotIndex].SetTarget(clamped);
            if (!toggleGlow[slotIndex].IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void SetFiring(size_t slotIndex, bool firing, bool snap = false) noexcept {
        if (slotIndex >= kSlotCount) {
            return;
        }
        const float target = firing ? 1.0f : 0.0f;
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            firingPulse[slotIndex].Snap(target);
        } else {
            firingPulse[slotIndex].SetTarget(target);
            if (!firingPulse[slotIndex].IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void TriggerRowAdd(size_t slotIndex) noexcept {
        if (slotIndex >= kSlotCount) {
            return;
        }
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            rowScale[slotIndex].Snap(1.0f);
            return;
        }
        rowScale[slotIndex].Snap(0.0f);
        rowScale[slotIndex].SetTarget(1.0f);
        animating_ = true;
    }

    void TriggerRowDelete(int deleteId, size_t slotIndex) noexcept {
        if (slotIndex >= kSlotCount) {
            return;
        }
        pendingDeleteId = deleteId;
        pendingDeleteSlot_ = slotIndex;
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            rowScale[slotIndex].Snap(0.0f);
            return;
        }
        rowScale[slotIndex].SetTarget(0.0f);
        animating_ = true;
    }

    [[nodiscard]] bool ConsumeFinishedDelete(int& outId) noexcept {
        if (pendingDeleteId < 0) {
            return false;
        }
        size_t slot = pendingDeleteSlot_;
        if (slot >= kSlotCount) {
            if (static_cast<size_t>(pendingDeleteId) < kSlotCount) {
                slot = static_cast<size_t>(pendingDeleteId);
            } else {
                outId = pendingDeleteId;
                pendingDeleteId = -1;
                pendingDeleteSlot_ = kSlotCount;
                return true;
            }
        }
        const bool off = (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off));
        if (off || (rowScale[slot].target <= 0.001f && rowScale[slot].value <= 0.025f)) {
            outId = pendingDeleteId;
            rowScale[slot].Snap(1.0f);
            pendingDeleteId = -1;
            pendingDeleteSlot_ = kSlotCount;
            return true;
        }
        return false;
    }

    void SetNewPickerOpen(bool open, bool snap = false) noexcept {
        const float target = open ? 1.0f : 0.0f;
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            newPickerOpen.Snap(target);
        } else {
            newPickerOpen.SetTarget(target);
            if (!newPickerOpen.IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void SetSettingsOpen(bool open, bool snap = false) noexcept {
        const float target = open ? 1.0f : 0.0f;
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            settingsOpen.Snap(target);
        } else {
            settingsOpen.SetTarget(target);
            if (!settingsOpen.IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void TriggerViewTransition() noexcept {
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            viewTransition.Snap(1.0f);
            return;
        }
        viewTransition.Snap(0.0f);
        viewTransition.SetTarget(1.0f);
        animating_ = true;
    }

    void SetSettingsTab(float x, float w, bool snap = false) noexcept {
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            settingsTabX.Snap(x);
            settingsTabW.Snap(w);
        } else {
            settingsTabX.SetTarget(x);
            settingsTabW.SetTarget(w);
            if (!settingsTabX.IsAtRest() || !settingsTabW.IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void SetSidebarIndicator(float y, float h, bool snap = false) noexcept {
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            sidebarIndY.Snap(y);
            sidebarIndH.Snap(h);
            return;
        }
        // When morphing into visibility from an uninitialized/hidden pill, anchor Y immediately
        // instead of flying in from y=0 at startup.
        if (sidebarIndH.value <= 0.5f && sidebarIndH.target <= 0.5f && h > 0.5f) {
            sidebarIndY.Snap(y);
        } else {
            sidebarIndY.SetTarget(y);
        }
        sidebarIndH.SetTarget(h);
        if (!sidebarIndY.IsAtRest() || !sidebarIndH.IsAtRest()) {
            animating_ = true;
        }
    }

    void SetListScrollTarget(float scrollY, bool snap = false) noexcept {
        const float clamped = std::max(0.0f, scrollY);
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            listScrollSmooth.Snap(clamped);
        } else {
            listScrollSmooth.SetTarget(clamped);
            if (!listScrollSmooth.IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void SetSeqScrollTarget(float scrollY, bool snap = false) noexcept {
        const float clamped = std::max(0.0f, scrollY);
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            seqScrollSmooth.Snap(clamped);
        } else {
            seqScrollSmooth.SetTarget(clamped);
            if (!seqScrollSmooth.IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void ClampScrollTargets(float maxListScroll, float maxSeqScroll) noexcept {
        const float maxL = std::max(0.0f, maxListScroll);
        const float maxS = std::max(0.0f, maxSeqScroll);
        listScrollSmooth.target = std::clamp(listScrollSmooth.target, 0.0f, maxL);
        listScrollSmooth.value  = std::clamp(listScrollSmooth.value, 0.0f, maxL);
        seqScrollSmooth.target  = std::clamp(seqScrollSmooth.target, 0.0f, maxS);
        seqScrollSmooth.value   = std::clamp(seqScrollSmooth.value, 0.0f, maxS);
    }

    void SetCursorPosition(float dipX, float dipY, bool inside, bool snap = false) noexcept {
        if (snap || quality != static_cast<uint8_t>(mask::settings::AnimationQuality::Full)) {
            cursorX.Snap(dipX);
            cursorY.Snap(dipY);
            cursorInside.Snap((inside && quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Full)) ? 1.0f : 0.0f);
            return;
        }
        if (inside && cursorInside.value <= 0.01f && cursorInside.target <= 0.01f) {
            cursorX.Snap(dipX);
            cursorY.Snap(dipY);
        } else if (inside) {
            cursorX.SetTarget(dipX);
            cursorY.SetTarget(dipY);
        }
        cursorInside.SetTarget(inside ? 1.0f : 0.0f);
        if (!cursorX.IsAtRest() || !cursorY.IsAtRest() || !cursorInside.IsAtRest()) {
            animating_ = true;
        }
    }

    void TriggerRipple(float dipX, float dipY, UINT32 rgb) noexcept {
        if (quality != static_cast<uint8_t>(mask::settings::AnimationQuality::Full)) {
            return;
        }
        size_t chosen = 0;
        float oldestProgress = -1.0f;
        for (size_t i = 0; i < kRippleCount; ++i) {
            if (!clickRipples[i].active) {
                chosen = i;
                oldestProgress = 999.0f;
                break;
            }
            if (clickRipples[i].progress > oldestProgress) {
                oldestProgress = clickRipples[i].progress;
                chosen = i;
            }
        }
        clickRipples[chosen] = ClickRipple{ dipX, dipY, 0.0f, rgb, true };
        animating_ = true;
    }

    void SetSectionExpanded(size_t index, bool expanded, bool snap = false) noexcept {
        if (index >= kSectionCount) {
            return;
        }
        const float target = expanded ? 1.0f : 0.0f;
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            sectionExpand[index].Snap(target);
        } else {
            sectionExpand[index].SetTarget(target);
            if (!sectionExpand[index].IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void SetToastVisible(bool visible, bool snap = false) noexcept {
        const float target = visible ? 1.0f : 0.0f;
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            toastSlide.Snap(target);
        } else {
            toastSlide.SetTarget(target);
            if (!toastSlide.IsAtRest()) {
                animating_ = true;
            }
        }
    }

    void SnapAll() noexcept {
        appOpen.Snap(appOpen.target);
        pressScale.Snap(1.0f);
        pressedAction = 0;
        pointerDown = false;
        for (auto& s : toggleSlide) {
            s.Snap(s.target);
        }
        for (auto& g : toggleGlow) {
            g.Snap(g.target);
        }
        for (auto& r : rowScale) {
            r.Snap(r.target);
        }
        for (auto& fp : firingPulse) {
            fp.Snap(fp.target);
        }
        newPickerOpen.Snap(newPickerOpen.target);
        settingsOpen.Snap(settingsOpen.target);
        viewTransition.Snap(viewTransition.target);
        settingsTabX.Snap(settingsTabX.target);
        settingsTabW.Snap(settingsTabW.target);
        sidebarIndY.Snap(sidebarIndY.target);
        sidebarIndH.Snap(sidebarIndH.target);
        listScrollSmooth.Snap(listScrollSmooth.target);
        seqScrollSmooth.Snap(seqScrollSmooth.target);
        cursorX.Snap(cursorX.target);
        cursorY.Snap(cursorY.target);
        cursorInside.Snap(cursorInside.target);
        for (auto& e : sectionExpand) {
            e.Snap(e.target);
        }
        toastSlide.Snap(toastSlide.target);
        for (auto& rp : clickRipples) {
            rp.active = false;
            rp.progress = 1.0f;
        }
        animating_ = false;
        lastTickSec_ = 0.0;
    }

    /// Advances active springs, smooth values, click ripples, and (when focused && active && Full)
    /// the cosmic ambient phase. Returns true while rendering should continue animating.
    bool Tick(double nowSec, bool windowFocused = true, bool windowActive = true) noexcept {
        if (quality >= static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            SnapAll();
            return false;
        }

        float dt = 1.0f / 60.0f;
        if (lastTickSec_ > 0.0 && nowSec > lastTickSec_) {
            dt = std::clamp(static_cast<float>(nowSec - lastTickSec_), 0.0005f, kAnimMaxDtSec);
        }
        lastTickSec_ = nowSec;

        const bool full    = (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Full));
        const bool reduced = !full;
        const float speedMul = reduced ? 2.0f : 1.0f;
        const float spStiff  = reduced ? 900.0f : 380.0f;
        const float spDamp   = reduced ? 60.0f  : 29.0f;

        bool transientActive = false;
        transientActive |= appOpen.Step(dt, 11.0f * speedMul);

        if (reduced) {
            pressScale.Snap(1.0f);
            pressedAction = 0;
            pointerDown = false;
            cursorX.Snap(cursorX.target);
            cursorY.Snap(cursorY.target);
            cursorInside.Snap(0.0f);
            for (auto& rp : clickRipples) {
                rp.active = false;
                rp.progress = 1.0f;
            }
        } else {
            const bool pressMoving = pressScale.Step(dt, 520.0f, 30.0f);
            transientActive |= pressMoving;
            if (!pressMoving && pressScale.target >= 0.999f) {
                pressedAction = 0;
                pointerDown = false;
            }

            transientActive |= cursorX.Step(dt, 24.0f);
            transientActive |= cursorY.Step(dt, 24.0f);
            transientActive |= cursorInside.Step(dt, cursorInside.target > 0.5f ? 16.0f : 8.0f);

            for (auto& rp : clickRipples) {
                if (!rp.active) {
                    continue;
                }
                rp.progress += dt * 2.35f;
                if (rp.progress >= 1.0f) {
                    rp.progress = 1.0f;
                    rp.active = false;
                } else {
                    transientActive = true;
                }
            }
        }

        for (size_t i = 0; i < kSlotCount; ++i) {
            transientActive |= toggleSlide[i].Step(dt, reduced ? 900.0f : 440.0f, reduced ? 60.0f : 30.0f);
            transientActive |= toggleGlow[i].Step(dt, 12.0f * speedMul);
            transientActive |= rowScale[i].Step(dt, 20.0f * speedMul);
            transientActive |= firingPulse[i].Step(dt, 14.0f * speedMul);
        }

        transientActive |= newPickerOpen.Step(dt, spStiff, spDamp);
        transientActive |= settingsOpen.Step(dt, spStiff, spDamp);
        transientActive |= viewTransition.Step(dt, 16.0f * speedMul);
        transientActive |= settingsTabX.Step(dt, spStiff, spDamp);
        transientActive |= settingsTabW.Step(dt, spStiff, spDamp);
        transientActive |= sidebarIndY.Step(dt, reduced ? 900.0f : 420.0f, reduced ? 60.0f : 31.0f);
        transientActive |= sidebarIndH.Step(dt, reduced ? 900.0f : 420.0f, reduced ? 60.0f : 31.0f);
        transientActive |= listScrollSmooth.Step(dt, 22.0f * speedMul);
        transientActive |= seqScrollSmooth.Step(dt, 22.0f * speedMul);

        for (auto& e : sectionExpand) {
            transientActive |= e.Step(dt, 14.0f * speedMul);
        }
        transientActive |= toastSlide.Step(dt, 15.0f * speedMul);

        if (pendingDeleteId >= 0) {
            transientActive = true;
        }

        const bool ambientActive = full && windowFocused && windowActive;
        if (ambientActive) {
            ambientPhase += dt;
        }

        animating_ = transientActive;
        const bool keepRunning = transientActive || ambientActive;
        if (!keepRunning) {
            lastTickSec_ = 0.0;
        }
        return keepRunning;
    }

    /// Returns true ONLY while transient springs/ripples are moving OR when
    /// (windowFocused && windowActive && quality == Full) for ambient cosmic starlight/nebula drift.
    [[nodiscard]] bool IsAnimating(bool windowFocused = true, bool windowActive = true) const noexcept {
        if (quality >= static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            return false;
        }
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Full) &&
            windowFocused && windowActive) {
            return true;
        }
        return HasTransientMotion();
    }

    [[nodiscard]] bool HasTransientMotion() const noexcept {
        if (quality >= static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            return false;
        }
        if (animating_ || pendingDeleteId >= 0) {
            return true;
        }
        if (!appOpen.IsAtRest() || !pressScale.IsAtRest() || !newPickerOpen.IsAtRest() ||
            !settingsOpen.IsAtRest() || !viewTransition.IsAtRest() ||
            !settingsTabX.IsAtRest() || !settingsTabW.IsAtRest() ||
            !sidebarIndY.IsAtRest() || !sidebarIndH.IsAtRest() ||
            !listScrollSmooth.IsAtRest() || !seqScrollSmooth.IsAtRest() ||
            !cursorX.IsAtRest() || !cursorY.IsAtRest() || !cursorInside.IsAtRest() ||
            !toastSlide.IsAtRest()) {
            return true;
        }
        for (const auto& e : sectionExpand) {
            if (!e.IsAtRest()) {
                return true;
            }
        }
        for (const auto& rp : clickRipples) {
            if (rp.active) {
                return true;
            }
        }
        for (size_t i = 0; i < kSlotCount; ++i) {
            if (!toggleSlide[i].IsAtRest() || !toggleGlow[i].IsAtRest() ||
                !rowScale[i].IsAtRest() || !firingPulse[i].IsAtRest()) {
                return true;
            }
        }
        return false;
    }

private:
    double lastTickSec_{0.0};
    bool   animating_{false};
    size_t pendingDeleteSlot_{kSlotCount};
};

} // namespace mask::ui
