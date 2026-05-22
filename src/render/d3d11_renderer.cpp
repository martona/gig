#include "render/video_renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>

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
        return createRenderTarget() && createPipeline();
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

    void render(const VideoFrame* frame) override
    {
        if (!context_ || !swapChain_ || !renderTargetView_) {
            return;
        }

        auto d3dLock = lockD3D11();
        if (frame && (frame->format == VideoFrameFormat::D3D11_NV12 || !frame->planes[0].empty())) {
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

        if (planeViews_[0]) {
            D3D11_VIEWPORT videoViewport = computeVideoViewport();
            context_->RSSetViewports(1, &videoViewport);

            UINT stride = sizeof(Vertex);
            UINT offset = 0;
            ID3D11Buffer* vertexBuffers[] = { vertexBuffer_.Get() };
            context_->IASetInputLayout(inputLayout_.Get());
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context_->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
            context_->VSSetShader(vertexShader_.Get(), nullptr, 0);

            ID3D11ShaderResourceView* shaderResources[] = {
                planeViews_[0].Get(),
                planeViews_[1].Get(),
                planeViews_[2].Get(),
            };
            ID3D11SamplerState* samplers[] = { sampler_.Get() };
            UINT shaderResourceCount = 1;
            if (textureFormat_ == VideoFrameFormat::NV12 || textureFormat_ == VideoFrameFormat::D3D11_NV12) {
                updateColorMatrix(textureFullRange_);
                context_->PSSetShader(nv12PixelShader_.Get(), nullptr, 0);
                ID3D11Buffer* constantBuffers[] = { colorMatrixBuffer_.Get() };
                context_->PSSetConstantBuffers(0, 1, constantBuffers);
                shaderResourceCount = 2;
            } else if (textureFormat_ == VideoFrameFormat::YUV420P) {
                updateColorMatrix(textureFullRange_);
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

        const HRESULT presentResult = swapChain_->Present(1, 0);
        if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET) {
            std::cerr << "D3D device was lost.\n";
        }
    }

    std::shared_ptr<D3D11DecodeContext> d3d11DecodeContext() const override
    {
        return decodeContext_;
    }

private:
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

    void resetFrameTextures()
    {
        for (std::size_t i = 0; i < planeTextures_.size(); ++i) {
            planeTextures_[i].Reset();
            planeViews_[i].Reset();
            planeWidths_[i] = 0;
            planeHeights_[i] = 0;
            planeFormats_[i] = DXGI_FORMAT_UNKNOWN;
        }
        activeFrameOwner_.reset();
        d3d11SourceTexture_ = nullptr;
        d3d11SourceSlice_ = 0;
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

    bool useD3D11FrameTexture(const VideoFrame& frame)
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

        if (d3d11SourceTexture_ == frame.d3d11Texture
            && d3d11SourceSlice_ == frame.d3d11ArraySlice
            && planeViews_[0]
            && planeViews_[1]) {
            activeFrameOwner_ = frame.owner;
            textureFullRange_ = frame.fullRange;
            return true;
        }

        resetFrameTextures();
        if (!createD3D11FramePlaneView(
                frame.d3d11Texture,
                textureDesc,
                frame.d3d11ArraySlice,
                DXGI_FORMAT_R8_UNORM,
                planeViews_[0])
            || !createD3D11FramePlaneView(
                frame.d3d11Texture,
                textureDesc,
                frame.d3d11ArraySlice,
                DXGI_FORMAT_R8G8_UNORM,
                planeViews_[1])) {
            resetFrameTextures();
            return false;
        }

        d3d11SourceTexture_ = frame.d3d11Texture;
        d3d11SourceSlice_ = frame.d3d11ArraySlice;
        activeFrameOwner_ = frame.owner;
        textureWidth_ = frame.width;
        textureHeight_ = frame.height;
        textureFormat_ = frame.format;
        textureFullRange_ = frame.fullRange;
        planeWidths_[0] = frame.width;
        planeHeights_[0] = frame.height;
        planeWidths_[1] = (frame.width + 1) / 2;
        planeHeights_[1] = (frame.height + 1) / 2;
        planeFormats_[0] = DXGI_FORMAT_R8_UNORM;
        planeFormats_[1] = DXGI_FORMAT_R8G8_UNORM;
        return true;
    }

    bool uploadPlane(
        std::size_t planeIndex,
        DXGI_FORMAT format,
        int width,
        int height,
        const std::vector<std::uint8_t>& source,
        int sourceStride,
        int copyBytes)
    {
        if (planeIndex >= planeTextures_.size() || width <= 0 || height <= 0 || source.empty()) {
            return false;
        }

        if (!planeTextures_[planeIndex]
            || planeWidths_[planeIndex] != width
            || planeHeights_[planeIndex] != height
            || planeFormats_[planeIndex] != format) {
            planeTextures_[planeIndex].Reset();
            planeViews_[planeIndex].Reset();

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

            HRESULT result = device_->CreateTexture2D(&textureDesc, nullptr, planeTextures_[planeIndex].GetAddressOf());
            if (failed(result, "ID3D11Device::CreateTexture2D")) {
                return false;
            }

            result = device_->CreateShaderResourceView(
                planeTextures_[planeIndex].Get(),
                nullptr,
                planeViews_[planeIndex].GetAddressOf());
            if (failed(result, "ID3D11Device::CreateShaderResourceView")) {
                planeTextures_[planeIndex].Reset();
                return false;
            }

            planeWidths_[planeIndex] = width;
            planeHeights_[planeIndex] = height;
            planeFormats_[planeIndex] = format;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result = context_->Map(planeTextures_[planeIndex].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
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

        context_->Unmap(planeTextures_[planeIndex].Get(), 0);
        return true;
    }

    void uploadFrame(const VideoFrame& frame)
    {
        if (uploadedFrameIndex_ == frame.index) {
            return;
        }

        if (textureFormat_ != frame.format || textureWidth_ != frame.width || textureHeight_ != frame.height) {
            resetFrameTextures();
            textureFormat_ = frame.format;
            textureWidth_ = frame.width;
            textureHeight_ = frame.height;
        }

        bool uploaded = false;
        if (frame.format == VideoFrameFormat::D3D11_NV12) {
            uploaded = useD3D11FrameTexture(frame);
        } else if (frame.format == VideoFrameFormat::BGRA) {
            uploaded = uploadPlane(
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
            uploaded = uploadPlane(0, DXGI_FORMAT_R8_UNORM, frame.width, frame.height, frame.planes[0], frame.strides[0], frame.width)
                && uploadPlane(1, DXGI_FORMAT_R8G8_UNORM, chromaWidth, chromaHeight, frame.planes[1], frame.strides[1], frame.strides[1]);
        } else if (frame.format == VideoFrameFormat::YUV420P) {
            const int chromaWidth = (frame.width + 1) / 2;
            const int chromaHeight = (frame.height + 1) / 2;
            uploaded = uploadPlane(0, DXGI_FORMAT_R8_UNORM, frame.width, frame.height, frame.planes[0], frame.strides[0], frame.width)
                && uploadPlane(1, DXGI_FORMAT_R8_UNORM, chromaWidth, chromaHeight, frame.planes[1], frame.strides[1], chromaWidth)
                && uploadPlane(2, DXGI_FORMAT_R8_UNORM, chromaWidth, chromaHeight, frame.planes[2], frame.strides[2], chromaWidth);
        }

        if (uploaded) {
            textureFullRange_ = frame.fullRange;
            uploadedFrameIndex_ = frame.index;
        }
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
    ComPtr<ID3D11PixelShader> bgraPixelShader_;
    ComPtr<ID3D11PixelShader> nv12PixelShader_;
    ComPtr<ID3D11PixelShader> yuv420PixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> colorMatrixBuffer_;
    std::array<ComPtr<ID3D11Texture2D>, 3> planeTextures_;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> planeViews_;
    std::array<int, 3> planeWidths_ = {};
    std::array<int, 3> planeHeights_ = {};
    std::array<DXGI_FORMAT, 3> planeFormats_ = {
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
    };

    VideoFrameFormat textureFormat_ = VideoFrameFormat::BGRA;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    bool textureFullRange_ = false;
    std::uint64_t uploadedFrameIndex_ = 0;
    std::shared_ptr<void> activeFrameOwner_;
    ID3D11Texture2D* d3d11SourceTexture_ = nullptr;
    int d3d11SourceSlice_ = 0;
    std::shared_ptr<D3D11DecodeContext> decodeContext_ = std::make_shared<D3D11DecodeContext>();
};

} // namespace

std::unique_ptr<VideoRenderer> createD3D11Renderer()
{
    return std::make_unique<D3D11Renderer>();
}
