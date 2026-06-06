#include "render/grid_layout.h"
#include "render/text_overlay.h"
#include "render/video_renderer.h"

#include "log.hpp"

#include <algorithm>
#include <array>
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

struct ColorMatrixConstants {
    float r[4];
    float g[4];
    float b[4];
    float unused[4];
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

            if (dpiDirty_) {
                dpiDirty_ = false;
                applyDpiScale();
            }

            if (tiles_.size() != frames.size()) {
                tiles_.resize(frames.size());
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

            if (focusedTile_ >= 0 && focusedTile_ < static_cast<int>(frames.size())) {
                renderSingleTile(static_cast<std::size_t>(focusedTile_), frames);
                context_->RSSetViewports(1, &fullViewport);
                overlay_.begin(static_cast<int>(backBufferWidth_), static_cast<int>(backBufferHeight_));
                drawTileLabel(
                    gig::TileRect { 0.0f, 0.0f, static_cast<float>(backBufferWidth_), static_cast<float>(backBufferHeight_) },
                    labelFor(static_cast<std::size_t>(focusedTile_)),
                    2.0f);
                overlay_.flush(context_.Get());
            } else {
                // Reserve one extra cell for the synthetic diagnostics tile.
                const bool showDiagnostics = overlayStats_.showDiagnostics;
                const int effectiveCount = static_cast<int>(frames.size()) + (showDiagnostics ? 1 : 0);
                const gig::GridLayout layout = gig::computeGridLayout(
                    effectiveCount,
                    static_cast<int>(backBufferWidth_),
                    static_cast<int>(backBufferHeight_));
                renderGridTiles(frames, layout);
                context_->RSSetViewports(1, &fullViewport);
                overlay_.begin(static_cast<int>(backBufferWidth_), static_cast<int>(backBufferHeight_));
                for (std::size_t i = 0; i < frames.size() && i < layout.tiles.size(); ++i) {
                    drawTileLabel(layout.tiles[i], labelFor(i), 1.0f);
                }
                if (showDiagnostics && frames.size() < layout.tiles.size()) {
                    drawDiagnosticsTile(layout.tiles[frames.size()]);
                }
                overlay_.flush(context_.Get());
            }

            renderImGui();

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
        if (!compileShader(VertexShaderSource, "VSMain", "vs_4_0", vertexShaderBlob)
            || !compileShader(BgraPixelShaderSource, "PSMain", "ps_4_0", bgraPixelShaderBlob)
            || !compileShader(Nv12PixelShaderSource, "PSMain", "ps_4_0", nv12PixelShaderBlob)
            || !compileShader(Yuv420PixelShaderSource, "PSMain", "ps_4_0", yuv420PixelShaderBlob)) {
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
        return !failed(result, "ID3D11Device::CreateBuffer(ColorMatrix)");
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

    // Draws every camera into its grid cell. Assumes the D3D11 lock is held and
    // the back buffer has already been cleared.
    void renderGridTiles(const std::vector<std::shared_ptr<VideoFrame>>& frames, const gig::GridLayout& layout)
    {
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

            if (!tile.planeViews[0] || i >= layout.tiles.size()) {
                continue;
            }

            const D3D11_VIEWPORT videoViewport =
                computeVideoViewport(layout.tiles[i], tile.textureWidth, tile.textureHeight);
            context_->RSSetViewports(1, &videoViewport);
            drawTile(tile);
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

        if (!tile.planeViews[0]) {
            return;
        }

        const gig::TileRect full {
            0.0f,
            0.0f,
            static_cast<float>(backBufferWidth_),
            static_cast<float>(backBufferHeight_),
        };
        const D3D11_VIEWPORT videoViewport = computeVideoViewport(full, tile.textureWidth, tile.textureHeight);
        context_->RSSetViewports(1, &videoViewport);
        drawTile(tile);
    }

    std::string labelFor(std::size_t index) const
    {
        return index < cameraLabels_.size() ? cameraLabels_[index] : std::string();
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
        if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", kBaseFontPx * dpiScale_) == nullptr) {
            ImFontConfig fontConfig;
            fontConfig.SizePixels = 13.0f * dpiScale_;
            io.Fonts->AddFontDefault(&fontConfig);
        }
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
        if (logViewVisible_) {
            buildLogWindow();
        }
        ImGui::Render();
        context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
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
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> colorMatrixBuffer_;

    std::vector<TileState> tiles_;
    int focusedTile_ = -1;
    gig::TextOverlay overlay_;
    std::vector<std::string> cameraLabels_;
    OverlayStats overlayStats_;
    std::shared_ptr<D3D11DecodeContext> decodeContext_ = std::make_shared<D3D11DecodeContext>();
    bool imguiReady_ = false;
    bool logViewVisible_ = false;
    std::vector<std::string> logScratch_;
    float dpiScale_ = 1.0f;
    bool dpiDirty_ = false;
};

} // namespace

std::unique_ptr<VideoRenderer> createD3D11Renderer()
{
    return std::make_unique<D3D11Renderer>();
}
