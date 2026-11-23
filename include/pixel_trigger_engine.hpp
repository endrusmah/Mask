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
#include <avrt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <intrin.h>

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
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
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
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
    uint8_t  loupeMatchMask[kLoupeSize * kLoupeSize]{}; // 0=outside zone, 1=inside zone, 3=inside zone + matched
    uint8_t  sampledR{0}, sampledG{0}, sampledB{0};
    uint32_t colorDelta{0};
    int32_t  sampleX{0}, sampleY{0};
    uint32_t lastPipelineUs{0};
    uint32_t avgPipelineUs{0};
    bool     isMatched{false};
    bool     isArmed{false};
    bool     usingDxgi{false};
    uint64_t triggersFired{0};
};

static_assert(std::is_trivially_copyable_v<ColorTriggerPreviewSnapshot>,
              "ColorTriggerPreviewSnapshot must remain trivially copyable for seqlock transfer.");

namespace detail {

inline constexpr int32_t kPatchSize = 32;
inline constexpr int32_t kMaxRoiRadius = 7; // max 15x15 ROI (matching kLoupeSize)
inline constexpr uint64_t kMinColorChangedReanchorUs = 50'000ULL;

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
    entry.width     = (std::max<int32_t>)(1, static_cast<int32_t>(mi.rcMonitor.right - mi.rcMonitor.left));
    entry.height    = (std::max<int32_t>)(1, static_cast<int32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top));
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
    if (c.sampleRadius > static_cast<uint8_t>(kMaxRoiRadius)) {
        c.sampleRadius = static_cast<uint8_t>(kMaxRoiRadius);
    }

    c.reserved[0] = (c.reserved[0] == static_cast<uint8_t>(mask::core::ColorTriggerMatchRule::ZoneAverage))
        ? static_cast<uint8_t>(mask::core::ColorTriggerMatchRule::ZoneAverage)
        : static_cast<uint8_t>(mask::core::ColorTriggerMatchRule::AnyPixel);
    c.reserved[1] = 0U;
    c.reserved[2] = 0U;

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
 * @brief Pre-allocated 32x32 CPU pixel patch (0x00RRGGBB) with origin metadata.
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

        {
            IDXGIDevice1* dxgiDev1 = nullptr;
            if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDev1))) &&
                dxgiDev1 != nullptr) {
                dxgiDev1->SetMaximumFrameLatency(1U);
                dxgiDev1->Release();
            }
        }

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

    [[nodiscard]] AcquireStatus AcquireMonitorFrame(
        const MonitorInfoEntry& mon,
        uint64_t nowUs,
        uint32_t timeoutMs,
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
        const HRESULT hr = slot.dup->AcquireNextFrame(timeoutMs, &frameInfo, &desktopRes);

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

        // Before the first real desktop present, DXGI may return a pointer-only update
        // whose surface is blank. Treat as timeout so GDI supplies real pixels until DWM presents.
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

    ID3D11Device*                       device_{nullptr};
    ID3D11DeviceContext*                context_{nullptr};
    ID3D11Texture2D*                    stagingTex_{nullptr};
    std::array<OutputSlot, kMaxMonitors> outputs_{};
    size_t                              outputCount_{0};
    uint64_t                            retryAfterUs_{0};
    bool                                initialized_{false};
};

/**
 * @brief Exact integer ROI pixel analyzer supporting MatchColor and ColorChanged
 *        under AnyPixel or ZoneAverage matching using Chebyshev per-channel max delta.
 */
class VisionAnalyzer {
public:
    static constexpr int32_t kMaxSpan   = ColorTriggerPreviewSnapshot::kLoupeSize; // 15
    static constexpr size_t  kGridCount = static_cast<size_t>(kMaxSpan * kMaxSpan);

    struct VisionState {
        std::array<uint32_t, kGridCount> baselinePixels{};
        uint8_t baseAvgR{0};
        uint8_t baseAvgG{0};
        uint8_t baseAvgB{0};
        int32_t baseCenterX{0};
        int32_t baseCenterY{0};
        uint8_t baseRad{0};
        bool    hasBaseline{false};

        void Reset() noexcept {
            hasBaseline = false;
        }
    };

    struct VisionResult {
        bool     matched{false};
        uint8_t  sampledR{0};
        uint8_t  sampledG{0};
        uint8_t  sampledB{0};
        uint32_t delta{0};
    };

    [[nodiscard]] static constexpr uint32_t ChebyshevDelta(
        uint8_t r1, uint8_t g1, uint8_t b1,
        uint8_t r2, uint8_t g2, uint8_t b2
    ) noexcept {
        const int32_t dr = static_cast<int32_t>(r1) - static_cast<int32_t>(r2);
        const int32_t dg = static_cast<int32_t>(g1) - static_cast<int32_t>(g2);
        const int32_t db = static_cast<int32_t>(b1) - static_cast<int32_t>(b2);
        const uint32_t ar = static_cast<uint32_t>(dr < 0 ? -dr : dr);
        const uint32_t ag = static_cast<uint32_t>(dg < 0 ? -dg : dg);
        const uint32_t ab = static_cast<uint32_t>(db < 0 ? -db : db);
        const uint32_t mrg = (ar > ag) ? ar : ag;
        return (mrg > ab) ? mrg : ab;
    }

    static VisionResult AnalyzeRoi(
        const mask::core::ColorTriggerConfig& cfg,
        const PixelPatch& patch,
        int32_t centerScreenX,
        int32_t centerScreenY,
        bool armed,
        VisionState& vs,
        uint8_t* outLoupeMask15x15 = nullptr
    ) noexcept {
        VisionResult res{};
        if (outLoupeMask15x15 != nullptr) {
            std::memset(outLoupeMask15x15, 0, kGridCount);
        }
        if (!patch.valid) {
            vs.hasBaseline = false;
            return res;
        }

        const int32_t rad = static_cast<int32_t>(std::min<uint8_t>(cfg.sampleRadius, static_cast<uint8_t>(kMaxRoiRadius)));
        const auto mode   = static_cast<mask::core::ColorTriggerMode>(cfg.mode);
        const auto rule   = mask::core::GetTriggerMatchRule(cfg);
        const uint32_t tol = static_cast<uint32_t>(cfg.tolerance);

        const bool isColorChanged = (mode == mask::core::ColorTriggerMode::ColorChanged);
        if (!isColorChanged || !armed) {
            vs.hasBaseline = false;
        } else if (vs.hasBaseline &&
                   (vs.baseCenterX != centerScreenX ||
                    vs.baseCenterY != centerScreenY ||
                    vs.baseRad != static_cast<uint8_t>(rad))) {
            vs.hasBaseline = false;
        }

        const bool seedBaseline = isColorChanged && armed && !vs.hasBaseline;

        uint32_t sumR = 0U, sumG = 0U, sumB = 0U;
        uint32_t totalPix   = 0U;
        uint32_t matchedPix = 0U;
        uint32_t minDelta   = 255U;
        uint32_t peakDelta  = 0U;

        for (int32_t dy = -rad; dy <= rad; ++dy) {
            const int32_t sy = centerScreenY + dy;
            for (int32_t dx = -rad; dx <= rad; ++dx) {
                const int32_t sx = centerScreenX + dx;
                const uint32_t rgb = patch.SampleClamped(sx, sy);
                const uint8_t pr = static_cast<uint8_t>((rgb >> 16) & 0xFFU);
                const uint8_t pg = static_cast<uint8_t>((rgb >> 8) & 0xFFU);
                const uint8_t pb = static_cast<uint8_t>(rgb & 0xFFU);

                const size_t gIdx = static_cast<size_t>((dy + kMaxRoiRadius) * kMaxSpan + (dx + kMaxRoiRadius));
                ++totalPix;
                sumR += pr;
                sumG += pg;
                sumB += pb;

                uint8_t maskVal = 1U;
                if (isColorChanged) {
                    if (seedBaseline) {
                        vs.baselinePixels[gIdx] = rgb;
                    } else if (armed) {
                        const uint32_t base = vs.baselinePixels[gIdx];
                        const uint8_t br = static_cast<uint8_t>((base >> 16) & 0xFFU);
                        const uint8_t bg = static_cast<uint8_t>((base >> 8) & 0xFFU);
                        const uint8_t bb = static_cast<uint8_t>(base & 0xFFU);
                        const uint32_t pixDelta = ChebyshevDelta(pr, pg, pb, br, bg, bb);
                        if (pixDelta > peakDelta) {
                            peakDelta = pixDelta;
                        }
                        if (rule == mask::core::ColorTriggerMatchRule::AnyPixel && pixDelta > tol) {
                            ++matchedPix;
                            maskVal = 3U;
                        }
                    }
                } else {
                    const uint32_t pixDelta = ChebyshevDelta(pr, pg, pb, cfg.r, cfg.g, cfg.b);
                    if (pixDelta < minDelta) {
                        minDelta = pixDelta;
                    }
                    if (rule == mask::core::ColorTriggerMatchRule::AnyPixel && pixDelta <= tol) {
                        ++matchedPix;
                        maskVal = 3U;
                    }
                }

                if (outLoupeMask15x15 != nullptr) {
                    outLoupeMask15x15[gIdx] = maskVal;
                }
            }
        }

        if (totalPix == 0U) {
            return res;
        }

        const uint8_t avgR = static_cast<uint8_t>((sumR + totalPix / 2U) / totalPix);
        const uint8_t avgG = static_cast<uint8_t>((sumG + totalPix / 2U) / totalPix);
        const uint8_t avgB = static_cast<uint8_t>((sumB + totalPix / 2U) / totalPix);
        res.sampledR = avgR;
        res.sampledG = avgG;
        res.sampledB = avgB;

        if (isColorChanged) {
            if (!armed) {
                res.delta   = 0U;
                res.matched = false;
            } else if (seedBaseline) {
                vs.baseAvgR    = avgR;
                vs.baseAvgG    = avgG;
                vs.baseAvgB    = avgB;
                vs.baseCenterX = centerScreenX;
                vs.baseCenterY = centerScreenY;
                vs.baseRad     = static_cast<uint8_t>(rad);
                vs.hasBaseline = true;
                res.delta      = 0U;
                res.matched    = false;
            } else if (rule == mask::core::ColorTriggerMatchRule::ZoneAverage) {
                const uint32_t avgDelta = ChebyshevDelta(avgR, avgG, avgB, vs.baseAvgR, vs.baseAvgG, vs.baseAvgB);
                res.delta   = avgDelta;
                res.matched = (avgDelta > tol);
            } else {
                res.delta   = peakDelta;
                res.matched = (matchedPix > 0U);
            }
        } else {
            if (rule == mask::core::ColorTriggerMatchRule::ZoneAverage) {
                const uint32_t avgDelta = ChebyshevDelta(avgR, avgG, avgB, cfg.r, cfg.g, cfg.b);
                res.delta   = avgDelta;
                res.matched = (avgDelta <= tol);
            } else {
                res.delta   = minDelta;
                res.matched = (matchedPix > 0U);
            }
        }

        if (outLoupeMask15x15 != nullptr &&
            rule == mask::core::ColorTriggerMatchRule::ZoneAverage &&
            res.matched) {
            for (int32_t dy = -rad; dy <= rad; ++dy) {
                for (int32_t dx = -rad; dx <= rad; ++dx) {
                    const size_t gIdx = static_cast<size_t>((dy + kMaxRoiRadius) * kMaxSpan + (dx + kMaxRoiRadius));
                    outLoupeMask15x15[gIdx] = 3U;
                }
            }
        }

        return res;
    }
};

struct TriggerRuntime {
    bool     toggleArmed{false};
    bool     prevArmKeyDown{false};
    bool     wasArmed{false};

    bool     delayPending{false};
    uint64_t delayFireAtUs{0};
    uint64_t cooldownUntilUs{0};
    bool     firedWhileMatched{false};
    bool     reanchorPending{false};
    uint64_t reanchorAtUs{0};

    bool     isHoldingKey{false};
    uint16_t heldVk{0};

    uint64_t triggersFired{0};
    uint32_t lastPipelineUs{0};
    uint32_t avgPipelineUs{0};
    PixelPatch cachedPatch{};
    VisionAnalyzer::VisionState vision{};
    VisionAnalyzer::VisionResult lastResult{};
    uint8_t  lastLoupeMask[VisionAnalyzer::kGridCount]{};
};

template <typename FireFn, typename ReleaseFn>
inline VisionAnalyzer::VisionResult EvaluateTriggerSlot(
    const mask::core::ColorTriggerConfig& cfg,
    TriggerRuntime& rt,
    const PixelPatch& patch,
    int32_t sx,
    int32_t sy,
    bool armed,
    bool suppressed,
    uint64_t evalUs,
    uint8_t* outLoupeMask15x15,
    FireFn&& onFire,
    ReleaseFn&& onRelease
) noexcept {
    // Re-anchor baseline for ColorChanged non-hold actions once lockout/cooldown expires,
    // without ever firing on the re-snapshot frame itself.
    if (rt.reanchorPending && armed && !suppressed && evalUs >= rt.reanchorAtUs) {
        rt.reanchorPending   = false;
        rt.firedWhileMatched = false;
        rt.delayPending      = false;
        rt.vision.Reset();
    }

    const auto vRes = VisionAnalyzer::AnalyzeRoi(
        cfg,
        patch,
        sx,
        sy,
        armed,
        rt.vision,
        outLoupeMask15x15
    );
    rt.lastResult = vRes;

    if (!armed || suppressed || !patch.valid) {
        rt.delayPending = false;
        if (rt.isHoldingKey) {
            onRelease(rt);
        }
        if (!armed) {
            rt.firedWhileMatched = false;
            rt.reanchorPending   = false;
            rt.wasArmed          = false;
        }
        return vRes;
    }

    rt.wasArmed = true;

    const uint64_t delayUs    = static_cast<uint64_t>(cfg.delayMs) * 1000ULL;
    const uint64_t cooldownUs = static_cast<uint64_t>(cfg.cooldownMs) * 1000ULL;
    const auto mode           = static_cast<mask::core::ColorTriggerMode>(cfg.mode);
    const auto actKind        = static_cast<mask::core::ColorTriggerActionKind>(cfg.actionKind);
    const bool isHoldAction   = (actKind == mask::core::ColorTriggerActionKind::HoldKey);
    const bool isColorChanged = (mode == mask::core::ColorTriggerMode::ColorChanged);

    if (isHoldAction) {
        if (vRes.matched) {
            if (!rt.isHoldingKey && evalUs >= rt.cooldownUntilUs) {
                if (delayUs == 0ULL) {
                    if (onFire(cfg, rt)) {
                        ++rt.triggersFired;
                    }
                } else if (!rt.delayPending) {
                    rt.delayPending  = true;
                    rt.delayFireAtUs = evalUs + delayUs;
                } else if (evalUs >= rt.delayFireAtUs) {
                    rt.delayPending = false;
                    if (onFire(cfg, rt)) {
                        ++rt.triggersFired;
                    }
                }
            }
        } else {
            rt.delayPending = false;
            if (rt.isHoldingKey) {
                onRelease(rt);
                rt.cooldownUntilUs = evalUs + cooldownUs;
            }
        }
        return vRes;
    }

    if (isColorChanged) {
        if (vRes.matched) {
            if (!rt.firedWhileMatched && !rt.reanchorPending && evalUs >= rt.cooldownUntilUs) {
                if (delayUs == 0ULL) {
                    if (onFire(cfg, rt)) {
                        ++rt.triggersFired;
                    }
                    rt.firedWhileMatched = true;
                    rt.cooldownUntilUs   = evalUs + cooldownUs;
                    rt.reanchorPending   = true;
                    rt.reanchorAtUs      = evalUs + (std::max<uint64_t>)(cooldownUs, kMinColorChangedReanchorUs);
                } else if (!rt.delayPending) {
                    rt.delayPending  = true;
                    rt.delayFireAtUs = evalUs + delayUs;
                } else if (evalUs >= rt.delayFireAtUs) {
                    rt.delayPending = false;
                    if (onFire(cfg, rt)) {
                        ++rt.triggersFired;
                    }
                    rt.firedWhileMatched = true;
                    rt.cooldownUntilUs   = evalUs + cooldownUs;
                    rt.reanchorPending   = true;
                    rt.reanchorAtUs      = evalUs + (std::max<uint64_t>)(cooldownUs, kMinColorChangedReanchorUs);
                }
            }
        } else {
            rt.delayPending = false;
            if (!rt.reanchorPending) {
                rt.firedWhileMatched = false;
            }
        }
    } else {
        if (vRes.matched) {
            const bool canRefire = (cooldownUs > 0ULL) ? (evalUs >= rt.cooldownUntilUs) : !rt.firedWhileMatched;
            if (canRefire) {
                if (delayUs == 0ULL) {
                    if (onFire(cfg, rt)) {
                        ++rt.triggersFired;
                    }
                    rt.cooldownUntilUs   = evalUs + cooldownUs;
                    rt.firedWhileMatched = true;
                } else if (!rt.delayPending) {
                    rt.delayPending  = true;
                    rt.delayFireAtUs = evalUs + delayUs;
                } else if (evalUs >= rt.delayFireAtUs) {
                    rt.delayPending = false;
                    if (onFire(cfg, rt)) {
                        ++rt.triggersFired;
                    }
                    rt.cooldownUntilUs   = evalUs + cooldownUs;
                    rt.firedWhileMatched = true;
                }
            }
        } else {
            rt.delayPending      = false;
            rt.firedWhileMatched = false;
        }
    }

    return vRes;
}

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

    /**
     * @brief Deterministic offline verification for MatchColor, ColorChanged (zero spam),
     *        HoldKey release, and 15x15 AnalyzeRoi throughput (<50 us/frame).
     */
    [[nodiscard]] static bool RunTriggerSelfTest(double& outAvgUs) noexcept {
        outAvgUs = 0.0;
        auto rtPtr = std::make_unique<detail::TriggerRuntime>();
        if (!rtPtr) {
            return false;
        }
        auto& rt = *rtPtr;

        detail::PixelPatch patch{};
        patch.originScreenX = 0;
        patch.originScreenY = 0;
        patch.width         = detail::kPatchSize;
        patch.height        = detail::kPatchSize;
        patch.valid         = true;

        auto packRgb = [](uint8_t r, uint8_t g, uint8_t b) noexcept -> uint32_t {
            return (static_cast<uint32_t>(r) << 16) |
                   (static_cast<uint32_t>(g) << 8)  |
                   static_cast<uint32_t>(b);
        };
        auto fillPatch = [&](uint8_t r, uint8_t g, uint8_t b) noexcept {
            const uint32_t c = packRgb(r, g, b);
            for (auto& px : patch.pixels) {
                px = c;
            }
        };
        auto setPixel = [&](int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b) noexcept {
            if (x >= 0 && x < detail::kPatchSize && y >= 0 && y < detail::kPatchSize) {
                patch.pixels[static_cast<size_t>(y * detail::kPatchSize + x)] = packRgb(r, g, b);
            }
        };

        // Case 1: MatchColor 1px hit, exact Chebyshev boundary, and ZoneAverage
        {
            rt = detail::TriggerRuntime{};
            mask::core::ColorTriggerConfig cfg{};
            cfg.mode         = static_cast<uint8_t>(mask::core::ColorTriggerMode::MatchColor);
            cfg.r            = 240U;
            cfg.g            = 20U;
            cfg.b            = 20U;
            cfg.tolerance    = 20U;
            cfg.sampleRadius = 2U; // 5x5 zone around (16, 16) -> [14..18] x [14..18]
            mask::core::SetTriggerMatchRule(cfg, mask::core::ColorTriggerMatchRule::AnyPixel);

            fillPatch(10U, 10U, 10U);
            uint8_t mask[detail::VisionAnalyzer::kGridCount]{};
            if (detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision, mask).matched) {
                return false;
            }

            // Single pixel at corner (14, 14) with Chebyshev delta == 20 -> MUST match
            setPixel(14, 14, 220U, 40U, 20U);
            const auto hitRes = detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision, mask);
            if (!hitRes.matched || hitRes.delta != 20U) {
                return false;
            }
            if (mask[5 * 15 + 5] != 3U || mask[7 * 15 + 7] != 1U || mask[0] != 0U) {
                return false;
            }

            // Single pixel at delta == 21 -> MUST NOT match
            setPixel(14, 14, 219U, 20U, 20U);
            const auto missRes = detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision, mask);
            if (missRes.matched || missRes.delta != 21U) {
                return false;
            }

            // ZoneAverage vs AnyPixel
            cfg.r = 200U; cfg.g = 100U; cfg.b = 50U;
            cfg.tolerance = 10U;
            cfg.sampleRadius = 1U; // 3x3
            fillPatch(0U, 0U, 0U);
            setPixel(16, 16, 200U, 100U, 50U);

            mask::core::SetTriggerMatchRule(cfg, mask::core::ColorTriggerMatchRule::AnyPixel);
            if (!detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision).matched) {
                return false;
            }

            mask::core::SetTriggerMatchRule(cfg, mask::core::ColorTriggerMatchRule::ZoneAverage);
            if (detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision).matched) {
                return false;
            }

            fillPatch(210U, 90U, 60U); // exact avg delta = 10
            const auto avgHit = detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision, mask);
            if (!avgHit.matched || avgHit.delta != 10U || mask[7 * 15 + 7] != 3U) {
                return false;
            }

            fillPatch(211U, 100U, 50U); // avg delta = 11
            if (detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision).matched) {
                return false;
            }
        }

        // Case 2: ColorChanged NEVER fires on Frame 0, fires ONCE, zero spam across 50 frames,
        //         re-anchors cleanly after cooldown, and fires ONCE on subsequent change.
        for (const uint32_t testCooldownMs : { 80U, 0U }) {
            rt = detail::TriggerRuntime{};
            mask::core::ColorTriggerConfig cfg{};
            cfg.mode         = static_cast<uint8_t>(mask::core::ColorTriggerMode::ColorChanged);
            cfg.actionKind   = static_cast<uint8_t>(mask::core::ColorTriggerActionKind::ClickKey);
            cfg.actionVk     = mask::core::kVkLeftButton;
            cfg.tolerance    = 20U;
            cfg.sampleRadius = 2U;
            cfg.delayMs      = 0U;
            cfg.cooldownMs   = testCooldownMs;
            mask::core::SetTriggerMatchRule(cfg, mask::core::ColorTriggerMatchRule::AnyPixel);

            uint32_t fireEvents = 0U;
            uint32_t releaseEvents = 0U;
            auto onFire = [&](const mask::core::ColorTriggerConfig&, detail::TriggerRuntime&) noexcept -> bool {
                ++fireEvents;
                return true;
            };
            auto onRelease = [&](detail::TriggerRuntime& r) noexcept {
                ++releaseEvents;
                r.isHoldingKey = false;
                r.heldVk = 0U;
            };

            // Frame 0 (Blue): must capture baseline without firing
            fillPatch(20U, 40U, 200U);
            const auto f0 = detail::EvaluateTriggerSlot(
                cfg, rt, patch, 16, 16, true, false, 0ULL, nullptr, onFire, onRelease);
            if (f0.matched || fireEvents != 0U || !rt.vision.hasBaseline) {
                return false;
            }

            // Within tolerance (delta = 15 <= 20): no fire
            fillPatch(20U, 55U, 200U);
            const auto f1 = detail::EvaluateTriggerSlot(
                cfg, rt, patch, 16, 16, true, false, 1'000ULL, nullptr, onFire, onRelease);
            if (f1.matched || fireEvents != 0U) {
                return false;
            }

            // 1px changes to Red -> fires ONCE
            setPixel(15, 17, 240U, 40U, 40U);
            const auto f2 = detail::EvaluateTriggerSlot(
                cfg, rt, patch, 16, 16, true, false, 2'000ULL, nullptr, onFire, onRelease);
            if (!f2.matched || fireEvents != 1U || rt.triggersFired != 1ULL) {
                return false;
            }

            // 50 consecutive frames of Red -> ZERO spam (must stay at 1 fire)
            uint64_t tUs = 3'000ULL;
            for (int f = 0; f < 50; ++f) {
                detail::EvaluateTriggerSlot(
                    cfg, rt, patch, 16, 16, true, false, tUs, nullptr, onFire, onRelease);
                if (fireEvents != 1U) {
                    return false;
                }
                tUs += 900ULL; // reaches 48,000 us (< 50,000 us min lockout and < 80,000 us cooldown)
            }

            // After cooldown/lockout: re-anchors cleanly to Red without firing
            const uint64_t reanchorUs = 2'000ULL + (std::max<uint64_t>)(
                static_cast<uint64_t>(testCooldownMs) * 1000ULL, detail::kMinColorChangedReanchorUs);
            const auto fReanchor = detail::EvaluateTriggerSlot(
                cfg, rt, patch, 16, 16, true, false, reanchorUs, nullptr, onFire, onRelease);
            if (fReanchor.matched || fireEvents != 1U) {
                return false;
            }

            // 10 more steady Red frames -> still 1 fire
            for (int f = 0; f < 10; ++f) {
                const auto fSteady = detail::EvaluateTriggerSlot(
                    cfg, rt, patch, 16, 16, true, false, reanchorUs + static_cast<uint64_t>(f + 1) * 1'000ULL,
                    nullptr, onFire, onRelease);
                if (fSteady.matched || fireEvents != 1U) {
                    return false;
                }
            }

            // Subsequent change to Green -> fires ONCE (total = 2)
            fillPatch(40U, 230U, 40U);
            const auto fNext = detail::EvaluateTriggerSlot(
                cfg, rt, patch, 16, 16, true, false, reanchorUs + 20'000ULL, nullptr, onFire, onRelease);
            if (!fNext.matched || fireEvents != 2U || releaseEvents != 0U) {
                return false;
            }
        }

        // Case 2b: ColorChanged with delayMs (cancel on revert, fire once after delay)
        {
            rt = detail::TriggerRuntime{};
            mask::core::ColorTriggerConfig cfg{};
            cfg.mode         = static_cast<uint8_t>(mask::core::ColorTriggerMode::ColorChanged);
            cfg.actionKind   = static_cast<uint8_t>(mask::core::ColorTriggerActionKind::RunMacroSlot);
            cfg.tolerance    = 20U;
            cfg.sampleRadius = 1U;
            cfg.delayMs      = 30U;
            cfg.cooldownMs   = 50U;

            uint32_t fireEvents = 0U;
            auto onFire = [&](const mask::core::ColorTriggerConfig&, detail::TriggerRuntime&) noexcept -> bool {
                ++fireEvents;
                return true;
            };
            auto onRelease = [&](detail::TriggerRuntime&) noexcept {};

            fillPatch(30U, 30U, 30U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 0ULL, nullptr, onFire, onRelease);
            fillPatch(250U, 30U, 30U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 1'000ULL, nullptr, onFire, onRelease);
            // Revert before 30ms delay completes
            fillPatch(30U, 30U, 30U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 20'000ULL, nullptr, onFire, onRelease);
            if (fireEvents != 0U || rt.delayPending) {
                return false;
            }

            fillPatch(250U, 30U, 30U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 25'000ULL, nullptr, onFire, onRelease);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 54'000ULL, nullptr, onFire, onRelease);
            if (fireEvents != 0U) {
                return false;
            }
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 55'000ULL, nullptr, onFire, onRelease);
            if (fireEvents != 1U) {
                return false;
            }
        }

        // Case 3: HoldKey holds on change (never re-anchors while held), releases on return to baseline
        {
            rt = detail::TriggerRuntime{};
            mask::core::ColorTriggerConfig cfg{};
            cfg.mode         = static_cast<uint8_t>(mask::core::ColorTriggerMode::ColorChanged);
            cfg.actionKind   = static_cast<uint8_t>(mask::core::ColorTriggerActionKind::HoldKey);
            cfg.actionVk     = 'F';
            cfg.tolerance    = 20U;
            cfg.sampleRadius = 1U;
            cfg.cooldownMs   = 50U;

            uint32_t downCount = 0U;
            uint32_t upCount   = 0U;
            auto onFire = [&](const mask::core::ColorTriggerConfig& c, detail::TriggerRuntime& r) noexcept -> bool {
                r.isHoldingKey = true;
                r.heldVk       = static_cast<uint16_t>(c.actionVk & 0xFFU);
                ++downCount;
                return true;
            };
            auto onRelease = [&](detail::TriggerRuntime& r) noexcept {
                if (r.isHoldingKey) {
                    r.isHoldingKey = false;
                    r.heldVk       = 0U;
                    ++upCount;
                }
            };

            fillPatch(20U, 40U, 200U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 0ULL, nullptr, onFire, onRelease);
            if (downCount != 0U || upCount != 0U || rt.isHoldingKey) {
                return false;
            }

            fillPatch(240U, 40U, 40U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 1'000ULL, nullptr, onFire, onRelease);
            if (downCount != 1U || upCount != 0U || !rt.isHoldingKey) {
                return false;
            }

            // Hold well past cooldownMs without releasing or re-anchoring
            for (int f = 0; f < 100; ++f) {
                (void)detail::EvaluateTriggerSlot(
                    cfg, rt, patch, 16, 16, true, false, 2'000ULL + static_cast<uint64_t>(f) * 2'000ULL,
                    nullptr, onFire, onRelease);
                if (downCount != 1U || upCount != 0U || !rt.isHoldingKey) {
                    return false;
                }
            }

            // Return within tolerance of armed baseline -> MUST release
            fillPatch(25U, 45U, 205U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 205'000ULL, nullptr, onFire, onRelease);
            if (downCount != 1U || upCount != 1U || rt.isHoldingKey) {
                return false;
            }

            // Change during cooldown -> MUST NOT re-hold yet
            fillPatch(240U, 40U, 40U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 215'000ULL, nullptr, onFire, onRelease);
            if (downCount != 1U || rt.isHoldingKey) {
                return false;
            }

            // After cooldown -> holds again, and disarm immediately releases
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 255'000ULL, nullptr, onFire, onRelease);
            if (downCount != 2U || !rt.isHoldingKey) {
                return false;
            }
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, false, false, 256'000ULL, nullptr, onFire, onRelease);
            if (upCount != 2U || rt.isHoldingKey || rt.vision.hasBaseline) {
                return false;
            }
        }

        // Case 3b: ColorChanged + ZoneAverage + MatchColor + HoldKey
        {
            rt = detail::TriggerRuntime{};
            mask::core::ColorTriggerConfig cfg{};
            cfg.mode         = static_cast<uint8_t>(mask::core::ColorTriggerMode::ColorChanged);
            cfg.actionKind   = static_cast<uint8_t>(mask::core::ColorTriggerActionKind::ClickKey);
            cfg.actionVk     = mask::core::kVkLeftButton;
            cfg.tolerance    = 20U;
            cfg.sampleRadius = 1U; // 3x3
            cfg.cooldownMs   = 50U;
            mask::core::SetTriggerMatchRule(cfg, mask::core::ColorTriggerMatchRule::ZoneAverage);

            uint32_t fires = 0U;
            auto onFire = [&](const mask::core::ColorTriggerConfig&, detail::TriggerRuntime&) noexcept -> bool {
                ++fires;
                return true;
            };
            auto onRelease = [&](detail::TriggerRuntime&) noexcept {};

            fillPatch(100U, 100U, 100U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 0ULL, nullptr, onFire, onRelease);
            // 1px spikes by +90 -> avg changes by 10 <= 20 -> ZoneAverage must NOT fire
            setPixel(16, 16, 190U, 100U, 100U);
            if (detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 1'000ULL, nullptr, onFire, onRelease).matched || fires != 0U) {
                return false;
            }
            // Whole zone changes by +25 (> 20) -> MUST fire once
            fillPatch(125U, 100U, 100U);
            if (!detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 2'000ULL, nullptr, onFire, onRelease).matched || fires != 1U) {
                return false;
            }

            // MatchColor + HoldKey: holds while matching, releases when target leaves
            rt = detail::TriggerRuntime{};
            cfg.mode       = static_cast<uint8_t>(mask::core::ColorTriggerMode::MatchColor);
            cfg.actionKind = static_cast<uint8_t>(mask::core::ColorTriggerActionKind::HoldKey);
            cfg.actionVk   = 'G';
            cfg.r = 240U; cfg.g = 20U; cfg.b = 20U;
            cfg.tolerance  = 15U;
            mask::core::SetTriggerMatchRule(cfg, mask::core::ColorTriggerMatchRule::AnyPixel);

            uint32_t downs = 0U, ups = 0U;
            auto holdFire = [&](const mask::core::ColorTriggerConfig& c, detail::TriggerRuntime& r) noexcept -> bool {
                r.isHoldingKey = true;
                r.heldVk       = static_cast<uint16_t>(c.actionVk & 0xFFU);
                ++downs;
                return true;
            };
            auto holdRel = [&](detail::TriggerRuntime& r) noexcept {
                if (r.isHoldingKey) {
                    r.isHoldingKey = false;
                    r.heldVk       = 0U;
                    ++ups;
                }
            };
            fillPatch(240U, 20U, 20U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 0ULL, nullptr, holdFire, holdRel);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 100'000ULL, nullptr, holdFire, holdRel);
            if (downs != 1U || ups != 0U || !rt.isHoldingKey) {
                return false;
            }
            fillPatch(10U, 10U, 10U);
            (void)detail::EvaluateTriggerSlot(cfg, rt, patch, 16, 16, true, false, 101'000ULL, nullptr, holdFire, holdRel);
            if (downs != 1U || ups != 1U || rt.isHoldingKey) {
                return false;
            }
        }

        // Case 4: AnalyzeRoi 15x15 speed benchmark (< 50 us/frame)
        {
            rt = detail::TriggerRuntime{};
            mask::core::ColorTriggerConfig cfg{};
            cfg.mode         = static_cast<uint8_t>(mask::core::ColorTriggerMode::MatchColor);
            cfg.r            = 255U;
            cfg.g            = 45U;
            cfg.b            = 45U;
            cfg.tolerance    = 28U;
            cfg.sampleRadius = 7U; // 15x15
            mask::core::SetTriggerMatchRule(cfg, mask::core::ColorTriggerMatchRule::AnyPixel);

            fillPatch(50U, 60U, 70U);
            setPixel(16, 16, 240U, 40U, 40U);

            uint8_t mask[detail::VisionAnalyzer::kGridCount]{};
            LARGE_INTEGER freq{}, t0{}, t1{};
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);
            uint32_t sink = 0U;
            for (int it = 0; it < 10'000; ++it) {
                const auto r = detail::VisionAnalyzer::AnalyzeRoi(cfg, patch, 16, 16, true, rt.vision, mask);
                sink += r.delta + (r.matched ? 1U : 0U);
            }
            QueryPerformanceCounter(&t1);
            if (sink == 0U) {
                return false;
            }

            const double sec = static_cast<double>(t1.QuadPart - t0.QuadPart) /
                               static_cast<double>(freq.QuadPart > 0 ? freq.QuadPart : 10'000'000LL);
            outAvgUs = (sec * 1'000'000.0) / 10'000.0;
            if (outAvgUs >= 50.0) {
                return false;
            }
        }

        return true;
    }

private:
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

    void ReleaseHeldKey(detail::TriggerRuntime& rt) noexcept {
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

    void ReleaseAllHeldKeys(std::array<detail::TriggerRuntime, mask::core::kMaxColorTriggers>& runtime) noexcept {
        for (auto& rt : runtime) {
            ReleaseHeldKey(rt);
            rt.delayPending      = false;
            rt.firedWhileMatched = false;
            rt.reanchorPending   = false;
        }
    }

    [[nodiscard]] bool DispatchTriggerAction(const mask::core::ColorTriggerConfig& cfg, detail::TriggerRuntime& rt) noexcept {
        mask::core::AuxOutputCommand cmd{};
        unsigned int aux = 0;
        cmd.timestampTsc = __rdtscp(&aux);

        const auto kind = static_cast<mask::core::ColorTriggerActionKind>(cfg.actionKind);
        switch (kind) {
            case mask::core::ColorTriggerActionKind::ClickKey: {
                const uint16_t vk = static_cast<uint16_t>(cfg.actionVk & 0xFFU);
                if (vk == 0U) {
                    return false;
                }
                cmd.kind   = static_cast<uint8_t>(mask::core::AuxActionKind::KeyClick);
                cmd.vk     = vk;
                cmd.holdMs = (cfg.holdMs > 0U) ? cfg.holdMs : 15U;
                PushAuxCommand(cmd);
                return true;
            }
            case mask::core::ColorTriggerActionKind::HoldKey: {
                const uint16_t vk = static_cast<uint16_t>(cfg.actionVk & 0xFFU);
                if (vk == 0U || rt.isHoldingKey) {
                    return false;
                }
                cmd.kind = static_cast<uint8_t>(mask::core::AuxActionKind::KeyDown);
                cmd.vk   = vk;
                PushAuxCommand(cmd);
                rt.isHoldingKey = true;
                rt.heldVk       = vk;
                return true;
            }
            case mask::core::ColorTriggerActionKind::RunMacroSlot: {
                cmd.kind     = static_cast<uint8_t>(mask::core::AuxActionKind::TriggerMacroSlot);
                cmd.auxIndex = static_cast<uint8_t>(cfg.linkedIndex % mask::core::kMaxMacroSlots);
                PushAuxCommand(cmd);
                return true;
            }
            case mask::core::ColorTriggerActionKind::RunComplexSeq: {
                cmd.kind     = static_cast<uint8_t>(mask::core::AuxActionKind::TriggerComplexSeq);
                cmd.auxIndex = static_cast<uint8_t>(cfg.linkedIndex % mask::core::kMaxComplexSequences);
                PushAuxCommand(cmd);
                return true;
            }
            default:
                return false;
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

        if ((cfg.sampleX >= mon.width || cfg.sampleY >= mon.height) &&
            cfg.sampleX >= mon.left && cfg.sampleX < (mon.left + mon.width) &&
            cfg.sampleY >= mon.top && cfg.sampleY < (mon.top + mon.height)) {
            outScreenX = cfg.sampleX;
            outScreenY = cfg.sampleY;
            return;
        }

        const int32_t localX = std::clamp(cfg.sampleX, 0, (std::max<int32_t>)(0, mon.width - 1));
        const int32_t localY = std::clamp(cfg.sampleY, 0, (std::max<int32_t>)(0, mon.height - 1));
        outScreenX = mon.left + localX;
        outScreenY = mon.top + localY;
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
            dueTime.QuadPart = -static_cast<LONGLONG>(intervalUs) * 10LL;
            if (SetWaitableTimer(waitTimer, &dueTime, 0, nullptr, nullptr, FALSE) != FALSE) {
                HANDLE handles[2] = { wakeEvent_, waitTimer };
                WaitForMultipleObjects(2U, handles, FALSE, INFINITE);
                return;
            }
        }
        const DWORD ms = (std::max<DWORD>)(1U, static_cast<DWORD>((intervalUs + 999U) / 1000U));
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
        auto runtimePtr = std::make_unique<std::array<detail::TriggerRuntime, mask::core::kMaxColorTriggers>>();
        if (!runtimePtr) {
            if (waitTimer != nullptr) {
                CloseHandle(waitTimer);
            }
            return;
        }
        auto& runtime = *runtimePtr;
        uint64_t observedConfigVersion = ~0ULL;

        DWORD mmcssTask = 0U;
        HANDLE mmcssHandle = nullptr;
        bool threadElevated = false;
        auto setLowLatencyThread = [&](bool on) noexcept {
            if (on == threadElevated) {
                return;
            }
            if (on) {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTask);
                threadElevated = true;
            } else {
                if (mmcssHandle != nullptr) {
                    AvRevertMmThreadCharacteristics(mmcssHandle);
                    mmcssHandle = nullptr;
                }
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                threadElevated = false;
            }
        };

        auto fireSlot = [&](const mask::core::ColorTriggerConfig& cfg, detail::TriggerRuntime& rt) noexcept -> bool {
            return DispatchTriggerAction(cfg, rt);
        };
        auto releaseSlot = [&](detail::TriggerRuntime& rt) noexcept {
            ReleaseHeldKey(rt);
        };

        while (!stopRequested_.load(std::memory_order_acquire)) {
            // 1. Sync latest configs only when changed.
            const uint64_t currentVersion = configVersion_.load(std::memory_order_acquire);
            if (currentVersion != observedConfigVersion) {
                const auto prevConfigs = localConfigs;
                AcquireSRWLockShared(&configLock_);
                localConfigs = configs_;
                observedConfigVersion = configVersion_.load(std::memory_order_relaxed);
                ReleaseSRWLockShared(&configLock_);

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
                        oldC.actionKind != c.actionKind ||
                        oldC.monitorIndex != c.monitorIndex ||
                        oldC.sampleX != c.sampleX ||
                        oldC.sampleY != c.sampleY ||
                        oldC.sampleRadius != c.sampleRadius ||
                        mask::core::GetTriggerMatchRule(oldC) != mask::core::GetTriggerMatchRule(c);
                    if (sampleChanged) {
                        rt.vision.Reset();
                        rt.cachedPatch.valid    = false;
                        rt.cachedPatch.fromDxgi = false;
                        rt.delayPending         = false;
                        rt.firedWhileMatched    = false;
                        rt.reanchorPending      = false;
                    }
                    if (!c.inUse || !c.enabled) {
                        rt.delayPending      = false;
                        rt.firedWhileMatched = false;
                        rt.reanchorPending   = false;
                        rt.vision.Reset();
                        rt.wasArmed          = false;
                    }
                    if (!c.inUse) {
                        rt.triggersFired  = 0ULL;
                        rt.toggleArmed    = false;
                        rt.lastPipelineUs = 0U;
                        rt.avgPipelineUs  = 0U;
                    }
                }
            }

            // 2. Honor explicit reset requests.
            if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
                ReleaseAllHeldKeys(runtime);
                for (auto& rt : runtime) {
                    rt.toggleArmed     = false;
                    rt.prevArmKeyDown  = false;
                    rt.wasArmed        = false;
                    rt.cooldownUntilUs = 0ULL;
                    rt.vision.Reset();
                }
            }

            const bool previewOn      = previewActive_.load(std::memory_order_acquire);
            const uint8_t previewSlot = previewSlotIndex_.load(std::memory_order_acquire);
            const bool paused         = sharedState_.automationPaused.load(std::memory_order_acquire) ||
                                        !sharedState_.engineRunning.load(std::memory_order_acquire);
            const bool suppressed     = sharedState_.suppressTriggers.load(std::memory_order_acquire);

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
                setLowLatencyThread(false);
                dxgi.Shutdown();
                for (auto& rt : runtime) {
                    rt.cachedPatch.valid    = false;
                    rt.cachedPatch.fromDxgi = false;
                }
                WaitForSingleObject(wakeEvent_, INFINITE);
                lastMonitorRefreshUs = 0ULL;
                continue;
            }

            const uint64_t nowUs = detail::QueryMonotonicUs();
            if (lastMonitorRefreshUs == 0ULL || (nowUs - lastMonitorRefreshUs) >= 2'000'000ULL) {
                monitorCount = EnumerateMonitors(monitors);
                lastMonitorRefreshUs = nowUs;
            }

            // 4. Evaluate arming state and identify needed monitors/slots.
            std::array<bool, mask::core::kMaxColorTriggers> slotArmed{};
            std::array<bool, mask::core::kMaxColorTriggers> slotNeedsCapture{};
            std::array<int32_t, mask::core::kMaxColorTriggers> slotScreenX{};
            std::array<int32_t, mask::core::kMaxColorTriggers> slotScreenY{};
            std::array<uint8_t, mask::core::kMaxColorTriggers> slotMonIdx{};
            std::array<bool, kMaxMonitors> monitorNeeded{};

            bool anyArmed     = false;
            bool anyDelaySoon = false;
            for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
                const auto& cfg = localConfigs[i];
                auto& rt        = runtime[i];

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
                    if (rt.delayPending && rt.delayFireAtUs <= nowUs + 2'000ULL) {
                        anyDelaySoon = true;
                    }
                } else {
                    ReleaseHeldKey(rt);
                    rt.delayPending      = false;
                    rt.firedWhileMatched = false;
                    rt.reanchorPending   = false;
                    rt.wasArmed          = false;
                    rt.vision.Reset();
                }

                const bool isPreviewTarget = previewOn && (i == static_cast<size_t>(previewSlot));
                if (armed || isPreviewTarget) {
                    slotNeedsCapture[i] = true;
                    monitorNeeded[mIdx] = true;
                }
            }

            setLowLatencyThread(anyArmed && !paused);

            int neededMonitorCount = 0;
            for (int m = 0; m < monitorCount; ++m) {
                if (monitorNeeded[static_cast<size_t>(m)]) {
                    ++neededMonitorCount;
                }
            }

            const bool useBlockingDxgi = (anyArmed && !paused && neededMonitorCount == 1 && !anyDelaySoon);
            bool dxgiBlockedThisPass = false;

            // 5. Acquire frame per needed monitor, copy ROI cleanly, evaluate & queue actions.
            for (int m = 0; m < monitorCount; ++m) {
                const size_t mu = static_cast<size_t>(m);
                if (!monitorNeeded[mu]) {
                    continue;
                }

                const auto& mon = monitors[mu];
                const uint32_t dxgiTimeoutMs = useBlockingDxgi ? 1U : 0U;
                size_t dxgiSlotIdx = kMaxMonitors;
                const auto status = dxgi.AcquireMonitorFrame(mon, nowUs, dxgiTimeoutMs, dxgiSlotIdx);
                const uint64_t tMonStartUs = detail::QueryMonotonicUs();

                if (status == detail::DxgiDesktopCapture::AcquireStatus::AccessLost) {
                    lastMonitorRefreshUs = 0ULL;
                }
                if (dxgiTimeoutMs > 0U &&
                    (status == detail::DxgiDesktopCapture::AcquireStatus::FrameReady ||
                     status == detail::DxgiDesktopCapture::AcquireStatus::WaitTimeout)) {
                    dxgiBlockedThisPass = true;
                }

                for (size_t i = 0; i < mask::core::kMaxColorTriggers; ++i) {
                    if (!slotNeedsCapture[i] || slotMonIdx[i] != static_cast<uint8_t>(m)) {
                        continue;
                    }

                    const auto& cfg = localConfigs[i];
                    auto& rt        = runtime[i];
                    const bool armed = slotArmed[i];
                    const bool isPreviewTarget = previewOn && (i == static_cast<size_t>(previewSlot));

                    const int32_t sx = slotScreenX[i];
                    const int32_t sy = slotScreenY[i];
                    const int32_t rad = static_cast<int32_t>(
                        std::min<uint8_t>(cfg.sampleRadius, static_cast<uint8_t>(detail::kMaxRoiRadius)));
                    const int32_t needRadius = isPreviewTarget ? detail::kMaxRoiRadius : rad;
                    const int32_t reqSpan = std::clamp<int32_t>(needRadius * 2 + 1, 1, detail::kPatchSize);

                    const int32_t monRight  = mon.left + mon.width;
                    const int32_t monBot    = mon.top + mon.height;
                    const int32_t patchLeft = std::clamp(sx - needRadius, mon.left, (std::max)(mon.left, monRight - reqSpan));
                    const int32_t patchTop  = std::clamp(sy - needRadius, mon.top,  (std::max)(mon.top, monBot - reqSpan));
                    const int32_t patchW    = std::clamp(monRight - patchLeft, 1, reqSpan);
                    const int32_t patchH    = std::clamp(monBot - patchTop,    1, reqSpan);

                    const bool cacheMatches =
                        rt.cachedPatch.valid &&
                        rt.cachedPatch.monitorIndex == static_cast<uint8_t>(m) &&
                        rt.cachedPatch.originScreenX == patchLeft &&
                        rt.cachedPatch.originScreenY == patchTop &&
                        rt.cachedPatch.width == patchW &&
                        rt.cachedPatch.height == patchH;

                    bool captured = false;
                    if (status == detail::DxgiDesktopCapture::AcquireStatus::FrameReady) {
                        captured = dxgi.CopyRoiFromAcquiredFrame(
                            dxgiSlotIdx, patchLeft, patchTop, patchW, patchH, rt.cachedPatch
                        );
                        if (captured) {
                            rt.cachedPatch.monitorIndex = static_cast<uint8_t>(m);
                        }
                    } else if (status == detail::DxgiDesktopCapture::AcquireStatus::WaitTimeout) {
                        if (cacheMatches && rt.cachedPatch.fromDxgi && dxgi.HasEverReceivedFrame(dxgiSlotIdx)) {
                            captured = true;
                        }
                    }

                    if (!captured) {
                        if (gdi.CaptureRect(patchLeft, patchTop, patchW, patchH, rt.cachedPatch.pixels)) {
                            rt.cachedPatch.originScreenX = patchLeft;
                            rt.cachedPatch.originScreenY = patchTop;
                            rt.cachedPatch.width         = patchW;
                            rt.cachedPatch.height        = patchH;
                            rt.cachedPatch.monitorIndex  = static_cast<uint8_t>(m);
                            rt.cachedPatch.valid         = true;
                            rt.cachedPatch.fromDxgi      = false;
                        } else {
                            rt.cachedPatch.valid    = false;
                            rt.cachedPatch.fromDxgi = false;
                        }
                    }

                    const uint64_t evalUs = detail::QueryMonotonicUs();
                    (void)detail::EvaluateTriggerSlot(
                        cfg,
                        rt,
                        rt.cachedPatch,
                        sx,
                        sy,
                        armed,
                        suppressed,
                        evalUs,
                        isPreviewTarget ? rt.lastLoupeMask : nullptr,
                        fireSlot,
                        releaseSlot
                    );

                    if (status == detail::DxgiDesktopCapture::AcquireStatus::FrameReady || !rt.cachedPatch.fromDxgi) {
                        const uint64_t pipeUs64 = detail::QueryMonotonicUs() - tMonStartUs;
                        const uint32_t pipeUs = static_cast<uint32_t>((std::min<uint64_t>)(999'999ULL, pipeUs64));
                        rt.lastPipelineUs = pipeUs;
                        rt.avgPipelineUs  = (rt.avgPipelineUs == 0U) ? pipeUs : ((rt.avgPipelineUs * 7U + pipeUs) / 8U);
                    }
                }

                if (status == detail::DxgiDesktopCapture::AcquireStatus::FrameReady) {
                    dxgi.ReleaseAcquiredFrame(dxgiSlotIdx);
                }
            }

            // 6. Publish UI preview snapshot.
            if (previewOn) {
                const size_t pIdx = static_cast<size_t>(previewSlot);
                const auto& rt    = runtime[pIdx];
                ColorTriggerPreviewSnapshot snap{};
                if (rt.cachedPatch.valid) {
                    FillLoupeFromPatch(rt.cachedPatch, slotScreenX[pIdx], slotScreenY[pIdx], snap.loupePixels);
                    std::memcpy(snap.loupeMatchMask, rt.lastLoupeMask, sizeof(snap.loupeMatchMask));
                }
                snap.sampledR       = rt.lastResult.sampledR;
                snap.sampledG       = rt.lastResult.sampledG;
                snap.sampledB       = rt.lastResult.sampledB;
                snap.colorDelta     = rt.lastResult.delta;
                snap.sampleX        = slotScreenX[pIdx];
                snap.sampleY        = slotScreenY[pIdx];
                snap.lastPipelineUs = rt.lastPipelineUs;
                snap.avgPipelineUs  = rt.avgPipelineUs;
                snap.isMatched      = rt.lastResult.matched;
                snap.isArmed        = slotArmed[pIdx];
                snap.usingDxgi      = rt.cachedPatch.valid && rt.cachedPatch.fromDxgi;
                snap.triggersFired  = rt.triggersFired;
                PublishPreview(snap);
            }

            // 7. Adaptive pacing.
            if (dxgiBlockedThisPass) {
                continue;
            }
            uint32_t waitUs = 16'000U;
            if (anyArmed && !paused) {
                if (anyDelaySoon) {
                    waitUs = 500U;
                } else {
                    const uint32_t cfgUs = sharedState_.armedPollIntervalUs.load(std::memory_order_relaxed);
                    waitUs = std::clamp(cfgUs, 500U, 16'000U);
                }
            } else if (anyEnabled && !paused && !previewOn) {
                waitUs = 4'000U;
            }
            WaitForNextTick(waitTimer, waitUs);
        }

        setLowLatencyThread(false);

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
