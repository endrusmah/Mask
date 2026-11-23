#pragma once

// Windows 11 GPU-Accelerated UI Rendering Engine for Mask.
//
// Pipeline:
// - Direct3D 11 (`D3D11_CREATE_DEVICE_BGRA_SUPPORT`) + DXGI 1.2/1.3 Flip-Model SwapChain
//   (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `BufferCount = 2`, `DXGI_ALPHA_MODE_IGNORE`, `DXGI_SCALING_STRETCH`)
// - Embedded HLSL Fullscreen-Triangle Vertex Shader + Cosmic Nebula & Starfield Pixel Shader
//   compiled once via `D3DCompile` (zero external shader file dependencies; clean D2D gradient
//   fallback if shader compilation ever fails; full device-lost recovery on DXGI reset/removed)
// - Direct2D 1.1 (`ID2D1DeviceContext`) + DirectWrite (`Segoe UI Variable` / `Cascadia Mono`
//   with `tnum` tabular figures for `fNum`) composited directly onto the DXGI backbuffer
// - `TrimGpu()` calls `IDXGIDevice3::Trim()` on minimize / tray hide for near-zero idle GPU footprint.

#include "ui_anim.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3dcompiler.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace mask::ui {

namespace detail {

template <typename T>
class ComPtr {
public:
    constexpr ComPtr() noexcept = default;
    constexpr ComPtr(std::nullptr_t) noexcept {}
    ~ComPtr() noexcept { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset(T* p = nullptr) noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
        ptr_ = p;
    }

    [[nodiscard]] T* Get() const noexcept { return ptr_; }
    [[nodiscard]] T** Put() noexcept { Reset(); return &ptr_; }
    [[nodiscard]] void** PutVoid() noexcept { Reset(); return reinterpret_cast<void**>(&ptr_); }
    [[nodiscard]] T* operator->() const noexcept { return ptr_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

    [[nodiscard]] T* Detach() noexcept {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

private:
    T* ptr_{nullptr};
};

inline constexpr const char kCosmicNebulaHlsl[] = R"hlsl(
cbuffer NebulaParams : register(b0) {
    float2 uResolution;
    float  uTime;
    float  uFocus;
    float2 uCursorNorm;
    float  uCursorIn;
    float  uPad0;
    float4 uAccent;
    float4 uAccentHi;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv  = p;
    return o;
}

float Hash21(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 Hash22(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float Noise2D(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash21(i);
    float b = Hash21(i + float2(1.0, 0.0));
    float c = Hash21(i + float2(0.0, 1.0));
    float d = Hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float Fbm4(float2 p) {
    float sum = 0.0;
    float amp = 0.5;
    float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);
    [unroll]
    for (int i = 0; i < 4; ++i) {
        sum += amp * Noise2D(p);
        p = mul(rot, p) * 2.07 + float2(13.7, 9.2);
        amp *= 0.5;
    }
    return sum;
}

float StarLayer(float2 uv, float2 aspect, float scale, float timeSec, float focus,
                float density, float haloSize, float bandWeight) {
    float2 g = uv * aspect * scale;
    float2 cell = floor(g);
    float2 local = frac(g);
    float2 rnd = Hash22(cell);
    float prob = Hash21(cell + float2(19.3, 47.1));
    if (prob > density * lerp(0.55, 1.25, bandWeight)) {
        return 0.0;
    }

    // Constrain star center strictly inside cell [0.20, 0.80] to avoid cell-edge clipping.
    float2 center = 0.20 + 0.60 * rnd;
    float d = length(local - center);
    float core = smoothstep(0.11, 0.015, d);
    float halo = exp(-d * haloSize) * 0.42;

    float phase = prob * 62.831853 + rnd.x * 17.0;
    float twinkleSpeed = 1.4 + rnd.y * 2.2;
    float twinkle = lerp(0.92, 0.72 + 0.28 * sin(timeSec * twinkleSpeed + phase), focus);
    float brightness = (0.35 + 0.65 * pow(rnd.y, 2.0)) * twinkle;
    return (core + halo) * brightness;
}

float4 PSMain(VSOut input) : SV_Target {
    float invH = 1.0 / max(uResolution.y, 1.0);
    float aspectX = uResolution.x * invH;
    float2 aspect = float2(aspectX, 1.0);
    float2 p = (input.uv - float2(0.45, 0.54)) * aspect;

    // Diagonal cosmic nebula frame: bottom-left -> center -> top-right
    float along  =  p.x * 0.72 - p.y * 0.69;
    float across =  p.x * 0.69 + p.y * 0.72;
    float2 np = float2(along, across);

    float drift = uTime * 0.022;
    float2 q = float2(
        Fbm4(np * 2.25 + float2(drift, -drift * 0.55)),
        Fbm4(np * 2.25 + float2(5.2 - drift * 0.45, 1.7))
    );
    float warp   = Fbm4(np * 3.60 + q * 1.65 + float2(0.0, drift * 0.30));
    float detail = Fbm4(np * 8.60 - q * 1.35);

    // Tight diagonal cosmic gas ribbon so edges/corners remain deep void #02050B
    float warpedAcross = across + (warp - 0.5) * 0.32 + (detail - 0.5) * 0.10;
    float ribbon = exp(-pow(abs(warpedAcross), 1.75) * 11.5) * exp(-along * along * 0.68);

    // Luminous stellar core centered slightly upper-left of the dark dust ridge
    float2 coreDelta = float2((along + 0.05) * 1.05, (across + 0.06) * 1.45);
    float coreEnv = exp(-dot(coreDelta, coreDelta) * 7.2);
    float upperBloom = exp(-pow(max(0.0, across + 0.12), 1.65) * 6.5) *
                       exp(-along * along * 1.40) * (0.35 + 0.65 * warp);

    // Asymmetric cosmic dust rift & absorption filaments cutting diagonally across the core
    float ridgeSigned = across - 0.010 + (warp - 0.5) * 0.24 + (detail - 0.5) * 0.12;
    float dustMask = smoothstep(-0.025, 0.065, ridgeSigned) * exp(-max(0.0, ridgeSigned) * 7.0);
    float dustTendrils = smoothstep(0.45, 0.76, detail) * exp(-abs(ridgeSigned) * 14.0);
    float dustOcclusion = saturate(1.0 - max(dustMask * 0.85, dustTendrils * 0.65));

    // Soft cyan rim where gas spills around the upper side of the dust cliff
    float cliffBacklight = exp(-pow(max(0.0, -ridgeSigned + 0.01) * 12.0, 1.4)) *
                           smoothstep(-0.06, 0.015, ridgeSigned) *
                           (0.35 + 0.65 * detail) * exp(-along * along * 0.90);

    // Cosmic palette: #02050B void -> #081B36 deep navy -> #0E4D92 sapphire -> #22D3EE cyan -> #CFFAFE core
    float3 voidBlack   = float3(0.0078, 0.0196, 0.0431);
    float3 deepNavy    = float3(0.0314, 0.1059, 0.2118);
    float3 sapphire    = lerp(float3(0.0549, 0.3020, 0.5725), uAccent.rgb, 0.32);
    float3 cyanPlasma  = lerp(float3(0.1333, 0.8275, 0.9333), uAccentHi.rgb, 0.42);
    float3 iceCore     = float3(0.8118, 0.9804, 0.9961);

    float cloudDensity = saturate((ribbon * 0.72 + upperBloom * 0.58) * (0.28 + 0.72 * warp));
    float3 col = lerp(voidBlack, deepNavy, saturate(pow(cloudDensity, 1.15) * 1.25));
    col = lerp(col, sapphire, saturate(pow(cloudDensity, 1.65) * 0.90));

    // Sculpt dark foreground dust silhouettes
    col = lerp(voidBlack * 1.10, col, lerp(0.22, 1.0, dustOcclusion));

    // Balanced luminous core & backlit cyan wisps (preserves internal gas texture without clipping)
    float coreTex = saturate(coreEnv * (0.30 + 0.70 * warp) * (0.55 + 0.45 * detail) * lerp(0.24, 1.0, dustOcclusion));
    col += cyanPlasma * (coreTex * 0.46 + cliffBacklight * 0.26);
    col += iceCore * (pow(coreTex, 2.35) * 0.42 + pow(saturate(cliffBacklight), 2.5) * 0.12);

    // Deep cosmic vignette toward #02050B void corners
    float vig = 1.0 - 0.78 * smoothstep(0.28, 1.02, length(p));
    col = lerp(voidBlack, col, saturate(vig));

    // Layered procedural starfield (denser near the cosmic band, pinpoint in dark space)
    float bandWeight = saturate(cloudDensity * 1.2 + coreEnv * 0.7);
    float sFar  = StarLayer(input.uv, aspect, 118.0, uTime, uFocus, 0.15, 20.0, bandWeight);
    float sMid  = StarLayer(input.uv + float2(0.17, 0.41), aspect, 52.0, uTime, uFocus, 0.105, 14.0, bandWeight);
    float sHero = StarLayer(input.uv + float2(0.63, 0.29), aspect, 21.0, uTime, uFocus, 0.072, 9.0, bandWeight);

    float3 starTintMid  = lerp(float3(0.68, 0.86, 1.0), uAccentHi.rgb, 0.35);
    float3 starTintHero = lerp(float3(0.86, 0.97, 1.0), uAccentHi.rgb, 0.25);
    col += float3(0.55, 0.75, 0.96) * (sFar * 0.34);
    col += starTintMid * (sMid * 0.54);
    col += starTintHero * (sHero * 0.82);

    // Interactive cursor nebula aura when focused
    if (uCursorIn > 0.001 && uFocus > 0.001) {
        float2 cDelta = (input.uv - uCursorNorm) * aspect;
        float cAura = exp(-dot(cDelta, cDelta) * 11.5) * uCursorIn * uFocus;
        float3 auraTint = lerp(uAccent.rgb, uAccentHi.rgb, 0.40);
        col += auraTint * (cAura * 0.068);
    }

    // Triangular Interleaved Gradient Noise micro-dither to prevent 8-bit dark-blue banding
    float ign = frac(52.9829189 * frac(dot(input.pos.xy, float2(0.06711056, 0.00583715))));
    col += (ign - 0.5) / 255.0;

    return float4(saturate(col), 1.0);
}
)hlsl";

} // namespace detail

// =====================================================================================
//  GpuCanvas: Direct3D 11 + DXGI Flip-Model + Direct2D 1.1 + DirectWrite Canvas
// =====================================================================================

class GpuCanvas {
public:
    // Cached DirectWrite Text Formats (Windows 11 Segoe UI Variable / Cascadia Mono with fallbacks)
    IDWriteTextFormat* fHero{nullptr};
    IDWriteTextFormat* fTitle{nullptr};
    IDWriteTextFormat* fHeading{nullptr};
    IDWriteTextFormat* fBody{nullptr};
    IDWriteTextFormat* fBodyStrong{nullptr};
    IDWriteTextFormat* fSmall{nullptr};
    IDWriteTextFormat* fLabel{nullptr};
    IDWriteTextFormat* fNum{nullptr};
    IDWriteTextFormat* fKey{nullptr};
    IDWriteTextFormat* fAxis{nullptr};
    IDWriteTextFormat* fMini{nullptr};

    GpuCanvas() noexcept = default;
    ~GpuCanvas() noexcept { Shutdown(); }

    GpuCanvas(const GpuCanvas&) = delete;
    GpuCanvas& operator=(const GpuCanvas&) = delete;

    bool Initialize(HWND hwnd) noexcept {
        hwnd_ = hwnd;
        if (!EnsureFactoriesAndFonts()) {
            return false;
        }
        CompileShadersOnce();
        if (hwnd_ != nullptr && IsWindow(hwnd_)) {
            const UINT winDpi = GetDpiForWindow(hwnd_);
            dpi_ = (winDpi > 0U) ? static_cast<float>(winDpi) : 96.0f;
            RECT rc{};
            if (GetClientRect(hwnd_, &rc)) {
                const UINT pw = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
                const UINT ph = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
                return EnsureDeviceAndSwapChain(hwnd_, pw, ph);
            }
        }
        return true;
    }

    void Shutdown() noexcept {
        DiscardDeviceResources();
        vsBlob_.Reset();
        psBlob_.Reset();
        tabularTypography_.Reset();

        for (IDWriteTextFormat** fmt : AllFormats()) {
            if (*fmt != nullptr) {
                (*fmt)->Release();
                *fmt = nullptr;
            }
        }
        dwriteFactory_.Reset();
        d2dFactory_.Reset();
        hwnd_ = nullptr;
        widthPx_ = 0U;
        heightPx_ = 0U;
        inFrame_ = false;
    }

    void Resize(UINT pixelW, UINT pixelH) noexcept {
        if (pixelW == 0U || pixelH == 0U) {
            return;
        }
        if (pixelW == widthPx_ && pixelH == heightPx_ && swapChain_ && d3dRtv_ && d2dTargetBitmap_) {
            return;
        }
        if (!swapChain_) {
            widthPx_ = pixelW;
            heightPx_ = pixelH;
            return;
        }
        ResizeSwapChainInternal(pixelW, pixelH);
    }

    void SetDpi(float dpi) noexcept {
        dpi_ = (dpi >= 48.0f && dpi <= 480.0f) ? dpi : 96.0f;
        if (d2dContext_) {
            d2dContext_->SetDpi(dpi_, dpi_);
        }
    }

    /// Drops DXGI/D3D11 staging and intermediate buffers when window minimizes or hides to tray.
    void TrimGpu() noexcept {
        if (inFrame_) {
            return;
        }
        if (d2dContext_) {
            d2dContext_->SetTarget(nullptr);
        }
        if (d3dContext_) {
            ID3D11RenderTargetView* nullRtv = nullptr;
            d3dContext_->OMSetRenderTargets(1U, &nullRtv, nullptr);
            d3dContext_->ClearState();
            d3dContext_->Flush();
        }
        if (dxgiDevice3_) {
            dxgiDevice3_->Trim();
        }
    }

    [[nodiscard]] bool BeginFrame(HWND hwnd, UINT pixelW, UINT pixelH, float dpi,
                                  bool focused, float ambientSec,
                                  float cursorX, float cursorY, bool cursorIn,
                                  UINT32 accentRgb, UINT32 accentHiRgb) noexcept {
        if (hwnd == nullptr || !IsWindow(hwnd) || pixelW == 0U || pixelH == 0U) {
            return false;
        }
        if (!EnsureFactoriesAndFonts()) {
            return false;
        }
        if (!vsBlob_ || !psBlob_) {
            CompileShadersOnce();
        }

        const float effectiveDpi = (dpi >= 48.0f && dpi <= 480.0f) ? dpi : 96.0f;
        dpi_ = effectiveDpi;

        if (hwnd_ != hwnd) {
            DiscardSwapChainResources();
            hwnd_ = hwnd;
        }

        if (!EnsureDeviceAndSwapChain(hwnd, pixelW, pixelH)) {
            return false;
        }
        if (pixelW != widthPx_ || pixelH != heightPx_) {
            if (!ResizeSwapChainInternal(pixelW, pixelH)) {
                return false;
            }
        }

        if (!d3dRtv_ || !d2dTargetBitmap_ || !d2dContext_ || !solidBrush_) {
            return false;
        }

        frameFocused_  = focused;
        frameCursorX_  = cursorX;
        frameCursorY_  = cursorY;
        frameCursorIn_ = cursorIn;
        frameAccentRgb_   = accentRgb;
        frameAccentHiRgb_ = accentHiRgb;
        clipDepth_ = 0U;

        const float scale = dpi_ / 96.0f;
        const float logicalW = static_cast<float>(widthPx_) / scale;
        const float logicalH = static_cast<float>(heightPx_) / scale;

        // Pass 1: Direct3D 11 Cosmic Nebula & Starfield HLSL fullscreen shader
        if (shaderReady_ && d3dContext_ && vs_ && ps_ && cb_) {
            d2dContext_->SetTarget(nullptr);

            NebulaConstantBuffer params{};
            params.resolution[0] = static_cast<float>(widthPx_);
            params.resolution[1] = static_cast<float>(heightPx_);
            params.timeSec       = ambientSec;
            params.focus         = focused ? 1.0f : 0.0f;
            params.cursorNorm[0] = std::clamp(cursorX / std::max(logicalW, 1.0f), 0.0f, 1.0f);
            params.cursorNorm[1] = std::clamp(cursorY / std::max(logicalH, 1.0f), 0.0f, 1.0f);
            params.cursorIn      = cursorIn ? 1.0f : 0.0f;
            UnpackRgbFloat(accentRgb, params.accent);
            UnpackRgbFloat(accentHiRgb, params.accentHi);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(d3dContext_->Map(cb_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped))) {
                std::memcpy(mapped.pData, &params, sizeof(params));
                d3dContext_->Unmap(cb_.Get(), 0U);
            }

            D3D11_VIEWPORT vp{};
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width    = static_cast<float>(widthPx_);
            vp.Height   = static_cast<float>(heightPx_);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;

            ID3D11RenderTargetView* rtv = d3dRtv_.Get();
            d3dContext_->OMSetRenderTargets(1U, &rtv, nullptr);
            d3dContext_->RSSetViewports(1U, &vp);
            d3dContext_->RSSetState(rasterState_.Get());
            d3dContext_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
            d3dContext_->OMSetDepthStencilState(nullptr, 0U);
            d3dContext_->IASetInputLayout(nullptr);
            d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            d3dContext_->VSSetShader(vs_.Get(), nullptr, 0U);
            d3dContext_->PSSetShader(ps_.Get(), nullptr, 0U);
            ID3D11Buffer* cbs[1] = { cb_.Get() };
            d3dContext_->PSSetConstantBuffers(0U, 1U, cbs);
            d3dContext_->Draw(3U, 0U);

            ID3D11RenderTargetView* nullRtv = nullptr;
            d3dContext_->OMSetRenderTargets(1U, &nullRtv, nullptr);
        }

        // Pass 2: Direct2D 1.1 UI Layer
        d2dContext_->SetTarget(d2dTargetBitmap_.Get());
        d2dContext_->SetDpi(dpi_, dpi_);
        d2dContext_->BeginDraw();
        d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());

        if (!shaderReady_) {
            DrawFallbackBackground(logicalW, logicalH, accentRgb);
        }

        inFrame_ = true;
        return true;
    }

    bool EndFrame(bool vsync = true) noexcept {
        if (!inFrame_) {
            return false;
        }
        while (clipDepth_ > 0U) {
            PopClip();
        }
        inFrame_ = false;

        if (!d2dContext_ || !swapChain_) {
            return false;
        }

        const HRESULT d2dHr = d2dContext_->EndDraw();
        d2dContext_->SetTarget(nullptr);

        if (d2dHr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
            DiscardDeviceResources();
            return false;
        }
        if (FAILED(d2dHr)) {
            return false;
        }

        const HRESULT presHr = swapChain_->Present(vsync ? 1U : 0U, 0U);
        if (presHr == DXGI_STATUS_OCCLUDED) {
            return true;
        }
        if (presHr == DXGI_ERROR_DEVICE_REMOVED || presHr == DXGI_ERROR_DEVICE_RESET) {
            DiscardDeviceResources();
            return false;
        }
        return SUCCEEDED(presHr);
    }

    // ---------------------------------------------------------------------------------
    //  State / Metrics Queries
    // ---------------------------------------------------------------------------------

    [[nodiscard]] float Dpi() const noexcept { return dpi_; }
    [[nodiscard]] float Scale() const noexcept { return dpi_ / 96.0f; }
    [[nodiscard]] UINT PixelWidth() const noexcept { return widthPx_; }
    [[nodiscard]] UINT PixelHeight() const noexcept { return heightPx_; }
    [[nodiscard]] D2D1_SIZE_F LogicalSize() const noexcept {
        const float s = Scale();
        return D2D1::SizeF(static_cast<float>(widthPx_) / s, static_cast<float>(heightPx_) / s);
    }
    [[nodiscard]] bool IsDeviceReady() const noexcept { return d3dDevice_ && d2dContext_ && swapChain_ && d2dTargetBitmap_; }
    [[nodiscard]] bool IsShaderActive() const noexcept { return shaderReady_; }
    [[nodiscard]] ID2D1DeviceContext* Context() const noexcept { return d2dContext_.Get(); }
    [[nodiscard]] IDWriteFactory* DWriteFactory() const noexcept { return dwriteFactory_.Get(); }
    [[nodiscard]] ID3D11Device* D3DDevice() const noexcept { return d3dDevice_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* D3DContext() const noexcept { return d3dContext_.Get(); }
    [[nodiscard]] IDXGISwapChain1* SwapChain() const noexcept { return swapChain_.Get(); }

    [[nodiscard]] bool CurrentClip(D2D1_RECT_F& out) const noexcept {
        if (clipDepth_ == 0U) {
            return false;
        }
        out = clipStack_[clipDepth_ - 1U];
        return true;
    }

    void SetTransform(const D2D1_MATRIX_3X2_F& m) noexcept {
        if (d2dContext_) {
            d2dContext_->SetTransform(m);
        }
    }

    void ResetTransform() noexcept {
        if (d2dContext_) {
            d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    // ---------------------------------------------------------------------------------
    //  Core Primitives
    // ---------------------------------------------------------------------------------

    void PushClip(const D2D1_RECT_F& r, bool antialiased = false) noexcept {
        if (!d2dContext_) {
            return;
        }
        D2D1_RECT_F effective = r;
        if (clipDepth_ > 0U) {
            const D2D1_RECT_F& prev = clipStack_[clipDepth_ - 1U];
            effective.left   = std::max(effective.left, prev.left);
            effective.top    = std::max(effective.top, prev.top);
            effective.right  = std::min(effective.right, prev.right);
            effective.bottom = std::min(effective.bottom, prev.bottom);
        }
        d2dContext_->PushAxisAlignedClip(
            r,
            antialiased ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
        if (clipDepth_ < clipStack_.size()) {
            clipStack_[clipDepth_++] = effective;
        }
    }

    void PopClip() noexcept {
        if (!d2dContext_ || clipDepth_ == 0U) {
            return;
        }
        d2dContext_->PopAxisAlignedClip();
        --clipDepth_;
    }

    void FillRoundRect(const D2D1_RECT_F& r, float radius, UINT32 rgb, float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        solidBrush_->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));
        if (radius <= 0.0f) {
            d2dContext_->FillRectangle(r, solidBrush_.Get());
        } else {
            d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), solidBrush_.Get());
        }
    }

    void FillRectRgba(const D2D1_RECT_F& r, float radius, UINT32 rgba, float alphaMul = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        solidBrush_->SetColor(ColorFromRgba(rgba, alphaMul));
        if (radius <= 0.0f) {
            d2dContext_->FillRectangle(r, solidBrush_.Get());
        } else {
            d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), solidBrush_.Get());
        }
    }

    void StrokeRoundRect(const D2D1_RECT_F& r, float radius, UINT32 rgb,
                         float width = 1.0f, float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        const float h = width * 0.5f;
        const D2D1_RECT_F ri = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
        if (ri.right <= ri.left || ri.bottom <= ri.top) {
            return;
        }
        solidBrush_->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));
        const float rr = std::max(0.0f, radius - h);
        if (rr <= 0.0f) {
            d2dContext_->DrawRectangle(ri, solidBrush_.Get(), width);
        } else {
            d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(ri, rr, rr), solidBrush_.Get(), width);
        }
    }

    void StrokeRectRgba(const D2D1_RECT_F& r, float radius, UINT32 rgba,
                        float width = 1.0f, float alphaMul = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        const float h = width * 0.5f;
        const D2D1_RECT_F ri = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
        if (ri.right <= ri.left || ri.bottom <= ri.top) {
            return;
        }
        solidBrush_->SetColor(ColorFromRgba(rgba, alphaMul));
        const float rr = std::max(0.0f, radius - h);
        if (rr <= 0.0f) {
            d2dContext_->DrawRectangle(ri, solidBrush_.Get(), width);
        } else {
            d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(ri, rr, rr), solidBrush_.Get(), width);
        }
    }

    void FillCircle(float cx, float cy, float radius, UINT32 rgb, float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || radius <= 0.0f) {
            return;
        }
        solidBrush_->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), solidBrush_.Get());
    }

    void StrokeCircle(float cx, float cy, float radius, UINT32 rgb,
                      float width = 1.0f, float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || radius <= 0.0f) {
            return;
        }
        solidBrush_->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));
        d2dContext_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), solidBrush_.Get(), width);
    }

    void Line(float x0, float y0, float x1, float y1, UINT32 rgb,
              float width = 1.0f, float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_) {
            return;
        }
        solidBrush_->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));
        d2dContext_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), solidBrush_.Get(), width);
    }

    void Text(const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, UINT32 rgb,
              DWRITE_TEXT_ALIGNMENT h = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
              float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || fmt == nullptr || text == nullptr || r.right <= r.left) {
            return;
        }
        const UINT32 len = static_cast<UINT32>(std::wcslen(text));
        if (len == 0U) {
            return;
        }
        fmt->SetTextAlignment(h);
        fmt->SetParagraphAlignment(v);
        solidBrush_->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));

        if (fmt == fNum && dwriteFactory_ && tabularTypography_) {
            detail::ComPtr<IDWriteTextLayout> layout;
            const float w = std::max(1.0f, r.right - r.left);
            const float ht = std::max(1.0f, r.bottom - r.top);
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(text, len, fmt, w, ht, layout.Put())) && layout) {
                layout->SetTypography(tabularTypography_.Get(), DWRITE_TEXT_RANGE{ 0U, len });
                d2dContext_->DrawTextLayout(
                    D2D1::Point2F(r.left, r.top),
                    layout.Get(),
                    solidBrush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
                return;
            }
        }
        d2dContext_->DrawText(text, len, fmt, r, solidBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    [[nodiscard]] float MeasureWidth(const wchar_t* text, IDWriteTextFormat* fmt) const noexcept {
        if (!dwriteFactory_ || fmt == nullptr || text == nullptr || text[0] == L'\0') {
            return 0.0f;
        }
        const UINT32 len = static_cast<UINT32>(std::wcslen(text));
        detail::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text, len, fmt, 4096.0f, 128.0f, layout.Put())) || !layout) {
            return 0.0f;
        }
        if (fmt == fNum && tabularTypography_) {
            layout->SetTypography(tabularTypography_.Get(), DWRITE_TEXT_RANGE{ 0U, len });
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    // ---------------------------------------------------------------------------------
    //  High-Craft Cosmic Glass, Gradients, Bloom & Micro-Interaction Helpers
    // ---------------------------------------------------------------------------------

    void FillLinearRect(const D2D1_RECT_F& r, float radius,
                        UINT32 colorTop, float aTop,
                        UINT32 colorBot, float aBot,
                        bool horizontal = false) noexcept {
        if (!d2dContext_ || r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        const D2D1_GRADIENT_STOP stops[2] = {
            { 0.0f, D2D1::ColorF(colorTop, std::clamp(aTop, 0.0f, 1.0f)) },
            { 1.0f, D2D1::ColorF(colorBot, std::clamp(aBot, 0.0f, 1.0f)) }
        };
        detail::ComPtr<ID2D1GradientStopCollection> stopCol;
        if (FAILED(d2dContext_->CreateGradientStopCollection(
                stops, 2U, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCol.Put())) || !stopCol) {
            return;
        }
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(r.left, r.top),
            horizontal ? D2D1::Point2F(r.right, r.top) : D2D1::Point2F(r.left, r.bottom));
        detail::ComPtr<ID2D1LinearGradientBrush> brush;
        if (FAILED(d2dContext_->CreateLinearGradientBrush(props, stopCol.Get(), brush.Put())) || !brush) {
            return;
        }
        if (radius <= 0.0f) {
            d2dContext_->FillRectangle(r, brush.Get());
        } else {
            d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
        }
    }

    void FillRadialGlow(float cx, float cy, float rx, float ry,
                        UINT32 centerColor, float centerAlpha,
                        UINT32 edgeColor, float edgeAlpha) noexcept {
        if (!d2dContext_ || rx <= 0.0f || ry <= 0.0f) {
            return;
        }
        const float ca = std::clamp(centerAlpha, 0.0f, 1.0f);
        const float ea = std::clamp(edgeAlpha, 0.0f, 1.0f);
        const float ma = ea + (ca - ea) * 0.36f;
        const UINT32 midColor = LerpRgb(centerColor, edgeColor, 0.50f);
        const D2D1_GRADIENT_STOP stops[3] = {
            { 0.0f,  D2D1::ColorF(centerColor, ca) },
            { 0.45f, D2D1::ColorF(midColor, ma) },
            { 1.0f,  D2D1::ColorF(edgeColor, ea) }
        };
        detail::ComPtr<ID2D1GradientStopCollection> stopCol;
        if (FAILED(d2dContext_->CreateGradientStopCollection(
                stops, 3U, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCol.Put())) || !stopCol) {
            return;
        }
        const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props =
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0.0f, 0.0f), rx, ry);
        detail::ComPtr<ID2D1RadialGradientBrush> brush;
        if (FAILED(d2dContext_->CreateRadialGradientBrush(props, stopCol.Get(), brush.Put())) || !brush) {
            return;
        }
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush.Get());
    }

    /// Dark obsidian translucent glass card with vertical gradient, inner top specular highlight,
    /// crisp 1px border, and interactive cursor-tracked border/surface sheen when focused.
    void FillGlassCard(const D2D1_RECT_F& r, float radius,
                       UINT32 fillTopRgba, UINT32 fillBotRgba, UINT32 borderRgba,
                       float topSpecularAlpha, float cursorX, float cursorY,
                       bool hoverHighlight, UINT32 glowRgb) noexcept {
        if (!d2dContext_ || r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        const float w = r.right - r.left;
        const float h = r.bottom - r.top;

        // 1. Dark obsidian translucent vertical gradient fill
        {
            const D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, ColorFromRgba(fillTopRgba) },
                { 1.0f, ColorFromRgba(fillBotRgba) }
            };
            detail::ComPtr<ID2D1GradientStopCollection> stopCol;
            if (SUCCEEDED(d2dContext_->CreateGradientStopCollection(
                    stops, 2U, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCol.Put())) && stopCol) {
                detail::ComPtr<ID2D1LinearGradientBrush> brush;
                if (SUCCEEDED(d2dContext_->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom)),
                        stopCol.Get(), brush.Put())) && brush) {
                    if (radius <= 0.0f) {
                        d2dContext_->FillRectangle(r, brush.Get());
                    } else {
                        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
                    }
                }
            }
        }

        // 2. Subtle top glass specular reflection + top edge highlight
        const float specA = std::clamp(topSpecularAlpha, 0.0f, 1.0f);
        if (specA > 0.001f) {
            const UINT32 specTint = LerpRgb(glowRgb, 0xE0F7FFU, 0.55f);
            const D2D1_GRADIENT_STOP specStops[3] = {
                { 0.0f,  D2D1::ColorF(specTint, specA * 0.30f) },
                { 0.18f, D2D1::ColorF(specTint, 0.0f) },
                { 1.0f,  D2D1::ColorF(specTint, 0.0f) }
            };
            detail::ComPtr<ID2D1GradientStopCollection> stopCol;
            if (SUCCEEDED(d2dContext_->CreateGradientStopCollection(
                    specStops, 3U, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCol.Put())) && stopCol) {
                detail::ComPtr<ID2D1LinearGradientBrush> specBrush;
                if (SUCCEEDED(d2dContext_->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom)),
                        stopCol.Get(), specBrush.Put())) && specBrush) {
                    if (radius <= 0.0f) {
                        d2dContext_->FillRectangle(r, specBrush.Get());
                    } else {
                        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), specBrush.Get());
                    }
                }
            }
            if (w > radius * 2.0f + 8.0f && solidBrush_) {
                const float inset = std::max(4.0f, radius);
                const float lineY = std::floor(r.top + 1.0f) + 0.5f;
                solidBrush_->SetColor(D2D1::ColorF(specTint, specA * 0.85f));
                d2dContext_->DrawLine(
                    D2D1::Point2F(r.left + inset, lineY),
                    D2D1::Point2F(r.right - inset, lineY),
                    solidBrush_.Get(),
                    1.0f);
            }
        }

        // 3. Base crisp 1px border
        StrokeRectRgba(r, radius, borderRgba, 1.0f);

        // 4. Interactive cursor spotlight on surface + luminous border sheen when focused
        const bool showCursor = frameFocused_ && (hoverHighlight || frameCursorIn_);
        if (showCursor) {
            const float nearX = std::clamp(cursorX, r.left, r.right);
            const float nearY = std::clamp(cursorY, r.top, r.bottom);
            const float dx = cursorX - nearX;
            const float dy = cursorY - nearY;
            const float distSq = dx * dx + dy * dy;
            const float sheenR = std::clamp(std::max(w, h) * 0.55f, 110.0f, 230.0f);
            if (distSq < sheenR * sheenR) {
                const UINT32 sheenCol = LerpRgb(glowRgb, 0xE0F7FFU, 0.35f);

                if (hoverHighlight) {
                    const D2D1_GRADIENT_STOP fillStops[3] = {
                        { 0.0f,  D2D1::ColorF(sheenCol, 0.085f) },
                        { 0.55f, D2D1::ColorF(glowRgb,  0.024f) },
                        { 1.0f,  D2D1::ColorF(glowRgb,  0.0f) }
                    };
                    detail::ComPtr<ID2D1GradientStopCollection> stopCol;
                    if (SUCCEEDED(d2dContext_->CreateGradientStopCollection(
                            fillStops, 3U, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCol.Put())) && stopCol) {
                        detail::ComPtr<ID2D1RadialGradientBrush> radFill;
                        if (SUCCEEDED(d2dContext_->CreateRadialGradientBrush(
                                D2D1::RadialGradientBrushProperties(
                                    D2D1::Point2F(cursorX, cursorY), D2D1::Point2F(0.0f, 0.0f), sheenR, sheenR),
                                stopCol.Get(), radFill.Put())) && radFill) {
                            if (radius <= 0.0f) {
                                d2dContext_->FillRectangle(r, radFill.Get());
                            } else {
                                d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), radFill.Get());
                            }
                        }
                    }
                }

                // Luminous cursor border sheen around the perimeter nearest the pointer
                const float borderPeak = hoverHighlight ? 0.68f : 0.36f;
                const D2D1_GRADIENT_STOP bStops[3] = {
                    { 0.0f,  D2D1::ColorF(sheenCol, borderPeak) },
                    { 0.50f, D2D1::ColorF(glowRgb,  borderPeak * 0.32f) },
                    { 1.0f,  D2D1::ColorF(glowRgb,  0.0f) }
                };
                detail::ComPtr<ID2D1GradientStopCollection> bStopCol;
                if (SUCCEEDED(d2dContext_->CreateGradientStopCollection(
                        bStops, 3U, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, bStopCol.Put())) && bStopCol) {
                    detail::ComPtr<ID2D1RadialGradientBrush> bBrush;
                    if (SUCCEEDED(d2dContext_->CreateRadialGradientBrush(
                            D2D1::RadialGradientBrushProperties(
                                D2D1::Point2F(cursorX, cursorY), D2D1::Point2F(0.0f, 0.0f), sheenR * 0.90f, sheenR * 0.90f),
                            bStopCol.Get(), bBrush.Put())) && bBrush) {
                        const float bw = 1.15f;
                        const float half = bw * 0.5f;
                        const D2D1_RECT_F ri = D2D1::RectF(r.left + half, r.top + half, r.right - half, r.bottom - half);
                        const float rr = std::max(0.0f, radius - half);
                        if (ri.right > ri.left && ri.bottom > ri.top) {
                            if (rr <= 0.0f) {
                                d2dContext_->DrawRectangle(ri, bBrush.Get(), bw);
                            } else {
                                d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(ri, rr, rr), bBrush.Get(), bw);
                            }
                        }
                    }
                }
            }
        }
    }

    /// Multi-pass glowing vector segment (oscilloscope traces, active rails, trigger waveforms).
    void DrawGlowSegment(float x0, float y0, float x1, float y1,
                         UINT32 color, float coreWidth = 1.8f,
                         float glowWidth = 7.0f, float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_) {
            return;
        }
        const float a = std::clamp(alpha, 0.0f, 1.0f);
        if (a <= 0.002f) {
            return;
        }
        const D2D1_POINT_2F p0 = D2D1::Point2F(x0, y0);
        const D2D1_POINT_2F p1 = D2D1::Point2F(x1, y1);
        ID2D1StrokeStyle* style = roundStrokeStyle_.Get();

        if (glowWidth > coreWidth) {
            solidBrush_->SetColor(D2D1::ColorF(color, 0.14f * a));
            d2dContext_->DrawLine(p0, p1, solidBrush_.Get(), glowWidth, style);

            solidBrush_->SetColor(D2D1::ColorF(color, 0.30f * a));
            d2dContext_->DrawLine(p0, p1, solidBrush_.Get(), std::max(coreWidth + 0.5f, glowWidth * 0.48f), style);
        }

        const UINT32 coreCol = LerpRgb(color, 0xECFEFFU, 0.20f);
        solidBrush_->SetColor(D2D1::ColorF(coreCol, a));
        d2dContext_->DrawLine(p0, p1, solidBrush_.Get(), coreWidth, style);
    }

    /// Dark sapphire pill toggle switch that illuminates with gradient, bloom halo, and floating icy thumb.
    void DrawToggleSwitch(const D2D1_RECT_F& r, float slideT, float glowT,
                          UINT32 accentRgb, UINT32 accentHiRgb,
                          bool hovered = false, float alpha = 1.0f) noexcept {
        if (!d2dContext_ || !solidBrush_ || r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        const float st = std::clamp(slideT, 0.0f, 1.0f);
        const float gt = std::clamp(glowT, 0.0f, 1.0f);
        const float a  = std::clamp(alpha, 0.0f, 1.0f);
        const float w  = r.right - r.left;
        const float h  = r.bottom - r.top;
        const float rad = h * 0.5f;

        // Soft outer bloom halo when enabled or firing
        const float haloStrength = std::max(gt * 0.28f, st * 0.16f) * a;
        if (haloStrength > 0.01f) {
            const float cx = (r.left + r.right) * 0.5f;
            const float cy = (r.top + r.bottom) * 0.5f;
            FillRadialGlow(cx, cy, w * 0.78f, h * 1.10f, accentRgb, haloStrength, accentRgb, 0.0f);
        }

        // Track fill interpolating from deep sapphire off-state to luminous accent gradient
        const UINT32 offTop = hovered ? 0x16243DU : 0x0C1526U;
        const UINT32 offBot = hovered ? 0x111C30U : 0x080F1DU;
        const UINT32 onTop  = LerpRgb(accentRgb, accentHiRgb, 0.35f);
        const UINT32 onBot  = accentRgb;
        const UINT32 topCol = LerpRgb(offTop, onTop, st);
        const UINT32 botCol = LerpRgb(offBot, onBot, st);
        FillLinearRect(r, rad, topCol, (0.92f + 0.08f * st) * a, botCol, (0.95f + 0.05f * st) * a, true);

        // Track border
        const UINT32 offBorder = hovered ? 0x395580U : 0x223452U;
        const UINT32 borderCol = LerpRgb(offBorder, accentHiRgb, st);
        StrokeRoundRect(r, rad, borderCol, 1.0f, (0.72f + 0.24f * st) * a);

        // Floating icy thumb with subtle drop shadow
        const float travel = std::max(0.0f, w - h);
        const float thumbX = r.left + rad + st * travel;
        const float thumbY = (r.top + r.bottom) * 0.5f;
        const float thumbR = std::max(2.0f, rad - 3.2f) + st * 0.65f;

        FillCircle(thumbX, thumbY + 1.0f, thumbR + 0.8f, 0x02050BU, 0.48f * a);
        if (st > 0.15f) {
            FillRadialGlow(thumbX, thumbY, thumbR * 1.85f, thumbR * 1.85f, accentHiRgb, 0.30f * st * a, accentRgb, 0.0f);
        }
        const UINT32 thumbOff = hovered ? 0xB8C9DFU : 0x7A8FA9U;
        const UINT32 thumbOn  = 0xECFEFFU;
        FillCircle(thumbX, thumbY, thumbR, LerpRgb(thumbOff, thumbOn, st), a);
        StrokeCircle(thumbX, thumbY, thumbR, 0xFFFFFFU, 0.75f, (0.12f + 0.38f * st) * a);
    }

    /// Expanding cosmic wavefront rings from recent clicks.
    void DrawClickRipples(const std::array<ClickRipple, UiAnimator::kRippleCount>& ripples) noexcept {
        if (!d2dContext_ || !solidBrush_) {
            return;
        }
        for (const auto& rp : ripples) {
            if (!rp.active) {
                continue;
            }
            const float t = std::clamp(rp.progress, 0.0f, 1.0f);
            const float easeOut = 1.0f - (1.0f - t) * (1.0f - t);
            const float radius = 4.0f + easeOut * 34.0f;
            const float fade = (1.0f - t) * (1.0f - t);

            if (t < 0.35f) {
                const float flashA = 0.22f * (1.0f - t / 0.35f);
                FillRadialGlow(rp.x, rp.y, radius * 0.75f, radius * 0.75f, rp.rgb, flashA, rp.rgb, 0.0f);
            }
            StrokeCircle(rp.x, rp.y, radius, rp.rgb, 3.0f, 0.22f * fade);
            StrokeCircle(rp.x, rp.y, radius, LerpRgb(rp.rgb, 0xE0F7FFU, 0.45f), 1.3f, 0.65f * fade);
        }
    }

private:
    struct alignas(16) NebulaConstantBuffer {
        float resolution[2]{};
        float timeSec{0.0f};
        float focus{1.0f};
        float cursorNorm[2]{};
        float cursorIn{0.0f};
        float pad0{0.0f};
        float accent[4]{};
        float accentHi[4]{};
    };
    static_assert((sizeof(NebulaConstantBuffer) % 16) == 0, "Constant buffer must be 16-byte aligned.");

    static void UnpackRgbFloat(UINT32 rgb, float out[4]) noexcept {
        out[0] = static_cast<float>((rgb >> 16) & 0xFFU) / 255.0f;
        out[1] = static_cast<float>((rgb >> 8) & 0xFFU) / 255.0f;
        out[2] = static_cast<float>(rgb & 0xFFU) / 255.0f;
        out[3] = 1.0f;
    }

    std::array<IDWriteTextFormat**, 11> AllFormats() noexcept {
        return {
            &fHero, &fTitle, &fHeading, &fBody, &fBodyStrong,
            &fSmall, &fLabel, &fNum, &fKey, &fAxis, &fMini
        };
    }

    bool EnsureFactoriesAndFonts() noexcept {
        if (!d2dFactory_) {
            if (FAILED(D2D1CreateFactory(
                    D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    __uuidof(ID2D1Factory1),
                    nullptr,
                    d2dFactory_.PutVoid())) || !d2dFactory_) {
                return false;
            }
        }
        if (!dwriteFactory_) {
            if (FAILED(DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(dwriteFactory_.Put()))) || !dwriteFactory_) {
                return false;
            }
        }
        if (!roundStrokeStyle_) {
            const D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND,
                D2D1_CAP_STYLE_ROUND,
                D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND,
                10.0f,
                D2D1_DASH_STYLE_SOLID,
                0.0f);
            d2dFactory_->CreateStrokeStyle(sp, nullptr, 0U, roundStrokeStyle_.Put());
        }
        if (!tabularTypography_ && dwriteFactory_) {
            if (SUCCEEDED(dwriteFactory_->CreateTypography(tabularTypography_.Put())) && tabularTypography_) {
                tabularTypography_->AddFontFeature(DWRITE_FONT_FEATURE{ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1U });
            }
        }
        if (fBody == nullptr) {
            CreateTextFormats();
        }
        return fBody != nullptr;
    }

    bool FontExists(const wchar_t* family) const noexcept {
        if (!dwriteFactory_ || family == nullptr) {
            return false;
        }
        detail::ComPtr<IDWriteFontCollection> collection;
        if (FAILED(dwriteFactory_->GetSystemFontCollection(collection.Put(), FALSE)) || !collection) {
            return false;
        }
        UINT32 index = 0U;
        BOOL exists = FALSE;
        collection->FindFamilyName(family, &index, &exists);
        return exists != FALSE;
    }

    IDWriteTextFormat* MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                  DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL) noexcept {
        if (!dwriteFactory_) {
            return nullptr;
        }
        detail::ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dwriteFactory_->CreateTextFormat(
                family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, stretch, size, L"en-us", fmt.Put())) || !fmt) {
            return nullptr;
        }
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0U, 0U };
        detail::ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(fmt.Get(), ellipsis.Put())) && ellipsis) {
            fmt->SetTrimming(&trimming, ellipsis.Get());
        }
        return fmt.Detach();
    }

    void CreateTextFormats() noexcept {
        const wchar_t* uiText = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        const wchar_t* uiDisp = FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : uiText;
        const wchar_t* mono   = FontExists(L"Cascadia Mono") ? L"Cascadia Mono"
                              : (FontExists(L"Consolas") ? L"Consolas" : uiText);

        fHero       = MakeFormat(uiDisp, 26.0f, DWRITE_FONT_WEIGHT_BOLD);
        fTitle      = MakeFormat(uiDisp, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        fHeading    = MakeFormat(uiDisp, 15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        fBody       = MakeFormat(uiText, 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
        fBodyStrong = MakeFormat(uiText, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        fSmall      = MakeFormat(uiText, 11.8f, DWRITE_FONT_WEIGHT_NORMAL);
        fLabel      = MakeFormat(uiText, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        fNum        = MakeFormat(uiText, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        fKey        = MakeFormat(mono,   12.8f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        fAxis       = MakeFormat(mono,   11.0f, DWRITE_FONT_WEIGHT_NORMAL);
        fMini       = MakeFormat(uiText, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    void CompileShadersOnce() noexcept {
        if (vsBlob_ && psBlob_) {
            return;
        }
        constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const size_t srcLen = sizeof(detail::kCosmicNebulaHlsl) - 1U;

        detail::ComPtr<ID3DBlob> err;
        if (!vsBlob_) {
            D3DCompile(detail::kCosmicNebulaHlsl, srcLen, "MaskCosmicNebula.hlsl",
                       nullptr, nullptr, "VSMain", "vs_4_0", flags, 0U, vsBlob_.Put(), err.Put());
        }
        err.Reset();
        if (!psBlob_) {
            D3DCompile(detail::kCosmicNebulaHlsl, srcLen, "MaskCosmicNebula.hlsl",
                       nullptr, nullptr, "PSMain", "ps_4_0", flags, 0U, psBlob_.Put(), err.Put());
        }
    }

    bool EnsureDeviceAndSwapChain(HWND hwnd, UINT pixelW, UINT pixelH) noexcept {
        if (d3dDevice_ && d2dContext_ && swapChain_ && d3dRtv_ && d2dTargetBitmap_) {
            return true;
        }
        if (!d3dDevice_) {
            if (!CreateDeviceResources()) {
                return false;
            }
        }
        if (!swapChain_) {
            if (!CreateSwapChainForWindow(hwnd, pixelW, pixelH)) {
                return false;
            }
        }
        if (!d3dRtv_ || !d2dTargetBitmap_) {
            return CreateBackBufferTargets();
        }
        return true;
    }

    bool CreateDeviceResources() noexcept {
        DiscardDeviceResources();

        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        static constexpr D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            kLevels,
            static_cast<UINT>(std::size(kLevels)),
            D3D11_SDK_VERSION,
            d3dDevice_.Put(),
            nullptr,
            d3dContext_.Put());

        if (FAILED(hr)) {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                flags,
                kLevels,
                static_cast<UINT>(std::size(kLevels)),
                D3D11_SDK_VERSION,
                d3dDevice_.Put(),
                nullptr,
                d3dContext_.Put());
        }
        if (FAILED(hr) || !d3dDevice_ || !d3dContext_) {
            DiscardDeviceResources();
            return false;
        }

        if (FAILED(d3dDevice_->QueryInterface(__uuidof(IDXGIDevice3), dxgiDevice3_.PutVoid())) || !dxgiDevice3_) {
            DiscardDeviceResources();
            return false;
        }
        dxgiDevice3_->SetMaximumFrameLatency(1U);

        if (FAILED(d2dFactory_->CreateDevice(dxgiDevice3_.Get(), d2dDevice_.Put())) || !d2dDevice_) {
            DiscardDeviceResources();
            return false;
        }
        if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext_.Put())) || !d2dContext_) {
            DiscardDeviceResources();
            return false;
        }
        d2dContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        d2dContext_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
        d2dContext_->SetDpi(dpi_, dpi_);

        if (FAILED(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), solidBrush_.Put())) || !solidBrush_) {
            DiscardDeviceResources();
            return false;
        }

        // Create GPU shader objects (falling back smoothly to Direct2D gradients if unavailable)
        shaderReady_ = false;
        if (vsBlob_ && psBlob_) {
            const HRESULT vsHr = d3dDevice_->CreateVertexShader(
                vsBlob_->GetBufferPointer(), vsBlob_->GetBufferSize(), nullptr, vs_.Put());
            const HRESULT psHr = d3dDevice_->CreatePixelShader(
                psBlob_->GetBufferPointer(), psBlob_->GetBufferSize(), nullptr, ps_.Put());

            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth      = sizeof(NebulaConstantBuffer);
            cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            const HRESULT cbHr = d3dDevice_->CreateBuffer(&cbDesc, nullptr, cb_.Put());

            D3D11_RASTERIZER_DESC rsDesc{};
            rsDesc.FillMode = D3D11_FILL_SOLID;
            rsDesc.CullMode = D3D11_CULL_NONE;
            rsDesc.DepthClipEnable = FALSE;
            const HRESULT rsHr = d3dDevice_->CreateRasterizerState(&rsDesc, rasterState_.Put());

            shaderReady_ = SUCCEEDED(vsHr) && SUCCEEDED(psHr) && SUCCEEDED(cbHr) && SUCCEEDED(rsHr) &&
                           vs_ && ps_ && cb_ && rasterState_;
        }
        return true;
    }

    bool CreateSwapChainForWindow(HWND hwnd, UINT pixelW, UINT pixelH) noexcept {
        if (!dxgiDevice3_ || !d3dDevice_) {
            return false;
        }
        detail::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice3_->GetAdapter(adapter.Put())) || !adapter) {
            return false;
        }
        detail::ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.PutVoid())) || !factory) {
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        scDesc.Width       = std::max(1U, pixelW);
        scDesc.Height      = std::max(1U, pixelH);
        scDesc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.Stereo      = FALSE;
        scDesc.SampleDesc  = { 1U, 0U };
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2U;
        scDesc.Scaling     = DXGI_SCALING_STRETCH;
        scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
        scDesc.Flags       = 0U;

        if (FAILED(factory->CreateSwapChainForHwnd(
                d3dDevice_.Get(), hwnd, &scDesc, nullptr, nullptr, swapChain_.Put())) || !swapChain_) {
            return false;
        }
        widthPx_  = scDesc.Width;
        heightPx_ = scDesc.Height;
        return CreateBackBufferTargets();
    }

    bool CreateBackBufferTargets() noexcept {
        if (!swapChain_ || !d3dDevice_ || !d2dContext_) {
            return false;
        }
        d2dTargetBitmap_.Reset();
        d3dRtv_.Reset();

        detail::ComPtr<ID3D11Texture2D> d3dBackBuffer;
        if (FAILED(swapChain_->GetBuffer(0U, __uuidof(ID3D11Texture2D), d3dBackBuffer.PutVoid())) || !d3dBackBuffer) {
            return false;
        }
        if (FAILED(d3dDevice_->CreateRenderTargetView(d3dBackBuffer.Get(), nullptr, d3dRtv_.Put())) || !d3dRtv_) {
            return false;
        }

        detail::ComPtr<IDXGISurface> dxgiSurface;
        if (FAILED(swapChain_->GetBuffer(0U, __uuidof(IDXGISurface), dxgiSurface.PutVoid())) || !dxgiSurface) {
            d3dRtv_.Reset();
            return false;
        }
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            dpi_,
            dpi_);
        if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bp, d2dTargetBitmap_.Put())) ||
            !d2dTargetBitmap_) {
            d3dRtv_.Reset();
            return false;
        }
        return true;
    }

    bool ResizeSwapChainInternal(UINT pixelW, UINT pixelH) noexcept {
        if (!swapChain_ || pixelW == 0U || pixelH == 0U) {
            return false;
        }
        if (d2dContext_) {
            d2dContext_->SetTarget(nullptr);
        }
        d2dTargetBitmap_.Reset();
        d3dRtv_.Reset();
        if (d3dContext_) {
            ID3D11RenderTargetView* nullRtv = nullptr;
            d3dContext_->OMSetRenderTargets(1U, &nullRtv, nullptr);
            d3dContext_->ClearState();
            d3dContext_->Flush();
        }

        const HRESULT hr = swapChain_->ResizeBuffers(0U, pixelW, pixelH, DXGI_FORMAT_UNKNOWN, 0U);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            DiscardDeviceResources();
            return EnsureDeviceAndSwapChain(hwnd_, pixelW, pixelH);
        }
        if (FAILED(hr)) {
            return false;
        }
        widthPx_  = pixelW;
        heightPx_ = pixelH;
        return CreateBackBufferTargets();
    }

    void DrawFallbackBackground(float logicalW, float logicalH, UINT32 accentRgb) noexcept {
        if (!d2dContext_) {
            return;
        }
        d2dContext_->Clear(D2D1::ColorF(0x02050BU));
        FillLinearRect(D2D1::RectF(0.0f, 0.0f, logicalW, logicalH), 0.0f, 0x02050BU, 1.0f, 0x07162EU, 0.85f, false);
        FillRadialGlow(logicalW * 0.44f, logicalH * 0.54f, logicalW * 0.42f, logicalH * 0.36f,
                       LerpRgb(0x0E4D92U, accentRgb, 0.35f), 0.36f, 0x02050BU, 0.0f);
    }

    void DiscardSwapChainResources() noexcept {
        if (d2dContext_) {
            d2dContext_->SetTarget(nullptr);
        }
        d2dTargetBitmap_.Reset();
        d3dRtv_.Reset();
        swapChain_.Reset();
        widthPx_ = 0U;
        heightPx_ = 0U;
    }

    void DiscardDeviceResources() noexcept {
        inFrame_ = false;
        clipDepth_ = 0U;
        DiscardSwapChainResources();
        solidBrush_.Reset();
        d2dContext_.Reset();
        d2dDevice_.Reset();
        rasterState_.Reset();
        cb_.Reset();
        ps_.Reset();
        vs_.Reset();
        dxgiDevice3_.Reset();
        d3dContext_.Reset();
        d3dDevice_.Reset();
        shaderReady_ = false;
    }

    HWND  hwnd_{nullptr};
    float dpi_{96.0f};
    UINT  widthPx_{0U};
    UINT  heightPx_{0U};
    bool  inFrame_{false};
    bool  shaderReady_{false};

    bool   frameFocused_{true};
    float  frameCursorX_{0.0f};
    float  frameCursorY_{0.0f};
    bool   frameCursorIn_{false};
    UINT32 frameAccentRgb_{0x38BDF8U};
    UINT32 frameAccentHiRgb_{0x7DD3FCU};

    std::array<D2D1_RECT_F, 8> clipStack_{};
    size_t                     clipDepth_{0U};

    // Device-independent resources
    detail::ComPtr<ID2D1Factory1>     d2dFactory_{};
    detail::ComPtr<IDWriteFactory>    dwriteFactory_{};
    detail::ComPtr<ID2D1StrokeStyle>  roundStrokeStyle_{};
    detail::ComPtr<IDWriteTypography> tabularTypography_{};
    detail::ComPtr<ID3DBlob>          vsBlob_{};
    detail::ComPtr<ID3DBlob>          psBlob_{};

    // Device-dependent resources
    detail::ComPtr<ID3D11Device>           d3dDevice_{};
    detail::ComPtr<ID3D11DeviceContext>    d3dContext_{};
    detail::ComPtr<IDXGIDevice3>           dxgiDevice3_{};
    detail::ComPtr<ID3D11VertexShader>     vs_{};
    detail::ComPtr<ID3D11PixelShader>      ps_{};
    detail::ComPtr<ID3D11Buffer>           cb_{};
    detail::ComPtr<ID3D11RasterizerState>  rasterState_{};
    detail::ComPtr<IDXGISwapChain1>        swapChain_{};
    detail::ComPtr<ID3D11RenderTargetView> d3dRtv_{};
    detail::ComPtr<ID2D1Device>            d2dDevice_{};
    detail::ComPtr<ID2D1DeviceContext>     d2dContext_{};
    detail::ComPtr<ID2D1Bitmap1>           d2dTargetBitmap_{};
    detail::ComPtr<ID2D1SolidColorBrush>   solidBrush_{};
};

} // namespace mask::ui
