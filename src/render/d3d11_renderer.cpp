#include "render/video_renderer.h"

#include <SDL_syswm.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

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

const char* PixelShaderSource = R"(
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

        SDL_SysWMinfo wmInfo = {};
        SDL_VERSION(&wmInfo.version);
        if (!SDL_GetWindowWMInfo(window_, &wmInfo)) {
            std::cerr << "SDL_GetWindowWMInfo failed: " << SDL_GetError() << "\n";
            return false;
        }

        hwnd_ = wmInfo.info.win.window;
        readClientSize();

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

        return createRenderTarget() && createPipeline();
    }

    void resize() override
    {
        if (!swapChain_ || !context_) {
            return;
        }

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

    void render(const VideoFrame* frame) override
    {
        if (!context_ || !swapChain_ || !renderTargetView_) {
            return;
        }

        if (frame && !frame->bgra.empty()) {
            uploadFrame(*frame);
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

        if (textureView_) {
            D3D11_VIEWPORT videoViewport = computeVideoViewport();
            context_->RSSetViewports(1, &videoViewport);

            UINT stride = sizeof(Vertex);
            UINT offset = 0;
            ID3D11Buffer* vertexBuffers[] = { vertexBuffer_.Get() };
            context_->IASetInputLayout(inputLayout_.Get());
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context_->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
            context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
            context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* shaderResources[] = { textureView_.Get() };
            ID3D11SamplerState* samplers[] = { sampler_.Get() };
            context_->PSSetShaderResources(0, 1, shaderResources);
            context_->PSSetSamplers(0, 1, samplers);
            context_->Draw(4, 0);

            ID3D11ShaderResourceView* nullResources[] = { nullptr };
            context_->PSSetShaderResources(0, 1, nullResources);
        }

        const HRESULT presentResult = swapChain_->Present(1, 0);
        if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET) {
            std::cerr << "D3D device was lost.\n";
        }
    }

private:
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
        ComPtr<ID3DBlob> pixelShaderBlob;
        if (!compileShader(VertexShaderSource, "VSMain", "vs_4_0", vertexShaderBlob)
            || !compileShader(PixelShaderSource, "PSMain", "ps_4_0", pixelShaderBlob)) {
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
            pixelShaderBlob->GetBufferPointer(),
            pixelShaderBlob->GetBufferSize(),
            nullptr,
            pixelShader_.GetAddressOf());
        if (failed(result, "ID3D11Device::CreatePixelShader")) {
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
        return !failed(result, "ID3D11Device::CreateSamplerState");
    }

    void uploadFrame(const VideoFrame& frame)
    {
        if (uploadedFrameIndex_ == frame.index) {
            return;
        }

        if (!texture_ || textureWidth_ != frame.width || textureHeight_ != frame.height) {
            texture_.Reset();
            textureView_.Reset();

            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = static_cast<UINT>(frame.width);
            textureDesc.Height = static_cast<UINT>(frame.height);
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_DYNAMIC;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            HRESULT result = device_->CreateTexture2D(&textureDesc, nullptr, texture_.GetAddressOf());
            if (failed(result, "ID3D11Device::CreateTexture2D")) {
                return;
            }

            result = device_->CreateShaderResourceView(texture_.Get(), nullptr, textureView_.GetAddressOf());
            if (failed(result, "ID3D11Device::CreateShaderResourceView")) {
                texture_.Reset();
                return;
            }

            textureWidth_ = frame.width;
            textureHeight_ = frame.height;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result = context_->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (failed(result, "ID3D11DeviceContext::Map")) {
            return;
        }

        const auto* source = frame.bgra.data();
        auto* destination = static_cast<std::uint8_t*>(mapped.pData);
        const int copyBytes = frame.width * 4;
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(
                destination + static_cast<std::size_t>(y) * mapped.RowPitch,
                source + static_cast<std::size_t>(y) * frame.stride,
                static_cast<std::size_t>(copyBytes));
        }

        context_->Unmap(texture_.Get(), 0);
        uploadedFrameIndex_ = frame.index;
    }

    D3D11_VIEWPORT computeVideoViewport() const
    {
        D3D11_VIEWPORT viewport = {};
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        if (textureWidth_ <= 0 || textureHeight_ <= 0) {
            viewport.Width = static_cast<float>(backBufferWidth_);
            viewport.Height = static_cast<float>(backBufferHeight_);
            return viewport;
        }

        const float outputAspect = static_cast<float>(backBufferWidth_) / static_cast<float>(backBufferHeight_);
        const float videoAspect = static_cast<float>(textureWidth_) / static_cast<float>(textureHeight_);

        if (outputAspect > videoAspect) {
            viewport.Height = static_cast<float>(backBufferHeight_);
            viewport.Width = viewport.Height * videoAspect;
            viewport.TopLeftX = (static_cast<float>(backBufferWidth_) - viewport.Width) * 0.5f;
            viewport.TopLeftY = 0.0f;
        } else {
            viewport.Width = static_cast<float>(backBufferWidth_);
            viewport.Height = viewport.Width / videoAspect;
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = (static_cast<float>(backBufferHeight_) - viewport.Height) * 0.5f;
        }

        return viewport;
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
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> textureView_;

    int textureWidth_ = 0;
    int textureHeight_ = 0;
    std::uint64_t uploadedFrameIndex_ = 0;
};

} // namespace

std::unique_ptr<VideoRenderer> createD3D11Renderer()
{
    return std::make_unique<D3D11Renderer>();
}
