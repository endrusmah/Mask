#include "app.hpp"

#include <windowsx.h>
#include <wtsapi32.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>

namespace mask::app {

App* App::activeInstance_ = nullptr;

int App::Run(const LaunchOptions& opts) noexcept {
    activeInstance_ = this;

    mask::input::GameInputGlobalContext::sharedState       = &sharedState_;
    mask::input::GameInputGlobalContext::engineWakeEvent   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    mask::input::GameInputGlobalContext::watchdogWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    mask::input::GameInputGlobalContext::outputWakeEvent   = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    int exitCode = 0;
    if (opts.bench) {
        exitCode = RunBenchmark();
    } else if (opts.selfTest) {
        exitCode = RunSelfTest();
    } else {
        exitCode = RunGuiLoop(opts);
    }

    if (mask::input::GameInputGlobalContext::engineWakeEvent != nullptr) {
        CloseHandle(mask::input::GameInputGlobalContext::engineWakeEvent);
        mask::input::GameInputGlobalContext::engineWakeEvent = nullptr;
    }
    if (mask::input::GameInputGlobalContext::watchdogWakeEvent != nullptr) {
        CloseHandle(mask::input::GameInputGlobalContext::watchdogWakeEvent);
        mask::input::GameInputGlobalContext::watchdogWakeEvent = nullptr;
    }
    if (mask::input::GameInputGlobalContext::outputWakeEvent != nullptr) {
        CloseHandle(mask::input::GameInputGlobalContext::outputWakeEvent);
        mask::input::GameInputGlobalContext::outputWakeEvent = nullptr;
    }
    mask::input::GameInputGlobalContext::sharedState = nullptr;

    activeInstance_ = nullptr;
    return exitCode;
}

int App::RunGuiLoop(const LaunchOptions& opts) noexcept {
    model_.ResolveProfilePaths();
    model_.InitializeDefaults();
    (void)model_.LoadSettingsFromDisk();

    bool startMinimized = opts.startMinimized || model_.settings.startMinimized;
    const int loaded = model_.LoadFromDisk();
    if (loaded == 2) {
        model_.SetToast(L"Your saved profile was damaged, so the previous backup was restored.");
        animator_.SetToastVisible(true, true);
    } else if (loaded == 0) {
        model_.SetToast(L"Welcome to Mask. Click + New to create autoclickers, sequences, triggers, or remaps.");
        animator_.SetToastVisible(true, true);
    }

    seqEngine_.SetToggleMacroCallback(&App::OnToggleMacroSlotFromSeqStatic);
    seqEngine_.SetCommandApprovalCallback(&App::OnCommandApprovalStatic);

    SyncAllEnginesFromModel();
    model_.RefreshDevicesAndMonitors(&remapEngine_);

    pixelEngine_.Start();
    seqEngine_.Start();
    remapEngine_.Start();

    mask::input::GameInputGlobalContext::perDeviceCallback.store(
        &App::OnPhysicalDeviceTransitionStatic, std::memory_order_release);

    watchdog_ = std::make_unique<mask::safety::Watchdog>(sharedState_, &App::OnWatchdogSafeStopStatic);
    watchdog_->SetAuxCleanupCallback(&App::OnWatchdogAuxCleanupStatic);
    watchdog_->Start();

    outputThreadStop_.store(false, std::memory_order_release);
    std::atomic<bool> outputReady{false};
    outputThread_ = std::thread([this, &outputReady]() noexcept {
        OutputThreadMain(&outputReady);
    });
    while (!outputReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    std::atomic<bool> inputReady{false};
    gameInputThread_ = std::thread([this, &inputReady]() noexcept {
        GameInputThreadMain(&inputReady);
    });
    while (!inputReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    engineThread_ = std::thread([this]() noexcept {
        EngineThreadMain(false);
    });

    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    RefreshAppIcons(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &App::MainWindowProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = (iconLarge_ != nullptr) ? iconLarge_ : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = iconSmall_;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const float scale = static_cast<float>(dpi > 0U ? dpi : 96U) / 96.0f;
    const float wantW = model_.settings.rememberWindowBounds
        ? (std::max)(kMinWidthDip, static_cast<float>(model_.settings.windowW)) : kDefaultWidthDip;
    const float wantH = model_.settings.rememberWindowBounds
        ? (std::max)(kMinHeightDip, static_cast<float>(model_.settings.windowH)) : kDefaultHeightDip;

    const DWORD exStyle = model_.settings.alwaysOnTop ? WS_EX_TOPMOST : 0U;
    RECT rc{ 0, 0, static_cast<LONG>(wantW * scale), static_cast<LONG>(wantH * scale) };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, exStyle, dpi > 0U ? dpi : 96U);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int ww = (std::min<int>)(rc.right - rc.left, work.right - work.left);
    const int wh = (std::min<int>)(rc.bottom - rc.top, work.bottom - work.top);
    int wx = work.left + (std::max)(0, static_cast<int>((work.right - work.left - ww) / 2));
    int wy = work.top + (std::max)(0, static_cast<int>((work.bottom - work.top - wh) / 2));

    if (model_.settings.rememberWindowBounds && model_.settings.windowX != -1 && model_.settings.windowY != -1) {
        RECT saved{ model_.settings.windowX, model_.settings.windowY,
                    model_.settings.windowX + ww, model_.settings.windowY + wh };
        if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL) != nullptr) {
            wx = model_.settings.windowX;
            wy = model_.settings.windowY;
        }
    }

    HWND hwndMain = CreateWindowExW(exStyle, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                    wx, wy, ww, wh, nullptr, nullptr, hInst, this);
    if (hwndMain != nullptr) {
        mask::input::GameInputGlobalContext::uiWindowHandle.store(hwndMain, std::memory_order_release);
        StartOutputWorker();

        if (startMinimized) {
            if (model_.settings.minimizeToTray) {
                HideToTray(hwndMain);
            } else {
                ShowWindow(hwndMain, SW_SHOWMINNOACTIVE);
            }
        } else {
            ShowWindow(hwndMain, SW_SHOW);
            UpdateWindow(hwndMain);
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyAppIcons();

    mask::input::GameInputGlobalContext::perDeviceCallback.store(nullptr, std::memory_order_release);
    seqEngine_.SetCommandApprovalCallback(nullptr);
    seqEngine_.SetToggleMacroCallback(nullptr);
    sharedState_.engineRunning.store(false, std::memory_order_release);
    mask::input::WakeEngine();
    mask::input::WakeWatchdog();

    pixelEngine_.Stop();
    seqEngine_.Stop();
    remapEngine_.Stop();

    if (engineThread_.joinable()) engineThread_.join();
    if (gameInputThread_.joinable()) gameInputThread_.join();

    outputThreadStop_.store(true, std::memory_order_release);
    mask::input::WakeOutput();
    if (outputThread_.joinable()) outputThread_.join();
    if (outputWorker_.joinable()) outputWorker_.join();

    if (watchdog_) {
        watchdog_->ExecuteEmergencyRecovery(false);
        watchdog_->Stop();
        watchdog_.reset();
    }

    return 0;
}

void App::EnsureAnimTimer(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    const bool windowActive = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_;
    const bool allowAnim = windowActive && (windowFocused_ || !model_.settings.lowResourceWhenUnfocused);
    const bool wantAnim = allowAnim && animator_.IsAnimating(windowFocused_, windowActive);
    if (wantAnim && !animTimerActive_) {
        SetTimer(hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
        animTimerActive_ = true;
    } else if (!wantAnim && animTimerActive_) {
        KillTimer(hwnd, kAnimTimerId);
        animTimerActive_ = false;
    }
}

void App::UpdateUiTimerCadence(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    const bool active = windowFocused_ && !IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_;
    const bool lowPower = !active && model_.settings.lowResourceWhenUnfocused;
    sharedState_.lowPowerUiMode.store(lowPower, std::memory_order_release);
    const UINT interval = active ? kUiFocusedIntervalMs : kUiBackgroundIntervalMs;
    if (uiTimerMs_ != interval) {
        SetTimer(hwnd, kUiTimerId, interval, nullptr);
        uiTimerMs_ = interval;
    }
    EnsureAnimTimer(hwnd);
    UpdateTriggerPreviewState(hwnd);
}

void App::UpdateTriggerPreviewState(HWND hwnd) noexcept {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        if (previewActiveSent_) {
            pixelEngine_.SetPreviewActive(false, 0U);
            previewActiveSent_ = false;
        }
        triggerPreviewTimerMs_ = 0U;
        return;
    }
    const bool visible = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_;
    const bool focusedOrUnrestricted = windowFocused_ || !model_.settings.lowResourceWhenUnfocused;
    const bool viewingTrigger = !model_.showSettings && model_.HasSelection() && model_.selectedKind == ItemKind::Trigger;

    const bool wantDropper = visible && model_.triggerColorPickActive;
    const bool wantLive    = visible && focusedOrUnrestricted && viewingTrigger && !model_.triggerColorPickActive;
    const uint8_t slot     = viewingTrigger
        ? static_cast<uint8_t>(std::clamp(model_.selectedIndex, 0, kColorTriggerCount - 1)) : 0U;

    if (wantLive != previewActiveSent_ || (wantLive && slot != previewSlotSent_)) {
        pixelEngine_.SetPreviewActive(wantLive, slot);
        previewActiveSent_ = wantLive;
        previewSlotSent_   = slot;
    }

    const UINT wantTimer = wantDropper ? kTrigDropperIntervalMs : (wantLive ? kTrigLiveIntervalMs : 0U);
    if (wantTimer == 0U) {
        if (triggerPreviewTimerMs_ != 0U) {
            KillTimer(hwnd, kTriggerPreviewTimerId);
            triggerPreviewTimerMs_ = 0U;
        }
    } else if (triggerPreviewTimerMs_ != wantTimer) {
        SetTimer(hwnd, kTriggerPreviewTimerId, wantTimer, nullptr);
        triggerPreviewTimerMs_ = wantTimer;
    }
}

uint32_t App::SelectedSlotPhysHeldMask() const noexcept {
    if (model_.showSettings || !model_.settings.showLiveKeyIndicators || !model_.HasSelection()) {
        return 0U;
    }
    auto isHeld = [this](uint16_t vk) noexcept -> uint32_t {
        if (vk == 0U || vk == mask::core::kVkWheelUp || vk == mask::core::kVkWheelDown) return 0U;
        return sharedState_.physicalKeyState[vk & 0xFFU].load(std::memory_order_relaxed) != 0U ? 1U : 0U;
    };
    if (model_.selectedKind == ItemKind::Autoclicker) {
        const auto& slot = model_.SelectedAutoclicker();
        return isHeld(slot.triggerVk) | (isHeld(slot.outputVk) << 1);
    }
    if (model_.selectedKind == ItemKind::Sequence) {
        const auto& seq = model_.SelectedSeq();
        uint32_t mask = isHeld(seq.triggerVk);
        if (model_.selectedSeqStep >= 0 && model_.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            mask |= (isHeld(seq.steps[static_cast<size_t>(model_.selectedSeqStep)].vk) << 1);
        }
        return mask;
    }
    if (model_.selectedKind == ItemKind::Trigger) {
        const auto& t = model_.SelectedTrigger();
        return isHeld(t.armVk) | (isHeld(t.actionVk) << 1);
    }
    if (model_.selectedKind == ItemKind::Remap) {
        const auto& r = model_.SelectedRemap();
        return isHeld(r.sourceVk) | (isHeld(r.targetVk) << 1);
    }
    return 0U;
}

FrameState App::BuildFrameState(HWND hwnd) const noexcept {
    const mask::core::TelemetrySnapshot snap = sharedState_.ReadTelemetry();
    FrameState frame{};
    frame.firingMask       = snap.activeSlotBitmask | snap.toggledSlotBitmask;
    frame.activeSeqMask    = seqEngine_.GetActiveSequenceBitmask();
    if (frame.activeSeqMask != 0U) {
        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            frame.activeSeqStep[i] = seqEngine_.GetCurrentStepIndex(i);
        }
    }
    frame.activeRemapLayer = sharedState_.activeRemapLayer.load(std::memory_order_relaxed);
    frame.remapUnlocked    = sharedState_.remapUnlocked.load(std::memory_order_relaxed);
    frame.paused           = sharedState_.automationPaused.load(std::memory_order_acquire);
    frame.windowFocused    = windowFocused_ && (hwnd == nullptr || (!IsIconic(hwnd) && !hiddenToTray_));
    frame.bindTarget       = sharedState_.captureBindTarget.load(std::memory_order_acquire);
    frame.safeStopReason   = sharedState_.safeStopReason.load(std::memory_order_acquire);
    frame.snap             = snap;
    if (!model_.showSettings && model_.HasSelection() && model_.selectedKind == ItemKind::Trigger) {
        if (model_.triggerColorPickActive) {
            frame.triggerPreview = model_.eyedropperPreview;
        } else {
            frame.triggerPreview = pixelEngine_.GetPreviewSnapshot();
        }
    }
    return frame;
}

void App::SaveCurrentWindowBounds(HWND hwnd) noexcept {
    if (!model_.settings.rememberWindowBounds || IsIconic(hwnd) || !IsWindowVisible(hwnd)) return;
    RECT rc{};
    if (GetWindowRect(hwnd, &rc)) {
        const UINT dpi = GetDpiForWindow(hwnd);
        const float scale = static_cast<float>(dpi > 0U ? dpi : 96U) / 96.0f;
        model_.settings.windowX = rc.left;
        model_.settings.windowY = rc.top;
        model_.settings.windowW = static_cast<int32_t>(std::lround(static_cast<float>(rc.right - rc.left) / scale));
        model_.settings.windowH = static_cast<int32_t>(std::lround(static_cast<float>(rc.bottom - rc.top) / scale));
    }
}

void App::OnAnimTimer(HWND hwnd) noexcept {
    const bool windowActive = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_;
    const bool stillRunning = animator_.Tick(mask::ui::UiAnimator::NowSeconds(), windowFocused_, windowActive);
    model_.listScroll    = animator_.listScrollSmooth.value;
    model_.seqStepScroll = animator_.seqScrollSmooth.value;

    int finishedDelId = -1;
    if (animator_.ConsumeFinishedDelete(finishedDelId)) {
        FinalizeItemDelete(hwnd, finishedDelId);
    }
    if (windowActive) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    const bool allowAnim = windowActive && (windowFocused_ || !model_.settings.lowResourceWhenUnfocused);
    if (!allowAnim || (!stillRunning && !animator_.IsAnimating(windowFocused_, windowActive))) {
        KillTimer(hwnd, kAnimTimerId);
        animTimerActive_ = false;
    }
}

void App::OnUiTimer(HWND hwnd) noexcept {
    if (!sharedState_.engineRunning.load(std::memory_order_acquire)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    bool repaint = false;

    if (model_.pendingSyncMask != 0ULL) {
        model_.FlushPendingSync();
    }

    const ULONGLONG autosaveDelay = std::clamp<ULONGLONG>(model_.settings.autosaveDelayMs, 200ULL, 30'000ULL);
    if (model_.saveState == SaveState::Pending && now - model_.lastChangeTick >= autosaveDelay &&
        model_.editField == Field::None && !model_.draggingDuty && model_.draggingTriggerSlider.IsNone()) {
        if (!model_.SaveToDisk()) {
            ShowToast(hwnd, L"Couldn't save your profile. Check that Mask's folder isn't read-only.");
        }
        repaint = true;
    }

    const bool visible = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_;
    const bool lowPowerUnfocused = !windowFocused_ && model_.settings.lowResourceWhenUnfocused;

    if (model_.toast[0] != L'\0' && now - model_.toastTick > kToastLifetimeMs &&
        sharedState_.captureBindTarget.load(std::memory_order_relaxed) == 0U) {
        const bool canAnimateToast = visible && (windowFocused_ || !model_.settings.lowResourceWhenUnfocused);
        if (!canAnimateToast) {
            animator_.SetToastVisible(false, true);
            model_.ClearToast();
        } else {
            animator_.SetToastVisible(false);
            EnsureAnimTimer(hwnd);
            if (animator_.toastSlide.value <= 0.02f) {
                model_.ClearToast();
            }
        }
        if (visible) {
            repaint = true;
        }
    }

    if (visible) {
        const uint16_t seqMask  = seqEngine_.GetActiveSequenceBitmask();
        const uint8_t  layer    = sharedState_.activeRemapLayer.load(std::memory_order_relaxed);
        const bool     unlocked = sharedState_.remapUnlocked.load(std::memory_order_relaxed);
        if (seqMask != lastActiveSeqMask_ || layer != lastActiveRemapLayer_ || unlocked != lastRemapUnlocked_ ||
            (!lowPowerUnfocused && seqMask != 0U)) {
            if (seqMask != lastActiveSeqMask_) {
                lastFiringSeqMask_ = seqMask;
                const bool snapPulse = lowPowerUnfocused;
                for (int i = 0; i < kComplexSeqCount; ++i) {
                    animator_.SetFiring(AnimSlotFor(ItemKind::Sequence, i), (seqMask & (1U << i)) != 0U, snapPulse);
                }
                if (!snapPulse) EnsureAnimTimer(hwnd);
            }
            lastActiveSeqMask_      = seqMask;
            lastActiveRemapLayer_ = layer;
            lastRemapUnlocked_    = unlocked;
            repaint = true;
        }
        if (!lowPowerUnfocused) {
            if (model_.showSettings && model_.settingsTab == 4) {
                const uint64_t seq = sharedState_.telemetrySequence.load(std::memory_order_acquire);
                if ((seq & 1ULL) == 0ULL && seq != lastUiTelemetrySeq_) {
                    lastUiTelemetrySeq_ = seq;
                    repaint = true;
                }
            } else {
                const uint32_t heldMask = SelectedSlotPhysHeldMask();
                if (heldMask != lastUiPhysHeldMask_) {
                    lastUiPhysHeldMask_ = heldMask;
                    repaint = true;
                }
            }
        }
    }

    if (model_.outputState == OutputState::Ready &&
        sharedState_.outputFailureStreak.load(std::memory_order_relaxed) >= kOutputFailureLimit) {
        model_.outputState = OutputState::NotResponding;
        autoReconnectAttempts_ = 0;
        nextAutoReconnectTick_ = now + 2000ULL;
        ShowToast(hwnd, L"Virtual devices stopped responding. Reconnecting\u2026");
        repaint = true;
    }
    if (model_.outputState == OutputState::NotResponding && autoReconnectAttempts_ < 3 &&
        now >= nextAutoReconnectTick_ && !outputWorkerBusy_.load(std::memory_order_acquire)) {
        ++autoReconnectAttempts_;
        nextAutoReconnectTick_ = now + 10'000ULL * static_cast<ULONGLONG>(autoReconnectAttempts_);
        StartOutputWorker();
        repaint = true;
    }

    if (repaint && visible) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void App::PasteClipboardIntoEditBuffer(size_t maxLen) noexcept {
    if (!OpenClipboard(nullptr)) return;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData != nullptr) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(hData));
        if (text != nullptr) {
            size_t len = std::wcslen(model_.editBuffer);
            for (size_t i = 0; text[i] != L'\0' && len < maxLen; ++i) {
                const wchar_t c = text[i];
                if (c >= 0x20 && c != 0x7F) {
                    model_.editBuffer[len++] = c;
                }
            }
            model_.editBuffer[len] = L'\0';
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
}

void App::SampleTriggerEyedropperLoupe(int32_t cx, int32_t cy) noexcept {
    auto& pv = model_.eyedropperPreview;
    pv.sampleX = cx;
    pv.sampleY = cy;
    pv.usingDxgi = false;
    constexpr int N = mask::triggers::ColorTriggerPreviewSnapshot::kLoupeSize;
    constexpr int kHalf = N / 2;
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        (void)mask::triggers::PixelTriggerEngine::SamplePixelAt(cx, cy, pv.sampledR, pv.sampledG, pv.sampledB);
        return;
    }
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = N;
    bmi.bmiHeader.biHeight      = -N;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = (memDc != nullptr)
        ? CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0U)
        : nullptr;
    if (memDc != nullptr && dib != nullptr && bits != nullptr) {
        HGDIOBJ oldObj = SelectObject(memDc, dib);
        if (BitBlt(memDc, 0, 0, N, N, screenDc, cx - kHalf, cy - kHalf, SRCCOPY) != FALSE) {
            const auto* px = static_cast<const uint32_t*>(bits);
            for (int i = 0; i < N * N; ++i) {
                pv.loupePixels[i] = px[i] & 0x00FFFFFFU;
            }
            const uint32_t center = pv.loupePixels[kHalf * N + kHalf];
            pv.sampledR = static_cast<uint8_t>((center >> 16) & 0xFFU);
            pv.sampledG = static_cast<uint8_t>((center >> 8) & 0xFFU);
            pv.sampledB = static_cast<uint8_t>(center & 0xFFU);
        } else {
            (void)mask::triggers::PixelTriggerEngine::SamplePixelAt(cx, cy, pv.sampledR, pv.sampledG, pv.sampledB);
        }
        SelectObject(memDc, oldObj);
    } else {
        (void)mask::triggers::PixelTriggerEngine::SamplePixelAt(cx, cy, pv.sampledR, pv.sampledG, pv.sampledB);
    }
    if (dib != nullptr) DeleteObject(dib);
    if (memDc != nullptr) DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    std::memset(pv.loupeMatchMask, 0, sizeof(pv.loupeMatchMask));
    if (model_.HasSelection() && model_.selectedKind == ItemKind::Trigger) {
        const auto& t = model_.SelectedTrigger();
        const int rad = std::clamp<int>(t.sampleRadius, 0, kHalf);
        for (int py = -rad; py <= rad; ++py) {
            for (int px = -rad; px <= rad; ++px) {
                const size_t idx = static_cast<size_t>((py + kHalf) * N + (px + kHalf));
                pv.loupeMatchMask[idx] = 1U;
            }
        }
        const int dr = std::abs(static_cast<int>(pv.sampledR) - static_cast<int>(t.r));
        const int dg = std::abs(static_cast<int>(pv.sampledG) - static_cast<int>(t.g));
        const int db = std::abs(static_cast<int>(pv.sampledB) - static_cast<int>(t.b));
        pv.colorDelta = static_cast<uint32_t>((std::max)({dr, dg, db}));
    }
}

void App::CancelTriggerEyedropper(HWND hwnd) noexcept {
    if (!model_.triggerColorPickActive) return;
    model_.triggerColorPickActive = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    UpdateTriggerPreviewState(hwnd);
}

void App::FinishTriggerEyedropperPick(HWND hwnd) noexcept {
    if (!model_.triggerColorPickActive) return;
    POINT pt{};
    GetCursorPos(&pt);
    uint8_t r = 0U, g = 0U, b = 0U;
    const bool ok = mask::triggers::PixelTriggerEngine::SamplePixelAt(pt.x, pt.y, r, g, b);
    CancelTriggerEyedropper(hwnd);
    if (ok && model_.selectedKind == ItemKind::Trigger && model_.HasSelection()) {
        model_.RefreshDevicesAndMonitors(&remapEngine_);
        auto& t = model_.SelectedTrigger();
        t.r = r;
        t.g = g;
        t.b = b;
        const int monCount = (std::max)(1, (std::min)(model_.monitorCount, 8));
        int matchedMon = std::clamp<int>(t.monitorIndex, 0, monCount - 1);
        for (int m = 0; m < model_.monitorCount; ++m) {
            const auto& mon = model_.monitors[static_cast<size_t>(m)];
            if (pt.x >= mon.left && pt.x < mon.right &&
                pt.y >= mon.top  && pt.y < mon.bottom) {
                matchedMon = m;
                break;
            }
        }
        const auto& mon = model_.monitors[static_cast<size_t>(matchedMon)];
        const int32_t w = (std::max<int32_t>)(1, mon.right - mon.left);
        const int32_t h = (std::max<int32_t>)(1, mon.bottom - mon.top);
        t.monitorIndex = static_cast<uint8_t>(matchedMon);
        t.sampleX = std::clamp<int32_t>(pt.x - mon.left, 0, w - 1);
        t.sampleY = std::clamp<int32_t>(pt.y - mon.top,  0, h - 1);
        SyncSelectedTrigger();
        wchar_t msg[96]{};
        std::swprintf(msg, std::size(msg), L"Picked color on Screen %d (%d, %d).",
                      matchedMon + 1, static_cast<int>(t.sampleX), static_cast<int>(t.sampleY));
        ShowToast(hwnd, msg);
    }
}

void App::QueueCommandApproval(uint8_t seqIdx, uint16_t stepIdx, bool isShell, const wchar_t* text) noexcept {
    const HWND hwnd = mask::input::GameInputGlobalContext::uiWindowHandle.load(std::memory_order_acquire);
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        seqEngine_.ResolveCommandApproval(seqIdx, false);
        return;
    }

    bool queued = false;
    AcquireSRWLockExclusive(&approvalLock_);
    if (approvalCount_ < approvalQueue_.size()) {
        auto& dst = approvalQueue_[approvalTail_];
        dst.seqIdx  = seqIdx;
        dst.stepIdx = stepIdx;
        dst.isShell = isShell;
        if (text != nullptr) {
            wcsncpy_s(dst.text, text, _TRUNCATE);
        } else {
            dst.text[0] = L'\0';
        }
        approvalTail_ = (approvalTail_ + 1U) % approvalQueue_.size();
        ++approvalCount_;
        queued = true;
    }
    ReleaseSRWLockExclusive(&approvalLock_);

    if (!queued || !PostMessageW(hwnd, WM_APP_APPROVE_IMPORTED_CMD, 0, 0)) {
        seqEngine_.ResolveCommandApproval(seqIdx, false);
    }
}

void App::ProcessPendingCommandApprovals(HWND hwnd) noexcept {
    if (commandApprovalDialogActive_) return;
    commandApprovalDialogActive_ = true;

    for (;;) {
        PendingCommandApproval item{};
        bool hasItem = false;
        AcquireSRWLockExclusive(&approvalLock_);
        if (approvalCount_ > 0U) {
            item = approvalQueue_[approvalHead_];
            approvalHead_ = (approvalHead_ + 1U) % approvalQueue_.size();
            --approvalCount_;
            hasItem = true;
        }
        ReleaseSRWLockExclusive(&approvalLock_);

        if (!hasItem) break;

        if (!IsWindow(hwnd) || !sharedState_.engineRunning.load(std::memory_order_acquire)) {
            seqEngine_.ResolveCommandApproval(item.seqIdx, false);
            continue;
        }

        if (hiddenToTray_ || IsIconic(hwnd)) {
            RestoreFromTray(hwnd);
        }

        wchar_t seqName[32]{L"Imported sequence"};
        if (item.seqIdx < mask::core::kMaxComplexSequences) {
            wcsncpy_s(seqName, model_.sequences[item.seqIdx].name, _TRUNCATE);
        }
        const wchar_t* kindStr = item.isShell ? L"Shell command (cmd)" : L"Launch program";
        const wchar_t* cmdText = (item.text[0] != L'\0') ? item.text : L"(empty command)";

        wchar_t msg[512]{};
        std::swprintf(
            msg,
            std::size(msg),
            L"This sequence came from an imported JSON profile and is attempting to execute a command:\n\n"
            L"Sequence: %ls (Step %u)\n"
            L"Action: %ls\n"
            L"Command: %ls\n\n"
            L"Do you trust this imported sequence and want to run this command?",
            seqName,
            static_cast<unsigned>(item.stepIdx + 1U),
            kindStr,
            cmdText);

        const int res = MessageBoxW(
            hwnd,
            msg,
            L"Mask \u2014 Confirm Imported Command",
            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND);

        const bool approved = (res == IDYES);
        if (approved && item.seqIdx < mask::core::kMaxComplexSequences) {
            mask::core::SetSequenceUntrustedImport(model_.sequences[item.seqIdx], false);
            model_.MarkDirty();
            (void)model_.SaveToDisk();
        } else if (!approved) {
            ShowToast(hwnd, L"Blocked unapproved command in imported sequence.");
        }

        seqEngine_.ResolveCommandApproval(item.seqIdx, approved);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    commandApprovalDialogActive_ = false;
}

LRESULT CALLBACK App::MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = (cs != nullptr) ? static_cast<App*>(cs->lpCreateParams) : activeInstance_;
        if (self != nullptr) {
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            self = activeInstance_;
        }
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    const LRESULT res = self->HandleMessage(hwnd, msg, wParam, lParam);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return res;
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    switch (msg) {
        case WM_CREATE:
            renderer_.Initialize(hwnd);
            RefreshAppIcons(hwnd);
            ApplyWin11Chrome(hwnd);
            WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
            AddTrayIcon(hwnd);
            animator_.SetQuality(model_.settings.animationQuality);
            SyncAnimatorToggles(true);
            animator_.SetSettingsOpen(model_.showSettings, true);
            animator_.TriggerAppOpen();
            UpdateUiTimerCadence(hwnd);
            EnsureAnimTimer(hwnd);
            return 0;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                windowFocused_ = false;
                animator_.SetCursorPosition(animator_.cursorX.value, animator_.cursorY.value, false, true);
                animator_.OnPointerUp();
                animator_.appOpen.Snap(animator_.appOpen.target);
                animator_.viewTransition.Snap(animator_.viewTransition.target);
                animator_.SnapAll();
                int finishedDelId = -1;
                if (animator_.ConsumeFinishedDelete(finishedDelId)) {
                    FinalizeItemDelete(hwnd, finishedDelId);
                }
                if (model_.settings.minimizeToTray) {
                    HideToTray(hwnd);
                } else {
                    renderer_.TrimGpu();
                    UpdateUiTimerCadence(hwnd);
                    StartTrimTimerIfEnabled(hwnd);
                }
            } else {
                renderer_.Resize(LOWORD(lParam), HIWORD(lParam));
                CancelTrimTimer(hwnd);
                settingsTabSnapped_ = false;
                sidebarIndSnapped_  = false;
                UpdateUiTimerCadence(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_DPICHANGED: {
            renderer_.SetDpi(static_cast<float>(HIWORD(wParam)));
            RefreshAppIcons(hwnd);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested != nullptr) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            settingsTabSnapped_ = false;
            sidebarIndSnapped_  = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd);
            const float scale = static_cast<float>(dpi > 0U ? dpi : 96U) / 96.0f;
            RECT rc{ 0, 0, static_cast<LONG>(kMinWidthDip * scale), static_cast<LONG>(kMinHeightDip * scale) };
            AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi > 0U ? dpi : 96U);
            mmi->ptMinTrackSize.x = rc.right - rc.left;
            mmi->ptMinTrackSize.y = rc.bottom - rc.top;
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            if (!IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_) {
                lastUiPhysHeldMask_ = SelectedSlotPhysHeldMask();
                lastUiTelemetrySeq_ = sharedState_.telemetrySequence.load(std::memory_order_acquire);
                renderer_.Render(model_, BuildFrameState(hwnd), animator_);

                animator_.ClampScrollTargets(renderer_.ListMaxScroll(), renderer_.SeqStepMaxScroll());
                model_.listScroll    = animator_.listScrollSmooth.value;
                model_.seqStepScroll = animator_.seqScrollSmooth.value;

                if (model_.showSettings) {
                    sidebarIndSnapped_ = false;
                    float tx = 0.0f, tw = 80.0f;
                    if (renderer_.GetSettingsTabRect(model_.settingsTab, tx, tw)) {
                        const bool snapTab = !settingsTabSnapped_ || !windowFocused_;
                        animator_.SetSettingsTab(tx, tw, snapTab);
                        settingsTabSnapped_ = true;
                    }
                } else {
                    settingsTabSnapped_ = false;
                    float indY = 0.0f, indH = 0.0f;
                    if (renderer_.GetSelectedSidebarRowRect(indY, indH)) {
                        const bool snapInd = !sidebarIndSnapped_ || !windowFocused_;
                        animator_.SetSidebarIndicator(indY, indH, snapInd);
                        sidebarIndSnapped_ = true;
                    } else {
                        animator_.SetSidebarIndicator(0.0f, 0.0f, true);
                        sidebarIndSnapped_ = false;
                    }
                }
                EnsureAnimTimer(hwnd);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                if (model_.seqColorPickActive || model_.triggerColorPickActive) {
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                    return TRUE;
                }
                const UiAction h = model_.hoveredAction;
                if (model_.draggingDuty || h.kind == ActionKind::DutySlider ||
                    !model_.draggingTriggerSlider.IsNone() || h.kind == ActionKind::TrigSliderTol) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                } else if (h.kind == ActionKind::FieldFocus) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                } else if (h.IsNone() || h.kind == ActionKind::ModalSurface || h.kind == ActionKind::PopoverBackdrop) {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                }
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_MOUSEMOVE: {
            const int xPx = GET_X_LPARAM(lParam);
            const int yPx = GET_Y_LPARAM(lParam);
            const float scale = renderer_.Scale();
            const float dipX = static_cast<float>(xPx) / scale;
            const float dipY = static_cast<float>(yPx) / scale;

            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            if (windowFocused_) {
                animator_.SetCursorPosition(dipX, dipY, true);
            }

            if (model_.triggerColorPickActive) {
                POINT pt{};
                if (GetCursorPos(&pt)) {
                    SampleTriggerEyedropperLoupe(pt.x, pt.y);
                }
                EnsureAnimTimer(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (model_.draggingDuty) {
                ApplyDutyFromPointer(xPx);
                EnsureAnimTimer(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!model_.draggingTriggerSlider.IsNone()) {
                ApplyTriggerSliderFromPointer(xPx);
                EnsureAnimTimer(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const UiAction hit = renderer_.HitTest(dipX, dipY);
            if (hit != model_.hoveredAction) {
                model_.hoveredAction = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            EnsureAnimTimer(hwnd);
            return 0;
        }

        case WM_MOUSELEAVE:
            animator_.OnPointerUp();
            animator_.SetCursorPosition(animator_.cursorX.target, animator_.cursorY.target, false, !windowFocused_);
            EnsureAnimTimer(hwnd);
            if (!model_.hoveredAction.IsNone() && !model_.draggingDuty && model_.draggingTriggerSlider.IsNone()) {
                model_.hoveredAction = UiAction{};
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            if (model_.triggerColorPickActive) {
                FinishTriggerEyedropperPick(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (model_.seqColorPickActive) {
                model_.seqColorPickActive = false;
                ReleaseCapture();
                POINT pt{};
                GetCursorPos(&pt);
                uint8_t r = 0, g = 0, b = 0;
                if (mask::triggers::PixelTriggerEngine::SamplePixelAt(pt.x, pt.y, r, g, b) &&
                    model_.selectedKind == ItemKind::Sequence) {
                    auto& seq = model_.SelectedSeq();
                    if (model_.selectedSeqStep >= 0 && model_.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                        auto& st = seq.steps[static_cast<size_t>(model_.selectedSeqStep)];
                        st.x = pt.x;
                        st.y = pt.y;
                        st.r = r;
                        st.g = g;
                        st.b = b;
                        SyncSelectedSequence();
                        wchar_t pickMsg[96]{};
                        std::swprintf(pickMsg, std::size(pickMsg), L"Picked color at (%d, %d).",
                                      static_cast<int>(pt.x), static_cast<int>(pt.y));
                        ShowToast(hwnd, pickMsg);
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            const int xPx = GET_X_LPARAM(lParam);
            const int yPx = GET_Y_LPARAM(lParam);
            const float scale = renderer_.Scale();
            const float dipX = static_cast<float>(xPx) / scale;
            const float dipY = static_cast<float>(yPx) / scale;

            animator_.TriggerRipple(dipX, dipY, mask::ui::GetAccentPalette(model_.settings.accentPreset).signalHi);
            const UiAction hit = renderer_.HitTest(dipX, dipY);
            if (!hit.IsNone() && hit.kind != ActionKind::ModalSurface && hit.kind != ActionKind::PopoverBackdrop) {
                animator_.OnPointerDown(EncodeUiActionKey(hit));
            }
            EnsureAnimTimer(hwnd);

            const uint8_t bindTarget = sharedState_.captureBindTarget.load(std::memory_order_acquire);
            if (bindTarget != 0U && hit == BindActionFor(bindTarget)) {
                AssignCapturedKey(hwnd, VK_LBUTTON);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.kind == ActionKind::DutySlider && model_.HasSelection() && model_.selectedKind == ItemKind::Autoclicker) {
                CommitEdit();
                if (bindTarget != 0U) CancelBinding();
                model_.draggingDuty = true;
                SetCapture(hwnd);
                ApplyDutyFromPointer(xPx);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.kind == ActionKind::TrigSliderTol && model_.HasSelection() && model_.selectedKind == ItemKind::Trigger) {
                CommitEdit();
                if (bindTarget != 0U) CancelBinding();
                model_.draggingTriggerSlider = hit;
                SetCapture(hwnd);
                ApplyTriggerSliderFromPointer(xPx);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (!hit.IsNone()) {
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                DispatchAction(hwnd, hit, shift, ctrl);
                if (hit.kind == ActionKind::ModalSurface) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else {
                if (model_.editField != Field::None) CommitEdit();
                if (bindTarget != 0U) {
                    CancelBinding();
                    ShowToast(hwnd, L"Cancelled. Nothing was changed.");
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            animator_.OnPointerUp();
            EnsureAnimTimer(hwnd);
            if (model_.draggingDuty || !model_.draggingTriggerSlider.IsNone()) {
                ReleaseCapture();
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (model_.draggingDuty) {
                model_.draggingDuty = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!model_.draggingTriggerSlider.IsNone()) {
                model_.draggingTriggerSlider = UiAction{};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (model_.seqColorPickActive) {
                model_.seqColorPickActive = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (model_.triggerColorPickActive) {
                model_.triggerColorPickActive = false;
                UpdateTriggerPreviewState(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            const float scale = renderer_.Scale();
            const float dipX = static_cast<float>(pt.x) / scale;
            const float dipY = static_cast<float>(pt.y) / scale;
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int dir = (delta > 0) ? 1 : -1;
            const WORD keys = GET_KEYSTATE_WPARAM(wParam);
            const UiAction hit = renderer_.HitTest(dipX, dipY);
            const bool onField = (hit.kind == ActionKind::FieldFocus) &&
                                 !IsTextField(static_cast<Field>(hit.index));
            const bool snapScroll = !windowFocused_ ||
                model_.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off);

            if (onField || (hit.kind == ActionKind::DutySlider && model_.selectedKind == ItemKind::Autoclicker)) {
                const Field f = onField ? static_cast<Field>(hit.index) : Field::Duty;
                if (model_.editField == f) CancelEdit();
                StepField(f, dir, (keys & MK_SHIFT) != 0, (keys & MK_CONTROL) != 0);
            } else if (model_.selectedKind == ItemKind::Trigger && hit.kind == ActionKind::TrigSliderTol) {
                auto& t = model_.SelectedTrigger();
                const int step = ((keys & MK_SHIFT) != 0 ? 10 : 1) * dir;
                t.tolerance = static_cast<uint8_t>(std::clamp(static_cast<int>(t.tolerance) + step, 0, 150));
                SyncSelectedTrigger();
            } else if (renderer_.PointInSeqList(dipX, dipY)) {
                const float pitch = UiRenderer::kSeqStepRowH + 4.0f;
                const float nextScroll = std::clamp(
                    animator_.seqScrollSmooth.target - static_cast<float>(delta) / 120.0f * pitch * 2.0f,
                    0.0f, renderer_.SeqStepMaxScroll());
                animator_.SetSeqScrollTarget(nextScroll, snapScroll);
                if (snapScroll) model_.seqStepScroll = animator_.seqScrollSmooth.value;
                EnsureAnimTimer(hwnd);
            } else if (renderer_.PointInList(dipX, dipY)) {
                const float pitch = renderer_.CurrentListRowHeight() + UiRenderer::kListRowGap;
                const float nextScroll = std::clamp(
                    animator_.listScrollSmooth.target - static_cast<float>(delta) / 120.0f * pitch,
                    0.0f, renderer_.ListMaxScroll());
                animator_.SetListScrollTarget(nextScroll, snapScroll);
                if (snapScroll) model_.listScroll = animator_.listScrollSmooth.value;
                EnsureAnimTimer(hwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                if (model_.triggerColorPickActive) {
                    CancelTriggerEyedropper(hwnd);
                    ShowToast(hwnd, L"Color pick cancelled.");
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (model_.seqColorPickActive) {
                    model_.seqColorPickActive = false;
                    ReleaseCapture();
                    ShowToast(hwnd, L"Color pick cancelled.");
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (model_.showStepKindMenu) {
                    model_.showStepKindMenu = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (model_.showNewPicker) {
                    model_.showNewPicker = false;
                    animator_.SetNewPickerOpen(false);
                    EnsureAnimTimer(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (model_.showSettings && model_.editField == Field::None &&
                    sharedState_.captureBindTarget.load(std::memory_order_relaxed) == 0U) {
                    model_.showSettings = false;
                    settingsHwTabOpen_.store(false, std::memory_order_release);
                    animator_.SetSettingsOpen(false);
                    animator_.SetSettingsTab(0.0f, 0.0f, true);
                    settingsTabSnapped_ = false;
                    sidebarIndSnapped_  = false;
                    animator_.TriggerViewTransition();
                    EnsureAnimTimer(hwnd);
                    UpdateTriggerPreviewState(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (model_.editField != Field::None && IsTextField(model_.editField) &&
                wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                const size_t maxLen = (model_.editField == Field::SeqStepText)
                    ? 127U
                    : ((model_.editField == Field::Name) ? 23U : 31U);
                PasteClipboardIntoEditBuffer(maxLen);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const bool busy = model_.editField != Field::None ||
                              sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U ||
                              GetTickCount64() < suppressKeyNavUntil_ ||
                              model_.showNewPicker ||
                              model_.showStepKindMenu ||
                              model_.triggerColorPickActive;
            if (!busy && !model_.showSettings && model_.HasSelection()) {
                if (wParam == VK_UP)   { SelectNeighbour(hwnd, -1); InvalidateRect(hwnd, nullptr, FALSE); }
                if (wParam == VK_DOWN) { SelectNeighbour(hwnd, +1); InvalidateRect(hwnd, nullptr, FALSE); }
                if (wParam == VK_F2) {
                    if (model_.selectedKind == ItemKind::Autoclicker)  BeginEdit(Field::Name);
                    else if (model_.selectedKind == ItemKind::Sequence) BeginEdit(Field::SeqName);
                    else if (model_.selectedKind == ItemKind::Trigger)  BeginEdit(Field::TriggerName);
                    else if (model_.selectedKind == ItemKind::Remap)    BeginEdit(Field::RemapName);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_CHAR: {
            if (model_.editField == Field::None) return 0;
            const wchar_t ch = static_cast<wchar_t>(wParam);
            const size_t len = std::wcslen(model_.editBuffer);
            const Field f = model_.editField;
            const bool isText = IsTextField(f);
            size_t maxLen = 16U;
            if (f == Field::Name) maxLen = std::size(model_.SelectedAutoclicker().name) - 1U;
            else if (f == Field::SeqName || f == Field::RemapName || f == Field::TriggerName) maxLen = 31U;
            else if (f == Field::SeqStepText) maxLen = 127U;

            if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                CommitEdit();
            } else if (ch == 27) {
                CancelEdit();
            } else if (ch == L'\b') {
                if (len > 0U) model_.editBuffer[len - 1U] = L'\0';
            } else if (ch < 0x20 || ch == 0x7F) {
                // ignore control chars
            } else if (len < maxLen) {
                bool accept = isText;
                if (!isText) {
                    accept = (ch >= L'0' && ch <= L'9') ||
                             (AllowsDecimal(f) && (ch == L'.' || ch == L',') && std::wcschr(model_.editBuffer, L'.') == nullptr) ||
                             (AllowsNegative(f) && ch == L'-' && len == 0U);
                }
                if (accept) {
                    model_.editBuffer[len] = (ch == L',') ? L'.' : ch;
                    model_.editBuffer[len + 1U] = L'\0';
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                windowFocused_ = false;
                model_.showStepKindMenu = false;
                if (model_.editField != Field::None) CommitEdit();
                if (model_.triggerColorPickActive) CancelTriggerEyedropper(hwnd);
                if (model_.draggingDuty || !model_.draggingTriggerSlider.IsNone()) ReleaseCapture();
                animator_.OnPointerUp();
                animator_.SetCursorPosition(animator_.cursorX.value, animator_.cursorY.value, false, true);
                if (model_.settings.lowResourceWhenUnfocused) {
                    animator_.appOpen.Snap(animator_.appOpen.target);
                    animator_.viewTransition.Snap(animator_.viewTransition.target);
                    animator_.SnapAll();
                    int finishedDelId = -1;
                    if (animator_.ConsumeFinishedDelete(finishedDelId)) {
                        FinalizeItemDelete(hwnd, finishedDelId);
                    }
                }
                UpdateUiTimerCadence(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                windowFocused_ = true;
                CancelTrimTimer(hwnd);
                UpdateUiTimerCadence(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_APP_TRAY_ICON:
            if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                RestoreFromTray(hwnd);
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayContextMenu(hwnd);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kTrayCmdOpen:    RestoreFromTray(hwnd); break;
                case kTrayCmdPause:   DispatchAction(hwnd, UiAction{ ActionKind::TogglePause }, false, false); break;
                case kTrayCmdStopAll: DispatchAction(hwnd, UiAction{ ActionKind::StopAll }, false, false); break;
                case kTrayCmdExit:    PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
                default: break;
            }
            return 0;

        case WM_APP_RESTORE_WINDOW:
            RestoreFromTray(hwnd);
            return 0;

        case WM_APP_APPROVE_IMPORTED_CMD:
            ProcessPendingCommandApprovals(hwnd);
            return 0;

        case WM_APP_TOGGLE_MACRO_SLOT: {
            const int idx = static_cast<int>(wParam);
            if (idx >= 0 && idx < kSlotCount &&
                model_.IsAutoclickerInUse(idx) &&
                model_.slots[static_cast<size_t>(idx)].triggerVk != 0U) {
                auto& s = model_.slots[static_cast<size_t>(idx)];
                s.enabled = !s.enabled;
                animator_.SetToggle(AnimSlotFor(ItemKind::Autoclicker, idx), s.enabled);
                model_.SyncSlotToEngine(idx, false);
                EnsureAnimTimer(hwnd);
                if (!IsIconic(hwnd) && IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case mask::input::WM_APP_KEY_CAPTURED:
            OnKeyCaptured(hwnd, static_cast<uint16_t>(wParam & 0xFFU), lParam != 0);
            return 0;

        case WM_APP_ENGINE_STATE: {
            uiNotifyPending_.store(false, std::memory_order_release);
            const bool nowPaused   = sharedState_.automationPaused.load(std::memory_order_acquire);
            const uint8_t nowLayer = sharedState_.activeRemapLayer.load(std::memory_order_acquire);
            const bool nowUnlocked = sharedState_.remapUnlocked.load(std::memory_order_acquire);
            const bool visible = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_;
            const bool lowPowerUnfocused = !windowFocused_ && model_.settings.lowResourceWhenUnfocused;
            bool stateChanged = false;

            if (nowPaused != lastObservedPaused_) {
                lastObservedPaused_ = nowPaused;
                stateChanged = true;
                if (nowPaused) {
                    OnWatchdogAuxCleanupStatic();
                    PlayFeedbackTone(false);
                } else {
                    remapEngine_.RecomputeSuppressedMask();
                    pixelEngine_.ResetArmedStates();
                    PlayFeedbackTone(true);
                }
            }
            if (nowLayer != lastObservedLayer_ || nowUnlocked != lastObservedUnlocked_) {
                lastObservedLayer_    = nowLayer;
                lastObservedUnlocked_ = nowUnlocked;
                stateChanged = true;
                remapEngine_.RecomputeSuppressedMask();
            }

            const mask::core::TelemetrySnapshot tel = sharedState_.ReadTelemetry();
            const uint64_t firingMask = tel.activeSlotBitmask | tel.toggledSlotBitmask;
            const uint16_t seqMask    = seqEngine_.GetActiveSequenceBitmask();
            if (firingMask != lastFiringMask_ || seqMask != lastFiringSeqMask_) {
                lastFiringMask_    = firingMask;
                lastFiringSeqMask_ = seqMask;
                stateChanged = true;
                const bool snapPulse = !visible || lowPowerUnfocused;
                for (int i = 0; i < kSlotCount; ++i) {
                    animator_.SetFiring(AnimSlotFor(ItemKind::Autoclicker, i),
                                        (firingMask & (1ULL << i)) != 0ULL, snapPulse);
                }
                for (int i = 0; i < kComplexSeqCount; ++i) {
                    animator_.SetFiring(AnimSlotFor(ItemKind::Sequence, i),
                                        (seqMask & (1U << i)) != 0U, snapPulse);
                }
                if (!snapPulse) {
                    EnsureAnimTimer(hwnd);
                }
            }

            UpdateTrayTooltip(hwnd);
            if (visible && (!lowPowerUnfocused || stateChanged)) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_APP_OUTPUT_STATE: {
            const auto state = static_cast<OutputState>(wParam);
            auto& host = mask::bridge::HIDMaestroHost::Instance();
            wcsncpy_s(model_.outputMessage, host.GetLastStatusMessage(), _TRUNCATE);
            bool hmLive = false;
            uint8_t kbLeds = 0;
            host.QueryDriverTelemetry(
                hmLive,
                model_.processElevated,
                model_.driverInstalled,
                model_.mouseTransportIdx,
                model_.kbTransportIdx,
                model_.deliversEveryReport,
                model_.releasesOnExit,
                kbLeds);
            model_.outputState = state;
            if (state == OutputState::Ready) {
                if (autoReconnectAttempts_ > 0) {
                    ShowToast(hwnd, L"Virtual keyboard and mouse connected again.");
                }
                autoReconnectAttempts_ = 0;
            } else if (state == OutputState::NeedsAdmin) {
                ShowToast(hwnd, L"Virtual devices need administrator rights to send input.");
            } else if (state == OutputState::Unavailable) {
                ShowToast(hwnd, model_.outputMessage[0] != L'\0'
                              ? model_.outputMessage
                              : L"Virtual keyboard and mouse couldn't be started.");
            }
            if (!IsIconic(hwnd) && IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kAnimTimerId) {
                OnAnimTimer(hwnd);
            } else if (wParam == kUiTimerId) {
                OnUiTimer(hwnd);
            } else if (wParam == kBindGraceTimerId) {
                OnBindGraceTimer(hwnd);
            } else if (wParam == kTriggerPreviewTimerId) {
                if (model_.triggerColorPickActive) {
                    POINT pt{};
                    if (GetCursorPos(&pt)) SampleTriggerEyedropperLoupe(pt.x, pt.y);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (!IsIconic(hwnd) && IsWindowVisible(hwnd) && !hiddenToTray_ &&
                           (windowFocused_ || !model_.settings.lowResourceWhenUnfocused) &&
                           !model_.showSettings && model_.selectedKind == ItemKind::Trigger) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (wParam == kTrimMemTimerId) {
                KillTimer(hwnd, kTrimMemTimerId);
                trimTimerActive_ = false;
                if (model_.settings.trimMemoryWhenMinimized && (IsIconic(hwnd) || hiddenToTray_)) {
                    EmptyWorkingSet(GetCurrentProcess());
                }
            }
            return 0;

        case WM_WTSSESSION_CHANGE:
            if (model_.settings.stopOnSessionLock &&
                (wParam == WTS_SESSION_LOCK || wParam == WTS_CONSOLE_DISCONNECT)) {
                StopEverythingRunning();
            }
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND) {
                StopEverythingRunning();
                if (model_.saveState == SaveState::Pending) (void)model_.SaveToDisk();
            }
            return TRUE;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wParam != FALSE) {
                CommitEdit();
                SaveCurrentWindowBounds(hwnd);
                (void)model_.SaveToDisk();
                StopEverythingRunning();
            }
            return 0;

        case WM_CLOSE:
            CommitEdit();
            SaveCurrentWindowBounds(hwnd);
            (void)model_.SaveToDisk();
            CancelTriggerEyedropper(hwnd);
            if (previewActiveSent_) {
                pixelEngine_.SetPreviewActive(false, 0U);
                previewActiveSent_ = false;
            }
            sharedState_.engineRunning.store(false, std::memory_order_release);
            mask::input::WakeEngine();
            mask::input::WakeWatchdog();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kUiTimerId);
            KillTimer(hwnd, kBindGraceTimerId);
            KillTimer(hwnd, kAnimTimerId);
            KillTimer(hwnd, kTrimMemTimerId);
            KillTimer(hwnd, kTriggerPreviewTimerId);
            uiTimerMs_             = 0U;
            triggerPreviewTimerMs_ = 0U;
            animTimerActive_       = false;
            trimTimerActive_       = false;
            RemoveTrayIcon(hwnd);
            WTSUnRegisterSessionNotification(hwnd);
            mask::input::GameInputGlobalContext::uiWindowHandle.store(nullptr, std::memory_order_release);
            renderer_.Shutdown();
            sharedState_.engineRunning.store(false, std::memory_order_release);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace mask::app
