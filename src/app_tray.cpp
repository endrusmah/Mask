#include "app.hpp"

#include <dwmapi.h>
#include <shellapi.h>

#include <cstdio>
#include <iterator>

namespace mask::app {

void App::ApplyWin11Chrome(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
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

void App::ApplyAlwaysOnTop(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    const HWND insertAfter = model_.settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::RefreshAppIcons(HWND hwnd) noexcept {
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
        if (hwnd != nullptr) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(newLarge));
        }
        if (iconLarge_ != nullptr) {
            DestroyIcon(iconLarge_);
        }
        iconLarge_ = newLarge;
    }
    if (newSmall != nullptr) {
        if (hwnd != nullptr) {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(newSmall));
        }
        if (iconSmall_ != nullptr) {
            DestroyIcon(iconSmall_);
        }
        iconSmall_ = newSmall;
    }
    if (hwnd != nullptr) {
        UpdateTrayTooltip(hwnd);
    }
}

void App::DestroyAppIcons() noexcept {
    if (iconLarge_ != nullptr) {
        DestroyIcon(iconLarge_);
        iconLarge_ = nullptr;
    }
    if (iconSmall_ != nullptr) {
        DestroyIcon(iconSmall_);
        iconSmall_ = nullptr;
    }
}

void App::UpdateTrayTooltip(HWND hwnd) noexcept {
    if (!trayAdded_ || hwnd == nullptr) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize        = sizeof(nid);
    nid.hWnd          = hwnd;
    nid.uID           = kTrayIconId;
    nid.uFlags        = NIF_ICON | NIF_TIP;
    nid.hIcon         = (iconSmall_ != nullptr) ? iconSmall_ : iconLarge_;
    const bool paused = sharedState_.automationPaused.load(std::memory_order_relaxed);
    std::swprintf(nid.szTip, std::size(nid.szTip), L"Mask \u2014 %ls (Bank %u)",
                  paused ? L"Paused" : L"Active",
                  static_cast<unsigned>(model_.settings.activeProfileBank + 1U));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::AddTrayIcon(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    if (trayAdded_) {
        UpdateTrayTooltip(hwnd);
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = kTrayIconId;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;
    nid.hIcon            = (iconSmall_ != nullptr) ? iconSmall_ : iconLarge_;
    wcscpy_s(nid.szTip, L"Mask \u2014 Active");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        trayAdded_ = true;
        UpdateTrayTooltip(hwnd);
    }
}

void App::RemoveTrayIcon(HWND hwnd) noexcept {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void App::StartTrimTimerIfEnabled(HWND hwnd) noexcept {
    if (hwnd != nullptr && model_.settings.trimMemoryWhenMinimized && !trimTimerActive_) {
        SetTimer(hwnd, kTrimMemTimerId, kTrimMemDelayMs, nullptr);
        trimTimerActive_ = true;
    }
}

void App::CancelTrimTimer(HWND hwnd) noexcept {
    if (hwnd != nullptr && trimTimerActive_) {
        KillTimer(hwnd, kTrimMemTimerId);
        trimTimerActive_ = false;
    }
}

void App::HideToTray(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    AddTrayIcon(hwnd);
    hiddenToTray_  = true;
    windowFocused_ = false;
    animator_.SetCursorPosition(animator_.cursorX.value, animator_.cursorY.value, false, true);
    animator_.OnPointerUp();
    animator_.appOpen.Snap(animator_.appOpen.target);
    animator_.viewTransition.Snap(animator_.viewTransition.target);
    animator_.SnapAll();
    ShowWindow(hwnd, SW_HIDE);
    renderer_.TrimGpu();
    UpdateUiTimerCadence(hwnd);
    StartTrimTimerIfEnabled(hwnd);
}

void App::RestoreFromTray(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    CancelTrimTimer(hwnd);
    hiddenToTray_       = false;
    windowFocused_      = true;
    settingsTabSnapped_ = false;
    sidebarIndSnapped_  = false;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateUiTimerCadence(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void App::ShowTrayContextMenu(HWND hwnd) noexcept {
    if (hwnd == nullptr) return;
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    const bool paused = sharedState_.automationPaused.load(std::memory_order_acquire);
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

} // namespace mask::app
