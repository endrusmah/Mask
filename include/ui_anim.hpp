#pragma once

// Direct2D Animation & Micro-Interaction Engine for Mask.
//
// Design goals:
// - 0% idle CPU: every SpringValue and SmoothValue snaps to its target within rest epsilon (0.001f)
//   and IsAnimating() returns false once all active animations settle.
// - Frame-rate independent: dt is clamped to <= 0.033s and springs sub-step safely so frame hitches
//   never explode or overshoot erratically.
// - Quality-aware: supports Full (0), Reduced (1: 2x snappier, no button squeeze), and Off (2: instant
//   snap, IsAnimating() always false).

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
//  Accent Theme Palettes (matches mask::settings::AccentPreset)
// =====================================================================================

struct AccentPalette {
    UINT32 signal{0xF2C94C};
    UINT32 signalHi{0xFFDD75};
    UINT32 onSignal{0x1E1800};
    UINT32 secondary{0x5CCFE6};
};

[[nodiscard]] constexpr AccentPalette GetAccentPalette(uint8_t preset) noexcept {
    switch (static_cast<mask::settings::AccentPreset>(preset)) {
        case mask::settings::AccentPreset::Cyan:
            return AccentPalette{ 0x5CCFE6U, 0x8BE3F5U, 0x081B22U, 0xF2C94CU };
        case mask::settings::AccentPreset::Emerald:
            return AccentPalette{ 0x5AD696U, 0x83E8B4U, 0x072015U, 0x5CCFE6U };
        case mask::settings::AccentPreset::Violet:
            return AccentPalette{ 0xA882FFU, 0xC4A8FFU, 0x160C29U, 0x5CCFE6U };
        case mask::settings::AccentPreset::Crimson:
            return AccentPalette{ 0xF0625DU, 0xFF8B87U, 0x260A09U, 0xF2C94CU };
        case mask::settings::AccentPreset::Amber:
        default:
            return AccentPalette{ 0xF2C94CU, 0xFFDD75U, 0x1E1800U, 0x5CCFE6U };
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
    const UINT32 r = static_cast<UINT32>(std::clamp(ar + (br - ar) * t + 0.5f, 0.0f, 255.0f));
    const UINT32 g = static_cast<UINT32>(std::clamp(ag + (bg - ag) * t + 0.5f, 0.0f, 255.0f));
    const UINT32 bl = static_cast<UINT32>(std::clamp(ab + (bb - ab) * t + 0.5f, 0.0f, 255.0f));
    return (r << 16) | (g << 8) | bl;
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

    uint8_t quality{static_cast<uint8_t>(mask::settings::AnimationQuality::Full)};

    // App open entrance (0 -> 1 drives fade-in + subtle 14px slide-up)
    SmoothValue appOpen{0.0f, 1.0f};

    // Button / control squeeze feedback
    int         pressedAction{0};
    SpringValue pressScale{1.0f, 1.0f, 0.0f};

    // Per-item toggle switch slides (0=off, 1=on), glows, and row add/delete collapse scales
    std::array<SpringValue, kSlotCount> toggleSlide{};
    std::array<SmoothValue, kSlotCount> toggleGlow{};
    std::array<SmoothValue, kSlotCount> rowScale{};
    int                                 pendingDeleteId{-1};

    // Extension / drawer / popover / tab / toast animations
    SpringValue                            newPickerOpen{0.0f, 0.0f, 0.0f};
    SmoothValue                            viewTransition{1.0f, 1.0f};
    SpringValue                            settingsTabX{0.0f, 0.0f, 0.0f};
    SpringValue                            settingsTabW{80.0f, 80.0f, 0.0f};
    std::array<SmoothValue, kSectionCount> sectionExpand{};
    SmoothValue                            toastSlide{0.0f, 0.0f};

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

    void OnPointerDown(int action) noexcept {
        if (action == 0) {
            return;
        }
        pressedAction = action;
        if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Full)) {
            pressScale.SetTarget(0.955f);
            animating_ = true;
        } else {
            pressScale.Snap(1.0f);
        }
    }

    void OnPointerUp() noexcept {
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
            animating_ = true;
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
            animating_ = true;
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
            animating_ = true;
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
            animating_ = true;
        }
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
            animating_ = true;
        }
    }

    void SetToastVisible(bool visible, bool snap = false) noexcept {
        const float target = visible ? 1.0f : 0.0f;
        if (snap || quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            toastSlide.Snap(target);
        } else {
            toastSlide.SetTarget(target);
            animating_ = true;
        }
    }

    void SnapAll() noexcept {
        appOpen.Snap(appOpen.target);
        pressScale.Snap(1.0f);
        pressedAction = 0;
        for (auto& s : toggleSlide) {
            s.Snap(s.target);
        }
        for (auto& g : toggleGlow) {
            g.Snap(g.target);
        }
        for (auto& r : rowScale) {
            r.Snap(r.target);
        }
        newPickerOpen.Snap(newPickerOpen.target);
        viewTransition.Snap(viewTransition.target);
        settingsTabX.Snap(settingsTabX.target);
        settingsTabW.Snap(settingsTabW.target);
        for (auto& e : sectionExpand) {
            e.Snap(e.target);
        }
        toastSlide.Snap(toastSlide.target);
        animating_ = false;
        lastTickSec_ = 0.0;
    }

    bool Tick(double nowSec) noexcept {
        if (quality >= static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            SnapAll();
            return false;
        }

        float dt = 1.0f / 60.0f;
        if (lastTickSec_ > 0.0 && nowSec > lastTickSec_) {
            dt = std::clamp(static_cast<float>(nowSec - lastTickSec_), 0.0005f, kAnimMaxDtSec);
        }
        lastTickSec_ = nowSec;

        const bool reduced = (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Reduced));
        const float speedMul = reduced ? 2.0f : 1.0f;
        const float spStiff  = reduced ? 900.0f : 380.0f;
        const float spDamp   = reduced ? 60.0f  : 29.0f;

        bool active = false;
        active |= appOpen.Step(dt, 11.0f * speedMul);

        if (reduced) {
            pressScale.Snap(1.0f);
            pressedAction = 0;
        } else {
            const bool pressMoving = pressScale.Step(dt, 520.0f, 30.0f);
            active |= pressMoving;
            if (!pressMoving && pressScale.target >= 0.999f) {
                pressedAction = 0;
            }
        }

        for (size_t i = 0; i < kSlotCount; ++i) {
            active |= toggleSlide[i].Step(dt, reduced ? 900.0f : 440.0f, reduced ? 60.0f : 30.0f);
            active |= toggleGlow[i].Step(dt, 12.0f * speedMul);
            active |= rowScale[i].Step(dt, 20.0f * speedMul);
        }

        active |= newPickerOpen.Step(dt, spStiff, spDamp);
        active |= viewTransition.Step(dt, 16.0f * speedMul);
        active |= settingsTabX.Step(dt, spStiff, spDamp);
        active |= settingsTabW.Step(dt, spStiff, spDamp);
        for (auto& e : sectionExpand) {
            active |= e.Step(dt, 14.0f * speedMul);
        }
        active |= toastSlide.Step(dt, 15.0f * speedMul);

        if (pendingDeleteId >= 0) {
            active = true;
        }

        animating_ = active;
        if (!active) {
            lastTickSec_ = 0.0;
        }
        return active;
    }

    [[nodiscard]] bool IsAnimating() const noexcept {
        if (quality >= static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
            return false;
        }
        if (animating_ || pendingDeleteId >= 0) {
            return true;
        }
        if (!appOpen.IsAtRest() || !pressScale.IsAtRest() || !newPickerOpen.IsAtRest() ||
            !viewTransition.IsAtRest() || !settingsTabX.IsAtRest() || !settingsTabW.IsAtRest() ||
            !toastSlide.IsAtRest()) {
            return true;
        }
        for (const auto& e : sectionExpand) {
            if (!e.IsAtRest()) {
                return true;
            }
        }
        for (size_t i = 0; i < kSlotCount; ++i) {
            if (!toggleSlide[i].IsAtRest() || !toggleGlow[i].IsAtRest() || !rowScale[i].IsAtRest()) {
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
