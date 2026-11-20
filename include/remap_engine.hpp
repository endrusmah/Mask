#pragma once

// Multi-Keyboard, Multi-Mouse, Multi-Monitor Remapping Engine + Layer / Unlock State + Opt-In SuppressionGate.
// Zero Win32 hooks are installed by default; SuppressionGate activates ONLY when at least one active rule
// requires suppressing the original physical input (suppressOriginal or BlockKey) and automation is not paused.

#include "core_contracts.hpp"
#include "input_pipeline.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <new>

namespace mask::remap {

inline constexpr uint32_t kRemapRulesMagic   = 0x4D524D50U; // 'MRMP'
inline constexpr uint32_t kRemapRulesVersion = 1U;
inline constexpr size_t   kMaxMonitors       = 8U;

struct MonitorRectInfo {
    int32_t  left{0};
    int32_t  top{0};
    int32_t  right{1920};
    int32_t  bottom{1080};
    uint32_t dpi{96};
    bool     isPrimary{true};
    wchar_t  name[32]{L"Monitor 1"};
};

namespace detail {

inline constexpr uint64_t kMouseButtonVkMaskWord0 =
    (1ULL << VK_LBUTTON) |
    (1ULL << VK_RBUTTON) |
    (1ULL << VK_MBUTTON) |
    (1ULL << VK_XBUTTON1) |
    (1ULL << VK_XBUTTON2);

[[nodiscard]] inline bool SanitizeBool(const bool& value) noexcept {
    uint8_t raw = 0;
    std::memcpy(&raw, &value, 1);
    return raw != 0U;
}

[[nodiscard]] inline bool IsProtectedSafetyVk(uint16_t vk, uint16_t pauseVk, uint16_t stopAllVk) noexcept {
    const uint16_t b = static_cast<uint16_t>(vk & 0xFFU);
    if (b == 0U || b == VK_ESCAPE || b == VK_LCONTROL || b == VK_RCONTROL || b == VK_CONTROL) {
        return true;
    }
    if (pauseVk != 0U && b == (pauseVk & 0xFFU)) {
        return true;
    }
    if (stopAllVk != 0U && b == (stopAllVk & 0xFFU)) {
        return true;
    }
    return false;
}

inline void SanitizeRemapRule(mask::core::RemapRuleConfig& r, size_t slotIdx) noexcept {
    r.slotIndex        = static_cast<uint8_t>(slotIdx);
    r.enabled          = SanitizeBool(r.enabled);
    r.inUse            = SanitizeBool(r.inUse);
    r.suppressOriginal = SanitizeBool(r.suppressOriginal);
    r.anyDevice        = SanitizeBool(r.anyDevice);
    if (r.deviceKind > 2U) {
        r.deviceKind = 0U;
    }
    if (r.layerIndex >= mask::core::kMaxRemapLayers) {
        r.layerIndex = 0U;
    }
    if (r.unlockPolicy > static_cast<uint8_t>(mask::core::RemapUnlockPolicy::WhenLocked)) {
        r.unlockPolicy = static_cast<uint8_t>(mask::core::RemapUnlockPolicy::Always);
    }
    if (r.actionKind > static_cast<uint8_t>(mask::core::RemapLayerAction::WarpToMonitor)) {
        r.actionKind = static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey);
    }
    if (r.targetLayer >= mask::core::kMaxRemapLayers) {
        r.targetLayer = 0U;
    }
    if (r.monitorIndex < -1 || r.monitorIndex >= static_cast<int8_t>(kMaxMonitors)) {
        r.monitorIndex = -1;
    }
    r.reserved = 0U;
    r.sourceVk = static_cast<uint16_t>(r.sourceVk & 0xFFU);
    r.targetVk = static_cast<uint16_t>(r.targetVk & 0xFFU);

    r.deviceLabel[std::size(r.deviceLabel) - 1] = L'\0';
    if (r.deviceLabel[0] == L'\0') {
        wcscpy_s(r.deviceLabel, L"Any device");
    }
    r.name[std::size(r.name) - 1] = L'\0';
    if (r.name[0] == L'\0') {
        std::swprintf(r.name, std::size(r.name), L"Remap %zu", slotIdx + 1U);
    }
}

struct RemapsFilePayload {
    uint32_t magic{kRemapRulesMagic};
    uint32_t version{kRemapRulesVersion};
    uint32_t count{static_cast<uint32_t>(mask::core::kMaxRemapRules)};
    uint32_t reserved{0};
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules{};
};

static_assert(std::is_trivially_copyable_v<RemapsFilePayload>,
              "RemapsFilePayload must be trivially copyable.");

[[nodiscard]] inline bool WriteFileAtomic(const wchar_t* path, const void* data, size_t size) noexcept {
    if (path == nullptr || path[0] == L'\0' || data == nullptr || size == 0U) {
        return false;
    }

    wchar_t tmp[1040]{};
    wchar_t bak[1040]{};
    if (std::swprintf(tmp, std::size(tmp), L"%ls.tmp", path) < 0 ||
        std::swprintf(bak, std::size(bak), L"%ls.bak", path) < 0) {
        return false;
    }

    wchar_t dir[1040]{};
    if (wcscpy_s(dir, path) == 0) {
        if (wchar_t* slash = std::wcsrchr(dir, L'\\')) {
            *slash = L'\0';
            if (dir[0] != L'\0') {
                CreateDirectoryW(dir, nullptr);
            }
        }
    }

    const HANDLE h = CreateFileW(
        tmp,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
                    written == static_cast<DWORD>(size) &&
                    FlushFileBuffers(h) != FALSE;
    CloseHandle(h);

    if (!ok) {
        DeleteFileW(tmp);
        return false;
    }

    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
        ReplaceFileW(path, tmp, bak, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE) {
        return true;
    }
    if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
        return true;
    }

    DeleteFileW(tmp);
    return false;
}

[[nodiscard]] inline bool ReadRemapsFileRaw(const wchar_t* path, RemapsFilePayload& out) noexcept {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const HANDLE h = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    bool ok = GetFileSizeEx(h, &fileSize) != FALSE &&
              fileSize.QuadPart == static_cast<LONGLONG>(sizeof(RemapsFilePayload));

    if (ok) {
        DWORD bytesRead = 0;
        ok = ReadFile(h, &out, static_cast<DWORD>(sizeof(RemapsFilePayload)), &bytesRead, nullptr) != FALSE &&
             bytesRead == static_cast<DWORD>(sizeof(RemapsFilePayload)) &&
             out.magic == kRemapRulesMagic &&
             out.version == kRemapRulesVersion &&
             out.count == static_cast<uint32_t>(mask::core::kMaxRemapRules);
    }

    CloseHandle(h);
    return ok;
}

[[nodiscard]] inline uint32_t QueryMonitorDpi(HMONITOR hMon) noexcept {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn fn = []() noexcept -> GetDpiForMonitorFn {
        HMODULE shcore = GetModuleHandleW(L"shcore.dll");
        if (shcore == nullptr) {
            shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (shcore == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
    }();

    if (fn != nullptr) {
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(fn(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) && dpiX >= 72U && dpiX <= 480U) {
            return static_cast<uint32_t>(dpiX);
        }
    }

    const HDC hdc = GetDC(nullptr);
    uint32_t fallbackDpi = 96U;
    if (hdc != nullptr) {
        const int px = GetDeviceCaps(hdc, LOGPIXELSX);
        if (px >= 72 && px <= 480) {
            fallbackDpi = static_cast<uint32_t>(px);
        }
        ReleaseDC(nullptr, hdc);
    }
    return fallbackDpi;
}

/**
 * @brief Opt-In Ring-3 SuppressionGate.
 *
 * Strictly swallow-only gate that spawns its lightweight Win32 hook message loop ONLY when
 * `EngineSharedState::anySuppressionActive` is true AND `!automationPaused`.
 * Never modifies input, never installs mouse hooks unless a mouse button is suppressed, and always
 * passes through injected events, HIDMaestro echo window events, global pause/stop hotkeys, and panic chord keys.
 */
class SuppressionGate {
public:
    explicit SuppressionGate(mask::core::EngineSharedState& sharedState) noexcept
        : sharedState_(sharedState) {
        InitializeSRWLock(&lock_);
    }

    ~SuppressionGate() noexcept {
        Shutdown();
    }

    SuppressionGate(const SuppressionGate&) = delete;
    SuppressionGate& operator=(const SuppressionGate&) = delete;

    void Sync(bool wantKeyboardHook, bool wantMouseHook) noexcept {
        const bool wantAny = wantKeyboardHook || wantMouseHook;

        AcquireSRWLockExclusive(&lock_);
        wantKeyboard_.store(wantKeyboardHook, std::memory_order_release);
        wantMouse_.store(wantMouseHook, std::memory_order_release);
        desiredActive_.store(wantAny, std::memory_order_release);

        if (wantAny) {
            activeSharedState_.store(&sharedState_, std::memory_order_release);

            // Reap finished handle if previous thread exited cleanly
            if (threadHandle_ != nullptr && WaitForSingleObject(threadHandle_, 0U) == WAIT_OBJECT_0) {
                CloseHandle(threadHandle_);
                threadHandle_ = nullptr;
                threadId_.store(0U, std::memory_order_release);
            }

            if (threadHandle_ == nullptr) {
                DWORD tid = 0;
                threadHandle_ = CreateThread(nullptr, 0, &SuppressionGate::HookThreadEntry, this, 0, &tid);
                if (threadHandle_ != nullptr) {
                    threadId_.store(tid, std::memory_order_release);
                }
            } else {
                const DWORD tid = threadId_.load(std::memory_order_acquire);
                if (tid != 0U) {
                    PostThreadMessageW(tid, kMsgSyncHooks, 0, 0);
                }
            }
        } else {
            const DWORD tid = threadId_.load(std::memory_order_acquire);
            if (tid != 0U) {
                PostThreadMessageW(tid, WM_QUIT, 0, 0);
            }
            if (threadHandle_ != nullptr) {
                // Re-post WM_QUIT briefly in case the thread had not yet created its message queue
                if (WaitForSingleObject(threadHandle_, 50U) == WAIT_TIMEOUT && tid != 0U) {
                    PostThreadMessageW(tid, WM_QUIT, 0, 0);
                    WaitForSingleObject(threadHandle_, 2000U);
                }
                CloseHandle(threadHandle_);
                threadHandle_ = nullptr;
            }
            threadId_.store(0U, std::memory_order_release);
        }
        ReleaseSRWLockExclusive(&lock_);
    }

    void Shutdown() noexcept {
        HANDLE handleToJoin = nullptr;
        DWORD tid = 0U;
        AcquireSRWLockExclusive(&lock_);
        wantKeyboard_.store(false, std::memory_order_release);
        wantMouse_.store(false, std::memory_order_release);
        desiredActive_.store(false, std::memory_order_release);

        tid = threadId_.load(std::memory_order_acquire);
        if (tid != 0U) {
            PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        handleToJoin = threadHandle_;
        threadHandle_ = nullptr;
        threadId_.store(0U, std::memory_order_release);
        ReleaseSRWLockExclusive(&lock_);

        if (handleToJoin != nullptr) {
            // Re-post WM_QUIT briefly in case the thread had not yet created its message queue
            if (WaitForSingleObject(handleToJoin, 50U) == WAIT_TIMEOUT && tid != 0U) {
                PostThreadMessageW(tid, WM_QUIT, 0, 0);
                WaitForSingleObject(handleToJoin, 2000U);
            }
            CloseHandle(handleToJoin);
        }

        mask::core::EngineSharedState* expected = &sharedState_;
        activeSharedState_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }

private:
    static constexpr UINT kMsgSyncHooks = WM_APP + 101U;

    inline static std::atomic<mask::core::EngineSharedState*> activeSharedState_{nullptr};

    [[nodiscard]] static bool ShouldPassThroughEcho(
        const mask::core::EngineSharedState* st,
        uint16_t vk,
        bool isDown
    ) noexcept {
        unsigned int aux = 0;
        const uint64_t nowTsc = __rdtscp(&aux);
        const uint64_t lastSynth = st->lastSyntheticHardwareWriteTsc.load(std::memory_order_acquire);
        const uint64_t echoWin = mask::input::GameInputGlobalContext::echoSuppressionWindowTsc.load(std::memory_order_relaxed);
        if (nowTsc >= lastSynth && (nowTsc - lastSynth) <= echoWin) {
            const uint8_t synthVal = st->syntheticHeldState[vk & 0xFFU].load(std::memory_order_acquire);
            if (synthVal == (isDown ? 1U : 0U)) {
                return true;
            }
        }
        return false;
    }

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
        if (nCode != HC_ACTION || lParam == 0) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const auto* st = activeSharedState_.load(std::memory_order_acquire);
        if (st == nullptr ||
            st->automationPaused.load(std::memory_order_relaxed) ||
            !st->anySuppressionActive.load(std::memory_order_relaxed) ||
            st->captureBindTarget.load(std::memory_order_relaxed) != 0U ||
            st->suppressTriggers.load(std::memory_order_relaxed)) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        // Always pass through injected events
        if ((kb->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0U) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const uint16_t vk = static_cast<uint16_t>(kb->vkCode & 0xFFU);
        const uint16_t pauseVk = st->globalPauseVk.load(std::memory_order_relaxed);
        const uint16_t stopAllVk = st->globalStopAllVk.load(std::memory_order_relaxed);
        if (IsProtectedSafetyVk(vk, pauseVk, stopAllVk)) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const uint64_t word = st->suppressedVkBits[vk >> 6].load(std::memory_order_acquire);
        if ((word & (1ULL << (vk & 63U))) == 0ULL) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        if (ShouldPassThroughEcho(st, vk, isDown)) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        return 1;
    }

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
        if (nCode != HC_ACTION || lParam == 0) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        uint16_t vk = 0U;
        bool isDown = false;
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);

        switch (wParam) {
        case WM_LBUTTONDOWN: vk = VK_LBUTTON; isDown = true;  break;
        case WM_LBUTTONUP:   vk = VK_LBUTTON; isDown = false; break;
        case WM_RBUTTONDOWN: vk = VK_RBUTTON; isDown = true;  break;
        case WM_RBUTTONUP:   vk = VK_RBUTTON; isDown = false; break;
        case WM_MBUTTONDOWN: vk = VK_MBUTTON; isDown = true;  break;
        case WM_MBUTTONUP:   vk = VK_MBUTTON; isDown = false; break;
        case WM_XBUTTONDOWN:
            vk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            isDown = true;
            break;
        case WM_XBUTTONUP:
            vk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            isDown = false;
            break;
        default:
            // Immediately pass through WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_MOUSEHWHEEL, etc.
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const auto* st = activeSharedState_.load(std::memory_order_acquire);
        if (st == nullptr ||
            st->automationPaused.load(std::memory_order_relaxed) ||
            !st->anySuppressionActive.load(std::memory_order_relaxed) ||
            st->captureBindTarget.load(std::memory_order_relaxed) != 0U ||
            st->suppressTriggers.load(std::memory_order_relaxed)) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if ((ms->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0U) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const uint16_t pauseVk = st->globalPauseVk.load(std::memory_order_relaxed);
        const uint16_t stopAllVk = st->globalStopAllVk.load(std::memory_order_relaxed);
        if (IsProtectedSafetyVk(vk, pauseVk, stopAllVk)) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const uint64_t word = st->suppressedVkBits[0].load(std::memory_order_acquire);
        if ((word & (1ULL << (vk & 63U))) == 0ULL) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (ShouldPassThroughEcho(st, vk, isDown)) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        return 1;
    }

    static DWORD WINAPI HookThreadEntry(LPVOID param) noexcept {
        auto* self = static_cast<SuppressionGate*>(param);
        if (self != nullptr) {
            self->HookThreadLoop();
        }
        return 0;
    }

    void SyncInstalledHooks(HHOOK& kbHook, HHOOK& mouseHook) noexcept {
        const bool wantKb = wantKeyboard_.load(std::memory_order_acquire) &&
                            desiredActive_.load(std::memory_order_acquire);
        const bool wantMs = wantMouse_.load(std::memory_order_acquire) &&
                            desiredActive_.load(std::memory_order_acquire);

        if (wantKb && kbHook == nullptr) {
            kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, &SuppressionGate::LowLevelKeyboardProc, nullptr, 0);
        } else if (!wantKb && kbHook != nullptr) {
            UnhookWindowsHookEx(kbHook);
            kbHook = nullptr;
        }

        if (wantMs && mouseHook == nullptr) {
            mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &SuppressionGate::LowLevelMouseProc, nullptr, 0);
        } else if (!wantMs && mouseHook != nullptr) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
        }
    }

    void HookThreadLoop() noexcept {
        MSG msg{};
        // Force creation of this thread's Win32 message queue before checking desiredActive_
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        HHOOK kbHook = nullptr;
        HHOOK mouseHook = nullptr;

        if (desiredActive_.load(std::memory_order_acquire)) {
            SyncInstalledHooks(kbHook, mouseHook);

            while (desiredActive_.load(std::memory_order_acquire) &&
                   (kbHook != nullptr || mouseHook != nullptr)) {
                const BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
                if (ret <= 0) {
                    break; // WM_QUIT or error
                }
                if (msg.message == kMsgSyncHooks) {
                    SyncInstalledHooks(kbHook, mouseHook);
                }
            }
        }

        if (kbHook != nullptr) {
            UnhookWindowsHookEx(kbHook);
            kbHook = nullptr;
        }
        if (mouseHook != nullptr) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
        }

        threadId_.store(0U, std::memory_order_release);
    }

    mask::core::EngineSharedState& sharedState_;
    SRWLOCK                        lock_{};
    HANDLE                         threadHandle_{nullptr};
    std::atomic<DWORD>             threadId_{0};
    std::atomic<bool>              desiredActive_{false};
    std::atomic<bool>              wantKeyboard_{false};
    std::atomic<bool>              wantMouse_{false};
};

} // namespace detail

/**
 * @brief Multi-Keyboard, Multi-Mouse, Multi-Monitor Remapping Engine with Layers, Unlock Policy,
 *        Monitor Warp, and an Opt-In Ring-3 SuppressionGate.
 */
class RemapEngine {
public:
    explicit RemapEngine(mask::core::EngineSharedState& sharedState) noexcept
        : sharedState_(sharedState)
        , suppressionGate_(sharedState) {
        InitializeSRWLock(&ruleLock_);
        InitializeSRWLock(&monitorLock_);
        InitializeSRWLock(&holdLock_);

        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            rules_[i].slotIndex = static_cast<uint8_t>(i);
        }
        RefreshMonitors();
    }

    ~RemapEngine() noexcept {
        Stop();
    }

    RemapEngine(const RemapEngine&) = delete;
    RemapEngine& operator=(const RemapEngine&) = delete;

    void Start() {
        started_.store(true, std::memory_order_release);
        RefreshMonitors();
        RecomputeSuppressedMask();
    }

    void Stop() noexcept {
        started_.store(false, std::memory_order_release);
        ResetStateAndSuppression();
        suppressionGate_.Shutdown();
    }

    void RefreshMonitors() noexcept {
        struct EnumCtx {
            std::array<MonitorRectInfo, kMaxMonitors> list{};
            int count{0};
        } ctx{};

        auto enumProc = [](HMONITOR hMon, HDC /*hdc*/, LPRECT /*rect*/, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lParam);
            if (c == nullptr || c->count >= static_cast<int>(kMaxMonitors)) {
                return FALSE;
            }
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi) == FALSE) {
                return TRUE;
            }
            auto& info = c->list[static_cast<size_t>(c->count)];
            info.left = mi.rcMonitor.left;
            info.top = mi.rcMonitor.top;
            info.right = mi.rcMonitor.right;
            info.bottom = mi.rcMonitor.bottom;
            info.dpi = detail::QueryMonitorDpi(hMon);
            info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0U;
            ++c->count;
            return TRUE;
        };

        EnumDisplayMonitors(nullptr, nullptr, enumProc, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.count == 0) {
            ctx.count = 1;
            ctx.list[0].left = 0;
            ctx.list[0].top = 0;
            ctx.list[0].right = GetSystemMetrics(SM_CXSCREEN);
            ctx.list[0].bottom = GetSystemMetrics(SM_CYSCREEN);
            if (ctx.list[0].right <= 0) {
                ctx.list[0].right = 1920;
            }
            if (ctx.list[0].bottom <= 0) {
                ctx.list[0].bottom = 1080;
            }
            ctx.list[0].dpi = 96U;
            ctx.list[0].isPrimary = true;
        } else {
            // Place primary display at index 0 for predictable "Monitor 1" indexing
            for (int i = 1; i < ctx.count; ++i) {
                if (ctx.list[static_cast<size_t>(i)].isPrimary) {
                    std::swap(ctx.list[0], ctx.list[static_cast<size_t>(i)]);
                    break;
                }
            }
        }

        for (int i = 0; i < ctx.count; ++i) {
            auto& m = ctx.list[static_cast<size_t>(i)];
            if (m.isPrimary) {
                std::swprintf(m.name, std::size(m.name), L"Monitor %d (Primary)", i + 1);
            } else {
                std::swprintf(m.name, std::size(m.name), L"Monitor %d", i + 1);
            }
        }

        AcquireSRWLockExclusive(&monitorLock_);
        monitors_ = ctx.list;
        monitorCount_ = ctx.count;
        ReleaseSRWLockExclusive(&monitorLock_);
    }

    [[nodiscard]] int GetMonitors(std::array<MonitorRectInfo, 8>& outMonitors) const noexcept {
        AcquireSRWLockShared(&monitorLock_);
        outMonitors = monitors_;
        const int count = monitorCount_;
        ReleaseSRWLockShared(&monitorLock_);
        return count;
    }

    void UpdateRule(size_t index, const mask::core::RemapRuleConfig& rule) noexcept {
        if (index >= mask::core::kMaxRemapRules) {
            return;
        }
        mask::core::RemapRuleConfig clean = rule;
        detail::SanitizeRemapRule(clean, index);

        AcquireSRWLockExclusive(&ruleLock_);
        rules_[index] = clean;
        ReleaseSRWLockExclusive(&ruleLock_);

        RecomputeSuppressedMask();
    }

    void UpdateAllRules(
        const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& rules
    ) noexcept {
        AcquireSRWLockExclusive(&ruleLock_);
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            mask::core::RemapRuleConfig clean = rules[i];
            detail::SanitizeRemapRule(clean, i);
            rules_[i] = clean;
        }
        ReleaseSRWLockExclusive(&ruleLock_);

        RecomputeSuppressedMask();
    }

    // Called directly by GameInput per-device transition callback
    void OnPhysicalDeviceTransition(
        const uint8_t deviceId[32],
        uint8_t deviceKind,
        uint16_t vk,
        bool isDown,
        uint64_t nowTsc
    ) noexcept {
        if (!started_.load(std::memory_order_acquire)) {
            return;
        }
        const uint16_t boundedVk = static_cast<uint16_t>(vk & 0xFFU);
        if (boundedVk == 0U) {
            return;
        }

        // Handle key-up first: always release any active hold created on key-down, even if layer/monitor/pause changed
        if (!isDown) {
            HandleKeyRelease(deviceId, boundedVk, nowTsc);
            return;
        }

        // Gate new key-down actions on pause / bind capture / UI text suppression
        if (sharedState_.automationPaused.load(std::memory_order_acquire) ||
            sharedState_.suppressTriggers.load(std::memory_order_relaxed) ||
            sharedState_.captureBindTarget.load(std::memory_order_relaxed) != 0U) {
            return;
        }

        // Ignore duplicate key-down if this physical key is already tracked as held
        if (IsHoldAlreadyActive(deviceId, boundedVk)) {
            return;
        }

        uint8_t activeLayer = sharedState_.activeRemapLayer.load(std::memory_order_acquire);
        if (activeLayer >= mask::core::kMaxRemapLayers) {
            activeLayer = 0U;
        }
        const bool unlocked = sharedState_.remapUnlocked.load(std::memory_order_acquire);

        bool hasCursorPos = false;
        POINT cursorPt{};

        // Snapshot matching rule under shared ruleLock_
        bool foundExact = false;
        bool foundFallback = false;
        mask::core::RemapRuleConfig exactRule{};
        mask::core::RemapRuleConfig fallbackRule{};

        AcquireSRWLockShared(&ruleLock_);
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            const auto& r = rules_[i];
            if (!r.enabled || !r.inUse) {
                continue;
            }
            if ((r.sourceVk & 0xFFU) != boundedVk) {
                continue;
            }
            if (r.deviceKind != 0U && r.deviceKind != deviceKind) {
                continue;
            }
            if (!r.anyDevice) {
                if (deviceId == nullptr || std::memcmp(r.deviceIdBytes, deviceId, 32U) != 0) {
                    continue;
                }
            }
            if (!MatchesUnlockPolicy(r.unlockPolicy, unlocked)) {
                continue;
            }

            // Layer filter: exact layer match takes priority; fallback to Base layer (0) when activeLayer > 0
            const bool isExact = (r.layerIndex == activeLayer);
            const bool isFallback = (activeLayer > 0U && r.layerIndex == 0U);
            if (!isExact && !isFallback) {
                continue;
            }

            // Monitor filter (for WarpToMonitor, monitorIndex designates the destination monitor, not a source filter)
            const auto action = static_cast<mask::core::RemapLayerAction>(r.actionKind);
            if (action != mask::core::RemapLayerAction::WarpToMonitor && r.monitorIndex >= 0) {
                if (!hasCursorPos) {
                    hasCursorPos = (GetCursorPos(&cursorPt) != FALSE);
                }
                if (!hasCursorPos || !IsPointInMonitor(cursorPt, r.monitorIndex)) {
                    continue;
                }
            }

            if (isExact) {
                // Prefer device-specific rule over anyDevice wildcard within the same layer
                if (!foundExact || (exactRule.anyDevice && !r.anyDevice)) {
                    exactRule = r;
                    foundExact = true;
                }
            } else if (isFallback) {
                if (!foundFallback || (fallbackRule.anyDevice && !r.anyDevice)) {
                    fallbackRule = r;
                    foundFallback = true;
                }
            }
        }
        ReleaseSRWLockShared(&ruleLock_);

        if (!foundExact && !foundFallback) {
            return;
        }

        const mask::core::RemapRuleConfig& winner = foundExact ? exactRule : fallbackRule;
        ExecuteRuleOnDown(winner, deviceId, boundedVk, activeLayer, unlocked, nowTsc);
    }

    void ResetStateAndSuppression() noexcept {
        // Release any active target keys held by remaps (if engine is still running) and clear holds
        std::array<uint16_t, kMaxActiveHolds> keysToRelease{};
        size_t releaseCount = 0U;
        bool hadShiftedLayer = false;

        AcquireSRWLockExclusive(&holdLock_);
        for (auto& h : activeHolds_) {
            if (!h.active) {
                continue;
            }
            if (h.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey) &&
                h.emittedTargetVk != 0U && releaseCount < keysToRelease.size()) {
                keysToRelease[releaseCount++] = h.emittedTargetVk;
            } else if (h.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::ShiftLayerHold)) {
                hadShiftedLayer = true;
            }
            h = ActiveRemapHold{};
        }
        ReleaseSRWLockExclusive(&holdLock_);

        if (releaseCount > 0U && sharedState_.engineRunning.load(std::memory_order_acquire)) {
            unsigned int aux = 0;
            const uint64_t nowTsc = __rdtscp(&aux);
            bool pushed = false;
            for (size_t i = 0; i < releaseCount; ++i) {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = nowTsc;
                cmd.vk = keysToRelease[i];
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
                if (sharedState_.auxOutputQueue.try_push(cmd)) {
                    pushed = true;
                }
            }
            if (pushed) {
                mask::input::WakeEngine();
            }
        }

        if (hadShiftedLayer) {
            sharedState_.activeRemapLayer.store(0U, std::memory_order_release);
        }

        for (auto& word : sharedState_.suppressedVkBits) {
            word.store(0ULL, std::memory_order_release);
        }
        sharedState_.anySuppressionActive.store(false, std::memory_order_release);
        suppressionGate_.Sync(false, false);
    }

    void RecomputeSuppressedMask() noexcept {
        if (!started_.load(std::memory_order_acquire) ||
            sharedState_.automationPaused.load(std::memory_order_acquire)) {
            for (auto& word : sharedState_.suppressedVkBits) {
                word.store(0ULL, std::memory_order_release);
            }
            sharedState_.anySuppressionActive.store(false, std::memory_order_release);
            suppressionGate_.Sync(false, false);
            return;
        }

        uint8_t activeLayer = sharedState_.activeRemapLayer.load(std::memory_order_acquire);
        if (activeLayer >= mask::core::kMaxRemapLayers) {
            activeLayer = 0U;
        }
        const bool unlocked = sharedState_.remapUnlocked.load(std::memory_order_acquire);
        const uint16_t pauseVk = sharedState_.globalPauseVk.load(std::memory_order_relaxed);
        const uint16_t stopAllVk = sharedState_.globalStopAllVk.load(std::memory_order_relaxed);

        uint64_t bits[4]{0ULL, 0ULL, 0ULL, 0ULL};

        AcquireSRWLockShared(&ruleLock_);
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            const auto& r = rules_[i];
            if (!r.enabled || !r.inUse) {
                continue;
            }
            const bool wantsSuppress =
                r.suppressOriginal ||
                (r.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::BlockKey));
            if (!wantsSuppress) {
                continue;
            }
            if (r.layerIndex != activeLayer && r.layerIndex != 0U) {
                continue;
            }
            if (!MatchesUnlockPolicy(r.unlockPolicy, unlocked)) {
                continue;
            }
            const uint16_t srcVk = static_cast<uint16_t>(r.sourceVk & 0xFFU);
            if (detail::IsProtectedSafetyVk(srcVk, pauseVk, stopAllVk)) {
                continue;
            }
            bits[srcVk >> 6] |= (1ULL << (srcVk & 63U));
        }
        ReleaseSRWLockShared(&ruleLock_);

        for (size_t w = 0; w < 4; ++w) {
            sharedState_.suppressedVkBits[w].store(bits[w], std::memory_order_release);
        }

        const bool wantMouse = (bits[0] & detail::kMouseButtonVkMaskWord0) != 0ULL;
        const bool wantKeyboard =
            ((bits[0] & ~detail::kMouseButtonVkMaskWord0) | bits[1] | bits[2] | bits[3]) != 0ULL;
        const bool anyActive = wantKeyboard || wantMouse;

        sharedState_.anySuppressionActive.store(anyActive, std::memory_order_release);
        suppressionGate_.Sync(wantKeyboard, wantMouse);
    }

    [[nodiscard]] static bool SaveRemaps(
        const wchar_t* path,
        const std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& rules
    ) noexcept {
        std::unique_ptr<detail::RemapsFilePayload> payload(new (std::nothrow) detail::RemapsFilePayload{});
        if (!payload) {
            return false;
        }
        payload->magic = kRemapRulesMagic;
        payload->version = kRemapRulesVersion;
        payload->count = static_cast<uint32_t>(mask::core::kMaxRemapRules);
        payload->reserved = 0U;
        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            payload->rules[i] = rules[i];
            detail::SanitizeRemapRule(payload->rules[i], i);
        }
        return detail::WriteFileAtomic(path, payload.get(), sizeof(detail::RemapsFilePayload));
    }

    [[nodiscard]] static bool LoadRemaps(
        const wchar_t* path,
        std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules>& outRules
    ) noexcept {
        if (path == nullptr || path[0] == L'\0') {
            return false;
        }
        std::unique_ptr<detail::RemapsFilePayload> payload(new (std::nothrow) detail::RemapsFilePayload{});
        if (!payload) {
            return false;
        }

        bool ok = detail::ReadRemapsFileRaw(path, *payload);
        if (!ok) {
            wchar_t bak[1040]{};
            if (std::swprintf(bak, std::size(bak), L"%ls.bak", path) >= 0) {
                ok = detail::ReadRemapsFileRaw(bak, *payload);
            }
        }
        if (!ok) {
            return false;
        }

        for (size_t i = 0; i < mask::core::kMaxRemapRules; ++i) {
            detail::SanitizeRemapRule(payload->rules[i], i);
            outRules[i] = payload->rules[i];
        }
        return true;
    }

private:
    static constexpr size_t kMaxActiveHolds = 32U;

    struct ActiveRemapHold {
        bool     active{false};
        bool     hasDeviceId{false};
        uint8_t  deviceId[32]{};
        uint16_t sourceVk{0};
        uint16_t emittedTargetVk{0};
        uint8_t  actionKind{0};
        uint8_t  prevLayer{0};
    };

    [[nodiscard]] static bool MatchesUnlockPolicy(uint8_t policyRaw, bool unlocked) noexcept {
        const auto policy = static_cast<mask::core::RemapUnlockPolicy>(policyRaw);
        switch (policy) {
        case mask::core::RemapUnlockPolicy::Always:       return true;
        case mask::core::RemapUnlockPolicy::WhenUnlocked: return unlocked;
        case mask::core::RemapUnlockPolicy::WhenLocked:   return !unlocked;
        default:                                          return true;
        }
    }

    [[nodiscard]] static bool SameDevice(const ActiveRemapHold& h, const uint8_t deviceId[32]) noexcept {
        if (!h.hasDeviceId || deviceId == nullptr) {
            return true;
        }
        return std::memcmp(h.deviceId, deviceId, 32U) == 0;
    }

    [[nodiscard]] bool IsPointInMonitor(const POINT& pt, int8_t monitorIndex) const noexcept {
        if (monitorIndex < 0) {
            return true;
        }
        AcquireSRWLockShared(&monitorLock_);
        bool inside = false;
        if (monitorIndex < monitorCount_) {
            const auto& m = monitors_[static_cast<size_t>(monitorIndex)];
            inside = (pt.x >= m.left && pt.x < m.right && pt.y >= m.top && pt.y < m.bottom);
        }
        ReleaseSRWLockShared(&monitorLock_);
        return inside;
    }

    [[nodiscard]] bool IsHoldAlreadyActive(const uint8_t deviceId[32], uint16_t sourceVk) const noexcept {
        AcquireSRWLockShared(&holdLock_);
        bool found = false;
        for (const auto& h : activeHolds_) {
            if (h.active && h.sourceVk == sourceVk && SameDevice(h, deviceId)) {
                found = true;
                break;
            }
        }
        ReleaseSRWLockShared(&holdLock_);
        return found;
    }

    void RecordActiveHold(
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint16_t emittedTargetVk,
        uint8_t actionKind,
        uint8_t prevLayer
    ) noexcept {
        AcquireSRWLockExclusive(&holdLock_);
        for (auto& h : activeHolds_) {
            if (!h.active) {
                h.active = true;
                h.hasDeviceId = (deviceId != nullptr);
                if (deviceId != nullptr) {
                    std::memcpy(h.deviceId, deviceId, 32U);
                } else {
                    std::memset(h.deviceId, 0, 32U);
                }
                h.sourceVk = sourceVk;
                h.emittedTargetVk = emittedTargetVk;
                h.actionKind = actionKind;
                h.prevLayer = prevLayer;
                break;
            }
        }
        ReleaseSRWLockExclusive(&holdLock_);
    }

    void HandleKeyRelease(const uint8_t deviceId[32], uint16_t sourceVk, uint64_t nowTsc) noexcept {
        uint16_t releaseTargetVk = 0U;
        bool restoreLayer = false;
        uint8_t prevLayer = 0U;

        AcquireSRWLockExclusive(&holdLock_);
        for (auto& h : activeHolds_) {
            if (h.active && h.sourceVk == sourceVk && SameDevice(h, deviceId)) {
                const uint8_t kind = h.actionKind;
                const uint16_t targetVk = h.emittedTargetVk;
                const uint8_t savedLayer = h.prevLayer;
                h = ActiveRemapHold{};

                if (kind == static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey) && targetVk != 0U) {
                    bool stillHeldElsewhere = false;
                    for (const auto& other : activeHolds_) {
                        if (other.active &&
                            other.actionKind == static_cast<uint8_t>(mask::core::RemapLayerAction::RemapKey) &&
                            other.emittedTargetVk == targetVk) {
                            stillHeldElsewhere = true;
                            break;
                        }
                    }
                    if (!stillHeldElsewhere) {
                        releaseTargetVk = targetVk;
                    }
                } else if (kind == static_cast<uint8_t>(mask::core::RemapLayerAction::ShiftLayerHold)) {
                    restoreLayer = true;
                    prevLayer = savedLayer;
                }
                break;
            }
        }
        ReleaseSRWLockExclusive(&holdLock_);

        if (releaseTargetVk != 0U) {
            mask::core::AuxOutputCommand cmd{};
            cmd.timestampTsc = nowTsc;
            cmd.vk = releaseTargetVk;
            cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
            if (sharedState_.auxOutputQueue.try_push(cmd)) {
                mask::input::WakeEngine();
            }
        }

        if (restoreLayer) {
            sharedState_.activeRemapLayer.store(prevLayer, std::memory_order_release);
            RecomputeSuppressedMask();
        }
    }

    void ExecuteRuleOnDown(
        const mask::core::RemapRuleConfig& rule,
        const uint8_t deviceId[32],
        uint16_t sourceVk,
        uint8_t currentLayer,
        bool currentlyUnlocked,
        uint64_t nowTsc
    ) noexcept {
        const auto action = static_cast<mask::core::RemapLayerAction>(rule.actionKind);
        switch (action) {
        case mask::core::RemapLayerAction::RemapKey: {
            const uint16_t targetVk = static_cast<uint16_t>(rule.targetVk & 0xFFU);
            if (targetVk != 0U) {
                RecordActiveHold(deviceId, sourceVk, targetVk, rule.actionKind, currentLayer);
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = nowTsc;
                cmd.vk = targetVk;
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown);
                if (sharedState_.auxOutputQueue.try_push(cmd)) {
                    mask::input::WakeEngine();
                }
            }
            break;
        }
        case mask::core::RemapLayerAction::BlockKey: {
            // Original input is swallowed by SuppressionGate; emit nothing.
            break;
        }
        case mask::core::RemapLayerAction::ShiftLayerHold: {
            const uint8_t nextLayer = static_cast<uint8_t>(rule.targetLayer % mask::core::kMaxRemapLayers);
            RecordActiveHold(deviceId, sourceVk, 0U, rule.actionKind, currentLayer);
            sharedState_.activeRemapLayer.store(nextLayer, std::memory_order_release);
            RecomputeSuppressedMask();
            break;
        }
        case mask::core::RemapLayerAction::ToggleLayer: {
            const uint8_t targetLayer = static_cast<uint8_t>(rule.targetLayer % mask::core::kMaxRemapLayers);
            const uint8_t nextLayer = (currentLayer == targetLayer) ? 0U : targetLayer;
            sharedState_.activeRemapLayer.store(nextLayer, std::memory_order_release);
            RecomputeSuppressedMask();
            break;
        }
        case mask::core::RemapLayerAction::ToggleUnlockState: {
            sharedState_.remapUnlocked.store(!currentlyUnlocked, std::memory_order_release);
            RecomputeSuppressedMask();
            break;
        }
        case mask::core::RemapLayerAction::WarpToMonitor: {
            int32_t cx = 0;
            int32_t cy = 0;
            bool valid = false;
            AcquireSRWLockShared(&monitorLock_);
            if (monitorCount_ > 0) {
                int idx = (rule.monitorIndex >= 0) ? static_cast<int>(rule.monitorIndex) : 0;
                if (idx >= monitorCount_) {
                    idx = monitorCount_ - 1;
                }
                const auto& m = monitors_[static_cast<size_t>(idx)];
                cx = m.left + ((m.right - m.left) / 2);
                cy = m.top + ((m.bottom - m.top) / 2);
                valid = true;
            }
            ReleaseSRWLockShared(&monitorLock_);

            if (valid) {
                mask::core::AuxOutputCommand cmd{};
                cmd.timestampTsc = nowTsc;
                cmd.argX = cx;
                cmd.argY = cy;
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::MouseMoveAbs);
                if (sharedState_.auxOutputQueue.try_push(cmd)) {
                    mask::input::WakeEngine();
                }
            }
            break;
        }
        default:
            break;
        }
    }

    mask::core::EngineSharedState& sharedState_;
    detail::SuppressionGate        suppressionGate_;
    std::atomic<bool>              started_{false};

    mutable SRWLOCK ruleLock_{};
    std::array<mask::core::RemapRuleConfig, mask::core::kMaxRemapRules> rules_{};

    mutable SRWLOCK monitorLock_{};
    std::array<MonitorRectInfo, kMaxMonitors> monitors_{};
    int monitorCount_{0};

    mutable SRWLOCK holdLock_{};
    std::array<ActiveRemapHold, kMaxActiveHolds> activeHolds_{};
};

} // namespace mask::remap
