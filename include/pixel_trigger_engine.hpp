#pragma once

// Pure Ring-3 user-mode Screen / Pixel Color Trigger engine.
// ZERO process memory reading, ZERO DLL injection.
// Uses standard Windows DXGI Desktop Duplication (ID3D11Device + IDXGIOutputDuplication)
// into a reusable 32x32 D3D11_USAGE_STAGING texture with zero per-frame heap allocations,
// gracefully falling back to a persistent cached 32x32 GDI DIBSection + BitBlt ROI capture.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core_contracts.hpp"
#include "input_pipeline.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <intrin.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <type_traits>

namespace mask::triggers {

inline constexpr uint32_t kTriggersFileMagic   = 0x4D545247U; // 'MTRG'
inline constexpr uint32_t kTriggersFileVersion = 1U;
inline constexpr size_t   kMaxMonitors         = 8;

struct MonitorInfoEntry {
    int32_t  left{0}, top{0}, width{1920}, height{1080};
    uint32_t dpi{96};
    bool     isPrimary{true};
    wchar_t  name[32]{L"Monitor 1"};
};

struct ColorTriggerPreviewSnapshot {
    static constexpr int kLoupeSize = 15; // 15x15 pixels (0x00RRGGBB)
    uint32_t loupePixels[kLoupeSize * kLoupeSize]{};
    uint8_t  sampledR{0}, sampledG{0}, sampledB{0};
    uint32_t colorDelta{0};
    int32_t  sampleX{0}, sampleY{0};
    bool     isMatched{false};
    bool     isArmed{false};
    bool     usingDxgi{false};
    uint64_t triggersFired{0};
};

static_assert(std::is_trivially_copyable_v<ColorTriggerPreviewSnapshot>,
              "ColorTriggerPreviewSnapshot must remain trivially copyable for seqlock transfer.");

namespace detail {

inline constexpr int32_t kPatchSize = 32;
inline constexpr int32_t kPatchHalf = kPatchSize / 2; // 16

template <typename T>
inline void SafeRelease(T*& ptr) noexcept {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

[[nodiscard]] inline bool SanitizeBool(const bool& value) noexcept {
    uint8_t raw = 0;
    std::memcpy(&raw, &value, 1);
    return raw != 0;
}

[[nodiscard]] inline uint64_t QueryMonotonicUs() noexcept {
    static const int64_t kQpf = []() noexcept {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return (f.QuadPart > 0) ? f.QuadPart : 10'000'000LL;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint64_t ticks = static_cast<uint64_t>(now.QuadPart);
    const uint64_t freq  = static_cast<uint64_t>(kQpf);
    const uint64_t sec   = ticks / freq;
    const uint64_t rem   = ticks % freq;
    return sec * 1'000'000ULL + (rem * 1'000'000ULL) / freq;
}

[[nodiscard]] constexpr uint32_t ColorDistanceEuclidean(
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2
) noexcept {
    const int32_t dr = static_cast<int32_t>(r1) - static_cast<int32_t>(r2);
    const int32_t dg = static_cast<int32_t>(g1) - static_cast<int32_t>(g2);
    const int32_t db = static_cast<int32_t>(b1) - static_cast<int32_t>(b2);
    const uint32_t sq = static_cast<uint32_t>(dr * dr + dg * dg + db * db);
    if (sq == 0U) {
        return 0U;
    }
    uint32_t x = sq;
    uint32_t y = (x + 1U) >> 1;
    while (y < x) {
        x = y;
        y = (x + sq / x) >> 1;
    }
    if ((x + 1U) * (x + 1U) - sq < sq - x * x) {
        ++x;
    }
    return x;
}

[[nodiscard]] inline uint32_t QueryMonitorEffectiveDpi(HMONITOR hMonitor) noexcept {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const GetDpiForMonitorFn fn = []() noexcept -> GetDpiForMonitorFn {
        HMODULE mod = GetModuleHandleW(L"shcore.dll");
        if (mod == nullptr) {
            mod = LoadLibraryW(L"shcore.dll");
        }
        if (mod == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(mod, "GetDpiForMonitor"));
    }();

    if (fn != nullptr && hMonitor != nullptr) {
        UINT dpiX = 96U;
        UINT dpiY = 96U;
        if (SUCCEEDED(fn(hMonitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) && dpiX >= 48U && dpiX <= 960U) {
            return static_cast<uint32_t>(dpiX);
        }
    }

    const HDC screenDc = GetDC(nullptr);
    uint32_t dpi = 96U;
    if (screenDc != nullptr) {
        const int logPixels = GetDeviceCaps(screenDc, LOGPIXELSX);
        ReleaseDC(nullptr, screenDc);
        if (logPixels >= 48 && logPixels <= 960) {
            dpi = static_cast<uint32_t>(logPixels);
        }
    }
    return dpi;
}

struct MonitorEnumContext {
    std::array<MonitorInfoEntry, kMaxMonitors>* out{nullptr};
    int count{0};
};

inline BOOL CALLBACK EnumMonitorsProc(
    HMONITOR hMonitor,
    HDC /*hdcMonitor*/,
    LPRECT /*lprcMonitor*/,
    LPARAM dwData
) noexcept {
    auto* ctx = reinterpret_cast<MonitorEnumContext*>(dwData);
    if (ctx == nullptr || ctx->out == nullptr || ctx->count >= static_cast<int>(kMaxMonitors)) {
        return FALSE;
    }

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMonitor, &mi) == FALSE) {
        return TRUE;
    }

    MonitorInfoEntry entry{};
    entry.left      = static_cast<int32_t>(mi.rcMonitor.left);
    entry.top       = static_cast<int32_t>(mi.rcMonitor.top);
    entry.width     = std::max<int32_t>(1, static_cast<int32_t>(mi.rcMonitor.right - mi.rcMonitor.left));
    entry.height    = std::max<int32_t>(1, static_cast<int32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top));
    entry.dpi       = QueryMonitorEffectiveDpi(hMonitor);
    entry.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0U;

    (*ctx->out)[static_cast<size_t>(ctx->count)] = entry;
    ++ctx->count;
    return (ctx->count < static_cast<int>(kMaxMonitors)) ? TRUE : FALSE;
}

struct TriggersFileImage {
    uint32_t magic{kTriggersFileMagic};
    uint32_t version{kTriggersFileVersion};
    uint32_t count{static_cast<uint32_t>(mask::core::kMaxColorTriggers)};
    uint32_t reserved{0};
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers> triggers{};
};

static_assert(std::is_trivially_copyable_v<TriggersFileImage>,
              "TriggersFileImage must remain trivially copyable.");

inline void SanitizeTriggerConfig(mask::core::ColorTriggerConfig& c, size_t slotIndex) noexcept {
    c.slotIndex = static_cast<uint8_t>(slotIndex);
    c.inUse     = SanitizeBool(c.inUse);
    c.enabled   = c.inUse ? SanitizeBool(c.enabled) : false;

    if (c.mode > static_cast<uint8_t>(mask::core::ColorTriggerMode::ColorChanged)) {
        c.mode = static_cast<uint8_t>(mask::core::ColorTriggerMode::MatchColor);
    }
    if (c.actionKind > static_cast<uint8_t>(mask::core::ColorTriggerActionKind::RunComplexSeq)) {
        c.actionKind = static_cast<uint8_t>(mask::core::ColorTriggerActionKind::ClickKey);
    }
    if (c.armMode > 2U) {
        c.armMode = 0U;
    }
    if (c.monitorIndex >= static_cast<uint8_t>(kMaxMonitors)) {
        c.monitorIndex = 0U;
    }
    if (c.sampleRadius > 8U) {
        c.sampleRadius = 8U;
    }

    c.armVk      = static_cast<uint16_t>(c.armVk & 0xFFU);
    c.actionVk   = static_cast<uint16_t>(c.actionVk & 0xFFU);
    c.delayMs    = std::min<uint32_t>(c.delayMs, 10'000U);
    c.holdMs     = std::min<uint32_t>(c.holdMs, 10'000U);
    c.cooldownMs = std::min<uint32_t>(c.cooldownMs, 60'000U);

    if (c.actionKind == static_cast<uint8_t>(mask::core::ColorTriggerActionKind::RunMacroSlot) &&
        c.linkedIndex >= static_cast<uint8_t>(mask::core::kMaxMacroSlots)) {
        c.linkedIndex = 0U;
    } else if (c.actionKind == static_cast<uint8_t>(mask::core::ColorTriggerActionKind::RunComplexSeq) &&
               c.linkedIndex >= static_cast<uint8_t>(mask::core::kMaxComplexSequences)) {
        c.linkedIndex = 0U;
    }

    c.name[std::size(c.name) - 1U] = L'\0';
    if (c.name[0] == L'\0') {
        wcscpy_s(c.name, L"Color trigger");
    }
}

[[nodiscard]] inline bool ReadTriggersFileRaw(const wchar_t* path, TriggersFileImage& out) noexcept {
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
              fileSize.QuadPart == static_cast<LONGLONG>(sizeof(TriggersFileImage));

    TriggersFileImage loaded{};
    if (ok) {
        DWORD bytesRead = 0;
        ok = ReadFile(h, &loaded, static_cast<DWORD>(sizeof(TriggersFileImage)), &bytesRead, nullptr) != FALSE &&
             bytesRead == static_cast<DWORD>(sizeof(TriggersFileImage)) &&
             loaded.magic == kTriggersFileMagic &&
             loaded.version == kTriggersFileVersion &&
             loaded.count == static_cast<uint32_t>(mask::core::kMaxColorTriggers);
    }

    CloseHandle(h);
    if (ok) {
        out = loaded;
    }
    return ok;
}

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

/**
 * @brief Persistent reusable 32x32 32-bit BGRA DIBSection for zero-allocation GDI BitBlt ROI fallback.
 */
class GdiRoiCapture {
public:
    GdiRoiCapture() noexcept = default;
    ~GdiRoiCapture() noexcept { Shutdown(); }

    GdiRoiCapture(const GdiRoiCapture&) = delete;
    GdiRoiCapture& operator=(const GdiRoiCapture&) = delete;

    [[nodiscard]] bool EnsureInitialized() noexcept {
        if (memDc_ != nullptr && dib_ != nullptr && bits_ != nullptr) {
            return true;
        }
        Shutdown();

        const HDC screenDc = GetDC(nullptr);
        if (screenDc == nullptr) {
            return false;
        }

        memDc_ = CreateCompatibleDC(screenDc);
        ReleaseDC(nullptr, screenDc);
        if (memDc_ == nullptr) {
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = kPatchSize;
        bmi.bmiHeader.biHeight      = -kPatchSize; // Top-down DIB
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* rawBits = nullptr;
        dib_ = CreateDIBSection(memDc_, &bmi, DIB_RGB_COLORS, &rawBits, nullptr, 0);
        if (dib_ == nullptr || rawBits == nullptr) {
            Shutdown();
            return false;
        }

        oldBmp_ = SelectObject(memDc_, dib_);
        bits_   = static_cast<uint32_t*>(rawBits);
        return true;
    }

    void Shutdown() noexcept {
        if (memDc_ != nullptr) {
            if (oldBmp_ != nullptr) {
                SelectObject(memDc_, oldBmp_);
                oldBmp_ = nullptr;
            }
            if (dib_ != nullptr) {
                DeleteObject(dib_);
                dib_ = nullptr;
            }
            DeleteDC(memDc_);
            memDc_ = nullptr;
        } else if (dib_ != nullptr) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        bits_ = nullptr;
    }

    [[nodiscard]] bool CaptureRect(
        int32_t screenLeft,
        int32_t screenTop,
        int32_t width,
        int32_t height,
        uint32_t outPatch[kPatchSize * kPatchSize]
    ) noexcept {
        if (width <= 0 || height <= 0 || width > kPatchSize || height > kPatchSize) {
            return false;
        }
        if (!EnsureInitialized()) {
            return false;
        }

        const HDC screenDc = GetDC(nullptr);
        if (screenDc == nullptr) {
            return false;
        }

        const BOOL ok = BitBlt(memDc_, 0, 0, width, height, screenDc, screenLeft, screenTop, SRCCOPY);
        ReleaseDC(nullptr, screenDc);
        if (ok == FALSE) {
            return false;
        }

        for (int32_t y = 0; y < height; ++y) {
            const uint32_t* srcRow = bits_ + static_cast<size_t>(y * kPatchSize);
            uint32_t*       dstRow = outPatch + static_cast<size_t>(y * kPatchSize);
            for (int32_t x = 0; x < width; ++x) {
                dstRow[x] = srcRow[x] & 0x00FFFFFFU; // BGRA little-endian -> 0x00RRGGBB
            }
        }
        return true;
    }

private:
    HDC       memDc_{nullptr};
    HBITMAP   dib_{nullptr};
    HGDIOBJ   oldBmp_{nullptr};
    uint32_t* bits_{nullptr};
};

/**
 * @brief Pre-allocated 32x32 CPU pixel patch (0x00RRGGBB) with origin metadata for DXGI WAIT_TIMEOUT reuse.
 */
struct PixelPatch {
    uint32_t pixels[kPatchSize * kPatchSize]{};
    int32_t  originScreenX{0};
    int32_t  originScreenY{0};
    int32_t  width{0};
    int32_t  height{0};
    uint8_t  monitorIndex{0};
    bool     valid{false};
    bool     fromDxgi{false};

    [[nodiscard]] uint32_t SampleClamped(int32_t screenX, int32_t screenY) const noexcept {
        if (!valid || width <= 0 || height <= 0) {
            return 0U;
        }
        const int32_t lx = std::clamp(screenX - originScreenX, 0, width - 1);
        const int32_t ly = std::clamp(screenY - originScreenY, 0, height - 1);
        return pixels[static_cast<size_t>(ly * kPatchSize + lx)];
    }
};

/**
 * @brief Pure Ring-3 DXGI Desktop Duplication backend with reusable 32x32 staging texture
 *        and clean automatic recovery on DXGI_ERROR_ACCESS_LOST.
 */
class DxgiDesktopCapture {
public:
    enum class AcquireStatus : uint8_t {
        FrameReady  = 0,
        WaitTimeout = 1,
        AccessLost  = 2,
        Unavailable = 3
    };

    DxgiDesktopCapture() noexcept = default;
    ~DxgiDesktopCapture() noexcept { Shutdown(); }

    DxgiDesktopCapture(const DxgiDesktopCapture&) = delete;
    DxgiDesktopCapture& operator=(const DxgiDesktopCapture&) = delete;

    void Shutdown() noexcept {
        for (auto& out : outputs_) {
            if (out.frameAcquired && out.dup != nullptr) {
                out.dup->ReleaseFrame();
            }
            SafeRelease(out.acquiredTex);
            SafeRelease(out.dup);
            out = OutputSlot{};
        }
        outputCount_ = 0;
        SafeRelease(stagingTex_);
        SafeRelease(context_);
        SafeRelease(device_);
        initialized_ = false;
    }

    [[nodiscard]] bool IsReady() const noexcept {
        return initialized_ && device_ != nullptr && context_ != nullptr && stagingTex_ != nullptr;
    }

    [[nodiscard]] bool EnsureInitialized(uint64_t nowUs) noexcept {
        if (IsReady()) {
            return true;
        }
        if (nowUs < retryAfterUs_) {
            return false;
        }

        Shutdown();

        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
            factory == nullptr) {
            retryAfterUs_ = nowUs + kRetryBackoffUs;
            return false;
        }

        // Select the first hardware adapter that owns an attached desktop output (preferring primary at 0,0).
        IDXGIAdapter1* chosenAdapter = nullptr;
        IDXGIAdapter1* fallbackAdapter = nullptr;
        for (UINT ai = 0; ; ++ai) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (adapter == nullptr) {
                continue;
            }

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
                adapter->Release();
                continue;
            }

            bool hasPrimary = false;
            bool hasAnyOutput = false;
            for (UINT oi = 0; ; ++oi) {
                IDXGIOutput* output = nullptr;
                if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (output != nullptr) {
                    DXGI_OUTPUT_DESC od{};
                    if (SUCCEEDED(output->GetDesc(&od)) && od.AttachedToDesktop != FALSE) {
                        hasAnyOutput = true;
                        if (od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0) {
                            hasPrimary = true;
                        }
                    }
                    output->Release();
                }
            }

            if (hasPrimary) {
                chosenAdapter = adapter;
                break;
            }
            if (hasAnyOutput && fallbackAdapter == nullptr) {
                fallbackAdapter = adapter;
            } else {
                adapter->Release();
            }
        }

        if (chosenAdapter == nullptr) {
            chosenAdapter = fallbackAdapter;
            fallbackAdapter = nullptr;
        } else {
            SafeRelease(fallbackAdapter);
        }

        const D3D_FEATURE_LEVEL featureLevels[3] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1
        };
        D3D_FEATURE_LEVEL actualLevel = D3D_FEATURE_LEVEL_11_0;
        const UINT createFlags = D3D11_CREATE_DEVICE_SINGLETHREADED;

        HRESULT hr = E_FAIL;
        if (chosenAdapter != nullptr) {
            hr = D3D11CreateDevice(
                chosenAdapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                createFlags,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                &device_,
                &actualLevel,
                &context_
            );
        } else {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                createFlags,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                &device_,
                &actualLevel,
                &context_
            );
        }

        SafeRelease(factory);

        if (FAILED(hr) || device_ == nullptr || context_ == nullptr) {
            SafeRelease(chosenAdapter);
            Shutdown();
            retryAfterUs_ = nowUs + kRetryBackoffUs;
            return false;
        }

        // Create tiny reusable 32x32 staging texture (zero per-frame heap/GPU allocation).
        D3D11_TEXTURE2D_DESC stDesc{};
        stDesc.Width              = static_cast<UINT>(kPatchSize);
        stDesc.Height             = static_cast<UINT>(kPatchSize);
        stDesc.MipLevels          = 1U;
        stDesc.ArraySize          = 1U;
        stDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
        stDesc.SampleDesc.Count   = 1U;
        stDesc.SampleDesc.Quality = 0U;
        stDesc.Usage              = D3D11_USAGE_STAGING;
        stDesc.BindFlags          = 0U;
        stDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
        stDesc.MiscFlags          = 0U;

        hr = device_->CreateTexture2D(&stDesc, nullptr, &stagingTex_);
        if (FAILED(hr) || stagingTex_ == nullptr) {
            SafeRelease(chosenAdapter);
            Shutdown();
            retryAfterUs_ = nowUs + kRetryBackoffUs;
            return false;
        }

        // Discover adapter outputs matching desktop coordinates (duplication is opened lazily per monitor).
        IDXGIAdapter* activeAdapter = nullptr;
        if (chosenAdapter != nullptr) {
            activeAdapter = chosenAdapter;
        } else {
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
                dxgiDevice != nullptr) {
                dxgiDevice->GetAdapter(&activeAdapter);
                dxgiDevice->Release();
            }
        }

        if (activeAdapter != nullptr) {
            for (UINT oi = 0; outputCount_ < kMaxMonitors; ++oi) {
                IDXGIOutput* output = nullptr;
                if (activeAdapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (output == nullptr) {
                    continue;
                }
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(output->GetDesc(&od)) && od.AttachedToDesktop != FALSE) {
                    auto& slot = outputs_[outputCount_];
                    slot.outputIndex = oi;
                    slot.desktopRect = od.DesktopCoordinates;
                    slot.valid       = true;
                    ++outputCount_;
                }
                output->Release();
            }
            activeAdapter->Release();
        }

        if (outputCount_ == 0U) {
            Shutdown();
            retryAfterUs_ = nowUs + kRetryBackoffUs;
            return false;
        }

        initialized_ = true;
        return true;
    }

    /**
     * @brief Acquires the latest DXGI desktop frame (non-blocking, timeout=0) for the output matching `mon`.
     * Caller MUST invoke `ReleaseAcquiredFrame(slotIdx)` when `AcquireStatus::FrameReady` is returned.
     */
    [[nodiscard]] AcquireStatus AcquireMonitorFrame(
        const MonitorInfoEntry& mon,
        uint64_t nowUs,
        size_t& outSlotIdx
    ) noexcept {
        outSlotIdx = kMaxMonitors;
        if (!EnsureInitialized(nowUs)) {
            return AcquireStatus::Unavailable;
        }

        const size_t idx = FindOutputForMonitor(mon);
        if (idx >= outputCount_) {
            return AcquireStatus::Unavailable;
        }
        auto& slot = outputs_[idx];

        if (slot.dup == nullptr) {
            if (nowUs < slot.dupRetryAfterUs) {
                return AcquireStatus::Unavailable;
            }
            if (!OpenDuplicationForSlot(slot)) {
                slot.dupRetryAfterUs = nowUs + kRetryBackoffUs;
                return AcquireStatus::Unavailable;
            }
        }

        if (slot.frameAcquired) {
            ReleaseAcquiredFrame(idx);
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource* desktopRes = nullptr;
        const HRESULT hr = slot.dup->AcquireNextFrame(0U, &frameInfo, &desktopRes);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            SafeRelease(desktopRes);
            outSlotIdx = idx;
            return AcquireStatus::WaitTimeout;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST ||
            hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET ||
            hr == E_ACCESSDENIED) {
            SafeRelease(desktopRes);
            Shutdown();
            retryAfterUs_ = nowUs + kRetryBackoffUs;
            return AcquireStatus::AccessLost;
        }

        if (FAILED(hr) || desktopRes == nullptr) {
            SafeRelease(desktopRes);
            return AcquireStatus::Unavailable;
        }

        // Before the first real desktop frame is presented, DXGI may return a pointer-only
        // update (LastPresentTime == 0) whose desktop surface is still blank. Treat as timeout
        // until the first real frame arrives so we never sample an uninitialized black frame.
        if (frameInfo.LastPresentTime.QuadPart == 0 && !slot.everGotFrame) {
            desktopRes->Release();
            slot.dup->ReleaseFrame();
            outSlotIdx = idx;
            return AcquireStatus::WaitTimeout;
        }

        ID3D11Texture2D* desktopTex = nullptr;
        const HRESULT qiHr = desktopRes->QueryInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&desktopTex)
        );
        desktopRes->Release();

        if (FAILED(qiHr) || desktopTex == nullptr) {
            slot.dup->ReleaseFrame();
            return AcquireStatus::Unavailable;
        }

        slot.acquiredTex   = desktopTex;
        slot.frameAcquired = true;
        slot.everGotFrame  = true;
        outSlotIdx         = idx;
        return AcquireStatus::FrameReady;
    }

    [[nodiscard]] bool CopyRoiFromAcquiredFrame(
        size_t slotIdx,
        int32_t screenLeft,
        int32_t screenTop,
        int32_t reqWidth,
        int32_t reqHeight,
        PixelPatch& outPatch
    ) noexcept {
        if (!IsReady() || slotIdx >= outputCount_) {
            return false;
        }
        auto& slot = outputs_[slotIdx];
        if (!slot.frameAcquired || slot.acquiredTex == nullptr) {
            return false;
        }

        D3D11_TEXTURE2D_DESC texDesc{};
        slot.acquiredTex->GetDesc(&texDesc);
        if (texDesc.Width == 0U || texDesc.Height == 0U ||
            (texDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             texDesc.Format != DXGI_FORMAT_B8G8R8X8_UNORM)) {
            return false;
        }

        const int32_t monLeft = static_cast<int32_t>(slot.desktopRect.left);
        const int32_t monTop  = static_cast<int32_t>(slot.desktopRect.top);
        const int32_t texW    = static_cast<int32_t>(texDesc.Width);
        const int32_t texH    = static_cast<int32_t>(texDesc.Height);

        const int32_t localLeft   = std::clamp(screenLeft - monLeft, 0, texW - 1);
        const int32_t localTop    = std::clamp(screenTop - monTop, 0, texH - 1);
        const int32_t localRight  = std::clamp(localLeft + std::clamp(reqWidth, 1, kPatchSize), localLeft + 1, texW);
        const int32_t localBottom = std::clamp(localTop + std::clamp(reqHeight, 1, kPatchSize), localTop + 1, texH);
        const int32_t copyW       = localRight - localLeft;
        const int32_t copyH       = localBottom - localTop;

        D3D11_BOX srcBox{};
        srcBox.left   = static_cast<UINT>(localLeft);
        srcBox.top    = static_cast<UINT>(localTop);
        srcBox.front  = 0U;
        srcBox.right  = static_cast<UINT>(localRight);
        srcBox.bottom = static_cast<UINT>(localBottom);
        srcBox.back   = 1U;

        context_->CopySubresourceRegion(stagingTex_, 0U, 0U, 0U, 0U, slot.acquiredTex, 0U, &srcBox);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapHr = context_->Map(stagingTex_, 0U, D3D11_MAP_READ, 0U, &mapped);
        if (FAILED(mapHr) || mapped.pData == nullptr) {
            return false;
        }

        const auto* baseBytes = static_cast<const uint8_t*>(mapped.pData);
        for (int32_t y = 0; y < copyH; ++y) {
            const auto* srcRow = reinterpret_cast<const uint32_t*>(baseBytes + static_cast<size_t>(y) * mapped.RowPitch);
            uint32_t*   dstRow = outPatch.pixels + static_cast<size_t>(y * kPatchSize);
            for (int32_t x = 0; x < copyW; ++x) {
                dstRow[x] = srcRow[x] & 0x00FFFFFFU; // B8G8R8A8 little-endian -> 0x00RRGGBB
            }
        }
        context_->Unmap(stagingTex_, 0U);

        outPatch.originScreenX = monLeft + localLeft;
        outPatch.originScreenY = monTop + localTop;
        outPatch.width         = copyW;
        outPatch.height        = copyH;
        outPatch.valid         = true;
        outPatch.fromDxgi      = true;
        return true;
    }

    void ReleaseAcquiredFrame(size_t slotIdx) noexcept {
        if (slotIdx >= outputCount_) {
            return;
        }
        auto& slot = outputs_[slotIdx];
        SafeRelease(slot.acquiredTex);
        if (slot.frameAcquired && slot.dup != nullptr) {
            slot.dup->ReleaseFrame();
            slot.frameAcquired = false;
        }
    }

    [[nodiscard]] bool HasEverReceivedFrame(size_t slotIdx) const noexcept {
        return (slotIdx < outputCount_) && outputs_[slotIdx].everGotFrame;
    }

private:
    static constexpr uint64_t kRetryBackoffUs = 1'000'000ULL; // 1.0 s

    struct OutputSlot {
        IDXGIOutputDuplication* dup{nullptr};
        ID3D11Texture2D*        acquiredTex{nullptr};
        RECT                    desktopRect{};
        UINT                    outputIndex{0};
        uint64_t                dupRetryAfterUs{0};
        bool                    valid{false};
        bool                    frameAcquired{false};
        bool                    everGotFrame{false};
    };

    [[nodiscard]] size_t FindOutputForMonitor(const MonitorInfoEntry& mon) const noexcept {
        for (size_t i = 0; i < outputCount_; ++i) {
            if (!outputs_[i].valid) {
                continue;
            }
            const RECT& r = outputs_[i].desktopRect;
            if (r.left == mon.left && r.top == mon.top) {
                return i;
            }
        }
        // Fallback: match by containment of monitor center.
        const int32_t cx = mon.left + mon.width / 2;
        const int32_t cy = mon.top + mon.height / 2;
        for (size_t i = 0; i < outputCount_; ++i) {
            if (!outputs_[i].valid) {
                continue;
            }
            const RECT& r = outputs_[i].desktopRect;
            if (cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom) {
                return i;
            }
        }
        return kMaxMonitors;
    }

    [[nodiscard]] bool OpenDuplicationForSlot(OutputSlot& slot) noexcept {
        if (device_ == nullptr) {
            return false;
        }
        IDXGIDevice* dxgiDevice = nullptr;
        if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) ||
            dxgiDevice == nullptr) {
            return false;
        }

        IDXGIAdapter* adapter = nullptr;
        const HRESULT adHr = dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
        if (FAILED(adHr) || adapter == nullptr) {
            return false;
        }

        IDXGIOutput* output = nullptr;
        const HRESULT outHr = adapter->EnumOutputs(slot.outputIndex, &output);
        adapter->Release();
        if (FAILED(outHr) || output == nullptr) {
            return false;
        }

        IDXGIOutput1* output1 = nullptr;
        const HRESULT qiHr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        output->Release();
        if (FAILED(qiHr) || output1 == nullptr) {
            return false;
        }

        const HRESULT dupHr = output1->DuplicateOutput(device_, &slot.dup);
        output1->Release();
        if (FAILED(dupHr) || slot.dup == nullptr) {
            SafeRelease(slot.dup);
            return false;
        }
        slot.everGotFrame = false;
        return true;
    }

    ID3D11Device*                  device_{nullptr};
    ID3D11DeviceContext*           context_{nullptr};
    ID3D11Texture2D*               stagingTex_{nullptr};
    std::array<OutputSlot, kMaxMonitors> outputs_{};
    size_t                         outputCount_{0};
    uint64_t                       retryAfterUs_{0};
    bool                           initialized_{false};
};

} // namespace detail

class PixelTriggerEngine {
public:
    explicit PixelTriggerEngine(mask::core::EngineSharedState& sharedState) noexcept
        : sharedState_(sharedState) {
        for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
            configs_[i].slotIndex = static_cast<uint8_t>(i);
        }
        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~PixelTriggerEngine() noexcept {
        Stop();
        if (wakeEvent_ != nullptr) {
            CloseHandle(wakeEvent_);
            wakeEvent_ = nullptr;
        }
    }

    PixelTriggerEngine(const PixelTriggerEngine&) = delete;
    PixelTriggerEngine& operator=(const PixelTriggerEngine&) = delete;

    void Start() {
        AcquireSRWLockExclusive(&lifecycleLock_);
        if (workerThread_ != nullptr) {
            ReleaseSRWLockExclusive(&lifecycleLock_);
            return;
        }

        if (wakeEvent_ == nullptr) {
            wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        stopRequested_.store(false, std::memory_order_release);

        workerThread_ = CreateThread(
            nullptr,
            0,
            &PixelTriggerEngine::WorkerThreadProc,
            this,
            0,
            nullptr
        );
        ReleaseSRWLockExclusive(&lifecycleLock_);
    }

    void Stop() noexcept {
        HANDLE threadToJoin = nullptr;

        AcquireSRWLockExclusive(&lifecycleLock_);
        stopRequested_.store(true, std::memory_order_release);
        SignalWake();
        threadToJoin = workerThread_;
        workerThread_ = nullptr;
        ReleaseSRWLockExclusive(&lifecycleLock_);

        if (threadToJoin != nullptr) {
            WaitForSingleObject(threadToJoin, INFINITE);
            CloseHandle(threadToJoin);
        }
    }

    void SetPreviewActive(bool active, uint8_t previewSlotIndex) noexcept {
        const uint8_t clamped = static_cast<uint8_t>(
            previewSlotIndex < mask::core::kMaxColorTriggers ? previewSlotIndex : 0U
        );
        previewSlotIndex_.store(clamped, std::memory_order_release);
        previewActive_.store(active, std::memory_order_release);
        SignalWake();
    }

    void UpdateConfig(size_t index, const mask::core::ColorTriggerConfig& cfg) noexcept {
        if (index >= mask::core::kMaxColorTriggers) {
            return;
        }
        mask::core::ColorTriggerConfig clean = cfg;
        detail::SanitizeTriggerConfig(clean, index);

        AcquireSRWLockExclusive(&configLock_);
        configs_[index] = clean;
        configVersion_.fetch_add(1ULL, std::memory_order_release);
        ReleaseSRWLockExclusive(&configLock_);

        SignalWake();
    }

    void UpdateAllConfigs(
        const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& cfgs
    ) noexcept {
        AcquireSRWLockExclusive(&configLock_);
        for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
            configs_[i] = cfgs[i];
            detail::SanitizeTriggerConfig(configs_[i], i);
        }
        configVersion_.fetch_add(1ULL, std::memory_order_release);
        ReleaseSRWLockExclusive(&configLock_);

        SignalWake();
    }

    [[nodiscard]] ColorTriggerPreviewSnapshot GetPreviewSnapshot() const noexcept {
        ColorTriggerPreviewSnapshot local{};
        uint64_t seq0 = 0;
        uint64_t seq1 = 0;
        do {
            seq0 = previewSeq_.load(std::memory_order_acquire);
            if ((seq0 & 1ULL) != 0ULL) {
                _mm_pause();
                continue;
            }
            std::atomic_signal_fence(std::memory_order_acq_rel);
            local = previewSnapshot_;
            std::atomic_signal_fence(std::memory_order_acq_rel);
            seq1 = previewSeq_.load(std::memory_order_acquire);
        } while (seq0 != seq1);
        return local;
    }

    void ResetArmedStates() noexcept {
        resetRequested_.store(true, std::memory_order_release);
        SignalWake();
    }

    [[nodiscard]] static int EnumerateMonitors(std::array<MonitorInfoEntry, 8>& outMonitors) noexcept {
        for (auto& m : outMonitors) {
            m = MonitorInfoEntry{};
        }

        detail::MonitorEnumContext ctx{};
        ctx.out   = &outMonitors;
        ctx.count = 0;
        EnumDisplayMonitors(nullptr, nullptr, &detail::EnumMonitorsProc, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.count <= 0) {
            MonitorInfoEntry fallback{};
            const int sx = GetSystemMetrics(SM_CXSCREEN);
            const int sy = GetSystemMetrics(SM_CYSCREEN);
            fallback.left      = 0;
            fallback.top       = 0;
            fallback.width     = (sx > 0) ? sx : 1920;
            fallback.height    = (sy > 0) ? sy : 1080;
            fallback.dpi       = detail::QueryMonitorEffectiveDpi(nullptr);
            fallback.isPrimary = true;
            wcscpy_s(fallback.name, L"Monitor 1 (Primary)");
            outMonitors[0] = fallback;
            return 1;
        }

        // Keep the primary monitor at index 0 for predictable default monitorIndex == 0.
        for (int i = 1; i < ctx.count; ++i) {
            if (outMonitors[static_cast<size_t>(i)].isPrimary) {
                std::swap(outMonitors[0], outMonitors[static_cast<size_t>(i)]);
                break;
            }
        }

        for (int i = 0; i < ctx.count; ++i) {
            auto& entry = outMonitors[static_cast<size_t>(i)];
            if (entry.isPrimary) {
                std::swprintf(entry.name, std::size(entry.name), L"Monitor %d (Primary)", i + 1);
            } else {
                std::swprintf(entry.name, std::size(entry.name), L"Monitor %d (%dx%d)",
                              i + 1, entry.width, entry.height);
            }
        }

        return ctx.count;
    }

    [[nodiscard]] static bool SamplePixelAt(
        int32_t screenX,
        int32_t screenY,
        uint8_t& outR,
        uint8_t& outG,
        uint8_t& outB
    ) noexcept {
        const HDC screenDc = GetDC(nullptr);
        if (screenDc == nullptr) {
            return false;
        }

        const HDC memDc = CreateCompatibleDC(screenDc);
        if (memDc == nullptr) {
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = 1;
        bmi.bmiHeader.biHeight      = -1;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* rawBits = nullptr;
        const HBITMAP dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &rawBits, nullptr, 0);
        if (dib == nullptr || rawBits == nullptr) {
            if (dib != nullptr) {
                DeleteObject(dib);
            }
            DeleteDC(memDc);
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        const HGDIOBJ oldBmp = SelectObject(memDc, dib);
        const BOOL ok = BitBlt(memDc, 0, 0, 1, 1, screenDc, screenX, screenY, SRCCOPY);
        if (ok != FALSE) {
            const uint32_t bgra = *static_cast<const uint32_t*>(rawBits);
            outB = static_cast<uint8_t>(bgra & 0xFFU);
            outG = static_cast<uint8_t>((bgra >> 8) & 0xFFU);
            outR = static_cast<uint8_t>((bgra >> 16) & 0xFFU);
        }

        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return ok != FALSE;
    }

    [[nodiscard]] static bool SaveTriggers(
        const wchar_t* path,
        const std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& cfgs
    ) noexcept {
        detail::TriggersFileImage image{};
        image.magic    = kTriggersFileMagic;
        image.version  = kTriggersFileVersion;
        image.count    = static_cast<uint32_t>(mask::core::kMaxColorTriggers);
        image.reserved = 0U;
        for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
            image.triggers[i] = cfgs[i];
            detail::SanitizeTriggerConfig(image.triggers[i], i);
        }
        return detail::WriteFileAtomic(path, &image, sizeof(detail::TriggersFileImage));
    }

    [[nodiscard]] static bool LoadTriggers(
        const wchar_t* path,
        std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers>& outCfgs
    ) noexcept {
        if (path == nullptr || path[0] == L'\0') {
            return false;
        }

        detail::TriggersFileImage loaded{};
        bool ok = detail::ReadTriggersFileRaw(path, loaded);
        if (!ok) {
            wchar_t bak[1040]{};
            if (std::swprintf(bak, std::size(bak), L"%ls.bak", path) >= 0) {
                ok = detail::ReadTriggersFileRaw(bak, loaded);
            }
        }
        if (!ok) {
            return false;
        }

        for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
            detail::SanitizeTriggerConfig(loaded.triggers[i], i);
            outCfgs[i] = loaded.triggers[i];
        }
        return true;
    }

private:
    struct TriggerRuntime {
        bool     toggleArmed{false};
        bool     prevArmKeyDown{false};
        bool     wasArmed{false};
        bool     hasBaseline{false};
        uint8_t  baseR{0};
        uint8_t  baseG{0};
        uint8_t  baseB{0};

        bool     delayPending{false};
        uint64_t delayFireAtUs{0};
        uint64_t cooldownUntilUs{0};
        bool     firedWhileMatched{false};

        bool     isHoldingKey{false};
        uint16_t heldVk{0};

        uint64_t triggersFired{0};
        detail::PixelPatch cachedPatch{};
    };

    void SignalWake() noexcept {
        if (wakeEvent_ != nullptr) {
            SetEvent(wakeEvent_);
        }
    }

    void PublishPreview(const ColorTriggerPreviewSnapshot& snap) noexcept {
        const uint64_t seq = previewSeq_.load(std::memory_order_relaxed);
        previewSeq_.store(seq + 1ULL, std::memory_order_release);
        std::atomic_signal_fence(std::memory_order_acq_rel);
        previewSnapshot_ = snap;
        std::atomic_signal_fence(std::memory_order_acq_rel);
        previewSeq_.store(seq + 2ULL, std::memory_order_release);
    }

    void PushAuxCommand(const mask::core::AuxOutputCommand& cmd) noexcept {
        if (sharedState_.auxOutputQueue.try_push(cmd)) {
            mask::input::WakeEngine();
        }
    }

    void ReleaseHeldKey(TriggerRuntime& rt) noexcept {
        if (!rt.isHoldingKey) {
            return;
        }
        if (rt.heldVk != 0U) {
            mask::core::AuxOutputCommand cmd{};
            unsigned int aux = 0;
            cmd.timestampTsc = __rdtscp(&aux);
            cmd.kind         = static_cast<uint8_t>(mask::core::AuxActionKind::KeyUp);
            cmd.vk           = rt.heldVk;
            PushAuxCommand(cmd);
        }
        rt.isHoldingKey = false;
        rt.heldVk       = 0U;
    }

    void ReleaseAllHeldKeys(std::array<TriggerRuntime, mask::core::kMaxColorTriggers>& runtime) noexcept {
        for (auto& rt : runtime) {
            ReleaseHeldKey(rt);
            rt.delayPending      = false;
            rt.firedWhileMatched = false;
        }
    }

    void DispatchTriggerAction(const mask::core::ColorTriggerConfig& cfg, TriggerRuntime& rt) noexcept {
        mask::core::AuxOutputCommand cmd{};
        unsigned int aux = 0;
        cmd.timestampTsc = __rdtscp(&aux);

        const auto kind = static_cast<mask::core::ColorTriggerActionKind>(cfg.actionKind);
        switch (kind) {
            case mask::core::ColorTriggerActionKind::ClickKey: {
                const uint16_t vk = static_cast<uint16_t>(cfg.actionVk & 0xFFU);
                if (vk == 0U) {
                    return;
                }
                cmd.kind   = static_cast<uint8_t>(mask::core::AuxActionKind::KeyClick);
                cmd.vk     = vk;
                cmd.holdMs = (cfg.holdMs > 0U) ? cfg.holdMs : 15U;
                PushAuxCommand(cmd);
                ++rt.triggersFired;
                break;
            }
            case mask::core::ColorTriggerActionKind::HoldKey: {
                const uint16_t vk = static_cast<uint16_t>(cfg.actionVk & 0xFFU);
                if (vk == 0U || rt.isHoldingKey) {
                    return;
                }
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown);
                cmd.vk   = vk;
                PushAuxCommand(cmd);
                rt.isHoldingKey = true;
                rt.heldVk       = vk;
                ++rt.triggersFired;
                break;
            }
            case mask::core::ColorTriggerActionKind::RunMacroSlot: {
                cmd.kind     = static_cast<uint8_t>(mask::core::AuxActionKind::TriggerMacroSlot);
                cmd.auxIndex = static_cast<uint8_t>(cfg.linkedIndex % mask::core::kMaxMacroSlots);
                PushAuxCommand(cmd);
                ++rt.triggersFired;
                break;
            }
            case mask::core::ColorTriggerActionKind::RunComplexSeq: {
                cmd.kind     = static_cast<uint8_t>(mask::core::AuxActionKind::TriggerComplexSeq);
                cmd.auxIndex = static_cast<uint8_t>(cfg.linkedIndex % mask::core::kMaxComplexSequences);
                PushAuxCommand(cmd);
                ++rt.triggersFired;
                break;
            }
            default:
                break;
        }
    }

    static void ResolveSampleCoords(
        const mask::core::ColorTriggerConfig& cfg,
        const MonitorInfoEntry& mon,
        int32_t& outScreenX,
        int32_t& outScreenY
    ) noexcept {
        if (cfg.sampleX < 0 || cfg.sampleY < 0) {
            outScreenX = mon.left + mon.width / 2;
            outScreenY = mon.top + mon.height / 2;
            return;
        }

        // Primary convention: monitor-local [0, width-1] x [0, height-1].
        // Also tolerate virtual-screen coordinates on non-origin monitors if they exceed local bounds
        // while landing squarely inside the selected monitor's desktop rectangle.
        if ((cfg.sampleX >= mon.width || cfg.sampleY >= mon.height) &&
            cfg.sampleX >= mon.left && cfg.sampleX < (mon.left + mon.width) &&
            cfg.sampleY >= mon.top && cfg.sampleY < (mon.top + mon.height)) {
            outScreenX = cfg.sampleX;
            outScreenY = cfg.sampleY;
            return;
        }

        const int32_t localX = std::clamp(cfg.sampleX, 0, std::max<int32_t>(0, mon.width - 1));
        const int32_t localY = std::clamp(cfg.sampleY, 0, std::max<int32_t>(0, mon.height - 1));
        outScreenX = mon.left + localX;
        outScreenY = mon.top + localY;
    }

    static void ComputeAverageRgbFromPatch(
        const detail::PixelPatch& patch,
        int32_t centerScreenX,
        int32_t centerScreenY,
        uint8_t sampleRadius,
        uint8_t& outR,
        uint8_t& outG,
        uint8_t& outB
    ) noexcept {
        const int32_t r = static_cast<int32_t>(std::min<uint8_t>(sampleRadius, 8U));
        uint32_t sumR = 0U;
        uint32_t sumG = 0U;
        uint32_t sumB = 0U;
        uint32_t count = 0U;

        for (int32_t dy = -r; dy <= r; ++dy) {
            for (int32_t dx = -r; dx <= r; ++dx) {
                const uint32_t rgb = patch.SampleClamped(centerScreenX + dx, centerScreenY + dy);
                sumR += (rgb >> 16) & 0xFFU;
                sumG += (rgb >> 8) & 0xFFU;
                sumB += rgb & 0xFFU;
                ++count;
            }
        }

        if (count == 0U) {
            outR = outG = outB = 0U;
            return;
        }
        outR = static_cast<uint8_t>((sumR + count / 2U) / count);
        outG = static_cast<uint8_t>((sumG + count / 2U) / count);
        outB = static_cast<uint8_t>((sumB + count / 2U) / count);
    }

    static void FillLoupeFromPatch(
        const detail::PixelPatch& patch,
        int32_t centerScreenX,
        int32_t centerScreenY,
        uint32_t outLoupe[ColorTriggerPreviewSnapshot::kLoupeSize * ColorTriggerPreviewSnapshot::kLoupeSize]
    ) noexcept {
        constexpr int32_t kHalf = ColorTriggerPreviewSnapshot::kLoupeSize / 2; // 7
        for (int32_t row = 0; row < ColorTriggerPreviewSnapshot::kLoupeSize; ++row) {
            const int32_t sy = centerScreenY + (row - kHalf);
            for (int32_t col = 0; col < ColorTriggerPreviewSnapshot::kLoupeSize; ++col) {
                const int32_t sx = centerScreenX + (col - kHalf);
                outLoupe[static_cast<size_t>(row * ColorTriggerPreviewSnapshot::kLoupeSize + col)] =
                    patch.SampleClamped(sx, sy);
            }
        }
    }

    static DWORD WINAPI WorkerThreadProc(LPVOID param) noexcept {
        auto* self = static_cast<PixelTriggerEngine*>(param);
        if (self != nullptr) {
            self->WorkerLoop();
        }
        return 0;
    }

    void WaitForNextTick(HANDLE waitTimer, uint32_t intervalUs) noexcept {
        if (stopRequested_.load(std::memory_order_acquire)) {
            return;
        }
        if (waitTimer != nullptr) {
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -static_cast<LONGLONG>(intervalUs) * 10LL; // relative 100ns units
            if (SetWaitableTimer(waitTimer, &dueTime, 0, nullptr, nullptr, FALSE) != FALSE) {
                HANDLE handles[2] = { wakeEvent_, waitTimer };
                WaitForMultipleObjects(2U, handles, FALSE, INFINITE);
                return;
            }
        }
        const DWORD ms = std::max<DWORD>(1U, static_cast<DWORD>((intervalUs + 999U) / 1000U));
        WaitForSingleObject(wakeEvent_, ms);
    }

    void WorkerLoop() noexcept {
        HANDLE waitTimer = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS
        );
        if (waitTimer == nullptr) {
            waitTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }

        detail::DxgiDesktopCapture dxgi{};
        detail::GdiRoiCapture      gdi{};

        std::array<MonitorInfoEntry, kMaxMonitors> monitors{};
        int monitorCount = EnumerateMonitors(monitors);
        uint64_t lastMonitorRefreshUs = detail::QueryMonotonicUs();

        std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers> localConfigs{};
        std::array<TriggerRuntime, mask::core::kMaxColorTriggers> runtime{};
        uint64_t observedConfigVersion = ~0ULL;

        while (!stopRequested_.load(std::memory_order_acquire)) {
            // 1. Sync latest configs only when changed.
            const uint64_t currentVersion = configVersion_.load(std::memory_order_acquire);
            if (currentVersion != observedConfigVersion) {
                const auto prevConfigs = localConfigs;
                AcquireSRWLockShared(&configLock_);
                localConfigs = configs_;
                observedConfigVersion = configVersion_.load(std::memory_order_relaxed);
                ReleaseSRWLockShared(&configLock_);

                // Release any held key if its slot was disabled or changed action/vk, and
                // invalidate baseline/patch cache when sampling geometry or mode changes.
                for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
                    const auto& oldC = prevConfigs[i];
                    const auto& c    = localConfigs[i];
                    auto& rt         = runtime[i];
                    if (rt.isHoldingKey) {
                        const bool stillHoldSameKey =
                            c.inUse && c.enabled &&
                            c.actionKind == static_cast<uint8_t>(mask::core::ColorTriggerActionKind::HoldKey) &&
                            static_cast<uint16_t>(c.actionVk & 0xFFU) == rt.heldVk;
                        if (!stillHoldSameKey) {
                            ReleaseHeldKey(rt);
                        }
                    }
                    const bool sampleChanged =
                        oldC.mode != c.mode ||
                        oldC.monitorIndex != c.monitorIndex ||
                        oldC.sampleX != c.sampleX ||
                        oldC.sampleY != c.sampleY ||
                        oldC.sampleRadius != c.sampleRadius;
                    if (sampleChanged) {
                        rt.hasBaseline       = false;
                        rt.cachedPatch.valid = false;
                    }
                    if (!c.inUse || !c.enabled) {
                        rt.delayPending      = false;
                        rt.firedWhileMatched = false;
                        rt.hasBaseline       = false;
                        rt.wasArmed          = false;
                    }
                    if (!c.inUse) {
                        rt.triggersFired = 0ULL;
                        rt.toggleArmed   = false;
                    }
                }
            }

            // 2. Honor explicit reset requests (e.g. SafeStop / Pause / Clear Armed).
            if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
                ReleaseAllHeldKeys(runtime);
                for (auto& rt : runtime) {
                    rt.toggleArmed    = false;
                    rt.prevArmKeyDown = false;
                    rt.wasArmed       = false;
                    rt.hasBaseline    = false;
                }
            }

            const bool previewOn   = previewActive_.load(std::memory_order_acquire);
            const uint8_t previewSlot = previewSlotIndex_.load(std::memory_order_acquire);
            const bool paused      = sharedState_.automationPaused.load(std::memory_order_acquire) ||
                                     !sharedState_.engineRunning.load(std::memory_order_acquire);
            const bool suppressed  = sharedState_.suppressTriggers.load(std::memory_order_acquire);

            if (paused) {
                ReleaseAllHeldKeys(runtime);
            }

            bool anyEnabled = false;
            for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
                if (localConfigs[i].inUse && localConfigs[i].enabled) {
                    anyEnabled = true;
                    break;
                }
            }

            // 3. 0.00% CPU idle sleep whenever no active work is required.
            if ((!anyEnabled && !previewOn) || (paused && !previewOn)) {
                // Release desktop duplication while completely idle so DWM pays zero overhead.
                dxgi.Shutdown();
                for (auto& rt : runtime) {
                    rt.cachedPatch.valid = false;
                }
                WaitForSingleObject(wakeEvent_, INFINITE);
                lastMonitorRefreshUs = 0ULL; // Refresh monitors on wake
                continue;
            }

            const uint64_t nowUs = detail::QueryMonotonicUs();
            if (lastMonitorRefreshUs == 0ULL || (nowUs - lastMonitorRefreshUs) >= 2'000'000ULL) {
                monitorCount = EnumerateMonitors(monitors);
                lastMonitorRefreshUs = nowUs;
            }

            // 4. Evaluate arming state for all slots and identify which slots/monitors need pixel capture.
            std::array<bool, mask::core::kMaxColorTriggers> slotArmed{};
            std::array<bool, mask::core::kMaxColorTriggers> slotNeedsCapture{};
            std::array<int32_t, mask::core::kMaxColorTriggers> slotScreenX{};
            std::array<int32_t, mask::core::kMaxColorTriggers> slotScreenY{};
            std::array<uint8_t, mask::core::kMaxColorTriggers> slotMonIdx{};
            std::array<bool, kMaxMonitors> monitorNeeded{};

            bool anyArmed = false;
            for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
                const auto& cfg = localConfigs[i];
                auto& rt = runtime[i];

                const uint8_t mIdx = (cfg.monitorIndex < static_cast<uint8_t>(monitorCount))
                    ? cfg.monitorIndex : 0U;
                slotMonIdx[i] = mIdx;
                ResolveSampleCoords(cfg, monitors[mIdx], slotScreenX[i], slotScreenY[i]);

                bool armed = false;
                if (cfg.inUse && cfg.enabled && !paused) {
                    if (cfg.armMode == 0U) {
                        armed = true;
                    } else if (cfg.armMode == 1U) {
                        const uint8_t armVk = static_cast<uint8_t>(cfg.armVk & 0xFFU);
                        armed = (armVk != 0U) &&
                                (sharedState_.physicalKeyState[armVk].load(std::memory_order_acquire) != 0U);
                    } else if (cfg.armMode == 2U) {
                        const uint8_t armVk = static_cast<uint8_t>(cfg.armVk & 0xFFU);
                        const bool armDown = (armVk != 0U) &&
                                             (sharedState_.physicalKeyState[armVk].load(std::memory_order_acquire) != 0U);
                        if (armDown && !rt.prevArmKeyDown && !suppressed) {
                            rt.toggleArmed = !rt.toggleArmed;
                        }
                        rt.prevArmKeyDown = armDown;
                        armed = (armVk != 0U) && rt.toggleArmed;
                    }
                } else {
                    rt.prevArmKeyDown = false;
                }

                slotArmed[i] = armed;
                if (armed) {
                    anyArmed = true;
                } else {
                    // Immediately release any held key and reset delay/baseline when disarmed.
                    ReleaseHeldKey(rt);
                    rt.delayPending      = false;
                    rt.firedWhileMatched = false;
                    rt.hasBaseline       = false;
                    rt.wasArmed          = false;
                }

                const bool isPreviewTarget = previewOn && (i == static_cast<size_t>(previewSlot));
                if (armed || isPreviewTarget) {
                    slotNeedsCapture[i] = true;
                    monitorNeeded[mIdx] = true;
                }
            }

            // 5. Acquire at most one DXGI frame per needed monitor, extract 32x32 ROIs, then release frames.
            for (int m = 0; m < monitorCount; ++m) {
                const size_t mu = static_cast<size_t>(m);
                if (!monitorNeeded[mu]) {
                    continue;
                }

                const auto& mon = monitors[mu];
                size_t dxgiSlotIdx = kMaxMonitors;
                const auto status = dxgi.AcquireMonitorFrame(mon, nowUs, dxgiSlotIdx);
                if (status == detail::DxgiDesktopCapture::AcquireStatus::AccessLost) {
                    lastMonitorRefreshUs = 0ULL;
                }

                for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
                    if (!slotNeedsCapture[i] || slotMonIdx[i] != static_cast<uint8_t>(m)) {
                        continue;
                    }

                    auto& rt = runtime[i];
                    const int32_t sx = slotScreenX[i];
                    const int32_t sy = slotScreenY[i];

                    const int32_t patchLeft = std::clamp(
                        sx - detail::kPatchHalf,
                        mon.left,
                        (std::max)(mon.left, mon.left + mon.width - 1)
                    );
                    const int32_t patchTop = std::clamp(
                        sy - detail::kPatchHalf,
                        mon.top,
                        (std::max)(mon.top, mon.top + mon.height - 1)
                    );
                    const int32_t patchRight  = (std::min)(mon.left + mon.width, patchLeft + detail::kPatchSize);
                    const int32_t patchBottom = (std::min)(mon.top + mon.height, patchTop + detail::kPatchSize);
                    const int32_t patchW      = (std::max)(int32_t{1}, patchRight - patchLeft);
                    const int32_t patchH      = (std::max)(int32_t{1}, patchBottom - patchTop);

                    bool captured = false;

                    if (status == detail::DxgiDesktopCapture::AcquireStatus::FrameReady) {
                        captured = dxgi.CopyRoiFromAcquiredFrame(
                            dxgiSlotIdx, patchLeft, patchTop, patchW, patchH, rt.cachedPatch
                        );
                        if (captured) {
                            rt.cachedPatch.monitorIndex = static_cast<uint8_t>(m);
                        }
                    } else if (status == detail::DxgiDesktopCapture::AcquireStatus::WaitTimeout) {
                        // Desktop unchanged since last DXGI frame: reuse cached ROI if coordinates still match.
                        const bool cacheMatches =
                            rt.cachedPatch.valid &&
                            rt.cachedPatch.monitorIndex == static_cast<uint8_t>(m) &&
                            rt.cachedPatch.originScreenX == patchLeft &&
                            rt.cachedPatch.originScreenY == patchTop &&
                            rt.cachedPatch.width == patchW &&
                            rt.cachedPatch.height == patchH;
                        if (cacheMatches) {
                            captured = true;
                            if (dxgi.HasEverReceivedFrame(dxgiSlotIdx)) {
                                rt.cachedPatch.fromDxgi = true;
                            }
                        }
                    }

                    // Fallback to persistent 32x32 GDI BitBlt ROI when DXGI is unavailable, lost, or
                    // when coordinates moved on a completely static desktop before DXGI emits a new frame.
                    if (!captured) {
                        if (gdi.CaptureRect(patchLeft, patchTop, patchW, patchH, rt.cachedPatch.pixels)) {
                            rt.cachedPatch.originScreenX = patchLeft;
                            rt.cachedPatch.originScreenY = patchTop;
                            rt.cachedPatch.width         = patchW;
                            rt.cachedPatch.height        = patchH;
                            rt.cachedPatch.monitorIndex  = static_cast<uint8_t>(m);
                            rt.cachedPatch.valid         = true;
                            rt.cachedPatch.fromDxgi      =
                                (status == detail::DxgiDesktopCapture::AcquireStatus::WaitTimeout) &&
                                dxgi.HasEverReceivedFrame(dxgiSlotIdx);
                        } else {
                            rt.cachedPatch.valid = false;
                        }
                    }
                }

                if (status == detail::DxgiDesktopCapture::AcquireStatus::FrameReady) {
                    dxgi.ReleaseAcquiredFrame(dxgiSlotIdx);
                }
            }

            // 6. Run color comparison, trigger state machine, and publish preview snapshot.
            for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
                if (!slotNeedsCapture[i]) {
                    continue;
                }

                const auto& cfg = localConfigs[i];
                auto& rt = runtime[i];
                const bool armed = slotArmed[i];
                const bool isPreviewTarget = previewOn && (i == static_cast<size_t>(previewSlot));

                uint8_t sampledR = 0U;
                uint8_t sampledG = 0U;
                uint8_t sampledB = 0U;
                if (rt.cachedPatch.valid) {
                    ComputeAverageRgbFromPatch(
                        rt.cachedPatch,
                        slotScreenX[i],
                        slotScreenY[i],
                        cfg.sampleRadius,
                        sampledR,
                        sampledG,
                        sampledB
                    );
                }

                uint32_t delta = 0U;
                bool matched = false;
                const auto colorMode = static_cast<mask::core::ColorTriggerMode>(cfg.mode);

                if (colorMode == mask::core::ColorTriggerMode::MatchColor) {
                    delta   = detail::ColorDistanceEuclidean(sampledR, sampledG, sampledB, cfg.r, cfg.g, cfg.b);
                    matched = rt.cachedPatch.valid && (delta <= static_cast<uint32_t>(cfg.tolerance));
                } else {
                    if (armed && rt.cachedPatch.valid) {
                        if (!rt.wasArmed || !rt.hasBaseline) {
                            rt.baseR       = sampledR;
                            rt.baseG       = sampledG;
                            rt.baseB       = sampledB;
                            rt.hasBaseline = true;
                            delta          = 0U;
                            matched        = false;
                        } else {
                            delta   = detail::ColorDistanceEuclidean(
                                sampledR, sampledG, sampledB, rt.baseR, rt.baseG, rt.baseB
                            );
                            matched = (delta > static_cast<uint32_t>(cfg.tolerance));
                        }
                    } else {
                        delta   = 0U;
                        matched = false;
                    }
                }

                if (armed && !suppressed && rt.cachedPatch.valid) {
                    rt.wasArmed = true;

                    const uint64_t delayUs    = static_cast<uint64_t>(cfg.delayMs) * 1000ULL;
                    const uint64_t cooldownUs = static_cast<uint64_t>(cfg.cooldownMs) * 1000ULL;
                    const bool isHoldAction   =
                        (cfg.actionKind == static_cast<uint8_t>(mask::core::ColorTriggerActionKind::HoldKey));

                    if (isHoldAction) {
                        if (matched) {
                            if (!rt.isHoldingKey && nowUs >= rt.cooldownUntilUs) {
                                if (delayUs == 0ULL) {
                                    DispatchTriggerAction(cfg, rt);
                                } else if (!rt.delayPending) {
                                    rt.delayPending  = true;
                                    rt.delayFireAtUs = nowUs + delayUs;
                                } else if (nowUs >= rt.delayFireAtUs) {
                                    rt.delayPending = false;
                                    DispatchTriggerAction(cfg, rt);
                                }
                            }
                        } else {
                            rt.delayPending = false;
                            if (rt.isHoldingKey) {
                                ReleaseHeldKey(rt);
                                rt.cooldownUntilUs = nowUs + cooldownUs;
                            }
                        }
                    } else {
                        if (matched) {
                            const bool canRefire = (cooldownUs > 0ULL)
                                ? (nowUs >= rt.cooldownUntilUs)
                                : !rt.firedWhileMatched;
                            if (canRefire) {
                                if (delayUs == 0ULL) {
                                    DispatchTriggerAction(cfg, rt);
                                    rt.cooldownUntilUs   = nowUs + cooldownUs;
                                    rt.firedWhileMatched = true;
                                } else if (!rt.delayPending) {
                                    rt.delayPending  = true;
                                    rt.delayFireAtUs = nowUs + delayUs;
                                } else if (nowUs >= rt.delayFireAtUs) {
                                    rt.delayPending = false;
                                    DispatchTriggerAction(cfg, rt);
                                    rt.cooldownUntilUs   = nowUs + cooldownUs;
                                    rt.firedWhileMatched = true;
                                }
                            }
                        } else {
                            rt.delayPending      = false;
                            rt.firedWhileMatched = false;
                        }
                    }
                } else if (armed && suppressed) {
                    // While typing in a UI text field, do not fire new triggers and release any held key.
                    rt.wasArmed = true;
                    rt.delayPending = false;
                    ReleaseHeldKey(rt);
                }

                if (isPreviewTarget) {
                    ColorTriggerPreviewSnapshot snap{};
                    if (rt.cachedPatch.valid) {
                        FillLoupeFromPatch(rt.cachedPatch, slotScreenX[i], slotScreenY[i], snap.loupePixels);
                    }
                    snap.sampledR      = sampledR;
                    snap.sampledG      = sampledG;
                    snap.sampledB      = sampledB;
                    snap.colorDelta    = delta;
                    snap.sampleX       = slotScreenX[i];
                    snap.sampleY       = slotScreenY[i];
                    snap.isMatched     = matched;
                    snap.isArmed       = armed;
                    snap.usingDxgi     = rt.cachedPatch.valid && rt.cachedPatch.fromDxgi;
                    snap.triggersFired = rt.triggersFired;
                    PublishPreview(snap);
                }
            }

            // 7. Adaptive low-overhead pacing.
            uint32_t waitUs = 16'000U; // 60 Hz preview-only default
            if (anyArmed && !paused) {
                const uint32_t cfgUs = sharedState_.armedPollIntervalUs.load(std::memory_order_relaxed);
                waitUs = std::clamp(cfgUs, 1'000U, 16'000U);
            } else if (anyEnabled && !paused && !previewOn) {
                // Waiting for WhileArmHeld / ToggleArmed armVk without touching GPU/GDI: 250 Hz key check.
                waitUs = 4'000U;
            }
            WaitForNextTick(waitTimer, waitUs);
        }

        // Clean shutdown: release any held keys and all DXGI/GDI/timer resources.
        ReleaseAllHeldKeys(runtime);
        dxgi.Shutdown();
        gdi.Shutdown();
        if (waitTimer != nullptr) {
            CloseHandle(waitTimer);
        }
    }

    mask::core::EngineSharedState& sharedState_;

    SRWLOCK lifecycleLock_ = SRWLOCK_INIT;
    HANDLE  workerThread_{nullptr};
    HANDLE  wakeEvent_{nullptr};

    mutable SRWLOCK configLock_ = SRWLOCK_INIT;
    std::array<mask::core::ColorTriggerConfig, mask::core::kMaxColorTriggers> configs_{};
    std::atomic<uint64_t> configVersion_{0};

    std::atomic<bool>    stopRequested_{false};
    std::atomic<bool>    resetRequested_{false};
    std::atomic<bool>    previewActive_{false};
    std::atomic<uint8_t> previewSlotIndex_{0};

    std::atomic<uint64_t>       previewSeq_{0};
    ColorTriggerPreviewSnapshot previewSnapshot_{};
};

} // namespace mask::triggers
