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

#include <array>
#include <cstddef>
#include <cstdint>

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
    ~GpuCanvas() noexcept;

    GpuCanvas(const GpuCanvas&) = delete;
    GpuCanvas& operator=(const GpuCanvas&) = delete;

    bool Initialize(HWND hwnd) noexcept;
    void Shutdown() noexcept;
    void Resize(UINT pixelW, UINT pixelH) noexcept;
    void SetDpi(float dpi) noexcept;

    /// Drops DXGI/D3D11 staging and intermediate buffers when window minimizes or hides to tray.
    void TrimGpu() noexcept;

    [[nodiscard]] bool BeginFrame(HWND hwnd, UINT pixelW, UINT pixelH, float dpi,
                                  bool focused, float ambientSec,
                                  float cursorX, float cursorY, bool cursorIn,
                                  UINT32 accentRgb, UINT32 accentHiRgb) noexcept;
    bool EndFrame(bool vsync = true) noexcept;

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

    [[nodiscard]] bool CurrentClip(D2D1_RECT_F& out) const noexcept;
    void SetTransform(const D2D1_MATRIX_3X2_F& m) noexcept;
    void ResetTransform() noexcept;

    // ---------------------------------------------------------------------------------
    //  Core Primitives
    // ---------------------------------------------------------------------------------

    void PushClip(const D2D1_RECT_F& r, bool antialiased = false) noexcept;
    void PopClip() noexcept;

    void FillRoundRect(const D2D1_RECT_F& r, float radius, UINT32 rgb, float alpha = 1.0f) noexcept;
    void FillRectRgba(const D2D1_RECT_F& r, float radius, UINT32 rgba, float alphaMul = 1.0f) noexcept;
    void StrokeRoundRect(const D2D1_RECT_F& r, float radius, UINT32 rgb,
                         float width = 1.0f, float alpha = 1.0f) noexcept;
    void StrokeRectRgba(const D2D1_RECT_F& r, float radius, UINT32 rgba,
                        float width = 1.0f, float alphaMul = 1.0f) noexcept;

    void FillCircle(float cx, float cy, float radius, UINT32 rgb, float alpha = 1.0f) noexcept;
    void StrokeCircle(float cx, float cy, float radius, UINT32 rgb,
                      float width = 1.0f, float alpha = 1.0f) noexcept;
    void Line(float x0, float y0, float x1, float y1, UINT32 rgb,
              float width = 1.0f, float alpha = 1.0f) noexcept;

    void Text(const wchar_t* text, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, UINT32 rgb,
              DWRITE_TEXT_ALIGNMENT h = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT v = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
              float alpha = 1.0f) noexcept;
    [[nodiscard]] float MeasureWidth(const wchar_t* text, IDWriteTextFormat* fmt) const noexcept;

    // ---------------------------------------------------------------------------------
    //  High-Craft Cosmic Glass, Gradients, Bloom & Micro-Interaction Helpers
    // ---------------------------------------------------------------------------------

    void FillLinearRect(const D2D1_RECT_F& r, float radius,
                        UINT32 colorTop, float aTop,
                        UINT32 colorBot, float aBot,
                        bool horizontal = false) noexcept;
    void FillRadialGlow(float cx, float cy, float rx, float ry,
                        UINT32 centerColor, float centerAlpha,
                        UINT32 edgeColor, float edgeAlpha) noexcept;

    /// Dark obsidian translucent glass card with vertical gradient, inner top specular highlight,
    /// crisp 1px border, and interactive cursor-tracked border/surface sheen when focused.
    void FillGlassCard(const D2D1_RECT_F& r, float radius,
                       UINT32 fillTopRgba, UINT32 fillBotRgba, UINT32 borderRgba,
                       float topSpecularAlpha, float cursorX, float cursorY,
                       bool hoverHighlight, UINT32 glowRgb) noexcept;

    /// Multi-pass glowing vector segment (oscilloscope traces, active rails, trigger waveforms).
    void DrawGlowSegment(float x0, float y0, float x1, float y1,
                         UINT32 color, float coreWidth = 1.8f,
                         float glowWidth = 7.0f, float alpha = 1.0f) noexcept;

    /// Dark sapphire pill toggle switch that illuminates with gradient, bloom halo, and floating icy thumb.
    void DrawToggleSwitch(const D2D1_RECT_F& r, float slideT, float glowT,
                          UINT32 accentRgb, UINT32 accentHiRgb,
                          bool hovered = false, float alpha = 1.0f) noexcept;

    /// Expanding cosmic wavefront rings from recent clicks.
    void DrawClickRipples(const std::array<ClickRipple, UiAnimator::kRippleCount>& ripples) noexcept;

private:
    std::array<IDWriteTextFormat**, 11> AllFormats() noexcept;

    bool EnsureFactoriesAndFonts() noexcept;
    bool FontExists(const wchar_t* family) const noexcept;
    IDWriteTextFormat* MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                  DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL) noexcept;
    void CreateTextFormats() noexcept;
    void CompileShadersOnce() noexcept;
    bool EnsureDeviceAndSwapChain(HWND hwnd, UINT pixelW, UINT pixelH) noexcept;
    bool CreateDeviceResources() noexcept;
    bool CreateSwapChainForWindow(HWND hwnd, UINT pixelW, UINT pixelH) noexcept;
    bool CreateBackBufferTargets() noexcept;
    bool ResizeSwapChainInternal(UINT pixelW, UINT pixelH) noexcept;
    void DrawFallbackBackground(float logicalW, float logicalH, UINT32 accentRgb) noexcept;
    void DiscardSwapChainResources() noexcept;
    void DiscardDeviceResources() noexcept;

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
