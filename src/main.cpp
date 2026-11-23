#include "core_contracts.hpp"
#include "timing_engine.hpp"
#include "hidmaestro_bridge.hpp"
#include "input_pipeline.hpp"
#include "macro_runtime.hpp"
#include "watchdog.hpp"
#include "app_settings.hpp"
#include "pixel_trigger_engine.hpp"
#include "complex_sequence_engine.hpp"
#include "remap_engine.hpp"
#include "ui_anim.hpp"
#include "app_model.hpp"
#include "app_view.hpp"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <psapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <GameInput.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>
#include <thread>
#include <atomic>

namespace {

using namespace mask::app;

// =====================================================================================
//  App-wide constants & state
// =====================================================================================

constexpr UINT_PTR  kUiTimerId               = 1001;
constexpr UINT_PTR  kBindGraceTimerId        = 1002;
constexpr UINT_PTR  kAnimTimerId             = 1003; // 16ms ONLY while focused & animating (0% idle CPU)
constexpr UINT_PTR  kTrimMemTimerId          = 1004; // 5s one-shot working-set trim when minimized
constexpr UINT_PTR  kTriggerPreviewTimerId   = 1005; // 50ms ONLY when visible & viewing a Trigger Bot
constexpr UINT      kUiFocusedIntervalMs     = 250;
constexpr UINT      kUiBackgroundIntervalMs  = 1000;
constexpr UINT      kAnimIntervalMs          = 16;
constexpr UINT      kTrimMemDelayMs          = 5000;
constexpr UINT      kTrigLiveIntervalMs      = 50;
constexpr UINT      kTrigDropperIntervalMs   = 16;

constexpr UINT      WM_APP_ENGINE_STATE      = WM_APP + 43;
constexpr UINT      WM_APP_OUTPUT_STATE      = WM_APP + 44;
constexpr UINT      WM_APP_TRAY_ICON         = WM_APP + 45;
constexpr UINT      WM_APP_TOGGLE_MACRO_SLOT = WM_APP + 46;
constexpr UINT      WM_APP_RESTORE_WINDOW    = WM_APP + 47;

constexpr UINT      kTrayIconId              = 1;
constexpr UINT      kTrayCmdOpen             = 40001;
constexpr UINT      kTrayCmdPause            = 40003;
constexpr UINT      kTrayCmdStopAll          = 40004;
constexpr UINT      kTrayCmdExit             = 40005;

constexpr ULONGLONG kToastLifetimeMs         = 5500;
constexpr uint32_t  kOutputFailureLimit      = 5;
constexpr const wchar_t* kWindowClass        = L"MaskMacroEngineWindow";
constexpr const wchar_t* kWindowTitle        = L"Mask";
constexpr const wchar_t* kMutexName          = L"Local\\MaskMacroEngine.SingleInstance";

constexpr float kDefaultWidthDip  = 1160.0f;
constexpr float kDefaultHeightDip = 760.0f;
constexpr float kMinWidthDip      = 980.0f;
constexpr float kMinHeightDip     = 690.0f;

std::atomic<bool>                         g_UiNotifyPending{false};
std::atomic<bool>                         g_SettingsHwTabOpen{false};
mask::safety::Watchdog*                   g_WatchdogPtr{nullptr};
mask::triggers::PixelTriggerEngine*       g_PixelEnginePtr{nullptr};
mask::workflows::ComplexSequenceEngine*   g_SeqEnginePtr{nullptr};
mask::remap::RemapEngine*                 g_RemapEnginePtr{nullptr};

uint32_t  g_LastUiPhysHeldMask{0};
uint64_t  g_LastUiTelemetrySeq{0};
uint16_t  g_LastActiveSeqMask{0};
uint8_t   g_LastActiveRemapLayer{0};
bool      g_LastRemapUnlocked{true};
bool      g_LastObservedPaused{false};
uint8_t   g_LastObservedLayer{0};
bool      g_LastObservedUnlocked{true};

HICON     g_IconLarge{nullptr};
HICON     g_IconSmall{nullptr};
bool      g_TrayAdded{false};
bool      g_HiddenToTray{false};
bool      g_WindowFocused{true};
bool      g_AnimTimerActive{false};
bool      g_TrimTimerActive{false};
bool      g_SettingsTabSnapped{false};
bool      g_SidebarIndSnapped{false};
uint64_t  g_LastFiringMask{0};
uint16_t  g_LastFiringSeqMask{0};
UINT      g_TriggerPreviewTimerMs{0};
bool      g_PreviewActiveSent{false};
uint8_t   g_PreviewSlotSent{0};

// Virtual device output worker
std::thread       g_OutputWorker{};
std::atomic<bool> g_OutputWorkerBusy{false};
int               g_AutoReconnectAttempts{0};
ULONGLONG         g_NextAutoReconnectTick{0};
ULONGLONG         g_SuppressKeyNavUntil{0};

uint16_t          g_BindGraceVk{0};
ULONGLONG         g_BindGraceUntil{0};

// Forward declarations
void EnsureAnimTimer(HWND hwnd) noexcept;
void UpdateUiTimerCadence(HWND hwnd) noexcept;
void UpdateTriggerPreviewState(HWND hwnd) noexcept;
void ApplyWin11Chrome(HWND hwnd) noexcept;
void RefreshAppIcons(HWND hwnd) noexcept;
void UpdateTrayTooltip(HWND hwnd) noexcept;
void RestoreFromTray(HWND hwnd) noexcept;
void HideToTray(HWND hwnd) noexcept;
void SyncAllEnginesFromModel() noexcept;
void SyncAnimatorToggles(bool snap) noexcept;

// =====================================================================================
//  Cross-thread notifications & engine callbacks
// =====================================================================================

void NotifyUi() noexcept {
    const HWND hwnd = mask::input::GameInputGlobalContext::uiWindowHandle.load(std::memory_order_acquire);
    if (hwnd == nullptr) return;
    if (!g_UiNotifyPending.exchange(true, std::memory_order_acq_rel)) {
        if (!PostMessageW(hwnd, WM_APP_ENGINE_STATE, 0, 0)) {
            g_UiNotifyPending.store(false, std::memory_order_release);
        }
    }
}

void PlayFeedbackTone(bool high) noexcept {
    if (!g_Ui.settings.soundFeedback) return;
    MessageBeep(high ? MB_OK : MB_ICONASTERISK);
}

void OnWatchdogSafeStop(mask::core::SafeStopReason /*reason*/) noexcept {
    NotifyUi();
}

void OnWatchdogAuxCleanup() noexcept {
    if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->ResetStateAndSuppression();
    if (g_SeqEnginePtr != nullptr)   g_SeqEnginePtr->AbortAll();
    if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->ResetArmedStates();
}

void OnPhysicalDeviceTransitionCallback(const uint8_t deviceId[32], uint8_t deviceKind, uint16_t vk,
                                        bool isDown, uint64_t nowTsc) noexcept {
    if (g_RemapEnginePtr != nullptr) {
        g_RemapEnginePtr->OnPhysicalDeviceTransition(deviceId, deviceKind, vk, isDown, nowTsc);
    }
    if (g_SeqEnginePtr != nullptr) {
        g_SeqEnginePtr->NotifyPhysicalKeyTransition(vk, isDown);
    }
}

void OnTriggerComplexSeq(uint8_t sequenceIndex) noexcept {
    if (g_SeqEnginePtr != nullptr) {
        g_SeqEnginePtr->TriggerSequence(sequenceIndex);
        NotifyUi();
    }
}

void OnToggleMacroSlotFromSeq(uint8_t slotIndex) noexcept {
    const HWND hwnd = mask::input::GameInputGlobalContext::uiWindowHandle.load(std::memory_order_acquire);
    if (hwnd != nullptr) {
        PostMessageW(hwnd, WM_APP_TOGGLE_MACRO_SLOT, static_cast<WPARAM>(slotIndex), 0);
    }
}

// =====================================================================================
//  Toast & Animation helpers
// =====================================================================================

void ShowToast(HWND hwnd, const wchar_t* message, int action = 0, const wchar_t* actionLabel = L"") noexcept {
    g_Ui.SetToast(message, action, actionLabel);
    g_Animator.SetToastVisible(true);
    EnsureAnimTimer(hwnd);
}

void SyncAnimatorToggles(bool snap) noexcept {
    for (int i = 0; i < kSlotCount; ++i) {
        g_Animator.SetToggle(AnimSlotFor(ItemKind::Macro, i), g_Ui.slots[i].enabled, snap);
    }
    for (int i = 0; i < kComplexSeqCount; ++i) {
        g_Animator.SetToggle(AnimSlotFor(ItemKind::Sequence, i), g_Ui.sequences[i].enabled, snap);
    }
    for (int i = 0; i < kColorTriggerCount; ++i) {
        g_Animator.SetToggle(AnimSlotFor(ItemKind::Trigger, i), g_Ui.triggers[i].enabled, snap);
    }
    for (int i = 0; i < kRemapRuleCount; ++i) {
        g_Animator.SetToggle(AnimSlotFor(ItemKind::Remap, i), g_Ui.remaps[i].enabled, snap);
    }
}

void SyncAllEnginesFromModel() noexcept {
    g_Ui.SyncAllSlotsToEngine();
    if (g_SeqEnginePtr != nullptr)   g_SeqEnginePtr->UpdateAllConfigs(g_Ui.sequences);
    if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->UpdateAllConfigs(g_Ui.triggers);
    if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->UpdateAllRules(g_Ui.remaps);
    g_Ui.PublishWatchedKeys();
}

void EnsureAnimTimer(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    const bool windowActive = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray;
    const bool allowAnim = windowActive && (g_WindowFocused || !g_Ui.settings.lowResourceWhenUnfocused);
    const bool wantAnim = allowAnim && g_Animator.IsAnimating(g_WindowFocused, windowActive);
    if (wantAnim && !g_AnimTimerActive) {
        SetTimer(hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
        g_AnimTimerActive = true;
    } else if (!wantAnim && g_AnimTimerActive) {
        KillTimer(hwnd, kAnimTimerId);
        g_AnimTimerActive = false;
    }
}

void UpdateUiTimerCadence(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    const bool active = g_WindowFocused && !IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray;
    const bool lowPower = !active && g_Ui.settings.lowResourceWhenUnfocused;
    g_SharedState.lowPowerUiMode.store(lowPower, std::memory_order_release);
    const UINT interval = active ? kUiFocusedIntervalMs : kUiBackgroundIntervalMs;
    SetTimer(hwnd, kUiTimerId, interval, nullptr);
    EnsureAnimTimer(hwnd);
    UpdateTriggerPreviewState(hwnd);
}

void UpdateTriggerPreviewState(HWND hwnd) noexcept {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        if (g_PixelEnginePtr != nullptr && g_PreviewActiveSent) {
            g_PixelEnginePtr->SetPreviewActive(false, 0U);
            g_PreviewActiveSent = false;
        }
        g_TriggerPreviewTimerMs = 0U;
        return;
    }
    const bool visible = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray;
    const bool focusedOrUnrestricted = g_WindowFocused || !g_Ui.settings.lowResourceWhenUnfocused;
    const bool viewingTrigger = !g_Ui.showSettings && g_Ui.HasSelection() && g_Ui.selectedKind == ItemKind::Trigger;

    const bool wantDropper = visible && g_Ui.triggerColorPickActive;
    const bool wantLive    = visible && focusedOrUnrestricted && viewingTrigger && !g_Ui.triggerColorPickActive;
    const uint8_t slot     = viewingTrigger ? static_cast<uint8_t>(std::clamp(g_Ui.selectedIndex, 0, kColorTriggerCount - 1)) : 0U;

    if (g_PixelEnginePtr != nullptr) {
        if (wantLive != g_PreviewActiveSent || (wantLive && slot != g_PreviewSlotSent)) {
            g_PixelEnginePtr->SetPreviewActive(wantLive, slot);
            g_PreviewActiveSent = wantLive;
            g_PreviewSlotSent   = slot;
        }
    }

    const UINT wantTimer = wantDropper ? kTrigDropperIntervalMs : (wantLive ? kTrigLiveIntervalMs : 0U);
    if (wantTimer == 0U) {
        if (g_TriggerPreviewTimerMs != 0U) {
            KillTimer(hwnd, kTriggerPreviewTimerId);
            g_TriggerPreviewTimerMs = 0U;
        }
    } else if (g_TriggerPreviewTimerMs != wantTimer) {
        SetTimer(hwnd, kTriggerPreviewTimerId, wantTimer, nullptr);
        g_TriggerPreviewTimerMs = wantTimer;
    }
}

void ApplyAlwaysOnTop(HWND hwnd) noexcept {
    const HWND insertAfter = g_Ui.settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// =====================================================================================
//  Virtual device (output) lifecycle
// =====================================================================================

void StartOutputWorker() noexcept {
    if (g_OutputWorkerBusy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (g_OutputWorker.joinable()) {
        g_OutputWorker.join();
    }
    std::array<wchar_t, 64> mouseId{};
    std::array<wchar_t, 64> kbId{};
    wcsncpy_s(mouseId.data(), mouseId.size(), g_Ui.mouseProfileId, _TRUNCATE);
    wcsncpy_s(kbId.data(), kbId.size(), g_Ui.keyboardProfileId, _TRUNCATE);
    g_Ui.outputState = OutputState::Starting;
    g_OutputWorker = std::thread([mouseId, kbId]() noexcept {
        auto& host = mask::bridge::HIDMaestroHost::Instance();
        const bool loaded = host.LoadAndInitialize(mouseId.data(), kbId.data());
        uint16_t mVid = 0, mPid = 0, kVid = 0, kPid = 0;
        host.GetActiveVirtualHardwareIds(mVid, mPid, kVid, kPid);
        mask::input::RegisterVirtualDeviceVidPid(mVid, mPid);
        mask::input::RegisterVirtualDeviceVidPid(kVid, kPid);
        OutputState state = OutputState::Unavailable;
        if (host.AreControllersLive()) {
            state = OutputState::Ready;
        } else if (loaded) {
            state = OutputState::NeedsAdmin;
        }
        g_SharedState.outputFailureStreak.store(0U, std::memory_order_release);
        g_OutputWorkerBusy.store(false, std::memory_order_release);
        const HWND hwnd = mask::input::GameInputGlobalContext::uiWindowHandle.load(std::memory_order_acquire);
        if (hwnd != nullptr) {
            PostMessageW(hwnd, WM_APP_OUTPUT_STATE, static_cast<WPARAM>(state), 0);
        }
    });
}

void RestartAsAdministrator(HWND hwnd) noexcept {
    wchar_t exePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    g_Ui.SaveToDisk();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.hwnd         = hwnd;
    sei.lpVerb       = L"runas";
    sei.lpFile       = exePath;
    sei.lpParameters = L"--relaunch";
    sei.nShow        = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else {
        ShowToast(hwnd, L"Mask wasn't restarted, so macros still can't send input.");
    }
}

// =====================================================================================
//  Embedded Multi-Resolution Mask Icon + System Tray
// =====================================================================================

void UpdateTrayTooltip(HWND hwnd) noexcept {
    if (!g_TrayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = kTrayIconId;
    nid.uFlags           = NIF_ICON | NIF_TIP;
    nid.hIcon            = g_IconSmall != nullptr ? g_IconSmall : g_IconLarge;
    const bool paused    = g_SharedState.automationPaused.load(std::memory_order_relaxed);
    std::swprintf(nid.szTip, std::size(nid.szTip), L"Mask \u2014 %ls (Bank %u)",
                  paused ? L"Paused" : L"Active",
                  static_cast<unsigned>(g_Ui.settings.activeProfileBank + 1U));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AddTrayIcon(HWND hwnd) noexcept {
    if (g_TrayAdded) {
        UpdateTrayTooltip(hwnd);
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = kTrayIconId;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;
    nid.hIcon            = g_IconSmall != nullptr ? g_IconSmall : g_IconLarge;
    wcscpy_s(nid.szTip, L"Mask \u2014 Active");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_TrayAdded = true;
        UpdateTrayTooltip(hwnd);
    }
}

void RemoveTrayIcon(HWND hwnd) noexcept {
    if (!g_TrayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_TrayAdded = false;
}

void RefreshAppIcons(HWND hwnd) noexcept {
    const UINT rawDpi = (hwnd != nullptr && IsWindow(hwnd)) ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    const UINT dpi = (rawDpi > 0U) ? rawDpi : 96U;
    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    const int cxBig   = GetSystemMetricsForDpi(SM_CXICON, dpi);
    const int cyBig   = GetSystemMetricsForDpi(SM_CYICON, dpi);
    const int cxSmall = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    const int cySmall = GetSystemMetricsForDpi(SM_CYSMICON, dpi);

    HICON newLarge = static_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_MASK_ICON), IMAGE_ICON, cxBig, cyBig, LR_DEFAULTCOLOR));
    HICON newSmall = static_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_MASK_ICON), IMAGE_ICON, cxSmall, cySmall, LR_DEFAULTCOLOR));

    if (newLarge != nullptr) {
        if (hwnd != nullptr) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(newLarge));
        if (g_IconLarge != nullptr) DestroyIcon(g_IconLarge);
        g_IconLarge = newLarge;
    }
    if (newSmall != nullptr) {
        if (hwnd != nullptr) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(newSmall));
        if (g_IconSmall != nullptr) DestroyIcon(g_IconSmall);
        g_IconSmall = newSmall;
    }
    if (hwnd != nullptr) {
        UpdateTrayTooltip(hwnd);
    }
}

void StartTrimTimerIfEnabled(HWND hwnd) noexcept {
    if (g_Ui.settings.trimMemoryWhenMinimized && !g_TrimTimerActive) {
        SetTimer(hwnd, kTrimMemTimerId, kTrimMemDelayMs, nullptr);
        g_TrimTimerActive = true;
    }
}

void CancelTrimTimer(HWND hwnd) noexcept {
    if (g_TrimTimerActive) {
        KillTimer(hwnd, kTrimMemTimerId);
        g_TrimTimerActive = false;
    }
}

void HideToTray(HWND hwnd) noexcept {
    AddTrayIcon(hwnd);
    g_HiddenToTray = true;
    g_WindowFocused = false;
    g_Animator.SetCursorPosition(g_Animator.cursorX.value, g_Animator.cursorY.value, false, true);
    g_Animator.OnPointerUp();
    g_Animator.appOpen.Snap(g_Animator.appOpen.target);
    g_Animator.viewTransition.Snap(g_Animator.viewTransition.target);
    g_Animator.SnapAll();
    ShowWindow(hwnd, SW_HIDE);
    g_Renderer.TrimGpu();
    UpdateUiTimerCadence(hwnd);
    StartTrimTimerIfEnabled(hwnd);
}

void RestoreFromTray(HWND hwnd) noexcept {
    CancelTrimTimer(hwnd);
    g_HiddenToTray = false;
    g_WindowFocused = true;
    g_SettingsTabSnapped = false;
    g_SidebarIndSnapped = false;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateUiTimerCadence(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void ShowTrayContextMenu(HWND hwnd) noexcept {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    const bool paused = g_SharedState.automationPaused.load(std::memory_order_acquire);
    AppendMenuW(menu, MF_STRING, kTrayCmdOpen, L"Open Mask");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdPause, paused ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_STRING, kTrayCmdStopAll, L"Stop All");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdExit, L"Exit");
    SetMenuDefaultItem(menu, kTrayCmdOpen, FALSE);

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// =====================================================================================
//  Editing, Binding & Selection helpers
// =====================================================================================

void StopEverythingRunning() noexcept {
    g_SharedState.flushAllRequested.store(true, std::memory_order_release);
    OnWatchdogAuxCleanup();
    mask::input::WakeEngine();
    if (g_WatchdogPtr != nullptr) {
        g_WatchdogPtr->ReleaseAllSyntheticHeldInputs();
    }
}

void UpdateTriggerSuppression() noexcept {
    const bool editSuppress = g_Ui.settings.suppressWhileTypingInApp && (g_Ui.editField != Field::None);
    const bool suppress = editSuppress || (g_BindGraceVk != 0);
    g_SharedState.suppressTriggers.store(suppress, std::memory_order_release);
    mask::input::WakeEngine();
}

void SetEditing(Field f) noexcept {
    g_Ui.editField = f;
    UpdateTriggerSuppression();
}

void BeginBindGrace(HWND hwnd, uint16_t vk) noexcept {
    g_BindGraceVk = vk;
    g_BindGraceUntil = GetTickCount64() + 2000;
    UpdateTriggerSuppression();
    SetTimer(hwnd, kBindGraceTimerId, 25, nullptr);
}

void OnBindGraceTimer(HWND hwnd) noexcept {
    const bool stillHeld = g_BindGraceVk != 0 && (GetAsyncKeyState(static_cast<int>(g_BindGraceVk)) & 0x8000) != 0;
    if (!stillHeld || GetTickCount64() >= g_BindGraceUntil) {
        KillTimer(hwnd, kBindGraceTimerId);
        g_BindGraceVk = 0;
        UpdateTriggerSuppression();
    }
}

void CancelEdit() noexcept {
    if (g_Ui.editField != Field::None) {
        SetEditing(Field::None);
    }
    g_Ui.editBuffer[0] = L'\0';
}

void SyncSelectedSequence() noexcept {
    if (g_Ui.selectedKind != ItemKind::Sequence) return;
    const int idx = std::clamp(g_Ui.selectedIndex, 0, kComplexSeqCount - 1);
    if (g_SeqEnginePtr != nullptr) {
        g_SeqEnginePtr->UpdateConfig(static_cast<size_t>(idx), g_Ui.sequences[idx]);
    }
    g_Ui.PublishWatchedKeys();
    g_Ui.MarkDirty();
}

void EnsureSelectedSeqStepVisible() noexcept {
    if (g_Ui.selectedKind != ItemKind::Sequence || !g_Ui.HasSelection()) return;
    const auto& seq = g_Ui.SelectedSeq();
    const bool snapScroll = !g_WindowFocused ||
        g_Ui.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off);
    if (seq.stepCount == 0U || g_Ui.selectedSeqStep < 0) {
        g_Ui.seqStepScroll = 0.0f;
        g_Animator.SetSeqScrollTarget(0.0f, snapScroll);
        return;
    }
    const float pitch = UiRenderer::kSeqStepRowH + 4.0f;
    const float top = static_cast<float>(g_Ui.selectedSeqStep) * pitch;
    const float view = g_Renderer.SeqStepViewHeight();
    float scroll = g_Animator.seqScrollSmooth.target;
    if (top < scroll) {
        scroll = top;
    }
    if (top + pitch + 8.0f > scroll + view) {
        scroll = top + pitch + 8.0f - view;
    }
    const float maxScroll = std::max(0.0f, static_cast<float>(seq.stepCount) * pitch + 8.0f - view);
    scroll = std::clamp(scroll, 0.0f, maxScroll);
    g_Animator.SetSeqScrollTarget(scroll, snapScroll);
    g_Ui.seqStepScroll = g_Animator.seqScrollSmooth.value;
}

void SyncSelectedRemap() noexcept {
    if (g_Ui.selectedKind != ItemKind::Remap) return;
    const int idx = std::clamp(g_Ui.selectedIndex, 0, kRemapRuleCount - 1);
    if (g_RemapEnginePtr != nullptr) {
        g_RemapEnginePtr->UpdateRule(static_cast<size_t>(idx), g_Ui.remaps[idx]);
    }
    g_Ui.PublishWatchedKeys();
    g_Ui.MarkDirty();
}

void SyncSelectedTrigger() noexcept {
    if (g_Ui.selectedKind != ItemKind::Trigger) return;
    const int idx = std::clamp(g_Ui.selectedIndex, 0, kColorTriggerCount - 1);
    if (g_PixelEnginePtr != nullptr) {
        g_PixelEnginePtr->UpdateConfig(static_cast<size_t>(idx), g_Ui.triggers[idx]);
    }
    g_Ui.PublishWatchedKeys();
    g_Ui.MarkDirty();
}

void SampleTriggerEyedropperLoupe(int32_t cx, int32_t cy) noexcept {
    auto& pv = g_Ui.eyedropperPreview;
    pv.sampleX = cx;
    pv.sampleY = cy;
    pv.usingDxgi = false;
    constexpr int N = mask::triggers::ColorTriggerPreviewSnapshot::kLoupeSize; // 15
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
    if (g_Ui.HasSelection() && g_Ui.selectedKind == ItemKind::Trigger) {
        const auto& t = g_Ui.SelectedTrigger();
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
        pv.colorDelta = static_cast<uint32_t>(std::max({dr, dg, db}));
    }
}

void CancelTriggerEyedropper(HWND hwnd) noexcept {
    if (!g_Ui.triggerColorPickActive) return;
    g_Ui.triggerColorPickActive = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    UpdateTriggerPreviewState(hwnd);
}

void FinishTriggerEyedropperPick(HWND hwnd) noexcept {
    if (!g_Ui.triggerColorPickActive) return;
    POINT pt{};
    GetCursorPos(&pt);
    uint8_t r = 0U, g = 0U, b = 0U;
    const bool ok = mask::triggers::PixelTriggerEngine::SamplePixelAt(pt.x, pt.y, r, g, b);
    CancelTriggerEyedropper(hwnd);
    if (ok && g_Ui.selectedKind == ItemKind::Trigger && g_Ui.HasSelection()) {
        g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
        auto& t = g_Ui.SelectedTrigger();
        t.r = r;
        t.g = g;
        t.b = b;
        const int monCount = std::max(1, std::min(g_Ui.monitorCount, 8));
        int matchedMon = std::clamp<int>(t.monitorIndex, 0, monCount - 1);
        for (int m = 0; m < g_Ui.monitorCount; ++m) {
            const auto& mon = g_Ui.monitors[static_cast<size_t>(m)];
            if (pt.x >= mon.left && pt.x < mon.right &&
                pt.y >= mon.top  && pt.y < mon.bottom) {
                matchedMon = m;
                break;
            }
        }
        const auto& mon = g_Ui.monitors[static_cast<size_t>(matchedMon)];
        const int32_t w = std::max<int32_t>(1, mon.right - mon.left);
        const int32_t h = std::max<int32_t>(1, mon.bottom - mon.top);
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

void CommitEdit() noexcept {
    const Field f = g_Ui.editField;
    if (f == Field::None) return;

    auto trimStr = [](wchar_t* buf) noexcept -> wchar_t* {
        wchar_t* start = buf;
        while (*start == L' ') ++start;
        size_t len = std::wcslen(start);
        while (len > 0 && start[len - 1] == L' ') start[--len] = L'\0';
        return start;
    };

    if (f == Field::Name && g_Ui.selectedKind == ItemKind::Macro) {
        wchar_t* s = trimStr(g_Ui.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(g_Ui.Selected().name, s, _TRUNCATE);
        CancelEdit();
        g_Ui.SyncSlotToEngine(g_Ui.selectedIndex);
        return;
    }
    if (f == Field::SeqName && g_Ui.selectedKind == ItemKind::Sequence) {
        wchar_t* s = trimStr(g_Ui.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(g_Ui.SelectedSeq().name, s, _TRUNCATE);
        CancelEdit();
        SyncSelectedSequence();
        return;
    }
    if (f == Field::RemapName && g_Ui.selectedKind == ItemKind::Remap) {
        wchar_t* s = trimStr(g_Ui.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(g_Ui.SelectedRemap().name, s, _TRUNCATE);
        CancelEdit();
        SyncSelectedRemap();
        return;
    }
    if (f == Field::TriggerName && g_Ui.selectedKind == ItemKind::Trigger) {
        wchar_t* s = trimStr(g_Ui.editBuffer);
        if (s[0] != L'\0') wcsncpy_s(g_Ui.SelectedTrigger().name, s, _TRUNCATE);
        CancelEdit();
        SyncSelectedTrigger();
        return;
    }
    if (f == Field::SeqStepText && g_Ui.selectedKind == ItemKind::Sequence) {
        auto& seq = g_Ui.SelectedSeq();
        if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            wcsncpy_s(seq.steps[g_Ui.selectedSeqStep].text, g_Ui.editBuffer, _TRUNCATE);
        }
        CancelEdit();
        SyncSelectedSequence();
        return;
    }

    wchar_t* end = nullptr;
    const double v = std::wcstod(g_Ui.editBuffer, &end);
    const bool validNum = (end != g_Ui.editBuffer && std::isfinite(v));

    if (validNum && f == Field::SettingsAutosaveMs) {
        g_Ui.settings.autosaveDelayMs = static_cast<uint32_t>(std::clamp<long long>(std::llround(v), 200, 30'000));
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
    } else if (validNum && IsMacroField(f) && g_Ui.selectedKind == ItemKind::Macro) {
        FieldSet(g_Ui.Selected(), f, v);
        g_Ui.SyncSlotToEngine(g_Ui.selectedIndex);
    } else if (validNum && IsTriggerField(f) && g_Ui.selectedKind == ItemKind::Trigger) {
        auto& t = g_Ui.SelectedTrigger();
        const long long iv = std::llround(v);
        switch (f) {
            case Field::TriggerDelay:    t.delayMs    = static_cast<uint32_t>(std::clamp<long long>(iv, 0, 10'000)); break;
            case Field::TriggerHold:     t.holdMs     = static_cast<uint32_t>(std::clamp<long long>(iv, 0, 10'000)); break;
            case Field::TriggerCooldown: t.cooldownMs = static_cast<uint32_t>(std::clamp<long long>(iv, 0, 60'000)); break;
            case Field::TriggerLinked: {
                const bool isMacro = (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot));
                const long long maxSlot = isMacro ? kSlotCount : kComplexSeqCount;
                t.linkedIndex = static_cast<uint8_t>(std::clamp<long long>(iv - 1, 0, maxSlot - 1));
                break;
            }
            default: break;
        }
        SyncSelectedTrigger();
    } else if (validNum && IsSeqStepField(f) && g_Ui.selectedKind == ItemKind::Sequence) {
        auto& seq = g_Ui.SelectedSeq();
        if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            auto& st = seq.steps[g_Ui.selectedSeqStep];
            const long long iv = std::llround(v);
            switch (f) {
                case Field::SeqStepDuration: st.durationMs = static_cast<uint32_t>(std::clamp<long long>(iv, 0, 3'600'000)); break;
                case Field::SeqStepX:        st.x = static_cast<int32_t>(std::clamp<long long>(iv, -16384, 16384)); break;
                case Field::SeqStepY:        st.y = static_cast<int32_t>(std::clamp<long long>(iv, -16384, 16384)); break;
                case Field::SeqStepParam: {
                    const auto kind = static_cast<ComplexStepKind>(st.kind);
                    if (kind == ComplexStepKind::MouseScroll) {
                        st.param = static_cast<uint32_t>(static_cast<int32_t>(std::clamp<long long>(iv, -120, 120)));
                    } else if (kind == ComplexStepKind::ToggleMacro ||
                               (kind == ComplexStepKind::IfCheck &&
                                mask::core::GetStepConditionKind(st) == ComplexConditionKind::MacroActive)) {
                        st.param = static_cast<uint32_t>(std::clamp<long long>(iv - 1, 0, kSlotCount - 1));
                    } else if (kind == ComplexStepKind::IfCheck &&
                               mask::core::GetStepConditionKind(st) == ComplexConditionKind::RandomChance) {
                        st.param = static_cast<uint32_t>(std::clamp<long long>(iv, 1, 99));
                    } else {
                        st.param = static_cast<uint32_t>(std::clamp<long long>(iv, 0, 3'600'000));
                    }
                    break;
                }
                case Field::SeqStepR:   st.r = static_cast<uint8_t>(std::clamp<long long>(iv, 0, 255)); break;
                case Field::SeqStepG:   st.g = static_cast<uint8_t>(std::clamp<long long>(iv, 0, 255)); break;
                case Field::SeqStepB:   st.b = static_cast<uint8_t>(std::clamp<long long>(iv, 0, 255)); break;
                case Field::SeqStepTol: st.tolerance = static_cast<uint8_t>(std::clamp<long long>(iv, 0, 255)); break;
                default: break;
            }
            SyncSelectedSequence();
        }
    }
    CancelEdit();
}

void BeginEdit(Field f) noexcept {
    if (g_Ui.editField == f) return;
    CommitEdit();
    g_Ui.editBuffer[0] = L'\0';
    if (f == Field::Name && g_Ui.selectedKind == ItemKind::Macro) {
        wcsncpy_s(g_Ui.editBuffer, g_Ui.Selected().name, _TRUNCATE);
    } else if (f == Field::SeqName && g_Ui.selectedKind == ItemKind::Sequence) {
        wcsncpy_s(g_Ui.editBuffer, g_Ui.SelectedSeq().name, _TRUNCATE);
    } else if (f == Field::RemapName && g_Ui.selectedKind == ItemKind::Remap) {
        wcsncpy_s(g_Ui.editBuffer, g_Ui.SelectedRemap().name, _TRUNCATE);
    } else if (f == Field::TriggerName && g_Ui.selectedKind == ItemKind::Trigger) {
        wcsncpy_s(g_Ui.editBuffer, g_Ui.SelectedTrigger().name, _TRUNCATE);
    } else if (f == Field::SettingsAutosaveMs) {
        std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(g_Ui.settings.autosaveDelayMs));
    } else if (IsMacroField(f) && g_Ui.selectedKind == ItemKind::Macro) {
        FormatFieldNumber(g_Ui.Selected(), f, g_Ui.editBuffer, std::size(g_Ui.editBuffer));
    } else if (IsTriggerField(f) && g_Ui.selectedKind == ItemKind::Trigger) {
        const auto& t = g_Ui.SelectedTrigger();
        switch (f) {
            case Field::TriggerDelay:    std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(t.delayMs)); break;
            case Field::TriggerHold:     std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(t.holdMs)); break;
            case Field::TriggerCooldown: std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(t.cooldownMs)); break;
            case Field::TriggerLinked: {
                const bool isMacro = (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot));
                const uint32_t maxSlots = isMacro ? static_cast<uint32_t>(kSlotCount) : static_cast<uint32_t>(kComplexSeqCount);
                std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>((t.linkedIndex % maxSlots) + 1U));
                break;
            }
            default: break;
        }
    } else if (IsSeqStepField(f) && g_Ui.selectedKind == ItemKind::Sequence) {
        const auto& seq = g_Ui.SelectedSeq();
        if (g_Ui.selectedSeqStep < 0 || g_Ui.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
        const auto& st = seq.steps[g_Ui.selectedSeqStep];
        switch (f) {
            case Field::SeqStepText:     wcsncpy_s(g_Ui.editBuffer, st.text, _TRUNCATE); break;
            case Field::SeqStepDuration: std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(st.durationMs)); break;
            case Field::SeqStepX:        std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%d", static_cast<int>(st.x)); break;
            case Field::SeqStepY:        std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%d", static_cast<int>(st.y)); break;
            case Field::SeqStepParam: {
                const auto kind = static_cast<ComplexStepKind>(st.kind);
                if (kind == ComplexStepKind::MouseScroll) {
                    std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%d", static_cast<int>(static_cast<int32_t>(st.param)));
                } else if (kind == ComplexStepKind::ToggleMacro ||
                           (kind == ComplexStepKind::IfCheck &&
                            mask::core::GetStepConditionKind(st) == ComplexConditionKind::MacroActive)) {
                    std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>((st.param % kSlotCount) + 1U));
                } else {
                    std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(st.param));
                }
                break;
            }
            case Field::SeqStepR:   std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(st.r)); break;
            case Field::SeqStepG:   std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(st.g)); break;
            case Field::SeqStepB:   std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(st.b)); break;
            case Field::SeqStepTol: std::swprintf(g_Ui.editBuffer, std::size(g_Ui.editBuffer), L"%u", static_cast<unsigned>(st.tolerance)); break;
            default: break;
        }
    }
    SetEditing(f);
}

void StepField(Field f, int direction, bool coarse, bool fine) noexcept {
    if (f == Field::SettingsAutosaveMs) {
        const int delta = (coarse ? 500 : 100) * direction;
        g_Ui.settings.autosaveDelayMs = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(g_Ui.settings.autosaveDelayMs) + delta, 200, 30'000));
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
        return;
    }
    if (IsMacroField(f) && g_Ui.selectedKind == ItemKind::Macro) {
        FieldStep(g_Ui.Selected(), f, direction, coarse, fine);
        g_Ui.SyncSlotToEngine(g_Ui.selectedIndex);
        return;
    }
    if (IsTriggerField(f) && g_Ui.selectedKind == ItemKind::Trigger) {
        auto& t = g_Ui.SelectedTrigger();
        switch (f) {
            case Field::TriggerDelay:
                t.delayMs = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(t.delayMs) + direction * (coarse ? 50 : (fine ? 1 : 5)), 0, 10'000));
                break;
            case Field::TriggerHold:
                t.holdMs = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(t.holdMs) + direction * (coarse ? 50 : (fine ? 1 : 5)), 0, 10'000));
                break;
            case Field::TriggerCooldown:
                t.cooldownMs = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(t.cooldownMs) + direction * (coarse ? 100 : (fine ? 1 : 10)), 0, 60'000));
                break;
            case Field::TriggerLinked: {
                const bool isMacro = (t.actionKind == static_cast<uint8_t>(ColorTriggerActionKind::RunMacroSlot));
                const int maxSlots = isMacro ? kSlotCount : kComplexSeqCount;
                const int cur = static_cast<int>(t.linkedIndex % static_cast<uint32_t>(maxSlots));
                t.linkedIndex = static_cast<uint8_t>((cur + direction + maxSlots) % maxSlots);
                break;
            }
            default: break;
        }
        SyncSelectedTrigger();
        return;
    }
    if (IsSeqStepField(f) && g_Ui.selectedKind == ItemKind::Sequence) {
        auto& seq = g_Ui.SelectedSeq();
        if (g_Ui.selectedSeqStep < 0 || g_Ui.selectedSeqStep >= static_cast<int>(seq.stepCount)) return;
        auto& st = seq.steps[g_Ui.selectedSeqStep];
        const int step = coarse ? 50 : (fine ? 1 : 10);
        switch (f) {
            case Field::SeqStepDuration:
                st.durationMs = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(st.durationMs) + direction * step, 0, 3'600'000));
                break;
            case Field::SeqStepX:
                st.x = std::clamp<int32_t>(st.x + direction * (coarse ? 50 : 5), -16384, 16384);
                break;
            case Field::SeqStepY:
                st.y = std::clamp<int32_t>(st.y + direction * (coarse ? 50 : 5), -16384, 16384);
                break;
            case Field::SeqStepParam: {
                const auto kind = static_cast<ComplexStepKind>(st.kind);
                if (kind == ComplexStepKind::MouseScroll) {
                    const int32_t cur = static_cast<int32_t>(st.param);
                    st.param = static_cast<uint32_t>(std::clamp<int32_t>(cur + direction * (coarse ? 5 : 1), -120, 120));
                } else if (kind == ComplexStepKind::ToggleMacro ||
                           (kind == ComplexStepKind::IfCheck &&
                            mask::core::GetStepConditionKind(st) == ComplexConditionKind::MacroActive)) {
                    const int cur = static_cast<int>(st.param % static_cast<uint32_t>(kSlotCount));
                    st.param = static_cast<uint32_t>((cur + direction + kSlotCount) % kSlotCount);
                } else if (kind == ComplexStepKind::IfCheck &&
                           mask::core::GetStepConditionKind(st) == ComplexConditionKind::RandomChance) {
                    st.param = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(st.param) + direction * (coarse ? 10 : 1), 1, 99));
                } else if (kind == ComplexStepKind::WaitForColor) {
                    st.param = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(st.param) + direction * (coarse ? 1000 : 250), 0, 3'600'000));
                } else {
                    st.param = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(st.param) + direction * (coarse ? 10 : 1), 0, 9999));
                }
                break;
            }
            case Field::SeqStepR:   st.r = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.r) + direction * (coarse ? 15 : 1), 0, 255)); break;
            case Field::SeqStepG:   st.g = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.g) + direction * (coarse ? 15 : 1), 0, 255)); break;
            case Field::SeqStepB:   st.b = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.b) + direction * (coarse ? 15 : 1), 0, 255)); break;
            case Field::SeqStepTol: st.tolerance = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(st.tolerance) + direction * (coarse ? 10 : 2), 0, 255)); break;
            default: break;
        }
        SyncSelectedSequence();
    }
}

void ApplyDutyFromPointer(int xPx) noexcept {
    if (g_Ui.selectedKind != ItemKind::Macro) return;
    auto& slot = g_Ui.Selected();
    const double wanted = g_Renderer.DutyFromSliderXPx(xPx);
    const bool same = (wanted >= 100.0) ? IsFullDuty(slot)
                                        : (!IsFullDuty(slot) && std::fabs(DutyPercent(slot) - wanted) < 0.01);
    if (!same) {
        SetDuty(slot, wanted);
        g_Ui.SyncSlotToEngine(g_Ui.selectedIndex);
    }
}

void ApplyTriggerSliderFromPointer(int xPx) noexcept {
    if (g_Ui.selectedKind != ItemKind::Trigger || !g_Ui.HasSelection()) return;
    const int nv = g_Renderer.TriggerToleranceFromXPx(xPx);
    auto& t = g_Ui.SelectedTrigger();
    if (t.tolerance != static_cast<uint8_t>(nv)) {
        t.tolerance = static_cast<uint8_t>(nv);
        SyncSelectedTrigger();
    }
}

void CancelBinding() noexcept {
    g_SharedState.captureBindTarget.store(0U, std::memory_order_release);
    mask::input::WakeEngine();
}

void StartBinding(HWND hwnd, uint8_t target, const wchar_t* message) noexcept {
    g_SharedState.flushAllRequested.store(true, std::memory_order_release);
    g_SharedState.captureSerial.fetch_add(1U, std::memory_order_acq_rel);
    g_SharedState.captureBindTarget.store(target, std::memory_order_release);
    mask::input::WakeEngine();
    g_Ui.captureStartTick = GetTickCount64();
    ShowToast(hwnd, message);
}

void SelectItem(HWND hwnd, ItemKind kind, int index) noexcept {
    const bool wasSettings = g_Ui.showSettings;
    const bool changed = wasSettings || g_Ui.selectedKind != kind || g_Ui.selectedIndex != index;
    g_Ui.showSettings = false;
    g_Ui.showStepKindMenu = false;
    g_SettingsHwTabOpen.store(false, std::memory_order_release);
    if (wasSettings) {
        g_Animator.SetSettingsOpen(false);
        g_Animator.SetSettingsTab(0.0f, 0.0f, true);
        g_SettingsTabSnapped = false;
        g_SidebarIndSnapped = false;
    }
    g_Ui.pendingDeleteConfirm = false;
    g_Ui.pendingResetConfirm = false;
    g_Ui.selectedKind = kind;
    g_Ui.selectedIndex = index;
    if (kind == ItemKind::Macro) g_Ui.selectedSlot = index;
    if (kind == ItemKind::Sequence) {
        const int maxStep = std::max(0, static_cast<int>(g_Ui.sequences[index].stepCount) - 1);
        g_Ui.selectedSeqStep = std::clamp(g_Ui.selectedSeqStep, 0, maxStep);
        g_Animator.SetSeqScrollTarget(0.0f, true);
        g_Ui.seqStepScroll = 0.0f;
    }
    // Keep the selected sidebar item scrolled into view
    {
        std::array<SidebarEntry, 128> entries{};
        const int n = g_Ui.BuildSidebarList(entries);
        for (int pos = 0; pos < n; ++pos) {
            if (entries[pos].kind == kind && entries[pos].index == index) {
                const float pitch = g_Renderer.CurrentListRowHeight() + UiRenderer::kListRowGap;
                const float top = static_cast<float>(pos) * pitch;
                const float view = g_Renderer.ListViewHeight();
                float scroll = g_Animator.listScrollSmooth.target;
                if (top < scroll) scroll = top;
                if (top + pitch + 12.0f > scroll + view) scroll = top + pitch + 12.0f - view;
                scroll = std::max(0.0f, scroll);
                const bool snapScroll = !g_WindowFocused ||
                    g_Ui.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off);
                g_Animator.SetListScrollTarget(scroll, snapScroll);
                g_Ui.listScroll = g_Animator.listScrollSmooth.value;
                break;
            }
        }
    }
    if (changed) {
        g_Animator.TriggerViewTransition();
        EnsureAnimTimer(hwnd);
    }
    UpdateTriggerPreviewState(hwnd);
}

void SelectNeighbour(HWND hwnd, int direction) noexcept {
    std::array<SidebarEntry, 128> entries{};
    const int n = g_Ui.BuildSidebarList(entries);
    if (n <= 0) return;
    int cur = -1;
    for (int i = 0; i < n; ++i) {
        if (entries[i].kind == g_Ui.selectedKind && entries[i].index == g_Ui.selectedIndex) {
            cur = i;
            break;
        }
    }
    const int next = std::clamp(cur + direction, 0, n - 1);
    if (next != cur) {
        SelectItem(hwnd, entries[next].kind, entries[next].index);
    }
}

[[nodiscard]] int BindActionFor(uint8_t target) noexcept {
    if (target == 1U)  return act::BindTrigger;
    if (target == 2U)  return act::BindOutput;
    if (target >= 3U && target < 3U + kStepCount) return act::BindStep + (static_cast<int>(target) - 3);
    if (target == 20U) return act::SeqBindTrigger;
    if (target == 21U) return act::SeqStepBindKey;
    if (target == 22U) return act::RemapBindSource;
    if (target == 23U) return act::RemapBindTarget;
    if (target == 24U) return act::SetBindPauseVk;
    if (target == 25U) return act::SetBindStopAllVk;
    if (target == 26U) return act::TrigBindArmVk;
    if (target == 27U) return act::TrigBindActionVk;
    return act::None;
}

void AssignCapturedKey(HWND hwnd, uint16_t vk) noexcept {
    const uint8_t target = g_SharedState.captureBindTarget.load(std::memory_order_acquire);
    if (target == 0U) {
        CancelBinding();
        return;
    }

    // Settings hotkeys
    if (target == 24U || target == 25U) {
        BeginBindGrace(hwnd, vk);
        CancelBinding();
        wchar_t msg[120]{};
        if (target == 24U) {
            g_Ui.settings.pauseHotkeyVk = vk;
            std::swprintf(msg, std::size(msg), L"Global pause hotkey set to %ls.", NameOf(vk).text);
        } else {
            g_Ui.settings.stopAllHotkeyVk = vk;
            std::swprintf(msg, std::size(msg), L"Global Stop All hotkey set to %ls.", NameOf(vk).text);
        }
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
        ShowToast(hwnd, msg);
        g_SuppressKeyNavUntil = GetTickCount64() + 300;
        return;
    }

    if (!g_Ui.HasSelection()) {
        CancelBinding();
        return;
    }

    // Trigger Bot binding
    if ((target == 26U || target == 27U) && g_Ui.selectedKind == ItemKind::Trigger) {
        if (target == 26U) BeginBindGrace(hwnd, vk);
        CancelBinding();
        auto& t = g_Ui.SelectedTrigger();
        wchar_t msg[120]{};
        if (target == 26U) {
            t.armVk = vk;
            if (t.armMode == 0U) t.armMode = 1U;
            std::swprintf(msg, std::size(msg), L"Arm key set to %ls.", NameOf(vk).text);
        } else {
            t.actionVk = vk;
            std::swprintf(msg, std::size(msg), L"Trigger output set to %ls.", NameOf(vk).text);
        }
        SyncSelectedTrigger();
        ShowToast(hwnd, msg);
        g_SuppressKeyNavUntil = GetTickCount64() + 300;
        return;
    }

    // Complex Sequence binding
    if ((target == 20U || target == 21U) && g_Ui.selectedKind == ItemKind::Sequence) {
        if (target == 20U) BeginBindGrace(hwnd, vk);
        CancelBinding();
        auto& seq = g_Ui.SelectedSeq();
        wchar_t msg[120]{};
        if (target == 20U) {
            const bool firstTrig = (seq.triggerVk == 0 && !seq.enabled);
            seq.triggerVk = vk;
            if (firstTrig && seq.stepCount > 0) {
                seq.enabled = true;
                g_Animator.SetToggle(AnimSlotFor(ItemKind::Sequence, g_Ui.selectedIndex), true);
            }
            std::swprintf(msg, std::size(msg), L"Sequence trigger set to %ls.", NameOf(vk).text);
        } else if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            seq.steps[g_Ui.selectedSeqStep].vk = vk;
            std::swprintf(msg, std::size(msg), L"Step %d key set to %ls.", g_Ui.selectedSeqStep + 1, NameOf(vk).text);
        }
        SyncSelectedSequence();
        ShowToast(hwnd, msg);
        g_SuppressKeyNavUntil = GetTickCount64() + 300;
        return;
    }

    // Remap binding
    if ((target == 22U || target == 23U) && g_Ui.selectedKind == ItemKind::Remap) {
        if (target == 22U) BeginBindGrace(hwnd, vk);
        CancelBinding();
        auto& r = g_Ui.SelectedRemap();
        wchar_t msg[120]{};
        if (target == 22U) {
            r.sourceVk = vk;
            std::swprintf(msg, std::size(msg), L"Remap source key set to %ls.", NameOf(vk).text);
        } else {
            r.targetVk = vk;
            std::swprintf(msg, std::size(msg), L"Remap target key set to %ls.", NameOf(vk).text);
        }
        SyncSelectedRemap();
        ShowToast(hwnd, msg);
        g_SuppressKeyNavUntil = GetTickCount64() + 300;
        return;
    }

    // Macro binding
    if (g_Ui.selectedKind != ItemKind::Macro) {
        CancelBinding();
        return;
    }
    if (target == 1U || static_cast<MacroMode>(g_Ui.Selected().mode) == MacroMode::SocdPair) {
        BeginBindGrace(hwnd, vk);
    }
    CancelBinding();
    auto& slot = g_Ui.Selected();
    const bool socd = static_cast<MacroMode>(slot.mode) == MacroMode::SocdPair;
    wchar_t msg[120]{};
    if (target == 1U) {
        const bool firstTrigger = (slot.triggerVk == 0 && !slot.enabled);
        slot.triggerVk = vk;
        if (firstTrigger) {
            slot.enabled = true;
            g_Animator.SetToggle(AnimSlotFor(ItemKind::Macro, g_Ui.selectedIndex), true);
            std::swprintf(msg, std::size(msg), L"%ls now starts \u201C%ls\u201D, and it's switched on.", NameOf(vk).text, slot.name);
        } else {
            std::swprintf(msg, std::size(msg), socd ? L"First key set to %ls." : L"Trigger set to %ls.", NameOf(vk).text);
        }
    } else if (target == 2U) {
        slot.outputVk = vk;
        std::swprintf(msg, std::size(msg), socd ? L"Second key set to %ls." : L"Output set to %ls.", NameOf(vk).text);
    } else if (target >= 3U && target < 3U + mask::core::kMaxSequenceSteps) {
        const size_t step = static_cast<size_t>(target - 3U);
        slot.steps[step].outputVk = vk;
        std::swprintf(msg, std::size(msg), L"Step %u set to %ls.", static_cast<unsigned>(step + 1), NameOf(vk).text);
    }
    ShowToast(hwnd, msg);
    g_Ui.SyncSlotToEngine(g_Ui.selectedIndex);
    g_SuppressKeyNavUntil = GetTickCount64() + 300;
}

void OnKeyCaptured(HWND hwnd, uint16_t vk, bool fromMouse) noexcept {
    const uint8_t target = g_SharedState.captureBindTarget.load(std::memory_order_acquire);
    if (target == 0U) return;

    if (vk == VK_ESCAPE) {
        CancelBinding();
        ShowToast(hwnd, L"Cancelled. Nothing was changed.");
        g_SuppressKeyNavUntil = GetTickCount64() + 300;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (fromMouse && vk == VK_LBUTTON) {
        if (GetTickCount64() - g_Ui.captureStartTick < 150) return;
        POINT pt{};
        GetCursorPos(&pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        ScreenToClient(hwnd, &pt);
        if (GetForegroundWindow() == hwnd && PtInRect(&rc, pt)) return;
    }

    AssignCapturedKey(hwnd, vk);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// =====================================================================================
//  Animated Deletion Finalizer & Undo
// =====================================================================================

void FinalizeItemDelete(HWND hwnd, int deleteId) noexcept {
    const ItemKind kind = DecodeDeleteKind(deleteId);
    const int idx = DecodeDeleteIndex(deleteId);
    wchar_t msg[120]{};

    g_Ui.hasUndo = true;
    g_Ui.undoKind = kind;
    g_Ui.undoSlot = idx;

    switch (kind) {
        case ItemKind::Macro:
            if (idx >= 0 && idx < kSlotCount) {
                g_Ui.undoConfig = g_Ui.slots[idx];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", g_Ui.slots[idx].name);
                g_Ui.ResetSlot(idx);
                g_Ui.SyncSlotToEngine(idx);
            }
            break;
        case ItemKind::Sequence:
            if (idx >= 0 && idx < kComplexSeqCount) {
                g_Ui.undoSeq = g_Ui.sequences[idx];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", g_Ui.sequences[idx].name);
                g_Ui.ResetSequence(idx);
                if (g_SeqEnginePtr != nullptr) g_SeqEnginePtr->UpdateConfig(static_cast<size_t>(idx), g_Ui.sequences[idx]);
                g_Ui.PublishWatchedKeys();
                g_Ui.MarkDirty();
            }
            break;
        case ItemKind::Trigger:
            if (idx >= 0 && idx < kColorTriggerCount) {
                g_Ui.undoTrigger = g_Ui.triggers[idx];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", g_Ui.triggers[idx].name);
                g_Ui.ResetTrigger(idx);
                if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->UpdateConfig(static_cast<size_t>(idx), g_Ui.triggers[idx]);
                g_Ui.PublishWatchedKeys();
                g_Ui.MarkDirty();
            }
            break;
        case ItemKind::Remap:
            if (idx >= 0 && idx < kRemapRuleCount) {
                g_Ui.undoRemap = g_Ui.remaps[idx];
                std::swprintf(msg, std::size(msg), L"Deleted \u201C%ls\u201D.", g_Ui.remaps[idx].name);
                g_Ui.ResetRemap(idx);
                if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->UpdateRule(static_cast<size_t>(idx), g_Ui.remaps[idx]);
                g_Ui.PublishWatchedKeys();
                g_Ui.MarkDirty();
            }
            break;
    }

    g_Ui.EnsureValidSelection();
    g_Ui.showStepKindMenu = false;
    UpdateTriggerPreviewState(hwnd);
    SyncAnimatorToggles(false);
    ShowToast(hwnd, msg, act::ToastAction, L"Undo");
    InvalidateRect(hwnd, nullptr, FALSE);
}

// =====================================================================================
//  UI Actions
// =====================================================================================

void OpenSettingsAtTab(HWND hwnd, int tab) noexcept {
    g_Ui.showNewPicker = false;
    g_Ui.showStepKindMenu = false;
    g_Animator.SetNewPickerOpen(false);
    const bool wasOpen = g_Ui.showSettings;
    g_Ui.showSettings = true;
    g_Ui.settingsTab = std::clamp(tab, 0, 4);
    g_Animator.SetSettingsOpen(true);
    g_SettingsHwTabOpen.store(g_Ui.settingsTab == 4, std::memory_order_release);
    if (g_Ui.settingsTab == 4) {
        g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
        mask::input::WakeEngine();
    }
    if (!wasOpen) {
        g_SettingsTabSnapped = false;
        g_Animator.SetSettingsTab(0.0f, 0.0f, true);
        g_Animator.TriggerViewTransition();
    } else {
        float tx = 0.0f, tw = 80.0f;
        if (g_Renderer.GetSettingsTabRect(g_Ui.settingsTab, tx, tw)) {
            g_Animator.SetSettingsTab(tx, tw, !g_SettingsTabSnapped || !g_WindowFocused);
            g_SettingsTabSnapped = true;
        }
    }
    EnsureAnimTimer(hwnd);
    UpdateTriggerPreviewState(hwnd);
}

void HandleUiAction(HWND hwnd, int a) noexcept {
    if (a == act::None || a == act::ModalSurface) return;

    if (a != act::SeqToggleStepKindMenu) {
        g_Ui.showStepKindMenu = false;
    }
    if (a == act::PopoverBackdrop) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (g_Ui.editField != Field::None && a != act::FieldFocus + static_cast<int>(g_Ui.editField)) {
        CommitEdit();
    }
    if (g_SharedState.captureBindTarget.load(std::memory_order_relaxed) != 0U) {
        CancelBinding();
    }
    if (a != act::DeleteItem) g_Ui.pendingDeleteConfirm = false;
    if (a != act::SetResetAll) g_Ui.pendingResetConfirm = false;

    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    // ---- Modal picker actions -----------------------------------------------------
    if (a == act::OpenNewPicker) {
        g_Ui.showNewPicker = true;
        g_Animator.SetNewPickerOpen(true);
        EnsureAnimTimer(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (a == act::CloseNewPicker || a == act::NewPickerBackdrop) {
        g_Ui.showNewPicker = false;
        g_Animator.SetNewPickerOpen(false);
        EnsureAnimTimer(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (a == act::CreateMacro) {
        g_Ui.showNewPicker = false;
        g_Animator.SetNewPickerOpen(false);
        const int target = g_Ui.FindFirstFreeSlot();
        if (target < 0) {
            ShowToast(hwnd, L"All 64 macro slots are in use. Delete one to make room.");
        } else {
            g_Ui.ResetSlot(target);
            g_Ui.slots[target].reserved[0] = 1;
            wcscpy_s(g_Ui.slots[target].name, L"New macro");
            g_Ui.SyncSlotToEngine(target);
            g_Animator.SetToggle(AnimSlotFor(ItemKind::Macro, target), false, true);
            g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Macro, target));
            if (g_Ui.sidebarFilter != 0 && g_Ui.sidebarFilter != 1) g_Ui.sidebarFilter = 0;
            SelectItem(hwnd, ItemKind::Macro, target);
            StartBinding(hwnd, 1U, L"Press the key or mouse button that should start this macro.");
        }
        EnsureAnimTimer(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (a == act::CreateSequence) {
        g_Ui.showNewPicker = false;
        g_Animator.SetNewPickerOpen(false);
        const int target = g_Ui.FindFirstFreeSeq();
        if (target < 0) {
            ShowToast(hwnd, L"All 16 complex sequences are in use. Delete one to make room.");
        } else {
            g_Ui.ResetSequence(target);
            auto& seq = g_Ui.sequences[target];
            seq.inUse = true;
            seq.stepCount = 2;
            seq.steps[0].kind = static_cast<uint8_t>(ComplexStepKind::KeyClick);
            seq.steps[0].vk = '1';
            seq.steps[0].durationMs = 25;
            seq.steps[1].kind = static_cast<uint8_t>(ComplexStepKind::WaitMs);
            seq.steps[1].durationMs = 100;
            g_Ui.selectedSeqStep = 0;
            if (g_SeqEnginePtr != nullptr) g_SeqEnginePtr->UpdateConfig(static_cast<size_t>(target), seq);
            g_Ui.PublishWatchedKeys();
            g_Ui.MarkDirty();
            g_Animator.SetToggle(AnimSlotFor(ItemKind::Sequence, target), false, true);
            g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Sequence, target));
            if (g_Ui.sidebarFilter != 0 && g_Ui.sidebarFilter != 2) g_Ui.sidebarFilter = 0;
            SelectItem(hwnd, ItemKind::Sequence, target);
            ShowToast(hwnd, L"Created Complex Sequence. Add up to 128 steps and bind a trigger.");
        }
        EnsureAnimTimer(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (a == act::CreateTrigger) {
        g_Ui.showNewPicker = false;
        g_Animator.SetNewPickerOpen(false);
        const int target = g_Ui.FindFirstFreeTrigger();
        if (target < 0) {
            ShowToast(hwnd, L"All 8 Trigger Bots are in use. Delete one to make room.");
        } else {
            g_Ui.ResetTrigger(target);
            g_Ui.triggers[target].inUse = true;
            g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
            if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->UpdateConfig(static_cast<size_t>(target), g_Ui.triggers[target]);
            g_Ui.PublishWatchedKeys();
            g_Ui.MarkDirty();
            g_Animator.SetToggle(AnimSlotFor(ItemKind::Trigger, target), false, true);
            g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Trigger, target));
            if (g_Ui.sidebarFilter != 0 && g_Ui.sidebarFilter != 3) g_Ui.sidebarFilter = 0;
            SelectItem(hwnd, ItemKind::Trigger, target);
            ShowToast(hwnd, L"Created Trigger Bot. Pick a pixel color and set an action.");
        }
        EnsureAnimTimer(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (a == act::CreateRemap) {
        g_Ui.showNewPicker = false;
        g_Animator.SetNewPickerOpen(false);
        const int target = g_Ui.FindFirstFreeRemap();
        if (target < 0) {
            ShowToast(hwnd, L"All 32 remap rules are in use. Delete one to make room.");
        } else {
            g_Ui.ResetRemap(target);
            g_Ui.remaps[target].inUse = true;
            g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
            if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->UpdateRule(static_cast<size_t>(target), g_Ui.remaps[target]);
            g_Ui.PublishWatchedKeys();
            g_Ui.MarkDirty();
            g_Animator.SetToggle(AnimSlotFor(ItemKind::Remap, target), false, true);
            g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Remap, target));
            if (g_Ui.sidebarFilter != 0 && g_Ui.sidebarFilter != 4) g_Ui.sidebarFilter = 0;
            SelectItem(hwnd, ItemKind::Remap, target);
            StartBinding(hwnd, 22U, L"Press the source key or mouse button to remap.");
        }
        EnsureAnimTimer(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    // ---- Top bar & navigation -----------------------------------------------------
    if (a == act::ToggleSettings) {
        if (g_Ui.showSettings) {
            g_Ui.showSettings = false;
            g_SettingsHwTabOpen.store(false, std::memory_order_release);
            g_Animator.SetSettingsOpen(false);
            g_Animator.SetSettingsTab(0.0f, 0.0f, true);
            g_SettingsTabSnapped = false;
            g_SidebarIndSnapped = false;
            g_Animator.TriggerViewTransition();
            EnsureAnimTimer(hwnd);
            UpdateTriggerPreviewState(hwnd);
        } else {
            OpenSettingsAtTab(hwnd, g_Ui.settingsTab);
        }
    } else if (a == act::OpenHardwareTab) {
        OpenSettingsAtTab(hwnd, 4);
    } else if (act::InRange(a, act::SettingsTab, 5)) {
        OpenSettingsAtTab(hwnd, a - act::SettingsTab);
    } else if (act::InRange(a, act::SidebarFilter, 5)) {
        g_Ui.sidebarFilter = a - act::SidebarFilter;
        g_Ui.listScroll = 0.0f;
        g_Animator.SetListScrollTarget(0.0f, true);
        g_SidebarIndSnapped = false;
    } else if (a == act::CycleTopLayer) {
        const uint8_t next = static_cast<uint8_t>((g_SharedState.activeRemapLayer.load(std::memory_order_relaxed) + 1U) & 3U);
        g_SharedState.activeRemapLayer.store(next, std::memory_order_release);
        if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->RecomputeSuppressedMask();
    } else if (a == act::ToggleTopLock || a == act::RemapToggleUnlockLive) {
        const bool next = !g_SharedState.remapUnlocked.load(std::memory_order_relaxed);
        g_SharedState.remapUnlocked.store(next, std::memory_order_release);
        if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->RecomputeSuppressedMask();
    } else if (a == act::TogglePause) {
        const bool nowPaused = !g_SharedState.automationPaused.load(std::memory_order_acquire);
        g_LastObservedPaused = nowPaused;
        g_SharedState.automationPaused.store(nowPaused, std::memory_order_release);
        if (nowPaused) {
            StopEverythingRunning();
            PlayFeedbackTone(false);
            ShowToast(hwnd, L"Paused. Macros, sequences, triggers and remaps are suspended.");
        } else {
            g_SharedState.safeStopReason.store(0U, std::memory_order_release);
            if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->RecomputeSuppressedMask();
            if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->ResetArmedStates();
            mask::input::WakeEngine();
            PlayFeedbackTone(true);
            ShowToast(hwnd, L"Automation is active again.");
        }
        UpdateTrayTooltip(hwnd);
    } else if (a == act::StopAll) {
        StopEverythingRunning();
        PlayFeedbackTone(false);
        ShowToast(hwnd, L"Stopped everything running and released all synthetic keys.");
    } else if (a == act::OutputAction) {
        if (g_Ui.outputState == OutputState::NeedsAdmin) {
            RestartAsAdministrator(hwnd);
        } else if (!g_OutputWorkerBusy.load(std::memory_order_acquire)) {
            g_AutoReconnectAttempts = 0;
            StartOutputWorker();
            ShowToast(hwnd, L"Connecting the virtual keyboard and mouse\u2026");
        }
    } else if (a == act::ToastAction) {
        if (g_Ui.hasUndo) {
            g_Ui.hasUndo = false;
            const int u = g_Ui.undoSlot;
            wchar_t msg[120]{};
            switch (g_Ui.undoKind) {
                case ItemKind::Macro:
                    g_Ui.slots[u] = g_Ui.undoConfig;
                    g_Ui.SyncSlotToEngine(u);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Macro, u));
                    SelectItem(hwnd, ItemKind::Macro, u);
                    std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", g_Ui.slots[u].name);
                    break;
                case ItemKind::Sequence:
                    g_Ui.sequences[u] = g_Ui.undoSeq;
                    if (g_SeqEnginePtr != nullptr) g_SeqEnginePtr->UpdateConfig(static_cast<size_t>(u), g_Ui.sequences[u]);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Sequence, u));
                    SelectItem(hwnd, ItemKind::Sequence, u);
                    std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", g_Ui.sequences[u].name);
                    break;
                case ItemKind::Trigger:
                    g_Ui.triggers[u] = g_Ui.undoTrigger;
                    if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->UpdateConfig(static_cast<size_t>(u), g_Ui.triggers[u]);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Trigger, u));
                    SelectItem(hwnd, ItemKind::Trigger, u);
                    std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", g_Ui.triggers[u].name);
                    break;
                case ItemKind::Remap:
                    g_Ui.remaps[u] = g_Ui.undoRemap;
                    if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->UpdateRule(static_cast<size_t>(u), g_Ui.remaps[u]);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Remap, u));
                    SelectItem(hwnd, ItemKind::Remap, u);
                    std::swprintf(msg, std::size(msg), L"Restored \u201C%ls\u201D.", g_Ui.remaps[u].name);
                    break;
            }
            g_Ui.PublishWatchedKeys();
            g_Ui.MarkDirty();
            SyncAnimatorToggles(false);
            ShowToast(hwnd, msg);
        }
    }
    // ---- Sidebar Select / Toggle --------------------------------------------------
    else if (act::InRange(a, act::SelectMacro, kSlotCount)) {
        SelectItem(hwnd, ItemKind::Macro, a - act::SelectMacro);
    } else if (act::InRange(a, act::SelectSeq, kComplexSeqCount)) {
        SelectItem(hwnd, ItemKind::Sequence, a - act::SelectSeq);
    } else if (act::InRange(a, act::SelectTrigger, kColorTriggerCount)) {
        g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
        SelectItem(hwnd, ItemKind::Trigger, a - act::SelectTrigger);
    } else if (act::InRange(a, act::SelectRemap, kRemapRuleCount)) {
        g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
        SelectItem(hwnd, ItemKind::Remap, a - act::SelectRemap);
    } else if (act::InRange(a, act::ToggleMacro, kSlotCount)) {
        const int i = a - act::ToggleMacro;
        auto& s = g_Ui.slots[i];
        if (!s.enabled && s.triggerVk == 0) {
            SelectItem(hwnd, ItemKind::Macro, i);
            StartBinding(hwnd, 1U, L"Press the key or mouse button that should start this macro.");
        } else {
            s.enabled = !s.enabled;
            g_Animator.SetToggle(AnimSlotFor(ItemKind::Macro, i), s.enabled);
            g_Ui.SyncSlotToEngine(i);
            EnsureAnimTimer(hwnd);
        }
    } else if (act::InRange(a, act::ToggleSeq, kComplexSeqCount)) {
        const int i = a - act::ToggleSeq;
        auto& seq = g_Ui.sequences[i];
        seq.enabled = !seq.enabled;
        g_Animator.SetToggle(AnimSlotFor(ItemKind::Sequence, i), seq.enabled);
        if (g_SeqEnginePtr != nullptr) g_SeqEnginePtr->UpdateConfig(static_cast<size_t>(i), seq);
        g_Ui.PublishWatchedKeys();
        g_Ui.MarkDirty();
        EnsureAnimTimer(hwnd);
    } else if (act::InRange(a, act::ToggleTrigger, kColorTriggerCount)) {
        const int i = a - act::ToggleTrigger;
        auto& t = g_Ui.triggers[i];
        t.enabled = !t.enabled;
        g_Animator.SetToggle(AnimSlotFor(ItemKind::Trigger, i), t.enabled);
        if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->UpdateConfig(static_cast<size_t>(i), t);
        g_Ui.PublishWatchedKeys();
        g_Ui.MarkDirty();
        EnsureAnimTimer(hwnd);
    } else if (act::InRange(a, act::ToggleRemap, kRemapRuleCount)) {
        const int i = a - act::ToggleRemap;
        auto& r = g_Ui.remaps[i];
        if (!r.enabled && r.sourceVk == 0) {
            SelectItem(hwnd, ItemKind::Remap, i);
            StartBinding(hwnd, 22U, L"Choose a source key before enabling this remap rule.");
        } else {
            r.enabled = !r.enabled;
            g_Animator.SetToggle(AnimSlotFor(ItemKind::Remap, i), r.enabled);
            if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->UpdateRule(static_cast<size_t>(i), r);
            g_Ui.PublishWatchedKeys();
            g_Ui.MarkDirty();
            EnsureAnimTimer(hwnd);
        }
    }
    // ---- Settings actions ---------------------------------------------------------
    else if (a >= act::SetToggleStartWin && a <= act::SetToggleSound) {
        auto& st = g_Ui.settings;
        switch (a) {
            case act::SetToggleStartWin:
                st.startWithWindows = !st.startWithWindows;
                if (!mask::settings::SetStartWithWindows(st.startWithWindows, st.startMinimized)) {
                    st.startWithWindows = mask::settings::QueryStartWithWindows();
                    ShowToast(hwnd, L"Couldn't update Windows startup registry entry.");
                }
                break;
            case act::SetToggleStartMin:
                st.startMinimized = !st.startMinimized;
                if (st.startWithWindows) (void)mask::settings::SetStartWithWindows(true, st.startMinimized);
                break;
            case act::SetToggleMinTray:      st.minimizeToTray = !st.minimizeToTray; break;
            case act::SetToggleTopmost:      st.alwaysOnTop = !st.alwaysOnTop; ApplyAlwaysOnTop(hwnd); break;
            case act::SetToggleBounds:       st.rememberWindowBounds = !st.rememberWindowBounds; break;
            case act::SetToggleConfirmDel:   st.confirmDelete = !st.confirmDelete; break;
            case act::SetToggleCompact:      st.compactSidebar = !st.compactSidebar; g_SidebarIndSnapped = false; break;
            case act::SetToggleSignal:       st.showSignalView = !st.showSignalView; break;
            case act::SetToggleLiveKeys:     st.showLiveKeyIndicators = !st.showLiveKeyIndicators; break;
            case act::SetToggleLowRes:       st.lowResourceWhenUnfocused = !st.lowResourceWhenUnfocused; UpdateUiTimerCadence(hwnd); break;
            case act::SetToggleTrimMem:      st.trimMemoryWhenMinimized = !st.trimMemoryWhenMinimized; break;
            case act::SetTogglePanic:        st.panicChordEnabled = !st.panicChordEnabled; break;
            case act::SetToggleSessionLock:  st.stopOnSessionLock = !st.stopOnSessionLock; break;
            case act::SetToggleSuppressType: st.suppressWhileTypingInApp = !st.suppressWhileTypingInApp; UpdateTriggerSuppression(); break;
            case act::SetToggleSound:        st.soundFeedback = !st.soundFeedback; break;
            default: break;
        }
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
    } else if (act::InRange(a, act::SetAnimQuality, 3)) {
        g_Ui.settings.animationQuality = static_cast<uint8_t>(a - act::SetAnimQuality);
        g_Animator.SetQuality(g_Ui.settings.animationQuality);
        g_Ui.listScroll    = g_Animator.listScrollSmooth.value;
        g_Ui.seqStepScroll = g_Animator.seqScrollSmooth.value;
        g_Ui.MarkDirty();
        EnsureAnimTimer(hwnd);
    } else if (act::InRange(a, act::SetAccent, 5)) {
        g_Ui.settings.accentPreset = static_cast<uint8_t>(a - act::SetAccent);
        g_Ui.MarkDirty();
    } else if (act::InRange(a, act::SetPowerMode, 3)) {
        g_Ui.settings.powerMode = static_cast<uint8_t>(a - act::SetPowerMode);
        g_Ui.ApplySettings();
        mask::input::WakeEngine();
        g_Ui.MarkDirty();
    } else if (act::InRange(a, act::SetPollHz, 3)) {
        static constexpr uint32_t kHz[3] = { 250U, 500U, 1000U };
        g_Ui.settings.armedPollHz = kHz[a - act::SetPollHz];
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
    } else if (act::InRange(a, act::SetSpinPolicy, 3)) {
        g_Ui.settings.spinPolicy = static_cast<uint8_t>(a - act::SetSpinPolicy);
        g_Ui.ApplySettings();
        mask::input::WakeEngine();
        g_Ui.MarkDirty();
    } else if (a == act::SetBindPauseVk) {
        StartBinding(hwnd, 24U, L"Press the key or mouse button for Global Pause / Resume.");
    } else if (a == act::SetResetPauseVk) {
        g_Ui.settings.pauseHotkeyVk = VK_F8;
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
        ShowToast(hwnd, L"Global pause hotkey reset to F8.");
    } else if (a == act::SetClearPauseVk) {
        g_Ui.settings.pauseHotkeyVk = 0;
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
        ShowToast(hwnd, L"Global pause hotkey cleared.");
    } else if (a == act::SetBindStopAllVk) {
        StartBinding(hwnd, 25U, L"Press the key or mouse button for Global Stop All.");
    } else if (a == act::SetClearStopAllVk) {
        g_Ui.settings.stopAllHotkeyVk = 0;
        g_Ui.ApplySettings();
        g_Ui.MarkDirty();
        ShowToast(hwnd, L"Global Stop All hotkey cleared.");
    } else if (act::InRange(a, act::SetProfileBank, 3)) {
        const uint8_t newBank = static_cast<uint8_t>(a - act::SetProfileBank);
        if (newBank != g_Ui.settings.activeProfileBank) {
            StopEverythingRunning();
            g_Ui.SaveToDisk();
            g_Ui.settings.activeProfileBank = newBank;
            (void)g_Ui.SaveSettingsOnly();
            (void)g_Ui.LoadFromDisk();
            SyncAllEnginesFromModel();
            SyncAnimatorToggles(true);
            UpdateTrayTooltip(hwnd);
            wchar_t msg[64]{};
            std::swprintf(msg, std::size(msg), L"Switched to Profile Bank %u.", static_cast<unsigned>(newBank + 1U));
            ShowToast(hwnd, msg);
        }
    } else if (a == act::SetOpenDataFolder) {
        ShellExecuteW(hwnd, L"open", g_Ui.dataDir, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (a == act::SetResetAll) {
        if (!g_Ui.pendingResetConfirm) {
            g_Ui.pendingResetConfirm = true;
            ShowToast(hwnd, L"Click Reset All again to reset the current profile bank.");
        } else {
            g_Ui.pendingResetConfirm = false;
            StopEverythingRunning();
            g_Ui.InitializeDefaults();
            SyncAllEnginesFromModel();
            SyncAnimatorToggles(true);
            g_Ui.SaveToDisk();
            ShowToast(hwnd, L"Current profile bank reset to clean defaults.");
        }
    } else if (a == act::SetRefreshHw || a == act::RemapRefreshDevices) {
        if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->RefreshMonitors();
        g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
        ShowToast(hwnd, L"Refreshed physical GameInput devices and connected monitors.");
    } else if (act::InRange(a, act::SelectMouseProfile, kMouseProfileCount)) {
        if (g_OutputWorkerBusy.load(std::memory_order_acquire)) {
            ShowToast(hwnd, L"Virtual devices are still updating. Try again in a moment.");
        } else {
            const auto& preset = kMouseProfiles[a - act::SelectMouseProfile];
            wcsncpy_s(g_Ui.mouseProfileId, preset.id, _TRUNCATE);
            wcsncpy_s(g_Ui.settings.mouseProfileId, preset.id, _TRUNCATE);
            g_Ui.MarkDirty();
            StartOutputWorker();
            wchar_t msg[96]{};
            std::swprintf(msg, std::size(msg), L"Switching virtual mouse to %ls\u2026", preset.label);
            ShowToast(hwnd, msg);
        }
    } else if (act::InRange(a, act::SelectKbProfile, kKbProfileCount)) {
        if (g_OutputWorkerBusy.load(std::memory_order_acquire)) {
            ShowToast(hwnd, L"Virtual devices are still updating. Try again in a moment.");
        } else {
            const auto& preset = kKbProfiles[a - act::SelectKbProfile];
            wcsncpy_s(g_Ui.keyboardProfileId, preset.id, _TRUNCATE);
            wcsncpy_s(g_Ui.settings.keyboardProfileId, preset.id, _TRUNCATE);
            g_Ui.MarkDirty();
            StartOutputWorker();
            wchar_t msg[96]{};
            std::swprintf(msg, std::size(msg), L"Switching virtual keyboard to %ls\u2026", preset.label);
            ShowToast(hwnd, msg);
        }
    }
    // ---- Shared Field Focus / Inc / Dec -------------------------------------------
    else if (act::InRange(a, act::FieldFocus, 64)) {
        BeginEdit(static_cast<Field>(a - act::FieldFocus));
    } else if (act::InRange(a, act::FieldDec, 64)) {
        StepField(static_cast<Field>(a - act::FieldDec), -1, shift, ctrl);
    } else if (act::InRange(a, act::FieldInc, 64)) {
        StepField(static_cast<Field>(a - act::FieldInc), +1, shift, ctrl);
    }
    // ---- Selected Item Common Actions (Delete / Duplicate / ToggleEnabled) --------
    else if (g_Ui.HasSelection()) {
        const ItemKind kind = g_Ui.selectedKind;
        const int sel = g_Ui.selectedIndex;

        if (a == act::DeleteItem) {
            if (g_Ui.settings.confirmDelete && !g_Ui.pendingDeleteConfirm) {
                g_Ui.pendingDeleteConfirm = true;
                ShowToast(hwnd, L"Click Delete again to confirm removal.");
            } else {
                g_Ui.pendingDeleteConfirm = false;
                const int delId = EncodeDeleteId(kind, sel);
                const size_t animSlot = AnimSlotFor(kind, sel);
                if (g_Ui.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off)) {
                    FinalizeItemDelete(hwnd, delId);
                } else {
                    g_Animator.TriggerRowDelete(delId, animSlot);
                    EnsureAnimTimer(hwnd);
                }
            }
        } else if (a == act::ToggleEnabled) {
            if (kind == ItemKind::Macro) {
                auto& slot = g_Ui.Selected();
                if (!slot.enabled && slot.triggerVk == 0) {
                    StartBinding(hwnd, 1U, L"Choose a trigger first: press any key or mouse button.");
                } else {
                    slot.enabled = !slot.enabled;
                    g_Animator.SetToggle(AnimSlotFor(ItemKind::Macro, sel), slot.enabled);
                    g_Ui.SyncSlotToEngine(sel);
                    EnsureAnimTimer(hwnd);
                }
            } else if (kind == ItemKind::Sequence) {
                auto& seq = g_Ui.SelectedSeq();
                seq.enabled = !seq.enabled;
                g_Animator.SetToggle(AnimSlotFor(ItemKind::Sequence, sel), seq.enabled);
                SyncSelectedSequence();
                EnsureAnimTimer(hwnd);
            } else if (kind == ItemKind::Trigger) {
                auto& t = g_Ui.SelectedTrigger();
                t.enabled = !t.enabled;
                g_Animator.SetToggle(AnimSlotFor(ItemKind::Trigger, sel), t.enabled);
                if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->UpdateConfig(static_cast<size_t>(sel), t);
                g_Ui.PublishWatchedKeys();
                g_Ui.MarkDirty();
                EnsureAnimTimer(hwnd);
            } else if (kind == ItemKind::Remap) {
                auto& r = g_Ui.SelectedRemap();
                if (!r.enabled && r.sourceVk == 0) {
                    StartBinding(hwnd, 22U, L"Choose a source key first.");
                } else {
                    r.enabled = !r.enabled;
                    g_Animator.SetToggle(AnimSlotFor(ItemKind::Remap, sel), r.enabled);
                    SyncSelectedRemap();
                    EnsureAnimTimer(hwnd);
                }
            }
        } else if (a == act::DuplicateItem) {
            if (kind == ItemKind::Macro) {
                const int target = g_Ui.FindFirstFreeSlot();
                if (target < 0) {
                    ShowToast(hwnd, L"All 64 macro slots are in use.");
                } else {
                    MacroConfig copy = g_Ui.Selected();
                    copy.slotIndex = static_cast<uint8_t>(target);
                    copy.enabled = false;
                    copy.reserved[0] = 1;
                    std::swprintf(copy.name, std::size(copy.name), L"%.18ls copy", g_Ui.Selected().name);
                    g_Ui.slots[target] = copy;
                    g_Ui.SyncSlotToEngine(target);
                    g_Animator.SetToggle(AnimSlotFor(ItemKind::Macro, target), false, true);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Macro, target));
                    SelectItem(hwnd, ItemKind::Macro, target);
                    ShowToast(hwnd, L"Duplicated macro (switched off until you enable it).");
                }
            } else if (kind == ItemKind::Sequence) {
                const int target = g_Ui.FindFirstFreeSeq();
                if (target < 0) {
                    ShowToast(hwnd, L"All 16 complex sequence slots are in use.");
                } else {
                    ComplexSequenceConfig copy = g_Ui.SelectedSeq();
                    copy.slotIndex = static_cast<uint8_t>(target);
                    copy.enabled = false;
                    copy.inUse = true;
                    std::swprintf(copy.name, std::size(copy.name), L"%.24ls copy", g_Ui.SelectedSeq().name);
                    g_Ui.sequences[target] = copy;
                    if (g_SeqEnginePtr != nullptr) g_SeqEnginePtr->UpdateConfig(static_cast<size_t>(target), copy);
                    g_Ui.MarkDirty();
                    g_Animator.SetToggle(AnimSlotFor(ItemKind::Sequence, target), false, true);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Sequence, target));
                    SelectItem(hwnd, ItemKind::Sequence, target);
                    ShowToast(hwnd, L"Duplicated complex sequence.");
                }
            } else if (kind == ItemKind::Trigger) {
                const int target = g_Ui.FindFirstFreeTrigger();
                if (target < 0) {
                    ShowToast(hwnd, L"All 8 trigger slots are in use.");
                } else {
                    ColorTriggerConfig copy = g_Ui.SelectedTrigger();
                    copy.slotIndex = static_cast<uint8_t>(target);
                    copy.enabled = false;
                    copy.inUse = true;
                    std::swprintf(copy.name, std::size(copy.name), L"%.24ls copy", g_Ui.SelectedTrigger().name);
                    g_Ui.triggers[target] = copy;
                    if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->UpdateConfig(static_cast<size_t>(target), copy);
                    g_Ui.PublishWatchedKeys();
                    g_Ui.MarkDirty();
                    g_Animator.SetToggle(AnimSlotFor(ItemKind::Trigger, target), false, true);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Trigger, target));
                    SelectItem(hwnd, ItemKind::Trigger, target);
                    ShowToast(hwnd, L"Duplicated trigger.");
                }
            } else if (kind == ItemKind::Remap) {
                const int target = g_Ui.FindFirstFreeRemap();
                if (target < 0) {
                    ShowToast(hwnd, L"All 32 remap slots are in use.");
                } else {
                    RemapRuleConfig copy = g_Ui.SelectedRemap();
                    copy.slotIndex = static_cast<uint8_t>(target);
                    copy.enabled = false;
                    copy.inUse = true;
                    std::swprintf(copy.name, std::size(copy.name), L"%.24ls copy", g_Ui.SelectedRemap().name);
                    g_Ui.remaps[target] = copy;
                    if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->UpdateRule(static_cast<size_t>(target), copy);
                    g_Ui.MarkDirty();
                    g_Animator.SetToggle(AnimSlotFor(ItemKind::Remap, target), false, true);
                    g_Animator.TriggerRowAdd(AnimSlotFor(ItemKind::Remap, target));
                    SelectItem(hwnd, ItemKind::Remap, target);
                    ShowToast(hwnd, L"Duplicated remap rule.");
                }
            }
        }
        // ---- Macro-specific actions -----------------------------------------------
        else if (kind == ItemKind::Macro) {
            auto& slot = g_Ui.Selected();
            const bool socd = static_cast<MacroMode>(slot.mode) == MacroMode::SocdPair;
            if (act::InRange(a, act::BindStep, kStepCount)) {
                StartBinding(hwnd, static_cast<uint8_t>(3 + (a - act::BindStep)), L"Press the key or mouse button for this step.");
            } else if (act::InRange(a, act::Mode, kModeCount)) {
                slot.mode = static_cast<uint8_t>(a - act::Mode);
                g_Ui.SyncSlotToEngine(sel);
            } else if (act::InRange(a, act::QuickOut, kQuickOutputCount)) {
                slot.outputVk = kQuickOutputs[a - act::QuickOut].vk;
                g_Ui.SyncSlotToEngine(sel);
            } else if (act::InRange(a, act::HumanizeBase, kHumanizeCount)) {
                mask::core::SetHumanizePercent(slot, kHumanizePresets[a - act::HumanizeBase].pct);
                g_Ui.SyncSlotToEngine(sel);
            } else if (a == act::BindTrigger) {
                StartBinding(hwnd, 1U, socd ? L"Press the first key of the pair." : L"Press the key or mouse button that should start this macro.");
            } else if (a == act::BindOutput) {
                StartBinding(hwnd, 2U, socd ? L"Press the second key of the pair." : L"Press the key or mouse button this macro should send.");
            } else if (a == act::StepAdd && slot.stepCount < kStepCount) {
                ++slot.stepCount;
                g_Ui.SyncSlotToEngine(sel);
            } else if (a == act::StepRemove && slot.stepCount > 1) {
                --slot.stepCount;
                g_Ui.SyncSlotToEngine(sel);
            }
        }
        // ---- Complex Sequence actions ---------------------------------------------
        else if (kind == ItemKind::Sequence) {
            auto& seq = g_Ui.SelectedSeq();
            if (a == act::SeqToggleStepKindMenu) {
                g_Ui.showStepKindMenu = !g_Ui.showStepKindMenu;
            } else if (a == act::SeqBindTrigger) {
                StartBinding(hwnd, 20U, L"Press the key or mouse button to trigger this sequence.");
            } else if (a == act::SeqStepBindKey) {
                StartBinding(hwnd, 21U, L"Press the key or mouse button for this workflow step.");
            } else if (act::InRange(a, act::SeqTriggerMode, 3)) {
                seq.triggerMode = static_cast<uint8_t>(a - act::SeqTriggerMode);
                SyncSelectedSequence();
            } else if (a == act::SeqCycleHumanize) {
                int cur = 0;
                for (int i = 0; i < kHumanizeCount; ++i) {
                    if (seq.humanizePct == kHumanizePresets[i].pct) { cur = i; break; }
                }
                seq.humanizePct = kHumanizePresets[(cur + 1) % kHumanizeCount].pct;
                SyncSelectedSequence();
            } else if (act::InRange(a, act::SelectSeqStep, kComplexStepMax)) {
                g_Ui.selectedSeqStep = std::clamp(a - act::SelectSeqStep, 0, std::max(0, static_cast<int>(seq.stepCount) - 1));
            } else if (a == act::SeqStepAdd) {
                if (seq.stepCount < kComplexStepMax) {
                    const int insertPos = (seq.stepCount == 0U)
                        ? 0
                        : std::clamp(g_Ui.selectedSeqStep + 1, 0, static_cast<int>(seq.stepCount));
                    for (int i = static_cast<int>(seq.stepCount); i > insertPos; --i) {
                        seq.steps[i] = seq.steps[i - 1];
                    }
                    ComplexStep ns{};
                    ns.kind = static_cast<uint8_t>(ComplexStepKind::KeyClick);
                    ns.vk = '1';
                    ns.durationMs = 25;
                    seq.steps[insertPos] = ns;
                    ++seq.stepCount;
                    g_Ui.selectedSeqStep = insertPos;
                    EnsureSelectedSeqStepVisible();
                    SyncSelectedSequence();
                } else {
                    ShowToast(hwnd, L"Maximum 128 steps reached for this sequence.");
                }
            } else if (a == act::SeqStepInsert) {
                if (seq.stepCount < kComplexStepMax) {
                    const int pos = std::clamp(g_Ui.selectedSeqStep, 0, static_cast<int>(seq.stepCount));
                    for (int i = static_cast<int>(seq.stepCount); i > pos; --i) {
                        seq.steps[i] = seq.steps[i - 1];
                    }
                    ComplexStep ns{};
                    ns.kind = static_cast<uint8_t>(ComplexStepKind::WaitMs);
                    ns.durationMs = 50;
                    seq.steps[pos] = ns;
                    ++seq.stepCount;
                    g_Ui.selectedSeqStep = pos;
                    EnsureSelectedSeqStepVisible();
                    SyncSelectedSequence();
                }
            } else if (a == act::SeqStepDup) {
                if (seq.stepCount > 0 && seq.stepCount < kComplexStepMax) {
                    const int pos = std::clamp(g_Ui.selectedSeqStep, 0, static_cast<int>(seq.stepCount) - 1);
                    for (int i = static_cast<int>(seq.stepCount); i > pos + 1; --i) {
                        seq.steps[i] = seq.steps[i - 1];
                    }
                    seq.steps[pos + 1] = seq.steps[pos];
                    ++seq.stepCount;

                    const auto dupKind = static_cast<ComplexStepKind>(seq.steps[pos].kind);
                    if ((dupKind == ComplexStepKind::LoopStart || dupKind == ComplexStepKind::IfCheck) &&
                        seq.stepCount < kComplexStepMax) {
                        for (int i = static_cast<int>(seq.stepCount); i > pos + 2; --i) {
                            seq.steps[i] = seq.steps[i - 1];
                        }
                        ComplexStep closer{};
                        closer.kind = static_cast<uint8_t>(
                            dupKind == ComplexStepKind::LoopStart ? ComplexStepKind::LoopEnd : ComplexStepKind::EndIf);
                        seq.steps[pos + 2] = closer;
                        ++seq.stepCount;
                    }

                    g_Ui.selectedSeqStep = pos + 1;
                    EnsureSelectedSeqStepVisible();
                    SyncSelectedSequence();
                }
            } else if (a == act::SeqStepUp) {
                if (g_Ui.selectedSeqStep > 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    std::swap(seq.steps[g_Ui.selectedSeqStep], seq.steps[g_Ui.selectedSeqStep - 1]);
                    --g_Ui.selectedSeqStep;
                    EnsureSelectedSeqStepVisible();
                    SyncSelectedSequence();
                }
            } else if (a == act::SeqStepDown) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep + 1 < static_cast<int>(seq.stepCount)) {
                    std::swap(seq.steps[g_Ui.selectedSeqStep], seq.steps[g_Ui.selectedSeqStep + 1]);
                    ++g_Ui.selectedSeqStep;
                    EnsureSelectedSeqStepVisible();
                    SyncSelectedSequence();
                }
            } else if (a == act::SeqStepDel) {
                if (seq.stepCount > 0 && g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    for (int i = g_Ui.selectedSeqStep; i + 1 < static_cast<int>(seq.stepCount); ++i) {
                        seq.steps[i] = seq.steps[i + 1];
                    }
                    --seq.stepCount;
                    g_Ui.selectedSeqStep = std::clamp(g_Ui.selectedSeqStep, 0, std::max(0, static_cast<int>(seq.stepCount) - 1));
                    EnsureSelectedSeqStepVisible();
                    SyncSelectedSequence();
                }
            } else if (act::InRange(a, act::SeqStepKindBase, 17)) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    const int pos = g_Ui.selectedSeqStep;
                    auto& st = seq.steps[pos];
                    const auto prevKind = static_cast<ComplexStepKind>(st.kind);
                    st.kind = static_cast<uint8_t>(a - act::SeqStepKindBase);
                    const auto nk = static_cast<ComplexStepKind>(st.kind);

                    if (nk == ComplexStepKind::MouseScroll && (st.param == 0U || static_cast<int32_t>(st.param) > 120 || static_cast<int32_t>(st.param) < -120)) {
                        st.param = 1U;
                    }
                    if (nk == ComplexStepKind::WaitForColor) {
                        st.durationMs = 0U;
                        if (st.param == 0U) st.param = 3000U;
                    }
                    if (nk == ComplexStepKind::LoopStart && (prevKind != ComplexStepKind::LoopStart || st.param == 0U)) {
                        st.param = 3U;
                    }
                    if (nk == ComplexStepKind::IfCheck && prevKind != ComplexStepKind::IfCheck) {
                        mask::core::SetStepConditionKind(st, ComplexConditionKind::KeyHeld);
                        mask::core::SetStepConditionNegated(st, false);
                        if (st.vk == 0U) st.vk = VK_LSHIFT;
                        if (st.tolerance == 0U) st.tolerance = 24U;
                    }
                    if (nk == ComplexStepKind::KeyClick && st.vk == 0U) st.vk = '1';

                    auto removeStepAt = [&](int rIdx) noexcept {
                        for (int i = rIdx; i + 1 < static_cast<int>(seq.stepCount); ++i) {
                            seq.steps[i] = seq.steps[i + 1];
                        }
                        --seq.stepCount;
                    };

                    // Empty adjacent closer cleanup when switching away from LoopStart or IfCheck
                    if (prevKind == ComplexStepKind::LoopStart && nk != ComplexStepKind::LoopStart) {
                        if (pos + 1 < static_cast<int>(seq.stepCount) &&
                            seq.steps[pos + 1].kind == static_cast<uint8_t>(ComplexStepKind::LoopEnd) &&
                            CountStepKind(seq, ComplexStepKind::LoopEnd) > CountStepKind(seq, ComplexStepKind::LoopStart)) {
                            removeStepAt(pos + 1);
                        }
                    } else if (prevKind == ComplexStepKind::IfCheck && nk != ComplexStepKind::IfCheck) {
                        if (pos + 1 < static_cast<int>(seq.stepCount) &&
                            seq.steps[pos + 1].kind == static_cast<uint8_t>(ComplexStepKind::EndIf) &&
                            CountStepKind(seq, ComplexStepKind::EndIf) > CountStepKind(seq, ComplexStepKind::IfCheck)) {
                            removeStepAt(pos + 1);
                        }
                    }

                    // Auto-spawn matching closer for LoopStart / IfCheck
                    if (nk == ComplexStepKind::LoopStart && prevKind != ComplexStepKind::LoopStart) {
                        if (CountStepKind(seq, ComplexStepKind::LoopStart) > CountStepKind(seq, ComplexStepKind::LoopEnd)) {
                            if (seq.stepCount < kComplexStepMax) {
                                for (int i = static_cast<int>(seq.stepCount); i > pos + 1; --i) {
                                    seq.steps[i] = seq.steps[i - 1];
                                }
                                ComplexStep endSt{};
                                endSt.kind = static_cast<uint8_t>(ComplexStepKind::LoopEnd);
                                seq.steps[pos + 1] = endSt;
                                ++seq.stepCount;
                                g_Ui.selectedSeqStep = pos;
                                EnsureSelectedSeqStepVisible();
                                ShowToast(hwnd, L"Added matching End loop step.");
                            } else {
                                ShowToast(hwnd, L"Sequence is full (128 steps); couldn\u2019t auto-add End loop.");
                            }
                        }
                    } else if (nk == ComplexStepKind::IfCheck && prevKind != ComplexStepKind::IfCheck) {
                        if (CountStepKind(seq, ComplexStepKind::IfCheck) > CountStepKind(seq, ComplexStepKind::EndIf)) {
                            if (seq.stepCount < kComplexStepMax) {
                                for (int i = static_cast<int>(seq.stepCount); i > pos + 1; --i) {
                                    seq.steps[i] = seq.steps[i - 1];
                                }
                                ComplexStep endSt{};
                                endSt.kind = static_cast<uint8_t>(ComplexStepKind::EndIf);
                                seq.steps[pos + 1] = endSt;
                                ++seq.stepCount;
                                g_Ui.selectedSeqStep = pos;
                                EnsureSelectedSeqStepVisible();
                                ShowToast(hwnd, L"Added matching End if step.");
                            } else {
                                ShowToast(hwnd, L"Sequence is full (128 steps); couldn\u2019t auto-add End if.");
                            }
                        }
                    }
                    SyncSelectedSequence();
                }
            } else if (act::InRange(a, act::SeqStepCondKind, 6)) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    auto& st = seq.steps[g_Ui.selectedSeqStep];
                    const auto ck = static_cast<ComplexConditionKind>(a - act::SeqStepCondKind);
                    mask::core::SetStepConditionKind(st, ck);
                    if (ck == ComplexConditionKind::KeyHeld) {
                        if (st.vk == 0U) st.vk = VK_LSHIFT;
                    } else if (ck == ComplexConditionKind::ColorMatches) {
                        if (st.tolerance == 0U) st.tolerance = 24U;
                    } else if (ck == ComplexConditionKind::LayerIs ||
                               ck == ComplexConditionKind::UnlockStateIs ||
                               ck == ComplexConditionKind::MacroActive) {
                        st.param = 0U;
                    } else if (ck == ComplexConditionKind::RandomChance) {
                        if (st.param < 1U || st.param > 99U) st.param = 50U;
                    }
                    if (ck == ComplexConditionKind::UnlockStateIs) {
                        mask::core::SetStepConditionNegated(st, false);
                    }
                    SyncSelectedSequence();
                }
            } else if (act::InRange(a, act::SeqStepCondNegate, 2)) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    mask::core::SetStepConditionNegated(seq.steps[g_Ui.selectedSeqStep], (a - act::SeqStepCondNegate) != 0);
                    SyncSelectedSequence();
                }
            } else if (act::InRange(a, act::SeqStepCondUnlock, 2)) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    auto& st = seq.steps[g_Ui.selectedSeqStep];
                    st.param = static_cast<uint32_t>(a - act::SeqStepCondUnlock);
                    mask::core::SetStepConditionNegated(st, false);
                    SyncSelectedSequence();
                }
            } else if (a == act::SeqStepCondAddElse) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    const int pos = g_Ui.selectedSeqStep;
                    if (seq.steps[pos].kind == static_cast<uint8_t>(ComplexStepKind::IfCheck) &&
                        !HasElseInIfBlock(seq, pos) &&
                        seq.stepCount < kComplexStepMax) {
                        const int endIdx = FindMatchingEndIf(seq, pos);
                        const int insertAt = (endIdx >= 0) ? endIdx : (pos + 1);
                        for (int i = static_cast<int>(seq.stepCount); i > insertAt; --i) {
                            seq.steps[i] = seq.steps[i - 1];
                        }
                        ComplexStep elseSt{};
                        elseSt.kind = static_cast<uint8_t>(ComplexStepKind::ElseBranch);
                        seq.steps[insertAt] = elseSt;
                        ++seq.stepCount;
                        g_Ui.selectedSeqStep = insertAt;
                        EnsureSelectedSeqStepVisible();
                        SyncSelectedSequence();
                    }
                }
            } else if (act::InRange(a, act::SeqStepCondQuickPct, 4)) {
                static constexpr uint32_t kPcts[4] = { 10U, 25U, 50U, 75U };
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    seq.steps[g_Ui.selectedSeqStep].param = kPcts[a - act::SeqStepCondQuickPct];
                    SyncSelectedSequence();
                }
            } else if (act::InRange(a, act::SeqStepLayer, kRemapLayerCount)) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    seq.steps[g_Ui.selectedSeqStep].param = static_cast<uint32_t>(a - act::SeqStepLayer);
                    SyncSelectedSequence();
                }
            } else if (act::InRange(a, act::SeqStepQuickWait, 4)) {
                static constexpr uint32_t kWaits[4] = { 10U, 50U, 250U, 1000U };
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    seq.steps[g_Ui.selectedSeqStep].durationMs = kWaits[a - act::SeqStepQuickWait];
                    SyncSelectedSequence();
                }
            } else if (act::InRange(a, act::SeqStepQuickScroll, 2)) {
                if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                    seq.steps[g_Ui.selectedSeqStep].param = static_cast<uint32_t>((a == act::SeqStepQuickScroll) ? -1 : 1);
                    SyncSelectedSequence();
                }
            } else if (a == act::SeqStepPickColor) {
                g_Ui.seqColorPickActive = true;
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                ShowToast(hwnd, L"Click anywhere on any screen to capture X/Y and RGB color (Esc cancels).");
            }
        }
        // ---- Inline Color Trigger ("Trigger Bot") actions -------------------------
        else if (kind == ItemKind::Trigger) {
            auto& t = g_Ui.SelectedTrigger();
            if (a == act::TrigPickPixel) {
                if (g_Ui.triggerColorPickActive) {
                    CancelTriggerEyedropper(hwnd);
                } else {
                    g_Ui.triggerColorPickActive = true;
                    g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
                    POINT pt{};
                    if (GetCursorPos(&pt)) SampleTriggerEyedropperLoupe(pt.x, pt.y);
                    SetCapture(hwnd);
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                    UpdateTriggerPreviewState(hwnd);
                    ShowToast(hwnd, L"Click anywhere to capture screen pixel color and position (Esc cancels).");
                }
            } else if (a == act::TrigLockCenter) {
                t.sampleX = -1;
                t.sampleY = -1;
                SyncSelectedTrigger();
            } else if (a == act::TrigBindArmVk) {
                StartBinding(hwnd, 26U, L"Press the key or mouse button to arm this trigger.");
            } else if (a == act::TrigClearArmVk) {
                t.armVk = 0U;
                SyncSelectedTrigger();
            } else if (a == act::TrigBindActionVk) {
                StartBinding(hwnd, 27U, L"Press the key or mouse button this trigger should press.");
            } else if (act::InRange(a, act::TrigColorPreset, kTriggerColorPresetCount)) {
                const auto& p = kTriggerColorPresets[a - act::TrigColorPreset];
                t.r = p.r;
                t.g = p.g;
                t.b = p.b;
                if (t.mode == static_cast<uint8_t>(ColorTriggerMode::ColorChanged)) {
                    t.mode = static_cast<uint8_t>(ColorTriggerMode::MatchColor);
                }
                SyncSelectedTrigger();
            } else if (act::InRange(a, act::TrigMode, 2)) {
                t.mode = static_cast<uint8_t>(a - act::TrigMode);
                SyncSelectedTrigger();
            } else if (act::InRange(a, act::TrigMatchRule, 2)) {
                mask::core::SetTriggerMatchRule(t, static_cast<ColorTriggerMatchRule>(a - act::TrigMatchRule));
                SyncSelectedTrigger();
            } else if (act::InRange(a, act::TrigRadius, 5)) {
                static constexpr uint8_t kRadVals[5] = { 0U, 1U, 2U, 4U, 7U };
                t.sampleRadius = kRadVals[a - act::TrigRadius];
                SyncSelectedTrigger();
            } else if (act::InRange(a, act::TrigMonitor, 8)) {
                t.monitorIndex = static_cast<uint8_t>(a - act::TrigMonitor);
                SyncSelectedTrigger();
            } else if (act::InRange(a, act::TrigArmMode, 3)) {
                t.armMode = static_cast<uint8_t>(a - act::TrigArmMode);
                SyncSelectedTrigger();
            } else if (act::InRange(a, act::TrigActionKind, 4)) {
                t.actionKind = static_cast<uint8_t>(a - act::TrigActionKind);
                SyncSelectedTrigger();
            } else if (act::InRange(a, act::TrigQuickOut, 4)) {
                static constexpr uint16_t kQ[4] = { VK_LBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
                t.actionVk = kQ[a - act::TrigQuickOut];
                SyncSelectedTrigger();
            }
        }
        // ---- Remap Rule actions ---------------------------------------------------
        else if (kind == ItemKind::Remap) {
            auto& r = g_Ui.SelectedRemap();
            if (act::InRange(a, act::RemapLiveLayer, kRemapLayerCount)) {
                g_SharedState.activeRemapLayer.store(static_cast<uint8_t>(a - act::RemapLiveLayer), std::memory_order_release);
                if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->RecomputeSuppressedMask();
            } else if (a == act::RemapBindSource) {
                StartBinding(hwnd, 22U, L"Press the source key or mouse button to remap.");
            } else if (a == act::RemapBindTarget) {
                StartBinding(hwnd, 23U, L"Press the target key or mouse button to emit.");
            } else if (a == act::RemapCycleDevice) {
                g_Ui.RefreshDevicesAndMonitors(g_RemapEnginePtr);
                if (g_Ui.physicalDeviceCount <= 0) {
                    r.anyDevice = true;
                    wcscpy_s(r.deviceLabel, L"Any device");
                    ShowToast(hwnd, L"Press any key/mouse first so GameInput enumerates your physical devices.");
                } else if (r.anyDevice) {
                    r.anyDevice = false;
                    std::memcpy(r.deviceIdBytes, g_Ui.physicalDevices[0].deviceIdBytes, 32);
                    wcsncpy_s(r.deviceLabel, g_Ui.physicalDevices[0].displayName, _TRUNCATE);
                } else {
                    int found = -1;
                    for (int i = 0; i < g_Ui.physicalDeviceCount; ++i) {
                        if (std::memcmp(r.deviceIdBytes, g_Ui.physicalDevices[i].deviceIdBytes, 32) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found < 0 || found + 1 >= g_Ui.physicalDeviceCount) {
                        r.anyDevice = true;
                        std::memset(r.deviceIdBytes, 0, 32);
                        wcscpy_s(r.deviceLabel, L"Any device");
                    } else {
                        const auto& d = g_Ui.physicalDevices[found + 1];
                        std::memcpy(r.deviceIdBytes, d.deviceIdBytes, 32);
                        wcsncpy_s(r.deviceLabel, d.displayName, _TRUNCATE);
                    }
                }
                SyncSelectedRemap();
            } else if (act::InRange(a, act::RemapDevKind, 3)) {
                r.deviceKind = static_cast<uint8_t>(a - act::RemapDevKind);
                SyncSelectedRemap();
            } else if (act::InRange(a, act::RemapMonitor, 9)) {
                const int m = a - act::RemapMonitor;
                r.monitorIndex = (m == 0) ? static_cast<int8_t>(-1) : static_cast<int8_t>(m - 1);
                SyncSelectedRemap();
            } else if (act::InRange(a, act::RemapActionKind, 6)) {
                r.actionKind = static_cast<uint8_t>(a - act::RemapActionKind);
                SyncSelectedRemap();
            } else if (act::InRange(a, act::RemapRuleLayer, kRemapLayerCount)) {
                r.layerIndex = static_cast<uint8_t>(a - act::RemapRuleLayer);
                SyncSelectedRemap();
            } else if (act::InRange(a, act::RemapTargetLayer, kRemapLayerCount)) {
                r.targetLayer = static_cast<uint8_t>(a - act::RemapTargetLayer);
                SyncSelectedRemap();
            } else if (act::InRange(a, act::RemapUnlockPolicy, 3)) {
                r.unlockPolicy = static_cast<uint8_t>(a - act::RemapUnlockPolicy);
                SyncSelectedRemap();
            } else if (a == act::RemapToggleSuppress) {
                r.suppressOriginal = !r.suppressOriginal;
                SyncSelectedRemap();
            }
        }
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

// =====================================================================================
//  Engine thread
// =====================================================================================

void RunStartupPacingSelfTest(mask::timing::TimingEngine& timingEngine) noexcept {
    const uint64_t stepTsc = timingEngine.NanosecondsToTsc(250'000ULL);
    uint64_t targetTsc = mask::timing::TimingEngine::ReadTsc() + stepTsc;
    for (int i = 0; i < 20; ++i) {
        const uint64_t actualTsc = timingEngine.WaitUntilTsc(targetTsc);
        timingEngine.RecordJitterSampleTsc(targetTsc, actualTsc);
        targetTsc = actualTsc + stepTsc;
    }
}

void EngineThreadMain(mask::timing::TimingEngine* timingEngine, bool selfTest) noexcept {
    timingEngine->BoostCurrentThreadRealtimePriority();

    mask::input::GameInputGlobalContext::echoSuppressionWindowTsc.store(
        timingEngine->NanosecondsToTsc(2'000'000ULL), std::memory_order_release);

    mask::input::InputDispatcher dispatcher(g_SharedState);
    mask::runtime::MacroRuntime runtime(g_SharedState, *timingEngine, dispatcher);
    runtime.SetComplexSeqTriggerCallback(OnTriggerComplexSeq);

    uint32_t lastSpinPolicy = g_SharedState.spinPolicy.load(std::memory_order_relaxed);
    timingEngine->SetSpinPolicy(lastSpinPolicy);

    RunStartupPacingSelfTest(*timingEngine);

    const HANDLE wakeEvent = mask::input::GameInputGlobalContext::engineWakeEvent;
    constexpr DWORD    kIdleHeartbeatMs = 10;
    constexpr uint64_t kMaxSliceNs      = 10'000'000ULL;
    const uint64_t     periodicTelemetryTsc = timingEngine->NanosecondsToTsc(100'000'000ULL);

    uint64_t loopCounter    = 0;
    uint64_t lastActive     = ~0ULL;
    uint64_t lastToggled    = ~0ULL;
    bool     lastPaused     = false;
    uint8_t  lastLayer      = 0U;
    bool     lastUnlocked   = true;
    uint32_t lastDevCount   = ~0U;
    uint64_t lastDispatched = ~0ULL;
    uint64_t lastPublishTsc = 0;
    bool     firstPublish   = true;

    while (g_SharedState.engineRunning.load(std::memory_order_acquire)) {
        g_SharedState.heartbeatCounter.fetch_add(1ULL, std::memory_order_release);

        const uint32_t sp = g_SharedState.spinPolicy.load(std::memory_order_relaxed);
        if (sp != lastSpinPolicy) {
            lastSpinPolicy = sp;
            timingEngine->SetSpinPolicy(sp);
        }

        const uint64_t nowTsc = mask::timing::TimingEngine::ReadTsc();
        runtime.ProcessPendingQueues(nowTsc);
        runtime.EvaluateAndExecute(nowTsc);
        ++loopCounter;

        const uint64_t active     = runtime.GetActiveSlotBitmask();
        const uint64_t toggled    = runtime.GetToggledSlotBitmask();
        const bool     paused     = g_SharedState.automationPaused.load(std::memory_order_relaxed);
        const uint8_t  layer      = g_SharedState.activeRemapLayer.load(std::memory_order_relaxed);
        const bool     unlocked   = g_SharedState.remapUnlocked.load(std::memory_order_relaxed);
        const uint32_t devCount   = mask::input::GameInputGlobalContext::activePhysicalDeviceCount.load(std::memory_order_relaxed);
        const uint64_t dispatched = dispatcher.GetTotalDispatched();
        const bool     hwOpen     = g_SettingsHwTabOpen.load(std::memory_order_relaxed);
        const bool     periodicDue = (nowTsc - lastPublishTsc >= periodicTelemetryTsc) &&
                                     (hwOpen || (active != 0ULL && dispatched != lastDispatched));

        if (firstPublish || active != lastActive || toggled != lastToggled || paused != lastPaused ||
            layer != lastLayer || unlocked != lastUnlocked ||
            devCount != lastDevCount || periodicDue || (selfTest && (loopCounter & 0x3FULL) == 0ULL)) {
            mask::core::TelemetrySnapshot snap{};
            timingEngine->ComputeJitterPercentiles(snap.p50JitterNs, snap.p99JitterNs, snap.maxJitterNs);
            timingEngine->GetJitterHistogram(snap.jitterHistogram);
            snap.calibratedTscFreqHz      = timingEngine->GetTscFrequencyHz();
            snap.totalEventsDispatched    = dispatched;
            snap.totalHookEventsProcessed = runtime.GetTotalHookEventsProcessed();
            snap.engineLoopIterations     = loopCounter;
            snap.activeSlotBitmask        = active;
            snap.toggledSlotBitmask       = toggled;
            snap.activeSlotCount          = runtime.GetActiveSlotCount();
            snap.avx2Bmi2Active           = runtime.HasAvx2Bmi2();
            snap.gameInputDeviceCount     = devCount;
            g_SharedState.PublishTelemetry(snap);

            firstPublish   = false;
            lastActive     = active;
            lastToggled    = toggled;
            lastPaused     = paused;
            lastLayer      = layer;
            lastUnlocked   = unlocked;
            lastDevCount   = devCount;
            lastDispatched = dispatched;
            lastPublishTsc = nowTsc;
            NotifyUi();
        }

        if (!g_SharedState.inputEventQueue.empty() || !g_SharedState.uiCommandQueue.empty() ||
            !g_SharedState.auxOutputQueue.empty()) {
            continue;
        }

        const uint64_t earliest = runtime.GetEarliestActiveDeadlineTsc();
        if (earliest == std::numeric_limits<uint64_t>::max()) {
            WaitForSingleObject(wakeEvent, kIdleHeartbeatMs);
        } else {
            (void)timingEngine->WaitUntilTscOrEvent(earliest, wakeEvent, kMaxSliceNs);
        }
    }
}

// =====================================================================================
//  Input thread (Microsoft GameInput + dynamic armed poll cadence)
// =====================================================================================

void GameInputThreadMain(std::atomic<bool>* inputReadyFlag) noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    IGameInput* gameInput = nullptr;
    const HRESULT hr = GameInputCreate(&gameInput);
    if (SUCCEEDED(hr) && gameInput != nullptr) {
        gameInput->SetFocusPolicy(GameInputDefaultFocusPolicy);
        mask::input::GameInputGlobalContext::gameInputInstance.store(gameInput, std::memory_order_release);
        g_SharedState.gameInputConnected.store(true, std::memory_order_release);

        GameInputCallbackToken token = 0;
        const HRESULT cbHr = gameInput->RegisterReadingCallback(
            nullptr,
            GameInputKindKeyboard | GameInputKindMouse,
            0.0f,
            nullptr,
            mask::input::OnGameInputReadingCallback,
            &token
        );
        if (SUCCEEDED(cbHr)) {
            mask::input::GameInputGlobalContext::readingToken.store(token, std::memory_order_release);
        }
    }

    inputReadyFlag->store(true, std::memory_order_release);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    std::array<uint8_t, 256> polledState{};
    uint32_t armedCaptureSerial = 0;

    while (g_SharedState.engineRunning.load(std::memory_order_acquire)) {
        if (gameInput != nullptr) {
            IGameInputReading* kbReading = nullptr;
            if (SUCCEEDED(gameInput->GetCurrentReading(GameInputKindKeyboard, nullptr, &kbReading)) && kbReading != nullptr) {
                mask::input::ProcessGameInputReading(kbReading);
                kbReading->Release();
            }
            IGameInputReading* msReading = nullptr;
            if (SUCCEEDED(gameInput->GetCurrentReading(GameInputKindMouse, nullptr, &msReading)) && msReading != nullptr) {
                mask::input::ProcessGameInputReading(msReading);
                msReading->Release();
            }
        }

        unsigned int aux = 0;
        const uint64_t nowTsc = __rdtscp(&aux);
        const uint8_t bindTarget = g_SharedState.captureBindTarget.load(std::memory_order_relaxed);

        if (bindTarget != 0U) {
            const uint32_t serial = g_SharedState.captureSerial.load(std::memory_order_acquire);
            if (serial != armedCaptureSerial) {
                armedCaptureSerial = serial;
                for (uint16_t vk = 1; vk < 255; ++vk) {
                    polledState[vk] = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0 ? 1U : 0U;
                }
            } else {
                for (uint16_t vk = 1; vk < 255; ++vk) {
                    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                        mask::input::InputDispatcher::IsWheelVirtualKey(vk)) {
                        continue;
                    }
                    if (g_SharedState.syntheticHeldState[vk].load(std::memory_order_relaxed) != 0U) {
                        polledState[vk] = 1U;
                        continue;
                    }
                    const uint8_t cur = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0 ? 1U : 0U;
                    if (cur != polledState[vk]) {
                        polledState[vk] = cur;
                        if (cur != 0U) {
                            const bool isMouse = mask::input::InputDispatcher::IsMouseVirtualKey(vk);
                            mask::input::UpdatePhysicalAndEnqueueTransition(&g_SharedState, vk, true, nowTsc, isMouse);
                            break;
                        }
                    }
                }
            }
        } else {
            for (unsigned word = 0; word < 4; ++word) {
                uint64_t bits = g_SharedState.watchedVkBits[word].load(std::memory_order_relaxed);
                while (bits != 0ULL) {
                    unsigned long index = 0;
                    _BitScanForward64(&index, bits);
                    bits &= (bits - 1ULL);
                    const uint8_t bvk = static_cast<uint8_t>(word * 64U + index);
                    if (g_SharedState.syntheticHeldState[bvk].load(std::memory_order_relaxed) != 0U) {
                        continue;
                    }
                    const bool down = (GetAsyncKeyState(static_cast<int>(bvk)) & 0x8000) != 0;
                    const uint8_t cur = down ? 1U : 0U;
                    const uint8_t prev = g_SharedState.physicalKeyState[bvk].load(std::memory_order_relaxed);
                    if (cur != prev) {
                        const bool isMouse = mask::input::InputDispatcher::IsMouseVirtualKey(bvk);
                        mask::input::UpdatePhysicalAndEnqueueTransition(&g_SharedState, bvk, down, nowTsc, isMouse);
                        // Also notify complex sequences in case hardware device stream didn't deliver it
                        if (g_SeqEnginePtr != nullptr) {
                            g_SeqEnginePtr->NotifyPhysicalKeyTransition(bvk, down);
                        }
                    }
                    polledState[bvk] = cur;
                }
            }
        }

        const bool armed = bindTarget != 0U ||
                           (g_SharedState.anyMacroEnabled.load(std::memory_order_relaxed) &&
                            !g_SharedState.automationPaused.load(std::memory_order_relaxed));
        const uint32_t armedUs = std::clamp(g_SharedState.armedPollIntervalUs.load(std::memory_order_relaxed), 500U, 10'000U);
        const uint32_t sleepUs = armed ? armedUs : 4000U;
        if (timer != nullptr) {
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -static_cast<LONGLONG>(sleepUs) * 10LL;
            SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, std::max<DWORD>(2, sleepUs / 500U));
        } else {
            Sleep(std::max<DWORD>(1, sleepUs / 1000U));
        }
    }

    if (timer != nullptr) CloseHandle(timer);

    if (gameInput != nullptr) {
        const GameInputCallbackToken tok = mask::input::GameInputGlobalContext::readingToken.exchange(0, std::memory_order_acq_rel);
        if (tok != 0) gameInput->UnregisterCallback(tok, 50'000ULL);
        mask::input::GameInputGlobalContext::gameInputInstance.store(nullptr, std::memory_order_release);
        gameInput->Release();
    }
}

// =====================================================================================
//  Window Chrome, Frame State & Timers
// =====================================================================================

void ApplyWin11Chrome(HWND hwnd) noexcept {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const COLORREF caption = RGB(0x03, 0x07, 0x12);
    const COLORREF border  = RGB(0x15, 0x36, 0x5C);
    const COLORREF text    = RGB(0xE2, 0xF1, 0xFF);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

[[nodiscard]] uint32_t SelectedSlotPhysHeldMask() noexcept {
    if (g_Ui.showSettings || !g_Ui.settings.showLiveKeyIndicators || !g_Ui.HasSelection()) {
        return 0U;
    }
    auto isHeld = [](uint16_t vk) noexcept -> uint32_t {
        if (vk == 0U || vk == mask::core::kVkWheelUp || vk == mask::core::kVkWheelDown) return 0U;
        return g_SharedState.physicalKeyState[vk & 0xFFU].load(std::memory_order_relaxed) != 0U ? 1U : 0U;
    };
    if (g_Ui.selectedKind == ItemKind::Macro) {
        const auto& slot = g_Ui.Selected();
        uint32_t mask = isHeld(slot.triggerVk) | (isHeld(slot.outputVk) << 1);
        const int count = std::clamp<int>(slot.stepCount, 1, kStepCount);
        for (int i = 0; i < count; ++i) {
            mask |= (isHeld(slot.steps[i].outputVk) << (2 + i));
        }
        return mask;
    }
    if (g_Ui.selectedKind == ItemKind::Sequence) {
        const auto& seq = g_Ui.SelectedSeq();
        uint32_t mask = isHeld(seq.triggerVk);
        if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
            mask |= (isHeld(seq.steps[g_Ui.selectedSeqStep].vk) << 1);
        }
        return mask;
    }
    if (g_Ui.selectedKind == ItemKind::Trigger) {
        const auto& t = g_Ui.SelectedTrigger();
        return isHeld(t.armVk) | (isHeld(t.actionVk) << 1);
    }
    if (g_Ui.selectedKind == ItemKind::Remap) {
        const auto& r = g_Ui.SelectedRemap();
        return isHeld(r.sourceVk) | (isHeld(r.targetVk) << 1);
    }
    return 0U;
}

[[nodiscard]] FrameState BuildFrameState(HWND hwnd) noexcept {
    const mask::core::TelemetrySnapshot snap = g_SharedState.ReadTelemetry();
    FrameState frame{};
    frame.firingMask       = snap.activeSlotBitmask | snap.toggledSlotBitmask;
    frame.activeSeqMask    = g_SeqEnginePtr != nullptr ? g_SeqEnginePtr->GetActiveSequenceBitmask() : 0U;
    if (g_SeqEnginePtr != nullptr && frame.activeSeqMask != 0U) {
        for (size_t i = 0; i < mask::core::kMaxComplexSequences; ++i) {
            frame.activeSeqStep[i] = g_SeqEnginePtr->GetCurrentStepIndex(i);
        }
    }
    frame.activeRemapLayer = g_SharedState.activeRemapLayer.load(std::memory_order_relaxed);
    frame.remapUnlocked    = g_SharedState.remapUnlocked.load(std::memory_order_relaxed);
    frame.paused           = g_SharedState.automationPaused.load(std::memory_order_acquire);
    frame.windowFocused    = g_WindowFocused && (hwnd == nullptr || (!IsIconic(hwnd) && !g_HiddenToTray));
    frame.bindTarget       = g_SharedState.captureBindTarget.load(std::memory_order_acquire);
    frame.safeStopReason   = g_SharedState.safeStopReason.load(std::memory_order_acquire);
    frame.snap             = snap;
    if (!g_Ui.showSettings && g_Ui.HasSelection() && g_Ui.selectedKind == ItemKind::Trigger) {
        if (g_Ui.triggerColorPickActive) {
            frame.triggerPreview = g_Ui.eyedropperPreview;
        } else if (g_PixelEnginePtr != nullptr) {
            frame.triggerPreview = g_PixelEnginePtr->GetPreviewSnapshot();
        }
    }
    return frame;
}

void SaveCurrentWindowBounds(HWND hwnd) noexcept {
    if (!g_Ui.settings.rememberWindowBounds || IsIconic(hwnd) || !IsWindowVisible(hwnd)) return;
    RECT rc{};
    if (GetWindowRect(hwnd, &rc)) {
        const UINT dpi = GetDpiForWindow(hwnd);
        const float scale = static_cast<float>(dpi > 0U ? dpi : 96U) / 96.0f;
        g_Ui.settings.windowX = rc.left;
        g_Ui.settings.windowY = rc.top;
        g_Ui.settings.windowW = static_cast<int32_t>(std::lround(static_cast<float>(rc.right - rc.left) / scale));
        g_Ui.settings.windowH = static_cast<int32_t>(std::lround(static_cast<float>(rc.bottom - rc.top) / scale));
    }
}

void OnAnimTimer(HWND hwnd) noexcept {
    const bool windowActive = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray;
    const bool stillRunning = g_Animator.Tick(mask::ui::UiAnimator::NowSeconds(), g_WindowFocused, windowActive);
    g_Ui.listScroll    = g_Animator.listScrollSmooth.value;
    g_Ui.seqStepScroll = g_Animator.seqScrollSmooth.value;

    int finishedDelId = -1;
    if (g_Animator.ConsumeFinishedDelete(finishedDelId)) {
        FinalizeItemDelete(hwnd, finishedDelId);
    }
    if (windowActive) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    const bool allowAnim = windowActive && (g_WindowFocused || !g_Ui.settings.lowResourceWhenUnfocused);
    if (!allowAnim || (!stillRunning && !g_Animator.IsAnimating(g_WindowFocused, windowActive))) {
        KillTimer(hwnd, kAnimTimerId);
        g_AnimTimerActive = false;
    }
}

void OnUiTimer(HWND hwnd) noexcept {
    if (!g_SharedState.engineRunning.load(std::memory_order_acquire)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    bool repaint = false;

    if (g_Ui.pendingSyncMask != 0ULL) {
        g_Ui.FlushPendingSync();
    }

    const ULONGLONG autosaveDelay = std::clamp<ULONGLONG>(g_Ui.settings.autosaveDelayMs, 200ULL, 30'000ULL);
    if (g_Ui.saveState == SaveState::Pending && now - g_Ui.lastChangeTick >= autosaveDelay &&
        g_Ui.editField == Field::None && !g_Ui.draggingDuty && g_Ui.draggingTriggerSlider == 0) {
        if (!g_Ui.SaveToDisk()) {
            ShowToast(hwnd, L"Couldn't save your profile. Check that Mask's folder isn't read-only.");
        }
        repaint = true;
    }

    const bool visible = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray;
    const bool lowPowerUnfocused = !g_WindowFocused && g_Ui.settings.lowResourceWhenUnfocused;

    if (g_Ui.toast[0] != L'\0' && now - g_Ui.toastTick > kToastLifetimeMs &&
        g_SharedState.captureBindTarget.load(std::memory_order_relaxed) == 0U) {
        const bool canAnimateToast = visible && (g_WindowFocused || !g_Ui.settings.lowResourceWhenUnfocused);
        if (!canAnimateToast) {
            g_Animator.SetToastVisible(false, true);
            g_Ui.ClearToast();
        } else {
            g_Animator.SetToastVisible(false);
            EnsureAnimTimer(hwnd);
            if (g_Animator.toastSlide.value <= 0.02f) {
                g_Ui.ClearToast();
            }
        }
        if (visible) {
            repaint = true;
        }
    }

    // Live UI updates only when visible and not throttled in unfocused low-resource mode
    if (visible) {
        const uint16_t seqMask = g_SeqEnginePtr != nullptr ? g_SeqEnginePtr->GetActiveSequenceBitmask() : 0U;
        const uint8_t layer    = g_SharedState.activeRemapLayer.load(std::memory_order_relaxed);
        const bool unlocked    = g_SharedState.remapUnlocked.load(std::memory_order_relaxed);
        if (seqMask != g_LastActiveSeqMask || layer != g_LastActiveRemapLayer || unlocked != g_LastRemapUnlocked ||
            (!lowPowerUnfocused && seqMask != 0U)) {
            if (seqMask != g_LastActiveSeqMask) {
                g_LastFiringSeqMask = seqMask;
                const bool snapPulse = lowPowerUnfocused;
                for (int i = 0; i < kComplexSeqCount; ++i) {
                    g_Animator.SetFiring(AnimSlotFor(ItemKind::Sequence, i), (seqMask & (1U << i)) != 0U, snapPulse);
                }
                if (!snapPulse) EnsureAnimTimer(hwnd);
            }
            g_LastActiveSeqMask = seqMask;
            g_LastActiveRemapLayer = layer;
            g_LastRemapUnlocked = unlocked;
            repaint = true;
        }
        if (!lowPowerUnfocused) {
            if (g_Ui.showSettings && g_Ui.settingsTab == 4) {
                const uint64_t seq = g_SharedState.telemetrySequence.load(std::memory_order_acquire);
                if ((seq & 1ULL) == 0ULL && seq != g_LastUiTelemetrySeq) {
                    g_LastUiTelemetrySeq = seq;
                    repaint = true;
                }
            } else {
                const uint32_t heldMask = SelectedSlotPhysHeldMask();
                if (heldMask != g_LastUiPhysHeldMask) {
                    g_LastUiPhysHeldMask = heldMask;
                    repaint = true;
                }
            }
        }
    }

    // Virtual device health check & auto-reconnect
    if (g_Ui.outputState == OutputState::Ready &&
        g_SharedState.outputFailureStreak.load(std::memory_order_relaxed) >= kOutputFailureLimit) {
        g_Ui.outputState = OutputState::NotResponding;
        g_AutoReconnectAttempts = 0;
        g_NextAutoReconnectTick = now + 2000;
        ShowToast(hwnd, L"Virtual devices stopped responding. Reconnecting\u2026");
        repaint = true;
    }
    if (g_Ui.outputState == OutputState::NotResponding && g_AutoReconnectAttempts < 3 &&
        now >= g_NextAutoReconnectTick && !g_OutputWorkerBusy.load(std::memory_order_acquire)) {
        ++g_AutoReconnectAttempts;
        g_NextAutoReconnectTick = now + 10'000ULL * static_cast<ULONGLONG>(g_AutoReconnectAttempts);
        StartOutputWorker();
        repaint = true;
    }

    if (repaint && visible) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void PasteClipboardIntoEditBuffer(size_t maxLen) noexcept {
    if (!OpenClipboard(nullptr)) return;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData != nullptr) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(hData));
        if (text != nullptr) {
            size_t len = std::wcslen(g_Ui.editBuffer);
            for (size_t i = 0; text[i] != L'\0' && len < maxLen; ++i) {
                const wchar_t c = text[i];
                if (c >= 0x20 && c != 0x7F) {
                    g_Ui.editBuffer[len++] = c;
                }
            }
            g_Ui.editBuffer[len] = L'\0';
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
}

// =====================================================================================
//  Main Window Procedure
// =====================================================================================

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    switch (msg) {
        case WM_CREATE:
            g_Renderer.Initialize(hwnd);
            RefreshAppIcons(hwnd);
            ApplyWin11Chrome(hwnd);
            WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
            AddTrayIcon(hwnd);
            g_Animator.SetQuality(g_Ui.settings.animationQuality);
            SyncAnimatorToggles(true);
            g_Animator.SetSettingsOpen(g_Ui.showSettings, true);
            g_Animator.TriggerAppOpen();
            UpdateUiTimerCadence(hwnd);
            EnsureAnimTimer(hwnd);
            return 0;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                g_WindowFocused = false;
                g_Animator.SetCursorPosition(g_Animator.cursorX.value, g_Animator.cursorY.value, false, true);
                g_Animator.OnPointerUp();
                g_Animator.appOpen.Snap(g_Animator.appOpen.target);
                g_Animator.viewTransition.Snap(g_Animator.viewTransition.target);
                g_Animator.SnapAll();
                int finishedDelId = -1;
                if (g_Animator.ConsumeFinishedDelete(finishedDelId)) {
                    FinalizeItemDelete(hwnd, finishedDelId);
                }
                if (g_Ui.settings.minimizeToTray) {
                    HideToTray(hwnd);
                } else {
                    g_Renderer.TrimGpu();
                    UpdateUiTimerCadence(hwnd);
                    StartTrimTimerIfEnabled(hwnd);
                }
            } else {
                g_Renderer.Resize(LOWORD(lParam), HIWORD(lParam));
                CancelTrimTimer(hwnd);
                g_SettingsTabSnapped = false;
                g_SidebarIndSnapped = false;
                UpdateUiTimerCadence(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_DPICHANGED: {
            g_Renderer.SetDpi(static_cast<float>(HIWORD(wParam)));
            RefreshAppIcons(hwnd);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested != nullptr) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            g_SettingsTabSnapped = false;
            g_SidebarIndSnapped = false;
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
            if (!IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray) {
                g_LastUiPhysHeldMask = SelectedSlotPhysHeldMask();
                g_LastUiTelemetrySeq = g_SharedState.telemetrySequence.load(std::memory_order_acquire);
                g_Renderer.Render(g_Ui, BuildFrameState(hwnd), g_Animator);

                g_Animator.ClampScrollTargets(g_Renderer.ListMaxScroll(), g_Renderer.SeqStepMaxScroll());
                g_Ui.listScroll    = g_Animator.listScrollSmooth.value;
                g_Ui.seqStepScroll = g_Animator.seqScrollSmooth.value;

                if (g_Ui.showSettings) {
                    g_SidebarIndSnapped = false;
                    float tx = 0.0f, tw = 80.0f;
                    if (g_Renderer.GetSettingsTabRect(g_Ui.settingsTab, tx, tw)) {
                        const bool snapTab = !g_SettingsTabSnapped || !g_WindowFocused;
                        g_Animator.SetSettingsTab(tx, tw, snapTab);
                        g_SettingsTabSnapped = true;
                    }
                } else {
                    g_SettingsTabSnapped = false;
                    float indY = 0.0f, indH = 0.0f;
                    if (g_Renderer.GetSelectedSidebarRowRect(indY, indH)) {
                        const bool snapInd = !g_SidebarIndSnapped || !g_WindowFocused;
                        g_Animator.SetSidebarIndicator(indY, indH, snapInd);
                        g_SidebarIndSnapped = true;
                    } else {
                        g_Animator.SetSidebarIndicator(0.0f, 0.0f, true);
                        g_SidebarIndSnapped = false;
                    }
                }
                EnsureAnimTimer(hwnd);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                if (g_Ui.seqColorPickActive || g_Ui.triggerColorPickActive) {
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                    return TRUE;
                }
                const int h = g_Ui.hoveredAction;
                if (g_Ui.draggingDuty || h == act::DutySlider ||
                    g_Ui.draggingTriggerSlider != 0 || h == act::TrigSliderTol) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                } else if (act::InRange(h, act::FieldFocus, 64)) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                } else if (h == act::None || h == act::ModalSurface || h == act::PopoverBackdrop) {
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
            const float scale = g_Renderer.Scale();
            const float dipX = static_cast<float>(xPx) / scale;
            const float dipY = static_cast<float>(yPx) / scale;

            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            if (g_WindowFocused) {
                g_Animator.SetCursorPosition(dipX, dipY, true);
            }

            if (g_Ui.triggerColorPickActive) {
                POINT pt{};
                if (GetCursorPos(&pt)) {
                    SampleTriggerEyedropperLoupe(pt.x, pt.y);
                }
                EnsureAnimTimer(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_Ui.draggingDuty) {
                ApplyDutyFromPointer(xPx);
                EnsureAnimTimer(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_Ui.draggingTriggerSlider != 0) {
                ApplyTriggerSliderFromPointer(xPx);
                EnsureAnimTimer(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int hit = g_Renderer.HitTest(dipX, dipY);
            if (hit != g_Ui.hoveredAction) {
                g_Ui.hoveredAction = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            EnsureAnimTimer(hwnd);
            return 0;
        }

        case WM_MOUSELEAVE:
            g_Animator.OnPointerUp();
            g_Animator.SetCursorPosition(g_Animator.cursorX.target, g_Animator.cursorY.target, false, !g_WindowFocused);
            EnsureAnimTimer(hwnd);
            if (g_Ui.hoveredAction != act::None && !g_Ui.draggingDuty && g_Ui.draggingTriggerSlider == 0) {
                g_Ui.hoveredAction = act::None;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            if (g_Ui.triggerColorPickActive) {
                FinishTriggerEyedropperPick(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Quick global eyedropper for Complex Sequence WaitForColor step
            if (g_Ui.seqColorPickActive) {
                g_Ui.seqColorPickActive = false;
                ReleaseCapture();
                POINT pt{};
                GetCursorPos(&pt);
                uint8_t r = 0, g = 0, b = 0;
                if (mask::triggers::PixelTriggerEngine::SamplePixelAt(pt.x, pt.y, r, g, b) &&
                    g_Ui.selectedKind == ItemKind::Sequence) {
                    auto& seq = g_Ui.SelectedSeq();
                    if (g_Ui.selectedSeqStep >= 0 && g_Ui.selectedSeqStep < static_cast<int>(seq.stepCount)) {
                        auto& st = seq.steps[g_Ui.selectedSeqStep];
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
            const float scale = g_Renderer.Scale();
            const float dipX = static_cast<float>(xPx) / scale;
            const float dipY = static_cast<float>(yPx) / scale;

            g_Animator.TriggerRipple(dipX, dipY, mask::ui::GetAccentPalette(g_Ui.settings.accentPreset).signalHi);
            const int hit = g_Renderer.HitTest(dipX, dipY);
            if (hit != act::None && hit != act::ModalSurface && hit != act::PopoverBackdrop) {
                g_Animator.OnPointerDown(hit);
            }
            EnsureAnimTimer(hwnd);

            const uint8_t bindTarget = g_SharedState.captureBindTarget.load(std::memory_order_acquire);
            if (bindTarget != 0U && hit == BindActionFor(bindTarget)) {
                AssignCapturedKey(hwnd, VK_LBUTTON);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit == act::DutySlider && g_Ui.HasSelection() && g_Ui.selectedKind == ItemKind::Macro) {
                CommitEdit();
                if (bindTarget != 0U) CancelBinding();
                g_Ui.draggingDuty = true;
                SetCapture(hwnd);
                ApplyDutyFromPointer(xPx);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit == act::TrigSliderTol &&
                g_Ui.HasSelection() && g_Ui.selectedKind == ItemKind::Trigger) {
                CommitEdit();
                if (bindTarget != 0U) CancelBinding();
                g_Ui.draggingTriggerSlider = hit;
                SetCapture(hwnd);
                ApplyTriggerSliderFromPointer(xPx);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit != act::None) {
                HandleUiAction(hwnd, hit);
                if (hit == act::ModalSurface) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else {
                if (g_Ui.editField != Field::None) CommitEdit();
                if (bindTarget != 0U) {
                    CancelBinding();
                    ShowToast(hwnd, L"Cancelled. Nothing was changed.");
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            g_Animator.OnPointerUp();
            EnsureAnimTimer(hwnd);
            if (g_Ui.draggingDuty || g_Ui.draggingTriggerSlider != 0) {
                ReleaseCapture();
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (g_Ui.draggingDuty) {
                g_Ui.draggingDuty = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_Ui.draggingTriggerSlider != 0) {
                g_Ui.draggingTriggerSlider = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_Ui.seqColorPickActive) {
                g_Ui.seqColorPickActive = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_Ui.triggerColorPickActive) {
                g_Ui.triggerColorPickActive = false;
                UpdateTriggerPreviewState(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            const float scale = g_Renderer.Scale();
            const float dipX = static_cast<float>(pt.x) / scale;
            const float dipY = static_cast<float>(pt.y) / scale;
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int dir = (delta > 0) ? 1 : -1;
            const WORD keys = GET_KEYSTATE_WPARAM(wParam);
            const int hit = g_Renderer.HitTest(dipX, dipY);
            const bool onField = act::InRange(hit, act::FieldFocus, 64) && !IsTextField(static_cast<Field>(hit - act::FieldFocus));
            const bool snapScroll = !g_WindowFocused ||
                g_Ui.settings.animationQuality == static_cast<uint8_t>(mask::settings::AnimationQuality::Off);
            if (onField || (hit == act::DutySlider && g_Ui.selectedKind == ItemKind::Macro)) {
                const Field f = onField ? static_cast<Field>(hit - act::FieldFocus) : Field::Duty;
                if (g_Ui.editField == f) CancelEdit();
                StepField(f, dir, (keys & MK_SHIFT) != 0, (keys & MK_CONTROL) != 0);
            } else if (g_Ui.selectedKind == ItemKind::Trigger && hit == act::TrigSliderTol) {
                auto& t = g_Ui.SelectedTrigger();
                const int step = ((keys & MK_SHIFT) != 0 ? 10 : 1) * dir;
                t.tolerance = static_cast<uint8_t>(std::clamp(static_cast<int>(t.tolerance) + step, 0, 150));
                SyncSelectedTrigger();
            } else if (g_Renderer.PointInSeqList(dipX, dipY)) {
                const float pitch = UiRenderer::kSeqStepRowH + 4.0f;
                const float nextScroll = std::clamp(
                    g_Animator.seqScrollSmooth.target - static_cast<float>(delta) / 120.0f * pitch * 2.0f,
                    0.0f, g_Renderer.SeqStepMaxScroll());
                g_Animator.SetSeqScrollTarget(nextScroll, snapScroll);
                if (snapScroll) g_Ui.seqStepScroll = g_Animator.seqScrollSmooth.value;
                EnsureAnimTimer(hwnd);
            } else if (g_Renderer.PointInList(dipX, dipY)) {
                const float pitch = g_Renderer.CurrentListRowHeight() + UiRenderer::kListRowGap;
                const float nextScroll = std::clamp(
                    g_Animator.listScrollSmooth.target - static_cast<float>(delta) / 120.0f * pitch,
                    0.0f, g_Renderer.ListMaxScroll());
                g_Animator.SetListScrollTarget(nextScroll, snapScroll);
                if (snapScroll) g_Ui.listScroll = g_Animator.listScrollSmooth.value;
                EnsureAnimTimer(hwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                if (g_Ui.triggerColorPickActive) {
                    CancelTriggerEyedropper(hwnd);
                    ShowToast(hwnd, L"Color pick cancelled.");
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (g_Ui.seqColorPickActive) {
                    g_Ui.seqColorPickActive = false;
                    ReleaseCapture();
                    ShowToast(hwnd, L"Color pick cancelled.");
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (g_Ui.showStepKindMenu) {
                    g_Ui.showStepKindMenu = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (g_Ui.showNewPicker) {
                    g_Ui.showNewPicker = false;
                    g_Animator.SetNewPickerOpen(false);
                    EnsureAnimTimer(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (g_Ui.showSettings && g_Ui.editField == Field::None &&
                    g_SharedState.captureBindTarget.load(std::memory_order_relaxed) == 0U) {
                    g_Ui.showSettings = false;
                    g_SettingsHwTabOpen.store(false, std::memory_order_release);
                    g_Animator.SetSettingsOpen(false);
                    g_Animator.SetSettingsTab(0.0f, 0.0f, true);
                    g_SettingsTabSnapped = false;
                    g_SidebarIndSnapped = false;
                    g_Animator.TriggerViewTransition();
                    EnsureAnimTimer(hwnd);
                    UpdateTriggerPreviewState(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            // Ctrl+V paste into inline text fields (e.g. LaunchProgram / RunCommand)
            if (g_Ui.editField != Field::None && IsTextField(g_Ui.editField) &&
                wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                const size_t maxLen = (g_Ui.editField == Field::SeqStepText) ? 127U : 23U;
                PasteClipboardIntoEditBuffer(maxLen);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const bool busy = g_Ui.editField != Field::None ||
                              g_SharedState.captureBindTarget.load(std::memory_order_relaxed) != 0U ||
                              GetTickCount64() < g_SuppressKeyNavUntil ||
                              g_Ui.showNewPicker ||
                              g_Ui.showStepKindMenu ||
                              g_Ui.triggerColorPickActive;
            if (!busy && !g_Ui.showSettings && g_Ui.HasSelection()) {
                if (wParam == VK_UP)   { SelectNeighbour(hwnd, -1); InvalidateRect(hwnd, nullptr, FALSE); }
                if (wParam == VK_DOWN) { SelectNeighbour(hwnd, +1); InvalidateRect(hwnd, nullptr, FALSE); }
                if (wParam == VK_F2) {
                    if (g_Ui.selectedKind == ItemKind::Macro)         BeginEdit(Field::Name);
                    else if (g_Ui.selectedKind == ItemKind::Sequence) BeginEdit(Field::SeqName);
                    else if (g_Ui.selectedKind == ItemKind::Trigger)  BeginEdit(Field::TriggerName);
                    else if (g_Ui.selectedKind == ItemKind::Remap)    BeginEdit(Field::RemapName);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_CHAR: {
            if (g_Ui.editField == Field::None) return 0;
            const wchar_t ch = static_cast<wchar_t>(wParam);
            const size_t len = std::wcslen(g_Ui.editBuffer);
            const Field f = g_Ui.editField;
            const bool isText = IsTextField(f);
            size_t maxLen = 10;
            if (f == Field::Name) maxLen = std::size(g_Ui.Selected().name) - 1;
            else if (f == Field::SeqName || f == Field::RemapName || f == Field::TriggerName) maxLen = 31;
            else if (f == Field::SeqStepText) maxLen = 127;

            if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                CommitEdit();
            } else if (ch == 27) {
                CancelEdit();
            } else if (ch == L'\b') {
                if (len > 0) g_Ui.editBuffer[len - 1] = L'\0';
            } else if (ch < 0x20 || ch == 0x7F) {
                // ignore control chars
            } else if (len < maxLen) {
                bool accept = isText;
                if (!isText) {
                    accept = (ch >= L'0' && ch <= L'9') ||
                             (AllowsDecimal(f) && (ch == L'.' || ch == L',') && std::wcschr(g_Ui.editBuffer, L'.') == nullptr) ||
                             (AllowsNegative(f) && ch == L'-' && len == 0);
                }
                if (accept) {
                    g_Ui.editBuffer[len] = (ch == L',') ? L'.' : ch;
                    g_Ui.editBuffer[len + 1] = L'\0';
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                g_WindowFocused = false;
                g_Ui.showStepKindMenu = false;
                if (g_Ui.editField != Field::None) CommitEdit();
                if (g_Ui.triggerColorPickActive) CancelTriggerEyedropper(hwnd);
                if (g_Ui.draggingDuty || g_Ui.draggingTriggerSlider != 0) ReleaseCapture();
                g_Animator.OnPointerUp();
                g_Animator.SetCursorPosition(g_Animator.cursorX.value, g_Animator.cursorY.value, false, true);
                if (g_Ui.settings.lowResourceWhenUnfocused) {
                    g_Animator.appOpen.Snap(g_Animator.appOpen.target);
                    g_Animator.viewTransition.Snap(g_Animator.viewTransition.target);
                    g_Animator.SnapAll();
                    int finishedDelId = -1;
                    if (g_Animator.ConsumeFinishedDelete(finishedDelId)) {
                        FinalizeItemDelete(hwnd, finishedDelId);
                    }
                }
                UpdateUiTimerCadence(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                g_WindowFocused = true;
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
                case kTrayCmdPause:   HandleUiAction(hwnd, act::TogglePause); break;
                case kTrayCmdStopAll: HandleUiAction(hwnd, act::StopAll); break;
                case kTrayCmdExit:    PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
                default: break;
            }
            return 0;

        case WM_APP_RESTORE_WINDOW:
            RestoreFromTray(hwnd);
            return 0;

        case WM_APP_TOGGLE_MACRO_SLOT: {
            const int idx = static_cast<int>(wParam);
            if (idx >= 0 && idx < kSlotCount && g_Ui.IsMacroInUse(idx) && g_Ui.slots[idx].triggerVk != 0) {
                g_Ui.slots[idx].enabled = !g_Ui.slots[idx].enabled;
                g_Animator.SetToggle(AnimSlotFor(ItemKind::Macro, idx), g_Ui.slots[idx].enabled);
                g_Ui.SyncSlotToEngine(idx, false);
                EnsureAnimTimer(hwnd);
                if (!IsIconic(hwnd) && IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case mask::input::WM_APP_KEY_CAPTURED:
            OnKeyCaptured(hwnd, static_cast<uint16_t>(wParam & 0xFFU), lParam != 0);
            return 0;

        case WM_APP_ENGINE_STATE: {
            g_UiNotifyPending.store(false, std::memory_order_release);
            const bool nowPaused   = g_SharedState.automationPaused.load(std::memory_order_acquire);
            const uint8_t nowLayer = g_SharedState.activeRemapLayer.load(std::memory_order_acquire);
            const bool nowUnlocked = g_SharedState.remapUnlocked.load(std::memory_order_acquire);
            const bool visible = !IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray;
            const bool lowPowerUnfocused = !g_WindowFocused && g_Ui.settings.lowResourceWhenUnfocused;
            bool stateChanged = false;

            if (nowPaused != g_LastObservedPaused) {
                g_LastObservedPaused = nowPaused;
                stateChanged = true;
                if (nowPaused) {
                    OnWatchdogAuxCleanup();
                    PlayFeedbackTone(false);
                } else {
                    if (g_RemapEnginePtr != nullptr) g_RemapEnginePtr->RecomputeSuppressedMask();
                    if (g_PixelEnginePtr != nullptr) g_PixelEnginePtr->ResetArmedStates();
                    PlayFeedbackTone(true);
                }
            }
            if (nowLayer != g_LastObservedLayer || nowUnlocked != g_LastObservedUnlocked) {
                g_LastObservedLayer    = nowLayer;
                g_LastObservedUnlocked = nowUnlocked;
                stateChanged = true;
                if (g_RemapEnginePtr != nullptr) {
                    g_RemapEnginePtr->RecomputeSuppressedMask();
                }
            }

            const mask::core::TelemetrySnapshot tel = g_SharedState.ReadTelemetry();
            const uint64_t firingMask = tel.activeSlotBitmask | tel.toggledSlotBitmask;
            const uint16_t seqMask    = g_SeqEnginePtr != nullptr ? g_SeqEnginePtr->GetActiveSequenceBitmask() : 0U;
            if (firingMask != g_LastFiringMask || seqMask != g_LastFiringSeqMask) {
                g_LastFiringMask    = firingMask;
                g_LastFiringSeqMask = seqMask;
                stateChanged = true;
                const bool snapPulse = !visible || lowPowerUnfocused;
                for (int i = 0; i < kSlotCount; ++i) {
                    g_Animator.SetFiring(AnimSlotFor(ItemKind::Macro, i), (firingMask & (1ULL << i)) != 0ULL, snapPulse);
                }
                for (int i = 0; i < kComplexSeqCount; ++i) {
                    g_Animator.SetFiring(AnimSlotFor(ItemKind::Sequence, i), (seqMask & (1U << i)) != 0U, snapPulse);
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
            wcsncpy_s(g_Ui.outputMessage, host.GetLastStatusMessage(), _TRUNCATE);
            bool hmLive = false;
            uint8_t kbLeds = 0;
            host.QueryDriverTelemetry(
                hmLive,
                g_Ui.processElevated,
                g_Ui.driverInstalled,
                g_Ui.mouseTransportIdx,
                g_Ui.kbTransportIdx,
                g_Ui.deliversEveryReport,
                g_Ui.releasesOnExit,
                kbLeds);
            g_Ui.outputState = state;
            if (state == OutputState::Ready) {
                if (g_AutoReconnectAttempts > 0) {
                    ShowToast(hwnd, L"Virtual keyboard and mouse connected again.");
                }
                g_AutoReconnectAttempts = 0;
            } else if (state == OutputState::NeedsAdmin) {
                ShowToast(hwnd, L"Virtual devices need administrator rights to send input.");
            } else if (state == OutputState::Unavailable) {
                ShowToast(hwnd, g_Ui.outputMessage[0] != L'\0' ? g_Ui.outputMessage
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
                if (g_Ui.triggerColorPickActive) {
                    POINT pt{};
                    if (GetCursorPos(&pt)) SampleTriggerEyedropperLoupe(pt.x, pt.y);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (!IsIconic(hwnd) && IsWindowVisible(hwnd) && !g_HiddenToTray &&
                           (g_WindowFocused || !g_Ui.settings.lowResourceWhenUnfocused) &&
                           !g_Ui.showSettings && g_Ui.selectedKind == ItemKind::Trigger) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (wParam == kTrimMemTimerId) {
                KillTimer(hwnd, kTrimMemTimerId);
                g_TrimTimerActive = false;
                if (g_Ui.settings.trimMemoryWhenMinimized && (IsIconic(hwnd) || g_HiddenToTray)) {
                    EmptyWorkingSet(GetCurrentProcess());
                }
            }
            return 0;

        case WM_WTSSESSION_CHANGE:
            if (g_Ui.settings.stopOnSessionLock && (wParam == WTS_SESSION_LOCK || wParam == WTS_CONSOLE_DISCONNECT)) {
                StopEverythingRunning();
            }
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND) {
                StopEverythingRunning();
                if (g_Ui.saveState == SaveState::Pending) g_Ui.SaveToDisk();
            }
            return TRUE;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wParam != FALSE) {
                CommitEdit();
                SaveCurrentWindowBounds(hwnd);
                g_Ui.SaveToDisk();
                StopEverythingRunning();
            }
            return 0;

        case WM_CLOSE:
            CommitEdit();
            SaveCurrentWindowBounds(hwnd);
            g_Ui.SaveToDisk();
            CancelTriggerEyedropper(hwnd);
            if (g_PixelEnginePtr != nullptr) {
                g_PixelEnginePtr->SetPreviewActive(false, 0U);
                g_PreviewActiveSent = false;
            }
            g_SharedState.engineRunning.store(false, std::memory_order_release);
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
            g_TriggerPreviewTimerMs = 0U;
            g_AnimTimerActive = false;
            g_TrimTimerActive = false;
            RemoveTrayIcon(hwnd);
            WTSUnRegisterSessionNotification(hwnd);
            mask::input::GameInputGlobalContext::uiWindowHandle.store(nullptr, std::memory_order_release);
            g_Renderer.Shutdown();
            g_SharedState.engineRunning.store(false, std::memory_order_release);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void AttachParentConsole() noexcept {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(out);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    bool selfTestOnly   = false;
    bool relaunched     = false;
    bool startMinimized = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) selfTestOnly = true;
        if (std::strcmp(argv[i], "--relaunch") == 0)  relaunched = true;
        if (std::strcmp(argv[i], "--minimized") == 0) startMinimized = true;
    }

    if (selfTestOnly) {
        AttachParentConsole();
    }

    HANDLE instanceMutex = nullptr;
    if (!selfTestOnly) {
        instanceMutex = CreateMutexW(nullptr, FALSE, kMutexName);
        const DWORD wait = (instanceMutex != nullptr) ? WaitForSingleObject(instanceMutex, relaunched ? 10'000 : 0) : WAIT_FAILED;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            if (const HWND existing = FindWindowW(kWindowClass, nullptr)) {
                PostMessageW(existing, WM_APP_RESTORE_WINDOW, 0, 0);
            }
            if (instanceMutex != nullptr) CloseHandle(instanceMutex);
            return 0;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    mask::input::GameInputGlobalContext::sharedState       = &g_SharedState;
    mask::input::GameInputGlobalContext::engineWakeEvent   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    mask::input::GameInputGlobalContext::watchdogWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 1. Hardware TSC timing engine
    mask::timing::TimingEngine timingEngine{};

    // 2. Settings + active profile bank (macros, complex sequences, color triggers, remaps)
    g_Ui.ResolveProfilePaths();
    g_Ui.InitializeDefaults();
    if (!selfTestOnly) {
        (void)g_Ui.LoadSettingsFromDisk();
        if (g_Ui.settings.startMinimized) startMinimized = true;
        const int loaded = g_Ui.LoadFromDisk();
        if (loaded == 2) {
            g_Ui.SetToast(L"Your saved profile was damaged, so the previous backup was restored.");
            g_Animator.SetToastVisible(true, true);
        } else if (loaded == 0) {
            g_Ui.SetToast(L"Welcome to Mask. Click + New to create macros, sequences, triggers, or remaps.");
            g_Animator.SetToastVisible(true, true);
        }
    }

    // 3. Auxiliary engines (Pixel Triggers, 128-step Complex Sequences, Multi-Device Remap)
    mask::triggers::PixelTriggerEngine pixelEngine(g_SharedState);
    mask::workflows::ComplexSequenceEngine seqEngine(g_SharedState, &mask::triggers::PixelTriggerEngine::SamplePixelAt);
    mask::remap::RemapEngine remapEngine(g_SharedState);

    seqEngine.SetToggleMacroCallback(OnToggleMacroSlotFromSeq);
    g_PixelEnginePtr = &pixelEngine;
    g_SeqEnginePtr   = &seqEngine;
    g_RemapEnginePtr = &remapEngine;

    SyncAllEnginesFromModel();
    g_Ui.RefreshDevicesAndMonitors(&remapEngine);

    pixelEngine.Start();
    seqEngine.Start();
    remapEngine.Start();

    mask::input::GameInputGlobalContext::perDeviceCallback.store(
        OnPhysicalDeviceTransitionCallback, std::memory_order_release);

    // 4. Watchdog (safe stop + aux engine cleanup)
    mask::safety::Watchdog watchdog(g_SharedState, OnWatchdogSafeStop);
    watchdog.SetAuxCleanupCallback(OnWatchdogAuxCleanup);
    g_WatchdogPtr = &watchdog;
    watchdog.Start();

    // 5. Input thread (Microsoft GameInput)
    std::atomic<bool> inputReady{false};
    std::thread gameInputThread(GameInputThreadMain, &inputReady);
    while (!inputReady.load(std::memory_order_acquire)) {
        Sleep(0);
    }

    // 6. Real-time macro engine thread
    std::thread engineThread(EngineThreadMain, &timingEngine, selfTestOnly);

    bool selfTestOk = true;
    if (selfTestOnly) {
        auto& host = mask::bridge::HIDMaestroHost::Instance();
        host.LoadAndInitialize(L"logitech-g502-hero", L"keychron-q6");
        Sleep(180);
        const mask::core::TelemetrySnapshot snap = g_SharedState.ReadTelemetry();
        bool hmLive = false, hmElev = false, hmDrv = false, hmDeliv = false, hmRelExit = false;
        int32_t mIdx = -1, kIdx = -1;
        uint8_t kbLeds = 0;
        host.QueryDriverTelemetry(hmLive, hmElev, hmDrv, mIdx, kIdx, hmDeliv, hmRelExit, kbLeds);
        const bool seqLogicOk = mask::workflows::ComplexSequenceEngine::RunControlFlowSelfTest();
        double visionAvgUs = 0.0;
        const bool trigOk = mask::triggers::PixelTriggerEngine::RunTriggerSelfTest(visionAvgUs);
        const bool remapOk = mask::remap::RemapEngine::RunRemapSelfTest();
        selfTestOk = seqLogicOk && trigOk && remapOk;

        std::printf("\nMask self-test\n");
        std::printf("  Timer frequency     : %.4f GHz\n", static_cast<double>(snap.calibratedTscFreqHz) / 1e9);
        std::printf("  Fast path           : %s\n", snap.avx2Bmi2Active ? "AVX2 + BMI2" : "SSE2");
        std::printf("  Microsoft GameInput : %s (%u physical devices)\n",
                    g_SharedState.gameInputConnected.load(std::memory_order_acquire) ? "active" : "fallback polling",
                    static_cast<unsigned>(snap.gameInputDeviceCount));
        std::printf("  Virtual devices     : driver installed=%s | elevated=%s | live=%s\n",
                    hmDrv ? "yes" : "no", hmElev ? "yes" : "no", hmLive ? "yes" : "no");
        std::printf("  Bridge status       : %ls\n", host.GetLastStatusMessage());
        std::printf("  Timing error p50    : %llu ns\n", static_cast<unsigned long long>(snap.p50JitterNs));
        std::printf("  Timing error p99    : %llu ns\n", static_cast<unsigned long long>(snap.p99JitterNs));
        std::printf("  Timing error max    : %llu ns\n", static_cast<unsigned long long>(snap.maxJitterNs));
        std::printf("  Jitter histogram    : <100ns=%u | 100-500ns=%u | 500ns-2us=%u | >=2us=%u\n",
                    static_cast<unsigned>(snap.jitterHistogram[0]),
                    static_cast<unsigned>(snap.jitterHistogram[1]),
                    static_cast<unsigned>(snap.jitterHistogram[2]),
                    static_cast<unsigned>(snap.jitterHistogram[3]));
        std::printf("  Sequence logic: %s\n", seqLogicOk ? "PASS" : "FAIL");
        std::printf("  Trigger logic: %s (%.2f us/frame)\n", trigOk ? "PASS" : "FAIL", visionAvgUs);
        std::printf("  Remap logic: %s\n", remapOk ? "PASS" : "FAIL");
    } else {
        const HINSTANCE hInst = GetModuleHandleW(nullptr);
        RefreshAppIcons(nullptr);

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(WNDCLASSEXW);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = MainWindowProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = g_IconLarge != nullptr ? g_IconLarge : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm       = g_IconSmall;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);

        const UINT dpi = GetDpiForSystem();
        const float scale = static_cast<float>(dpi > 0U ? dpi : 96U) / 96.0f;
        const float wantW = g_Ui.settings.rememberWindowBounds
            ? std::max(kMinWidthDip, static_cast<float>(g_Ui.settings.windowW)) : kDefaultWidthDip;
        const float wantH = g_Ui.settings.rememberWindowBounds
            ? std::max(kMinHeightDip, static_cast<float>(g_Ui.settings.windowH)) : kDefaultHeightDip;

        const DWORD exStyle = g_Ui.settings.alwaysOnTop ? WS_EX_TOPMOST : 0U;
        RECT rc{ 0, 0, static_cast<LONG>(wantW * scale), static_cast<LONG>(wantH * scale) };
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, exStyle, dpi > 0U ? dpi : 96U);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int ww = std::min<int>(rc.right - rc.left, work.right - work.left);
        int wh = std::min<int>(rc.bottom - rc.top, work.bottom - work.top);
        int wx = work.left + std::max(0, static_cast<int>((work.right - work.left - ww) / 2));
        int wy = work.top + std::max(0, static_cast<int>((work.bottom - work.top - wh) / 2));

        if (g_Ui.settings.rememberWindowBounds && g_Ui.settings.windowX != -1 && g_Ui.settings.windowY != -1) {
            RECT saved{ g_Ui.settings.windowX, g_Ui.settings.windowY,
                        g_Ui.settings.windowX + ww, g_Ui.settings.windowY + wh };
            if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL) != nullptr) {
                wx = g_Ui.settings.windowX;
                wy = g_Ui.settings.windowY;
            }
        }

        HWND hwndMain = CreateWindowExW(exStyle, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                        wx, wy, ww, wh, nullptr, nullptr, hInst, nullptr);
        mask::input::GameInputGlobalContext::uiWindowHandle.store(hwndMain, std::memory_order_release);

        StartOutputWorker();

        if (startMinimized) {
            if (g_Ui.settings.minimizeToTray) {
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

        if (g_IconLarge != nullptr) { DestroyIcon(g_IconLarge); g_IconLarge = nullptr; }
        if (g_IconSmall != nullptr) { DestroyIcon(g_IconSmall); g_IconSmall = nullptr; }
    }

    // 7. Clean shutdown
    mask::input::GameInputGlobalContext::perDeviceCallback.store(nullptr, std::memory_order_release);
    g_SharedState.engineRunning.store(false, std::memory_order_release);
    mask::input::WakeEngine();
    mask::input::WakeWatchdog();

    pixelEngine.Stop();
    seqEngine.Stop();
    remapEngine.Stop();

    if (engineThread.joinable()) engineThread.join();
    if (gameInputThread.joinable()) gameInputThread.join();
    if (g_OutputWorker.joinable()) g_OutputWorker.join();

    g_PixelEnginePtr = nullptr;
    g_SeqEnginePtr   = nullptr;
    g_RemapEnginePtr = nullptr;

    watchdog.ExecuteEmergencyRecovery(false);
    watchdog.Stop();
    g_WatchdogPtr = nullptr;

    CloseHandle(mask::input::GameInputGlobalContext::engineWakeEvent);
    CloseHandle(mask::input::GameInputGlobalContext::watchdogWakeEvent);
    mask::input::GameInputGlobalContext::engineWakeEvent = nullptr;
    mask::input::GameInputGlobalContext::watchdogWakeEvent = nullptr;

    if (instanceMutex != nullptr) {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
    }
    return selfTestOnly ? (selfTestOk ? 0 : 1) : 0;
}
