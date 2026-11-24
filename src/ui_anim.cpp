#include "ui_anim.hpp"

#include <algorithm>
#include <cmath>

namespace mask::ui {

bool SpringValue::Step(float dt, float stiffness, float damping) noexcept {
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

bool SmoothValue::Step(float dt, float speed) noexcept {
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

UiAnimator::UiAnimator() noexcept {
    for (auto& r : rowScale) {
        r.Snap(1.0f);
    }
    for (auto& s : sectionExpand) {
        s.Snap(0.0f);
    }
}

double UiAnimator::NowSeconds() noexcept {
    static const double kInvFreq = []() noexcept -> double {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return (freq.QuadPart > 0) ? (1.0 / static_cast<double>(freq.QuadPart)) : 0.0;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * kInvFreq;
}

void UiAnimator::SetQuality(uint8_t q) noexcept {
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

void UiAnimator::TriggerAppOpen() noexcept {
    if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
        appOpen.Snap(1.0f);
        return;
    }
    appOpen.Snap(0.0f);
    appOpen.SetTarget(1.0f);
    animating_ = true;
}

float UiAnimator::AppOpenOffsetY(float maxSlidePx) const noexcept {
    const float t = std::clamp(appOpen.value, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return inv * inv * maxSlidePx;
}

float UiAnimator::AppOpenAlpha() const noexcept {
    return std::clamp(appOpen.value, 0.0f, 1.0f);
}

float UiAnimator::PressAmount() const noexcept {
    return std::clamp((1.0f - pressScale.value) / 0.045f, 0.0f, 1.0f);
}

void UiAnimator::OnPointerDown(int action) noexcept {
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

void UiAnimator::OnPointerUp() noexcept {
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

D2D1_RECT_F UiAnimator::ApplySqueeze(const D2D1_RECT_F& r, int action) const noexcept {
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

void UiAnimator::SetToggle(size_t slotIndex, bool on, bool snap) noexcept {
    if (slotIndex >= kSlotAnimTotal) {
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

void UiAnimator::SetToggleGlow(size_t slotIndex, float glow, bool snap) noexcept {
    if (slotIndex >= kSlotAnimTotal) {
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

void UiAnimator::SetFiring(size_t slotIndex, bool firing, bool snap) noexcept {
    if (slotIndex >= kSlotAnimTotal) {
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

void UiAnimator::TriggerRowAdd(size_t slotIndex) noexcept {
    if (slotIndex >= kSlotAnimTotal) {
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

void UiAnimator::TriggerRowDelete(int deleteId, size_t slotIndex) noexcept {
    if (slotIndex >= kSlotAnimTotal) {
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

bool UiAnimator::ConsumeFinishedDelete(int& outId) noexcept {
    if (pendingDeleteId < 0) {
        return false;
    }
    size_t slot = pendingDeleteSlot_;
    if (slot >= kSlotAnimTotal) {
        if (static_cast<size_t>(pendingDeleteId) < kSlotAnimTotal) {
            slot = static_cast<size_t>(pendingDeleteId);
        } else {
            outId = pendingDeleteId;
            pendingDeleteId = -1;
            pendingDeleteSlot_ = kSlotAnimTotal;
            return true;
        }
    }
    const bool off = (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off));
    if (off || (rowScale[slot].target <= 0.001f && rowScale[slot].value <= 0.025f)) {
        outId = pendingDeleteId;
        rowScale[slot].Snap(1.0f);
        pendingDeleteId = -1;
        pendingDeleteSlot_ = kSlotAnimTotal;
        return true;
    }
    return false;
}

void UiAnimator::SetNewPickerOpen(bool open, bool snap) noexcept {
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

void UiAnimator::SetSettingsOpen(bool open, bool snap) noexcept {
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

void UiAnimator::TriggerViewTransition() noexcept {
    if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
        viewTransition.Snap(1.0f);
        return;
    }
    viewTransition.Snap(0.0f);
    viewTransition.SetTarget(1.0f);
    animating_ = true;
}

void UiAnimator::SetSettingsTab(float x, float w, bool snap) noexcept {
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

void UiAnimator::SetSidebarIndicator(float y, float h, bool snap) noexcept {
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

void UiAnimator::SetListScrollTarget(float scrollY, bool snap) noexcept {
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

void UiAnimator::SetSeqScrollTarget(float scrollY, bool snap) noexcept {
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

void UiAnimator::ClampScrollTargets(float maxListScroll, float maxSeqScroll) noexcept {
    const float maxL = std::max(0.0f, maxListScroll);
    const float maxS = std::max(0.0f, maxSeqScroll);
    listScrollSmooth.target = std::clamp(listScrollSmooth.target, 0.0f, maxL);
    listScrollSmooth.value  = std::clamp(listScrollSmooth.value, 0.0f, maxL);
    seqScrollSmooth.target  = std::clamp(seqScrollSmooth.target, 0.0f, maxS);
    seqScrollSmooth.value   = std::clamp(seqScrollSmooth.value, 0.0f, maxS);
}

void UiAnimator::SetCursorPosition(float dipX, float dipY, bool inside, bool snap) noexcept {
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

void UiAnimator::TriggerRipple(float dipX, float dipY, UINT32 rgb) noexcept {
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

void UiAnimator::SetSectionExpanded(size_t index, bool expanded, bool snap) noexcept {
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

void UiAnimator::SetToastVisible(bool visible, bool snap) noexcept {
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

void UiAnimator::SnapAll() noexcept {
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

bool UiAnimator::Tick(double nowSec, bool windowFocused, bool windowActive) noexcept {
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

    for (size_t i = 0; i < kSlotAnimTotal; ++i) {
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

bool UiAnimator::IsAnimating(bool windowFocused, bool windowActive) const noexcept {
    if (quality >= static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
        return false;
    }
    if (quality == static_cast<uint8_t>(mask::settings::AnimationQuality::Full) &&
        windowFocused && windowActive) {
        return true;
    }
    return HasTransientMotion();
}

bool UiAnimator::HasTransientMotion() const noexcept {
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
    for (size_t i = 0; i < kSlotAnimTotal; ++i) {
        if (!toggleSlide[i].IsAtRest() || !toggleGlow[i].IsAtRest() ||
            !rowScale[i].IsAtRest() || !firingPulse[i].IsAtRest()) {
            return true;
        }
    }
    return false;
}

} // namespace mask::ui
