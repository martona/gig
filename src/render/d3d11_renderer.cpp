#include "render/grid_layout.h"
#include "render/text_overlay.h"
#include "render/video_renderer.h"

#include "log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl3.h>

using Microsoft::WRL::ComPtr;

namespace {

struct Vertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr std::array<Vertex, 4> QuadVertices = {
    Vertex { -1.0f, 1.0f, 0.0f, 0.0f },
    Vertex { 1.0f, 1.0f, 1.0f, 0.0f },
    Vertex { -1.0f, -1.0f, 0.0f, 1.0f },
    Vertex { 1.0f, -1.0f, 1.0f, 1.0f },
};

// Logical (DPI-independent) base font height in pixels; multiplied by the window's
// display scale at init. ~16 at 100%, ~32 at 200% (the hand-tuned look). Used for
// both the overlay atlas and the ImGui log font -- bump this one knob to taste.
constexpr float kBaseFontPx = 16.0f;

// Ease curve (smooth in/out) for the click-to-zoom tile animation.
float smoothstep01(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

const char* VertexShaderSource = R"(
struct VSIn {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSIn VSMain(VSIn input) {
    PSIn output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}
)";

const char* BgraPixelShaderSource = R"(
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSIn input) : SV_TARGET {
    return frameTexture.Sample(frameSampler, input.uv);
}
)";

const char* Nv12PixelShaderSource = R"(
Texture2D yTexture : register(t0);
Texture2D uvTexture : register(t1);
SamplerState frameSampler : register(s0);

cbuffer ColorMatrix : register(b0) {
    float4 yuvToRgbR;
    float4 yuvToRgbG;
    float4 yuvToRgbB;
    float4 unusedRow;
};

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSIn input) : SV_TARGET {
    float y = yTexture.Sample(frameSampler, input.uv).r;
    float2 uv = uvTexture.Sample(frameSampler, input.uv).rg;
    float4 sample = float4(y, uv.x, uv.y, 1.0f);
    return float4(
        saturate(dot(yuvToRgbR, sample)),
        saturate(dot(yuvToRgbG, sample)),
        saturate(dot(yuvToRgbB, sample)),
        1.0f);
}
)";

const char* Yuv420PixelShaderSource = R"(
Texture2D yTexture : register(t0);
Texture2D uTexture : register(t1);
Texture2D vTexture : register(t2);
SamplerState frameSampler : register(s0);

cbuffer ColorMatrix : register(b0) {
    float4 yuvToRgbR;
    float4 yuvToRgbG;
    float4 yuvToRgbB;
    float4 unusedRow;
};

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSIn input) : SV_TARGET {
    float y = yTexture.Sample(frameSampler, input.uv).r;
    float u = uTexture.Sample(frameSampler, input.uv).r;
    float v = vTexture.Sample(frameSampler, input.uv).r;
    float4 sample = float4(y, u, v, 1.0f);
    return float4(
        saturate(dot(yuvToRgbR, sample)),
        saturate(dot(yuvToRgbG, sample)),
        saturate(dot(yuvToRgbB, sample)),
        1.0f);
}
)";

// Procedural "signal" shader for tiles with no displayable frame yet (waiting for
// a keyframe, reconnecting, or offline). It's a scope: a trace whose liveliness
// is driven by uEnergy (the real download rate), so a stream that's receiving but
// pre-keyframe reads alive/calm, and a stuck one goes cold with a searching sweep.
const char* SignalPixelShaderSource = R"(
cbuffer SignalConstants : register(b0) {
    float uTime;
    float uEnergy;
    float uSeed;
    float uAspect;
    float uAlpha;
    float3 _pad;
};

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float hash11(float n) { return frac(sin(n) * 43758.5453); }

float traceWave(float x, float t, float e, float seed) {
    // Calm, slow, low-frequency shape -- what you see when little/no data arrives.
    float smoothW = sin(x * 7.0 + t * 1.3 + seed) * 0.6
                  + sin(x * 3.0 - t * 0.9 + seed * 0.5) * 0.4;
    // Wild, fast, jagged shape -- what you see when data is pouring in.
    float chaosW = sin(x * 30.0 + t * 20.0 + seed * 2.3) * 0.5
                 + sin(x * 61.0 - t * 31.0 + seed) * 0.3
                 + (hash11(floor(x * 120.0) + floor(t * 55.0) * 7.0) - 0.5) * 1.0;
    float shape = lerp(smoothW, chaosW, e); // morph smooth -> crazy with energy
    float a = 0.04 + e * 0.34;              // resting height -> crazy height
    return shape * a;
}

float4 PSMain(PSIn input) : SV_TARGET {
    float2 uv = input.uv;
    float t = uTime;
    float e = saturate(uEnergy);

    float3 col = float3(0.043, 0.051, 0.071);
    col += smoothstep(0.004, 0.0, abs(uv.y - 0.5)) * 0.05; // faint baseline

    float3 cold = float3(0.88, 0.62, 0.16);
    float3 hot  = float3(0.27, 0.80, 0.66);
    float3 traceCol = lerp(cold, hot, saturate(e * 1.4));

    float w = traceWave(uv.x, t, e, uSeed);
    float d = abs(uv.y - 0.5 - w);
    float core = smoothstep(0.018, 0.0, d);
    float glow = smoothstep(0.085, 0.0, d) * 0.22;
    col += traceCol * (core + glow);

    float head = smoothstep(0.03, 0.0, distance(uv, float2(0.985, 0.5 + w))) * e;
    col += traceCol * head;

    float coldness = 1.0 - saturate(e / 0.18);
    if (coldness > 0.001) {
        float sweepX = frac(t * 0.5 + uSeed * 0.37);
        float beam = smoothstep(0.012, 0.0, abs(uv.x - sweepX));
        float behind = sweepX - uv.x;
        float trail = (behind > 0.0) ? saturate(1.0 - behind / 0.16) * 0.16 : 0.0;
        col += cold * (beam * 0.6 + trail) * coldness;
    }

    return float4(col, uAlpha);
}
)";

// Hover affordance for clickable tiles: a thin soft border plus a very faint
// overall brighten, drawn over the hovered tile so it reads as "hot".
const char* HoverPixelShaderSource = R"(
cbuffer HoverConstants : register(b0) {
    float2 uSizePx;   // tile size in pixels
    float uBorderPx;  // border thickness in pixels
    float uPad;
    float4 uColor;    // rgb + max border alpha
};

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSIn input) : SV_TARGET {
    float2 px = input.uv * uSizePx;
    float2 dEdge = min(px, uSizePx - px);
    float edge = min(dEdge.x, dEdge.y);
    float border = 1.0 - smoothstep(uBorderPx - 1.0, uBorderPx + 1.0, edge);
    float a = saturate(border + 0.06) * uColor.a; // border + faint fill
    return float4(uColor.rgb, a);
}
)";

struct ColorMatrixConstants {
    float r[4];
    float g[4];
    float b[4];
    float unused[4];
};

struct SignalConstants {
    float time;
    float energy;
    float seed;
    float aspect;
    float alpha;
    float pad[3];
};

struct HoverConstants {
    float sizePx[2];
    float borderPx;
    float pad;
    float color[4];
};

ColorMatrixConstants yuvToRgbMatrix(bool fullRange)
{
    if (fullRange) {
        return {
            { 1.0f, 0.0f, 1.5748f, -0.7874f },
            { 1.0f, -0.1873f, -0.4681f, 0.3277f },
            { 1.0f, 1.8556f, 0.0f, -0.9278f },
            { 0.0f, 0.0f, 0.0f, 0.0f },
        };
    }

    return {
        { 1.164383f, 0.0f, 1.792741f, -0.969430f },
        { 1.164383f, -0.213249f, -0.532909f, 0.300047f },
        { 1.164383f, 2.112402f, 0.0f, -1.129253f },
        { 0.0f, 0.0f, 0.0f, 0.0f },
    };
}

bool failed(HRESULT result, const char* action)
{
    if (SUCCEEDED(result)) {
        return false;
    }

    std::cerr << action << " failed: 0x" << std::hex << static_cast<unsigned long>(result) << std::dec << "\n";
    return true;
}

// "<%WINDIR%>\Fonts\<fileName>" as a UTF-8 path (ImGui wants UTF-8). Resolves the
// real Windows directory instead of assuming C:.
std::string windowsFontPath(const char* fileName)
{
    wchar_t winDir[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(winDir, MAX_PATH);
    const std::wstring dir = (length > 0 && length < MAX_PATH) ? std::wstring(winDir, length) : L"C:\\Windows";
    const std::wstring widePath = dir + L"\\Fonts\\";

    std::string utf8;
    const int needed = WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        utf8.resize(static_cast<std::size_t>(needed) - 1);
        WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
    }
    return utf8 + fileName;
}

class D3D11Renderer final : public VideoRenderer {
public:
    bool initialize(SDL_Window* window) override
    {
        window_ = window;
        decodeContext_->lock = std::make_shared<std::recursive_mutex>();

        SDL_PropertiesID properties = SDL_GetWindowProperties(window_);
        if (!properties) {
            std::cerr << "SDL_GetWindowProperties failed: " << SDL_GetError() << "\n";
            return false;
        }

        hwnd_ = static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd_) {
            std::cerr << "SDL window has no Win32 HWND property.\n";
            return false;
        }

        readClientSize();

        dpiScale_ = SDL_GetWindowDisplayScale(window_);
        if (!(dpiScale_ > 0.0f)) {
            dpiScale_ = 1.0f;
        }
        gig::logInfo() << "display scale: " << dpiScale_;

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.BufferDesc.Width = static_cast<UINT>(backBufferWidth_);
        swapChainDesc.BufferDesc.Height = static_cast<UINT>(backBufferHeight_);
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = hwnd_;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
        };

        D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &swapChainDesc,
            swapChain_.GetAddressOf(),
            device_.GetAddressOf(),
            &selectedFeatureLevel,
            context_.GetAddressOf());

        if (failed(result, "D3D11CreateDeviceAndSwapChain")) {
            return false;
        }

        decodeContext_->device = device_.Get();
        if (!createRenderTarget() || !createPipeline()) {
            return false;
        }
        const int overlayFontPx = std::max(8, static_cast<int>(kBaseFontPx * dpiScale_ + 0.5f));
        if (!overlay_.initialize(device_.Get(), overlayFontPx)) {
            gig::logWarning() << "Text overlay unavailable; labels and diagnostics disabled.";
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr; // don't write/read an imgui.ini
        loadImguiFont();
        applyImguiStyle();
        if (ImGui_ImplSDL3_InitForD3D(window_) && ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
            imguiReady_ = true;
        } else {
            gig::logWarning() << "ImGui init failed; log view disabled.";
            ImGui::DestroyContext();
        }
        return true;
    }

    ~D3D11Renderer() override
    {
        if (imguiReady_) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
    }

    void resize() override
    {
        if (!swapChain_ || !context_) {
            return;
        }

        auto d3dLock = lockD3D11();
        readClientSize();
        if (backBufferWidth_ <= 0 || backBufferHeight_ <= 0) {
            return;
        }

        context_->OMSetRenderTargets(0, nullptr, nullptr);
        renderTargetView_.Reset();

        HRESULT result = swapChain_->ResizeBuffers(
            0,
            static_cast<UINT>(backBufferWidth_),
            static_cast<UINT>(backBufferHeight_),
            DXGI_FORMAT_UNKNOWN,
            0);

        if (failed(result, "IDXGISwapChain::ResizeBuffers")) {
            return;
        }

        createRenderTarget();
    }

    void render(const std::vector<std::shared_ptr<VideoFrame>>& frames) override
    {
        if (!context_ || !swapChain_ || !renderTargetView_) {
            return;
        }

        {
            auto d3dLock = lockD3D11();

            // Set true by the tile loops when a frameless tile's signal scope or a
            // resolve fade draws this frame; folded into animating_ at the end.
            sawAnimatedContent_ = false;

            if (dpiDirty_) {
                dpiDirty_ = false;
                applyDpiScale();
            }

            if (tiles_.size() != frames.size()) {
                tiles_.resize(frames.size());
            }

            // Advance the animation clock and ease each tile's activity from the
            // latest byte counts (set just before this call by the run loop).
            const auto nowTp = std::chrono::steady_clock::now();
            float dt = haveRenderTp_ ? std::chrono::duration<float>(nowTp - lastRenderTp_).count() : 0.0f;
            lastRenderTp_ = nowTp;
            haveRenderTp_ = true;
            dt = std::clamp(dt, 0.0f, 0.1f);
            lastDt_ = dt;
            animTime_ += dt;
            updateActivity(dt);

            // Ease the zoom animation toward its target (1 = focused, 0 = grid).
            const float zoomTarget = (focusedTile_ >= 0) ? 1.0f : 0.0f;
            if (animProgress_ != zoomTarget) {
                const float step = (kZoomDuration > 0.0f) ? dt / kZoomDuration : 1.0f;
                animProgress_ = (animProgress_ < zoomTarget)
                    ? std::min(zoomTarget, animProgress_ + step)
                    : std::max(zoomTarget, animProgress_ - step);
            }

            const float clearColor[] = { 0.01f, 0.01f, 0.012f, 1.0f };
            context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), nullptr);

            D3D11_VIEWPORT fullViewport = {};
            fullViewport.TopLeftX = 0.0f;
            fullViewport.TopLeftY = 0.0f;
            fullViewport.Width = static_cast<float>(backBufferWidth_);
            fullViewport.Height = static_cast<float>(backBufferHeight_);
            fullViewport.MinDepth = 0.0f;
            fullViewport.MaxDepth = 1.0f;
            context_->RSSetViewports(1, &fullViewport);
            context_->ClearRenderTargetView(renderTargetView_.Get(), clearColor);

            const bool fullyFocused = focusedTile_ >= 0 && animProgress_ >= 1.0f
                && focusedTile_ < static_cast<int>(frames.size());
            if (fullyFocused) {
                renderSingleTile(static_cast<std::size_t>(focusedTile_), frames);
                if (hoveredTile_ == focusedTile_) {
                    drawHover(gig::TileRect {
                        0.0f, 0.0f, static_cast<float>(backBufferWidth_), static_cast<float>(backBufferHeight_) });
                }
                context_->RSSetViewports(1, &fullViewport);
                overlay_.begin(static_cast<int>(backBufferWidth_), static_cast<int>(backBufferHeight_));
                if (labelVisible(static_cast<std::size_t>(focusedTile_))) {
                    drawTileLabel(
                        gig::TileRect { 0.0f, 0.0f, static_cast<float>(backBufferWidth_), static_cast<float>(backBufferHeight_) },
                        labelFor(static_cast<std::size_t>(focusedTile_)),
                        2.0f);
                }
                overlay_.flush(context_.Get());
            } else {
                // Reserve the top strip for the toolbar so the grid sits below it
                // (undisturbed video). In focus view the toolbar auto-hides and the
                // image is full-bleed, so no reservation there.
                const int toolbarTop = static_cast<int>(toolbarHeightPx());
                const int gridHeight = std::max(1, static_cast<int>(backBufferHeight_) - toolbarTop);
                // Reserve one extra cell for the synthetic diagnostics tile.
                const bool showDiagnostics = overlayStats_.showDiagnostics;
                const int effectiveCount = static_cast<int>(frames.size()) + (showDiagnostics ? 1 : 0);
                gig::GridLayout layout = gig::computeGridLayout(
                    effectiveCount,
                    static_cast<int>(backBufferWidth_),
                    gridHeight);
                for (gig::TileRect& tile : layout.tiles) {
                    tile.y += static_cast<float>(toolbarTop);
                }
                renderGridTiles(frames, layout);

                // Zoom transition: a tile growing out of (or shrinking back into)
                // its grid cell, drawn over the grid. At progress 1 we switch to
                // the static fullscreen path above, so the hand-off is seamless.
                if (animProgress_ > 0.0f && animTile_ >= 0
                    && animTile_ < static_cast<int>(frames.size())
                    && animTile_ < static_cast<int>(layout.tiles.size())) {
                    const gig::TileRect& cell = layout.tiles[static_cast<std::size_t>(animTile_)];
                    const float e = smoothstep01(animProgress_);
                    const gig::TileRect grown {
                        std::lerp(cell.x, 0.0f, e),
                        std::lerp(cell.y, 0.0f, e),
                        std::lerp(cell.width, static_cast<float>(backBufferWidth_), e),
                        std::lerp(cell.height, static_cast<float>(backBufferHeight_), e),
                    };
                    drawTileContentAt(static_cast<std::size_t>(animTile_), grown);
                }

                // Hover affordance on the tile under the mouse (not while zooming,
                // where the layout is in motion).
                if (animProgress_ == 0.0f && hoveredTile_ >= 0
                    && hoveredTile_ < static_cast<int>(frames.size())
                    && hoveredTile_ < static_cast<int>(layout.tiles.size())) {
                    drawHover(layout.tiles[static_cast<std::size_t>(hoveredTile_)]);
                }

                context_->RSSetViewports(1, &fullViewport);
                overlay_.begin(static_cast<int>(backBufferWidth_), static_cast<int>(backBufferHeight_));
                for (std::size_t i = 0; i < frames.size() && i < layout.tiles.size(); ++i) {
                    if (labelVisible(i)) {
                        drawTileLabel(layout.tiles[i], labelFor(i), 1.0f);
                    }
                }
                if (showDiagnostics && frames.size() < layout.tiles.size()) {
                    drawDiagnosticsTile(layout.tiles[frames.size()]);
                }
                overlay_.flush(context_.Get());
            }

            renderImGui();

            // Record whether anything is still animating, so the run loop knows to
            // keep rendering on demand: a zoom transition in flight, a frameless
            // tile's signal scope / resolve fade (sawAnimatedContent_), or the
            // focus-view toolbar still counting down to its auto-hide.
            const float zoomTargetNow = (focusedTile_ >= 0) ? 1.0f : 0.0f;
            const bool zoomAnimating = animProgress_ != zoomTargetNow;
            const bool toolbarAnimating = focusedTile_ >= 0 && toolbarIdle_ < kToolbarHideDelay;
            animating_ = sawAnimatedContent_ || zoomAnimating || toolbarAnimating;

            // Non-waiting Present: submits and returns immediately so the shared
            // D3D11 lock is never held across a vsync wait (which would starve
            // the decoder threads). The render loop paces itself instead.
            const HRESULT presentResult = swapChain_->Present(0, 0);
            if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET) {
                std::cerr << "D3D device was lost.\n";
            }
        }
    }

    std::shared_ptr<D3D11DecodeContext> d3d11DecodeContext() const override
    {
        return decodeContext_;
    }

    void setFocusedTile(int index) override
    {
        if (index == focusedTile_) {
            return;
        }
        // Remember the tile in motion: the target when zooming in, the outgoing
        // one when zooming back to the grid. animProgress_ keeps its current value
        // and eases toward the new target in render(), so clicking mid-zoom just
        // reverses smoothly.
        animTile_ = (index >= 0) ? index : focusedTile_;
        focusedTile_ = index;
    }

    int focusedTile() const override
    {
        return focusedTile_;
    }

    void setCameraLabels(const std::vector<std::string>& labels) override
    {
        cameraLabels_ = labels;
    }

    void setDiagnostics(const OverlayStats& stats) override
    {
        overlayStats_ = stats;
    }

    void setTileActivity(const std::vector<std::uint64_t>& byteCounts) override
    {
        tileBytes_ = byteCounts;
    }

    void setLabelMode(LabelMode mode) override
    {
        labelMode_ = mode;
    }

    void setHoveredTile(int index) override
    {
        hoveredTile_ = index;
    }

    bool handleEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
            dpiDirty_ = true; // re-baked in render() under the D3D lock
        }
        if (!imguiReady_) {
            return false;
        }
        ImGui_ImplSDL3_ProcessEvent(&event);
        const ImGuiIO& io = ImGui::GetIO();
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return io.WantCaptureMouse;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
            return io.WantCaptureKeyboard;
        default:
            return false;
        }
    }

    void setLogViewVisible(bool visible) override { logViewVisible_ = visible; }
    bool logViewVisible() const override { return logViewVisible_; }

    ToolbarAction takeToolbarAction() override
    {
        const ToolbarAction action = pendingToolbarAction_;
        pendingToolbarAction_ = ToolbarAction::None;
        return action;
    }

    float reservedTopLogical() const override
    {
        // The toolbar reserves space only in the grid view; focus view is full-bleed.
        return focusedTile_ < 0 ? kToolbarLogicalHeight : 0.0f;
    }

    bool isAnimating() const override { return animating_; }

private:
    // Per-camera GPU state. One frame's worth of textures/views plus the cached
    // identity of what was last uploaded, so each tile re-uploads only on change.
    struct TileState {
        std::array<ComPtr<ID3D11Texture2D>, 3> planeTextures;
        std::array<ComPtr<ID3D11ShaderResourceView>, 3> planeViews;
        std::array<int, 3> planeWidths = {};
        std::array<int, 3> planeHeights = {};
        std::array<DXGI_FORMAT, 3> planeFormats = {
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,
        };

        VideoFrameFormat textureFormat = VideoFrameFormat::BGRA;
        int textureWidth = 0;
        int textureHeight = 0;
        bool textureFullRange = false;
        std::uint64_t uploadedFrameIndex = 0;
        std::shared_ptr<void> activeFrameOwner;
        ID3D11Texture2D* d3d11SourceTexture = nullptr;
        int d3d11SourceSlice = 0;

        // Signal-animation state (for tiles with no displayable frame).
        float signalEnergy = 0.0f;          // smoothed download activity, 0..1
        std::uint64_t signalLastBytes = 0;  // last sampled cumulative byte count
        bool showedSignal = false;          // signal was drawn last frame (for the resolve fade)
        float frameFade = -1.0f;            // >=0 while crossfading signal out over the first frame
    };

    std::unique_lock<std::recursive_mutex> lockD3D11() const
    {
        if (decodeContext_ && decodeContext_->lock) {
            return std::unique_lock<std::recursive_mutex>(*decodeContext_->lock);
        }

        return {};
    }

    void readClientSize()
    {
        RECT rect = {};
        GetClientRect(hwnd_, &rect);
        backBufferWidth_ = std::max(1L, rect.right - rect.left);
        backBufferHeight_ = std::max(1L, rect.bottom - rect.top);
    }

    bool createRenderTarget()
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (failed(result, "IDXGISwapChain::GetBuffer")) {
            return false;
        }

        result = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTargetView_.GetAddressOf());
        return !failed(result, "ID3D11Device::CreateRenderTargetView");
    }

    bool compileShader(const char* source, const char* entry, const char* target, ComPtr<ID3DBlob>& shader)
    {
        ComPtr<ID3DBlob> errors;
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
        const HRESULT result = D3DCompile(
            source,
            std::strlen(source),
            nullptr,
            nullptr,
            nullptr,
            entry,
            target,
            flags,
            0,
            shader.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(result)) {
            if (errors) {
                std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
            }
            failed(result, "D3DCompile");
            return false;
        }

        return true;
    }

    bool createPipeline()
    {
        ComPtr<ID3DBlob> vertexShaderBlob;
        ComPtr<ID3DBlob> bgraPixelShaderBlob;
        ComPtr<ID3DBlob> nv12PixelShaderBlob;
        ComPtr<ID3DBlob> yuv420PixelShaderBlob;
        ComPtr<ID3DBlob> signalPixelShaderBlob;
        ComPtr<ID3DBlob> hoverPixelShaderBlob;
        if (!compileShader(VertexShaderSource, "VSMain", "vs_4_0", vertexShaderBlob)
            || !compileShader(BgraPixelShaderSource, "PSMain", "ps_4_0", bgraPixelShaderBlob)
            || !compileShader(Nv12PixelShaderSource, "PSMain", "ps_4_0", nv12PixelShaderBlob)
            || !compileShader(Yuv420PixelShaderSource, "PSMain", "ps_4_0", yuv420PixelShaderBlob)
            || !compileShader(SignalPixelShaderSource, "PSMain", "ps_4_0", signalPixelShaderBlob)
            || !compileShader(HoverPixelShaderSource, "PSMain", "ps_4_0", hoverPixelShaderBlob)) {
            return false;
        }

        HRESULT result = device_->CreateVertexShader(
            vertexShaderBlob->GetBufferPointer(),
            vertexShaderBlob->GetBufferSize(),
            nullptr,
            vertexShader_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreateVertexShader")) {
            return false;
        }

        result = device_->CreatePixelShader(
            bgraPixelShaderBlob->GetBufferPointer(),
            bgraPixelShaderBlob->GetBufferSize(),
            nullptr,
            bgraPixelShader_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreatePixelShader(BGRA)")) {
            return false;
        }

        result = device_->CreatePixelShader(
            nv12PixelShaderBlob->GetBufferPointer(),
            nv12PixelShaderBlob->GetBufferSize(),
            nullptr,
            nv12PixelShader_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreatePixelShader(NV12)")) {
            return false;
        }

        result = device_->CreatePixelShader(
            yuv420PixelShaderBlob->GetBufferPointer(),
            yuv420PixelShaderBlob->GetBufferSize(),
            nullptr,
            yuv420PixelShader_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreatePixelShader(YUV420P)")) {
            return false;
        }

        result = device_->CreatePixelShader(
            signalPixelShaderBlob->GetBufferPointer(),
            signalPixelShaderBlob->GetBufferSize(),
            nullptr,
            signalPixelShader_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreatePixelShader(Signal)")) {
            return false;
        }

        result = device_->CreatePixelShader(
            hoverPixelShaderBlob->GetBufferPointer(),
            hoverPixelShaderBlob->GetBufferSize(),
            nullptr,
            hoverPixelShader_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreatePixelShader(Hover)")) {
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC inputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };

        result = device_->CreateInputLayout(
            inputElements,
            static_cast<UINT>(std::size(inputElements)),
            vertexShaderBlob->GetBufferPointer(),
            vertexShaderBlob->GetBufferSize(),
            inputLayout_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreateInputLayout")) {
            return false;
        }

        D3D11_BUFFER_DESC vertexBufferDesc = {};
        vertexBufferDesc.ByteWidth = static_cast<UINT>(sizeof(Vertex) * QuadVertices.size());
        vertexBufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vertexData = {};
        vertexData.pSysMem = QuadVertices.data();

        result = device_->CreateBuffer(&vertexBufferDesc, &vertexData, vertexBuffer_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreateBuffer")) {
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        result = device_->CreateSamplerState(&samplerDesc, sampler_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreateSamplerState")) {
            return false;
        }

        D3D11_BUFFER_DESC colorMatrixDesc = {};
        colorMatrixDesc.ByteWidth = sizeof(ColorMatrixConstants);
        colorMatrixDesc.Usage = D3D11_USAGE_DEFAULT;
        colorMatrixDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        result = device_->CreateBuffer(&colorMatrixDesc, nullptr, colorMatrixBuffer_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreateBuffer(ColorMatrix)")) {
            return false;
        }

        D3D11_BUFFER_DESC signalDesc = {};
        signalDesc.ByteWidth = sizeof(SignalConstants);
        signalDesc.Usage = D3D11_USAGE_DEFAULT;
        signalDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device_->CreateBuffer(&signalDesc, nullptr, signalConstantBuffer_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreateBuffer(Signal)")) {
            return false;
        }

        D3D11_BUFFER_DESC hoverDesc = {};
        hoverDesc.ByteWidth = sizeof(HoverConstants);
        hoverDesc.Usage = D3D11_USAGE_DEFAULT;
        hoverDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device_->CreateBuffer(&hoverDesc, nullptr, hoverConstantBuffer_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreateBuffer(Hover)")) {
            return false;
        }

        // Straight alpha blending, used for the signal->frame crossfade and the
        // hover highlight.
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        result = device_->CreateBlendState(&blendDesc, alphaBlendState_.GetAddressOf());
        return !failed(result, "ID3D11Device::CreateBlendState");
    }

    void updateColorMatrix(bool fullRange)
    {
        const ColorMatrixConstants constants = yuvToRgbMatrix(fullRange);
        context_->UpdateSubresource(colorMatrixBuffer_.Get(), 0, nullptr, &constants, 0, 0);
    }

    void resetFrameTextures(TileState& tile)
    {
        for (std::size_t i = 0; i < tile.planeTextures.size(); ++i) {
            tile.planeTextures[i].Reset();
            tile.planeViews[i].Reset();
            tile.planeWidths[i] = 0;
            tile.planeHeights[i] = 0;
            tile.planeFormats[i] = DXGI_FORMAT_UNKNOWN;
        }
        tile.activeFrameOwner.reset();
        tile.d3d11SourceTexture = nullptr;
        tile.d3d11SourceSlice = 0;
    }

    bool createD3D11FramePlaneView(
        ID3D11Texture2D* texture,
        const D3D11_TEXTURE2D_DESC& textureDesc,
        int arraySlice,
        DXGI_FORMAT format,
        ComPtr<ID3D11ShaderResourceView>& view)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = format;
        if (textureDesc.ArraySize > 1) {
            if (arraySlice < 0 || static_cast<UINT>(arraySlice) >= textureDesc.ArraySize) {
                std::cerr << "D3D11 decoded frame has invalid texture array slice.\n";
                return false;
            }

            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MostDetailedMip = 0;
            viewDesc.Texture2DArray.MipLevels = 1;
            viewDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(arraySlice);
            viewDesc.Texture2DArray.ArraySize = 1;
        } else {
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MostDetailedMip = 0;
            viewDesc.Texture2D.MipLevels = 1;
        }

        HRESULT result = device_->CreateShaderResourceView(texture, &viewDesc, view.GetAddressOf());
        return !failed(result, "ID3D11Device::CreateShaderResourceView(D3D11 frame)");
    }

    bool useD3D11FrameTexture(TileState& tile, const VideoFrame& frame)
    {
        if (!frame.d3d11Texture) {
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDesc = {};
        frame.d3d11Texture->GetDesc(&textureDesc);
        if (textureDesc.Format != DXGI_FORMAT_NV12) {
            std::cerr << "D3D11 decoded frame is not NV12.\n";
            return false;
        }

        if ((textureDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
            std::cerr << "D3D11 decoded frame was not created for shader resource binding.\n";
            return false;
        }

        if (tile.d3d11SourceTexture == frame.d3d11Texture
            && tile.d3d11SourceSlice == frame.d3d11ArraySlice
            && tile.planeViews[0]
            && tile.planeViews[1]) {
            tile.activeFrameOwner = frame.owner;
            tile.textureFullRange = frame.fullRange;
            return true;
        }

        resetFrameTextures(tile);
        if (!createD3D11FramePlaneView(
                frame.d3d11Texture,
                textureDesc,
                frame.d3d11ArraySlice,
                DXGI_FORMAT_R8_UNORM,
                tile.planeViews[0])
            || !createD3D11FramePlaneView(
                frame.d3d11Texture,
                textureDesc,
                frame.d3d11ArraySlice,
                DXGI_FORMAT_R8G8_UNORM,
                tile.planeViews[1])) {
            resetFrameTextures(tile);
            return false;
        }

        tile.d3d11SourceTexture = frame.d3d11Texture;
        tile.d3d11SourceSlice = frame.d3d11ArraySlice;
        tile.activeFrameOwner = frame.owner;
        tile.textureWidth = frame.width;
        tile.textureHeight = frame.height;
        tile.textureFormat = frame.format;
        tile.textureFullRange = frame.fullRange;
        tile.planeWidths[0] = frame.width;
        tile.planeHeights[0] = frame.height;
        tile.planeWidths[1] = (frame.width + 1) / 2;
        tile.planeHeights[1] = (frame.height + 1) / 2;
        tile.planeFormats[0] = DXGI_FORMAT_R8_UNORM;
        tile.planeFormats[1] = DXGI_FORMAT_R8G8_UNORM;
        return true;
    }

    bool uploadPlane(
        TileState& tile,
        std::size_t planeIndex,
        DXGI_FORMAT format,
        int width,
        int height,
        const std::vector<std::uint8_t>& source,
        int sourceStride,
        int copyBytes)
    {
        if (planeIndex >= tile.planeTextures.size() || width <= 0 || height <= 0 || source.empty()) {
            return false;
        }

        if (!tile.planeTextures[planeIndex]
            || tile.planeWidths[planeIndex] != width
            || tile.planeHeights[planeIndex] != height
            || tile.planeFormats[planeIndex] != format) {
            tile.planeTextures[planeIndex].Reset();
            tile.planeViews[planeIndex].Reset();

            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = static_cast<UINT>(width);
            textureDesc.Height = static_cast<UINT>(height);
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = format;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_DYNAMIC;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            HRESULT result = device_->CreateTexture2D(&textureDesc, nullptr, tile.planeTextures[planeIndex].GetAddressOf());
            if (failed(result, "ID3D11Device::CreateTexture2D")) {
                return false;
            }

            result = device_->CreateShaderResourceView(
                tile.planeTextures[planeIndex].Get(),
                nullptr,
                tile.planeViews[planeIndex].GetAddressOf());
            if (failed(result, "ID3D11Device::CreateShaderResourceView")) {
                tile.planeTextures[planeIndex].Reset();
                return false;
            }

            tile.planeWidths[planeIndex] = width;
            tile.planeHeights[planeIndex] = height;
            tile.planeFormats[planeIndex] = format;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result = context_->Map(tile.planeTextures[planeIndex].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (failed(result, "ID3D11DeviceContext::Map")) {
            return false;
        }

        const auto* sourceData = source.data();
        auto* destination = static_cast<std::uint8_t*>(mapped.pData);
        for (int y = 0; y < height; ++y) {
            std::memcpy(
                destination + static_cast<std::size_t>(y) * mapped.RowPitch,
                sourceData + static_cast<std::size_t>(y) * sourceStride,
                static_cast<std::size_t>(copyBytes));
        }

        context_->Unmap(tile.planeTextures[planeIndex].Get(), 0);
        return true;
    }

    void uploadFrame(TileState& tile, const VideoFrame& frame)
    {
        if (tile.uploadedFrameIndex == frame.index) {
            return;
        }

        if (tile.textureFormat != frame.format || tile.textureWidth != frame.width || tile.textureHeight != frame.height) {
            resetFrameTextures(tile);
            tile.textureFormat = frame.format;
            tile.textureWidth = frame.width;
            tile.textureHeight = frame.height;
        }

        bool uploaded = false;
        if (frame.format == VideoFrameFormat::D3D11_NV12) {
            uploaded = useD3D11FrameTexture(tile, frame);
        } else if (frame.format == VideoFrameFormat::BGRA) {
            uploaded = uploadPlane(
                tile,
                0,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                frame.width,
                frame.height,
                frame.planes[0],
                frame.strides[0],
                frame.width * 4);
        } else if (frame.format == VideoFrameFormat::NV12) {
            const int chromaWidth = frame.strides[1] / 2;
            const int chromaHeight = (frame.height + 1) / 2;
            uploaded = uploadPlane(tile, 0, DXGI_FORMAT_R8_UNORM, frame.width, frame.height, frame.planes[0], frame.strides[0], frame.width)
                && uploadPlane(tile, 1, DXGI_FORMAT_R8G8_UNORM, chromaWidth, chromaHeight, frame.planes[1], frame.strides[1], frame.strides[1]);
        } else if (frame.format == VideoFrameFormat::YUV420P) {
            const int chromaWidth = (frame.width + 1) / 2;
            const int chromaHeight = (frame.height + 1) / 2;
            uploaded = uploadPlane(tile, 0, DXGI_FORMAT_R8_UNORM, frame.width, frame.height, frame.planes[0], frame.strides[0], frame.width)
                && uploadPlane(tile, 1, DXGI_FORMAT_R8_UNORM, chromaWidth, chromaHeight, frame.planes[1], frame.strides[1], chromaWidth)
                && uploadPlane(tile, 2, DXGI_FORMAT_R8_UNORM, chromaWidth, chromaHeight, frame.planes[2], frame.strides[2], chromaWidth);
        }

        if (uploaded) {
            tile.textureFullRange = frame.fullRange;
            tile.uploadedFrameIndex = frame.index;
        }
    }

    D3D11_VIEWPORT computeVideoViewport(const gig::TileRect& cell, int textureWidth, int textureHeight) const
    {
        D3D11_VIEWPORT viewport = {};
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        if (textureWidth <= 0 || textureHeight <= 0 || cell.width <= 0.0f || cell.height <= 0.0f) {
            viewport.TopLeftX = cell.x;
            viewport.TopLeftY = cell.y;
            viewport.Width = cell.width;
            viewport.Height = cell.height;
            return viewport;
        }

        const float cellAspect = cell.width / cell.height;
        const float videoAspect = static_cast<float>(textureWidth) / static_cast<float>(textureHeight);

        if (cellAspect > videoAspect) {
            viewport.Height = cell.height;
            viewport.Width = viewport.Height * videoAspect;
            viewport.TopLeftX = cell.x + (cell.width - viewport.Width) * 0.5f;
            viewport.TopLeftY = cell.y;
        } else {
            viewport.Width = cell.width;
            viewport.Height = viewport.Width / videoAspect;
            viewport.TopLeftX = cell.x;
            viewport.TopLeftY = cell.y + (cell.height - viewport.Height) * 0.5f;
        }

        return viewport;
    }

    void drawTile(const TileState& tile)
    {
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        ID3D11Buffer* vertexBuffers[] = { vertexBuffer_.Get() };
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* shaderResources[] = {
            tile.planeViews[0].Get(),
            tile.planeViews[1].Get(),
            tile.planeViews[2].Get(),
        };
        ID3D11SamplerState* samplers[] = { sampler_.Get() };
        UINT shaderResourceCount = 1;
        if (tile.textureFormat == VideoFrameFormat::NV12 || tile.textureFormat == VideoFrameFormat::D3D11_NV12) {
            updateColorMatrix(tile.textureFullRange);
            context_->PSSetShader(nv12PixelShader_.Get(), nullptr, 0);
            ID3D11Buffer* constantBuffers[] = { colorMatrixBuffer_.Get() };
            context_->PSSetConstantBuffers(0, 1, constantBuffers);
            shaderResourceCount = 2;
        } else if (tile.textureFormat == VideoFrameFormat::YUV420P) {
            updateColorMatrix(tile.textureFullRange);
            context_->PSSetShader(yuv420PixelShader_.Get(), nullptr, 0);
            ID3D11Buffer* constantBuffers[] = { colorMatrixBuffer_.Get() };
            context_->PSSetConstantBuffers(0, 1, constantBuffers);
            shaderResourceCount = 3;
        } else {
            context_->PSSetShader(bgraPixelShader_.Get(), nullptr, 0);
        }

        context_->PSSetShaderResources(0, shaderResourceCount, shaderResources);
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(4, 0);

        ID3D11ShaderResourceView* nullResources[] = { nullptr, nullptr, nullptr };
        context_->PSSetShaderResources(0, 3, nullResources);
    }

    // Ease each tile's signal energy toward its current download rate, so the
    // animation tracks real bytes (alive vs stuck). Once per render frame.
    void updateActivity(float dt)
    {
        // Reactive, not averaged: any bytes this frame slam energy to full, a frame
        // with none lets it settle back toward calm. AVIO reads arrive in bursts, so
        // this pumps energetically while a stream flows and goes smooth/flat the
        // moment it stalls -- legible at a glance, where a smoothed average just sat
        // near the (low) data-bearing-frame duty cycle and always looked quiet.
        constexpr float kSettleTau = 0.25f; // seconds to calm down once data stops
        const float settle = std::exp(-dt / kSettleTau);
        for (std::size_t i = 0; i < tiles_.size(); ++i) {
            TileState& tile = tiles_[i];
            const std::uint64_t bytes = (i < tileBytes_.size()) ? tileBytes_[i] : tile.signalLastBytes;
            const bool gotData = bytes > tile.signalLastBytes;
            tile.signalLastBytes = bytes;
            tile.signalEnergy = gotData ? 1.0f : tile.signalEnergy * settle;
        }
    }

    // Draw the procedural signal animation across a whole cell (not letterboxed).
    // alpha < 1 blends it over whatever is already there (the resolve crossfade).
    void drawSignal(const gig::TileRect& cell, std::size_t index, float energy, float alpha)
    {
        if (cell.width <= 0.0f || cell.height <= 0.0f) {
            return;
        }
        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = cell.x;
        viewport.TopLeftY = cell.y;
        viewport.Width = cell.width;
        viewport.Height = cell.height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        SignalConstants constants = {};
        constants.time = animTime_;
        constants.energy = energy;
        constants.seed = static_cast<float>(index) * 2.39996f; // golden-angle phase per tile
        constants.aspect = cell.width / cell.height;
        constants.alpha = alpha;
        context_->UpdateSubresource(signalConstantBuffer_.Get(), 0, nullptr, &constants, 0, 0);

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        ID3D11Buffer* vertexBuffers[] = { vertexBuffer_.Get() };
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(signalPixelShader_.Get(), nullptr, 0);
        ID3D11Buffer* constantBuffers[] = { signalConstantBuffer_.Get() };
        context_->PSSetConstantBuffers(0, 1, constantBuffers);

        const bool blend = alpha < 0.999f;
        if (blend) {
            const float factors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            context_->OMSetBlendState(alphaBlendState_.Get(), factors, 0xffffffff);
        }
        context_->Draw(4, 0);
        if (blend) {
            context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        }
    }

    // Draw tile `index`'s current content (video letterboxed into `rect`, or the
    // signal animation) without mutating its upload/fade state -- that was already
    // done this frame by renderGridTiles. Used by the zoom transition overlay.
    void drawTileContentAt(std::size_t index, const gig::TileRect& rect)
    {
        if (index >= tiles_.size()) {
            return;
        }
        const TileState& tile = tiles_[index];
        if (tile.planeViews[0]) {
            const D3D11_VIEWPORT viewport = computeVideoViewport(rect, tile.textureWidth, tile.textureHeight);
            context_->RSSetViewports(1, &viewport);
            drawTile(tile);
        } else {
            drawSignal(rect, index, tile.signalEnergy, 1.0f);
        }
    }

    // Subtle hover affordance over a clickable tile: a soft border + faint fill,
    // drawn in the tile's own pixel viewport (no ImGui coord assumptions).
    void drawHover(const gig::TileRect& cell)
    {
        if (cell.width <= 0.0f || cell.height <= 0.0f) {
            return;
        }
        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = cell.x;
        viewport.TopLeftY = cell.y;
        viewport.Width = cell.width;
        viewport.Height = cell.height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        HoverConstants constants = {};
        constants.sizePx[0] = cell.width;
        constants.sizePx[1] = cell.height;
        constants.borderPx = 2.0f * dpiScale_;
        constants.color[0] = 0.80f;
        constants.color[1] = 0.90f;
        constants.color[2] = 1.0f;
        constants.color[3] = 0.6f; // max border alpha
        context_->UpdateSubresource(hoverConstantBuffer_.Get(), 0, nullptr, &constants, 0, 0);

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        ID3D11Buffer* vertexBuffers[] = { vertexBuffer_.Get() };
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(hoverPixelShader_.Get(), nullptr, 0);
        ID3D11Buffer* constantBuffers[] = { hoverConstantBuffer_.Get() };
        context_->PSSetConstantBuffers(0, 1, constantBuffers);

        const float factors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context_->OMSetBlendState(alphaBlendState_.Get(), factors, 0xffffffff);
        context_->Draw(4, 0);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }

    // Draws every camera into its grid cell. Assumes the D3D11 lock is held and
    // the back buffer has already been cleared.
    void renderGridTiles(const std::vector<std::shared_ptr<VideoFrame>>& frames, const gig::GridLayout& layout)
    {
        constexpr float kFadeDur = 0.22f; // quick resolve crossfade (signal -> first frame)
        for (std::size_t i = 0; i < frames.size(); ++i) {
            TileState& tile = tiles_[i];
            const VideoFrame* frame = frames[i].get();
            const bool hasFrame = frame
                && (frame->format == VideoFrameFormat::D3D11_NV12 || !frame->planes[0].empty());
            if (hasFrame) {
                uploadFrame(tile, *frame);
            } else if (tile.planeViews[0]) {
                resetFrameTextures(tile); // drop stale GPU state so an idle/offline tile blanks
            }

            if (i >= layout.tiles.size()) {
                continue;
            }
            const gig::TileRect& cell = layout.tiles[i];

            if (!tile.planeViews[0]) {
                // No displayable frame yet (waiting for a keyframe / reconnecting /
                // offline): the data-driven signal animation IS the tile.
                drawSignal(cell, i, tile.signalEnergy, 1.0f);
                sawAnimatedContent_ = true; // scope animates -> keep rendering
                tile.showedSignal = true;
                tile.frameFade = -1.0f;
                continue;
            }

            // We have a frame. If we were just showing the signal, start a quick
            // crossfade so it resolves into the first frame instead of popping.
            if (tile.showedSignal) {
                tile.frameFade = 0.0f;
                tile.showedSignal = false;
            }
            const D3D11_VIEWPORT videoViewport =
                computeVideoViewport(cell, tile.textureWidth, tile.textureHeight);
            context_->RSSetViewports(1, &videoViewport);
            drawTile(tile);
            if (tile.frameFade >= 0.0f) {
                sawAnimatedContent_ = true; // resolve crossfade in progress
                const float a = 1.0f - tile.frameFade / kFadeDur;
                if (a > 0.0f) {
                    drawSignal(cell, i, tile.signalEnergy, a);
                }
                tile.frameFade += lastDt_;
                if (tile.frameFade >= kFadeDur) {
                    tile.frameFade = -1.0f;
                }
            }
        }
    }

    // Draws a single focused camera letterboxed across the whole window.
    void renderSingleTile(std::size_t index, const std::vector<std::shared_ptr<VideoFrame>>& frames)
    {
        TileState& tile = tiles_[index];
        const VideoFrame* frame = frames[index].get();
        const bool hasFrame = frame
            && (frame->format == VideoFrameFormat::D3D11_NV12 || !frame->planes[0].empty());
        if (hasFrame) {
            uploadFrame(tile, *frame);
        } else if (tile.planeViews[0]) {
            resetFrameTextures(tile);
        }

        const gig::TileRect full {
            0.0f,
            0.0f,
            static_cast<float>(backBufferWidth_),
            static_cast<float>(backBufferHeight_),
        };

        if (!tile.planeViews[0]) {
            // No frame yet: fill the focused view with the signal animation too.
            drawSignal(full, index, tile.signalEnergy, 1.0f);
            sawAnimatedContent_ = true; // scope animates -> keep rendering
            tile.showedSignal = true;
            tile.frameFade = -1.0f;
            return;
        }

        if (tile.showedSignal) {
            tile.frameFade = 0.0f;
            tile.showedSignal = false;
        }
        const D3D11_VIEWPORT videoViewport = computeVideoViewport(full, tile.textureWidth, tile.textureHeight);
        context_->RSSetViewports(1, &videoViewport);
        drawTile(tile);
        if (tile.frameFade >= 0.0f) {
            sawAnimatedContent_ = true; // resolve crossfade in progress
            constexpr float kFadeDur = 0.22f;
            const float a = 1.0f - tile.frameFade / kFadeDur;
            if (a > 0.0f) {
                drawSignal(full, index, tile.signalEnergy, a);
            }
            tile.frameFade += lastDt_;
            if (tile.frameFade >= kFadeDur) {
                tile.frameFade = -1.0f;
            }
        }
    }

    std::string labelFor(std::size_t index) const
    {
        return index < cameraLabels_.size() ? cameraLabels_[index] : std::string();
    }

    // Whether tile `index`'s label should draw this frame, per the label mode.
    // ErrorOnly shows it only while the tile is in the signal phase (no frame).
    bool labelVisible(std::size_t index) const
    {
        switch (labelMode_) {
        case LabelMode::Hide: return false;
        case LabelMode::Always: return true;
        case LabelMode::ErrorOnly: return index < tiles_.size() && tiles_[index].showedSignal;
        }
        return false;
    }

    void drawTileLabel(const gig::TileRect& cell, const std::string& label, float scale)
    {
        if (label.empty() || !overlay_.ready() || cell.width <= 0.0f || cell.height <= 0.0f) {
            return;
        }
        const float pad = 4.0f;
        const float glyphWidth = overlay_.glyphWidth(scale);
        if (glyphWidth <= 0.0f || cell.width <= 2.0f * pad) {
            return;
        }
        const std::size_t maxChars = static_cast<std::size_t>((cell.width - 2.0f * pad) / glyphWidth);
        if (maxChars == 0) {
            return;
        }
        const std::string shown = label.size() > maxChars ? label.substr(0, maxChars) : label;
        const float stripWidth = std::min(cell.width, overlay_.textWidth(shown, scale) + 2.0f * pad);
        const float stripHeight = overlay_.lineHeight(scale) + 2.0f * pad;
        overlay_.rect(cell.x, cell.y, stripWidth, stripHeight, gig::OverlayColor { 0.0f, 0.0f, 0.0f, 0.55f });
        overlay_.text(cell.x + pad, cell.y + pad, scale, gig::OverlayColor { 0.9f, 0.95f, 1.0f, 1.0f }, shown);
    }

    void drawDiagnosticsTile(const gig::TileRect& cell)
    {
        if (!overlay_.ready() || cell.width <= 0.0f || cell.height <= 0.0f) {
            return;
        }

        overlay_.rect(cell.x, cell.y, cell.width, cell.height, gig::OverlayColor { 0.04f, 0.05f, 0.07f, 0.92f });

        const float pad = 8.0f;
        // Scale text down so the widest line fits the cell.
        constexpr int widestLineChars = 30;
        float scale = (cell.width - 2.0f * pad) / (static_cast<float>(widestLineChars) * overlay_.glyphWidth(1.0f));
        scale = std::clamp(scale, 0.35f, 1.0f);
        const float lineHeight = overlay_.lineHeight(scale) + 2.0f;
        const float x = cell.x + pad;
        float y = cell.y + pad;

        const gig::OverlayColor heading { 0.65f, 0.78f, 1.0f, 1.0f };
        const gig::OverlayColor body { 0.85f, 0.9f, 1.0f, 1.0f };

        char line[128] = {};
        overlay_.text(x, y, scale, heading, "diagnostics");
        y += lineHeight * 1.4f;
        std::snprintf(line, sizeof(line), "cams good: %d  bad: %d", overlayStats_.camerasOnline, overlayStats_.camerasOffline);
        overlay_.text(x, y, scale, body, line);
        y += lineHeight;
        std::snprintf(line, sizeof(line), "frames: %d/s, %llu total",
            static_cast<int>(overlayStats_.fps + 0.5), static_cast<unsigned long long>(overlayStats_.framesTotal));
        overlay_.text(x, y, scale, body, line);
        y += lineHeight;
        std::snprintf(line, sizeof(line), "bandwidth: %d kbps", overlayStats_.kbps);
        overlay_.text(x, y, scale, body, line);
        y += lineHeight;
        std::snprintf(line, sizeof(line), "cpu: %.2f%%", overlayStats_.cpuPercent);
        overlay_.text(x, y, scale, body, line);
    }

    void loadImguiFont()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        // Prefer crisp monospace Consolas; fall back to the built-in bitmap font.
        const std::string consolasPath = windowsFontPath("consola.ttf");
        if (io.Fonts->AddFontFromFileTTF(consolasPath.c_str(), kBaseFontPx * dpiScale_) == nullptr) {
            ImFontConfig fontConfig;
            fontConfig.SizePixels = 13.0f * dpiScale_;
            io.Fonts->AddFontDefault(&fontConfig);
        }
        // Merge Segoe MDL2 Assets (system icon font) for the toolbar glyphs
        // (gear / refresh / list). Toolbar falls back to text labels if absent.
        // MDL2 glyphs sit high on a text baseline, so nudge them down to vertically
        // center within the button (tune kIconNudge if needed).
        static const ImWchar kIconRange[] = { 0xE700, 0xEA40, 0 };
        constexpr float kIconNudge = 0.25f; // fraction of font height, downward
        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.GlyphOffset.y = kBaseFontPx * dpiScale_ * kIconNudge;
        const std::string iconPath = windowsFontPath("segmdl2.ttf");
        iconFontLoaded_ = io.Fonts->AddFontFromFileTTF(
            iconPath.c_str(), kBaseFontPx * dpiScale_, &iconConfig, kIconRange) != nullptr;
    }

    void applyImguiStyle()
    {
        // Reset to a fresh style first so ScaleAllSizes never compounds across
        // repeated DPI changes.
        ImGui::GetStyle() = ImGuiStyle();
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(dpiScale_);
    }

    // Re-bake the overlay atlas + ImGui font/style for the window's current display
    // scale (SDL reported a DPI change, e.g. the window moved to another monitor).
    // Called from render() under the D3D lock.
    void applyDpiScale()
    {
        float scale = SDL_GetWindowDisplayScale(window_);
        if (!(scale > 0.0f)) {
            scale = 1.0f;
        }
        if (scale == dpiScale_) {
            return;
        }
        dpiScale_ = scale;
        gig::logInfo() << "display scale changed: " << dpiScale_;

        overlay_.rebakeAtlas(std::max(8, static_cast<int>(kBaseFontPx * dpiScale_ + 0.5f)));
        if (imguiReady_) {
            loadImguiFont();
            ImGui_ImplDX11_InvalidateDeviceObjects(); // font texture rebuilt on next NewFrame
            applyImguiStyle();
        }
    }

    // Run one ImGui frame into the bound back buffer (called after the tiles +
    // overlay, before Present). The full cycle runs every frame so ImGui's
    // WantCapture flags stay correct even when the log view is hidden.
    void renderImGui()
    {
        if (!imguiReady_) {
            return;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        buildToolbar();
        buildStatusBanner();
        if (logViewVisible_) {
            buildLogWindow();
        }
        ImGui::Render();
        context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // A thin auto-hiding control strip pinned to the top edge: app name + live
    // status on the left, buttons on the right. Always visible in the grid view;
    // in focus/zoom view it fades out after a few idle seconds (show on mouse
    // movement). Drawn in ImGui so it composites over the video.
    float toolbarHeightPx() const { return kToolbarLogicalHeight * dpiScale_; }

    void buildToolbar()
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool active = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f
            || ImGui::IsMouseDown(ImGuiMouseButton_Left);
        toolbarIdle_ = active ? 0.0f : toolbarIdle_ + io.DeltaTime;
        const bool focusView = focusedTile_ >= 0;
        if (focusView && toolbarIdle_ >= kToolbarHideDelay) {
            return; // immersive single-camera view: let it hide (image is full-bleed)
        }

        // Segoe MDL2 Assets glyphs (UTF-8): gear U+E713, refresh U+E72C, list U+EA37.
        const char* const settingsLabel = iconFontLoaded_ ? "\xEE\x9C\x93" : "Settings";
        const char* const reconnectLabel = iconFontLoaded_ ? "\xEE\x9C\xAC" : "Reconnect";
        const char* const logLabel = iconFontLoaded_ ? "\xEE\xA8\xB7" : "Log";

        const float height = toolbarHeightPx();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        // Opaque chrome in the grid (it reserves space below it); slightly
        // translucent in focus view, where it's a transient overlay.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.08f, focusView ? 0.82f : 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // flat icon buttons
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * dpiScale_, 0.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##toolbar", nullptr, flags)) {
            const float rowHeight = ImGui::GetFrameHeight();
            ImGui::SetCursorPosY((height - rowHeight) * 0.5f); // vertically center the row

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("gig");
            ImGui::SameLine();

            const int online = overlayStats_.camerasOnline;
            const int total = overlayStats_.camerasOnline + overlayStats_.camerasOffline;
            const ImVec4 green(0.40f, 0.85f, 0.40f, 1.0f);
            const ImVec4 amber(0.90f, 0.70f, 0.20f, 1.0f);
            const ImVec4 red(0.90f, 0.35f, 0.35f, 1.0f);
            ImGui::AlignTextToFramePadding();
            // The live count is only trustworthy while the control plane is up;
            // when it isn't, show the link state instead of a stale "N/N live".
            switch (overlayStats_.link) {
            case OverlayStats::LinkState::Disconnected:
                ImGui::TextColored(red, "disconnected");
                break;
            case OverlayStats::LinkState::Reconnecting:
                ImGui::TextColored(amber, "reconnecting");
                break;
            case OverlayStats::LinkState::Ok: {
                const ImVec4 statusColor = (total > 0 && online == total) ? green : (online == 0 ? red : amber);
                ImGui::TextColored(statusColor, "%d/%d live", online, total);
                break;
            }
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%d fps   %.1f Mbps   cpu %.0f%%",
                static_cast<int>(overlayStats_.fps + 0.5),
                overlayStats_.kbps / 1000.0,
                overlayStats_.cpuPercent);

            // Right-aligned icon buttons (tooltips carry the meaning + shortcut).
            const ImGuiStyle& style = ImGui::GetStyle();
            const auto buttonWidth = [&](const char* label) {
                return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
            };
            const float buttonsWidth = buttonWidth(settingsLabel) + buttonWidth(reconnectLabel)
                + buttonWidth(logLabel) + style.ItemSpacing.x * 2.0f;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonsWidth);
            if (ImGui::Button(settingsLabel)) {
                pendingToolbarAction_ = ToolbarAction::Settings;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Settings (F2)");
            }
            ImGui::SameLine();
            if (ImGui::Button(reconnectLabel)) {
                pendingToolbarAction_ = ToolbarAction::Reconnect;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reconnect (F5)");
            }
            ImGui::SameLine();
            if (ImGui::Button(logLabel)) {
                logViewVisible_ = !logViewVisible_;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Log");
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    // A slim colored strip pinned just under the toolbar, shown only while a
    // transient condition is active and cleared automatically when it resolves
    // (the run loop re-derives link/healthDegraded each refresh). Non-modal and
    // input-transparent -- it never steals a click or blocks the flow.
    void buildStatusBanner()
    {
        if (focusedTile_ >= 0) {
            return; // immersive single-camera view stays clean
        }
        const OverlayStats& s = overlayStats_;
        if (s.link == OverlayStats::LinkState::Ok && !s.healthDegraded) {
            return; // all clear
        }

        const std::string host = s.statusHost.empty() ? std::string("Frigate") : s.statusHost;
        ImVec4 background;
        std::string message;
        if (s.link == OverlayStats::LinkState::Disconnected) {
            background = ImVec4(0.42f, 0.10f, 0.10f, 0.94f); // red
            message = "Not connected to " + host + "  --  press Reconnect (F5)";
            if (!s.statusDetail.empty()) {
                message += "   [" + s.statusDetail + "]";
            }
        } else if (s.link == OverlayStats::LinkState::Reconnecting) {
            background = ImVec4(0.42f, 0.28f, 0.05f, 0.94f); // amber
            message = "Lost contact with " + host + "  --  reconnecting";
            if (s.secondsSinceData > 0) {
                message += " (last data " + std::to_string(s.secondsSinceData) + "s ago)";
            }
        } else { // Ok but health degraded
            background = ImVec4(0.42f, 0.28f, 0.05f, 0.94f); // amber
            message = "Camera health unreadable  --  go2rtc stream schema may have changed";
        }

        const float height = kBannerLogicalHeight * dpiScale_;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + toolbarHeightPx()));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.95f, 0.92f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * dpiScale_, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f)); // honor our slim height at 1x
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##statusbanner", nullptr, flags)) {
            ImGui::SetCursorPosY((height - ImGui::GetFrameHeight()) * 0.5f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(message.c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    void buildLogWindow()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        bool open = true;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("Log", &open, flags)) {
            gig::LogBuffer::instance().snapshot(logScratch_);

            if (ImGui::Button("Copy")) {
                std::string joined;
                for (const std::string& entry : logScratch_) {
                    joined += entry;
                    joined.push_back('\n');
                }
                ImGui::SetClipboardText(joined.c_str()); // routed to the OS clipboard via SDL
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                gig::LogBuffer::instance().clear();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("wheel / drag scrollbar to scroll, Esc or X to close");
            ImGui::Separator();

            ImGui::BeginChild("log_scroll", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(logScratch_.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    ImGui::TextUnformatted(logScratch_[static_cast<std::size_t>(i)].c_str());
                }
            }
            clipper.End();
            // Stick to the tail unless the user has scrolled up.
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }
        ImGui::End();
        if (!open) {
            logViewVisible_ = false;
        }
    }

    SDL_Window* window_ = nullptr;
    HWND hwnd_ = nullptr;
    LONG backBufferWidth_ = 1;
    LONG backBufferHeight_ = 1;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTargetView_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> bgraPixelShader_;
    ComPtr<ID3D11PixelShader> nv12PixelShader_;
    ComPtr<ID3D11PixelShader> yuv420PixelShader_;
    ComPtr<ID3D11PixelShader> signalPixelShader_;
    ComPtr<ID3D11Buffer> signalConstantBuffer_;
    ComPtr<ID3D11PixelShader> hoverPixelShader_;
    ComPtr<ID3D11Buffer> hoverConstantBuffer_;
    ComPtr<ID3D11BlendState> alphaBlendState_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> colorMatrixBuffer_;

    // Per-tile signal-animation driving state.
    std::vector<std::uint64_t> tileBytes_;   // latest per-camera cumulative bytes (from setTileActivity)
    float animTime_ = 0.0f;                  // accumulated animation clock (seconds)
    float lastDt_ = 0.0f;                    // last render delta, for the resolve fade
    std::chrono::steady_clock::time_point lastRenderTp_;
    bool haveRenderTp_ = false;

    std::vector<TileState> tiles_;
    int focusedTile_ = -1;
    int animTile_ = -1;                            // tile zooming in/out (-1 = none)
    float animProgress_ = 0.0f;                     // 0 = grid cell, 1 = fullscreen
    static constexpr float kZoomDuration = 0.30f;   // seconds for the zoom/unzoom
    int hoveredTile_ = -1;                          // tile under the mouse (-1 = none)
    gig::TextOverlay overlay_;
    std::vector<std::string> cameraLabels_;
    OverlayStats overlayStats_;
    LabelMode labelMode_ = LabelMode::ErrorOnly;
    std::shared_ptr<D3D11DecodeContext> decodeContext_ = std::make_shared<D3D11DecodeContext>();
    bool imguiReady_ = false;
    bool logViewVisible_ = false;
    std::vector<std::string> logScratch_;
    float dpiScale_ = 1.0f;
    bool dpiDirty_ = false;
    bool iconFontLoaded_ = false;
    ToolbarAction pendingToolbarAction_ = ToolbarAction::None;
    float toolbarIdle_ = 0.0f;

    // On-demand rendering: animating_ reports (to the run loop, via isAnimating())
    // whether the last render left an animation in flight; sawAnimatedContent_ is
    // the per-render scratch the tile loops set when a scope/fade drew. Start true
    // so the very first loop iteration paints.
    bool animating_ = true;
    bool sawAnimatedContent_ = false;

    // Toolbar height in logical (DPI-independent) points; reserved above the grid.
    static constexpr float kToolbarLogicalHeight = 32.0f;
    // Focus-view idle seconds before the (overlay) toolbar auto-hides. Read by
    // render() too, so the run loop keeps drawing until the hide actually lands.
    static constexpr float kToolbarHideDelay = 2.5f;
    // Status-banner height (logical points). Overlays the grid top transiently --
    // it does not reserve layout space, so the grid never reflows as it appears.
    static constexpr float kBannerLogicalHeight = 22.0f;
};

} // namespace

std::unique_ptr<VideoRenderer> createD3D11Renderer()
{
    return std::make_unique<D3D11Renderer>();
}
