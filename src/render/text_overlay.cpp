#include "render/text_overlay.h"

#include "log.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#include <windows.h>

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace gig {
namespace {

const char* TextVertexShaderSource = R"(
struct VSIn {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
PSIn VSMain(VSIn input) {
    PSIn output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

const char* TextPixelShaderSource = R"(
Texture2D atlasTexture : register(t0);
SamplerState atlasSampler : register(s0);
struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
float4 PSMain(PSIn input) : SV_TARGET {
    float coverage = atlasTexture.Sample(atlasSampler, input.uv).r;
    return float4(input.color.rgb, input.color.a * coverage);
}
)";

bool compile(const char* source, const char* entry, const char* target, ComPtr<ID3DBlob>& blob)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), nullptr, nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result)) {
        if (errors) {
            logError() << "text overlay shader compile failed: " << static_cast<const char*>(errors->GetBufferPointer());
        }
        return false;
    }
    return true;
}

} // namespace

bool TextOverlay::initialize(ID3D11Device* device, int pixelHeight)
{
    device_ = device;
    if (!bakeAtlas(device, pixelHeight) || !createPipeline(device)) {
        return false;
    }
    ready_ = true;
    logInfo() << "text overlay ready: atlas " << atlasWidth_ << "x" << atlasHeight_
              << " cell " << cellWidth_ << "x" << cellHeight_;
    return true;
}

bool TextOverlay::bakeAtlas(ID3D11Device* device, int pixelHeight)
{
    HFONT font = CreateFontW(
        -pixelHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font) {
        logError() << "text overlay: CreateFont failed";
        return false;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(font);
        return false;
    }
    HGDIOBJ oldFont = SelectObject(dc, font);

    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    // Use the average advance, NOT tmMaxCharWidth: for Consolas (and many fonts) GDI
    // reports tmMaxCharWidth ~= 2x the real monospace advance (it's the widest glyph in
    // the whole font), which doubles the letter spacing. For a fixed-pitch font the
    // average width IS the per-glyph advance.
    cellWidth_ = metrics.tmAveCharWidth > 0 ? metrics.tmAveCharWidth : (metrics.tmHeight + 1) / 2;
    cellHeight_ = metrics.tmHeight;
    if (cellWidth_ <= 0 || cellHeight_ <= 0) {
        SelectObject(dc, oldFont);
        DeleteDC(dc);
        DeleteObject(font);
        return false;
    }

    const int rows = 6; // 16 columns x 6 rows = 96 cells (95 glyphs + 1 white)
    atlasWidth_ = columns_ * cellWidth_;
    atlasHeight_ = rows * cellHeight_;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = atlasWidth_;
    bmi.bmiHeader.biHeight = -atlasHeight_; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        SelectObject(dc, oldFont);
        DeleteDC(dc);
        DeleteObject(font);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, dib);

    RECT fullRect = { 0, 0, atlasWidth_, atlasHeight_ };
    FillRect(dc, &fullRect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));

    for (int ch = firstChar_; ch <= lastChar_; ++ch) {
        const int index = ch - firstChar_;
        const int column = index % columns_;
        const int row = index / columns_;
        const wchar_t glyph = static_cast<wchar_t>(ch);
        TextOutW(dc, column * cellWidth_, row * cellHeight_, &glyph, 1);
    }

    // Reserved solid-white cell for rect() fills.
    const int whiteColumn = whiteCellIndex_ % columns_;
    const int whiteRow = whiteCellIndex_ / columns_;
    RECT whiteRect = {
        whiteColumn * cellWidth_,
        whiteRow * cellHeight_,
        whiteColumn * cellWidth_ + cellWidth_,
        whiteRow * cellHeight_ + cellHeight_,
    };
    FillRect(dc, &whiteRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    GdiFlush();

    // BGRA top-down -> R8 coverage (white text on black, so any channel works).
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(atlasWidth_) * static_cast<std::size_t>(atlasHeight_));
    const auto* source = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        coverage[i] = source[i * 4];
    }

    SelectObject(dc, oldBitmap);
    SelectObject(dc, oldFont);
    DeleteObject(dib);
    DeleteDC(dc);
    DeleteObject(font);

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = static_cast<UINT>(atlasWidth_);
    textureDesc.Height = static_cast<UINT>(atlasHeight_);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = coverage.data();
    data.SysMemPitch = static_cast<UINT>(atlasWidth_);

    if (FAILED(device->CreateTexture2D(&textureDesc, &data, atlas_.GetAddressOf()))) {
        logError() << "text overlay: atlas CreateTexture2D failed";
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(atlas_.Get(), nullptr, atlasView_.GetAddressOf()))) {
        return false;
    }
    return true;
}

bool TextOverlay::createPipeline(ID3D11Device* device)
{
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    if (!compile(TextVertexShaderSource, "VSMain", "vs_4_0", vsBlob)
        || !compile(TextPixelShaderSource, "PSMain", "ps_4_0", psBlob)) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vertexShader_.GetAddressOf()))
        || FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, pixelShader_.GetAddressOf()))) {
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), inputLayout_.GetAddressOf()))) {
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDesc, sampler_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, blendState_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE; // 2D quads: don't depend on winding
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, rasterizerState_.GetAddressOf()))) {
        return false;
    }
    return true;
}

void TextOverlay::begin(int backBufferWidth, int backBufferHeight)
{
    backWidth_ = static_cast<float>(std::max(1, backBufferWidth));
    backHeight_ = static_cast<float>(std::max(1, backBufferHeight));
    vertices_.clear();
}

void TextOverlay::appendQuad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, OverlayColor c)
{
    const float x0 = x / backWidth_ * 2.0f - 1.0f;
    const float x1 = (x + w) / backWidth_ * 2.0f - 1.0f;
    const float y0 = 1.0f - y / backHeight_ * 2.0f;
    const float y1 = 1.0f - (y + h) / backHeight_ * 2.0f;

    const Vertex topLeft { x0, y0, u0, v0, c.r, c.g, c.b, c.a };
    const Vertex topRight { x1, y0, u1, v0, c.r, c.g, c.b, c.a };
    const Vertex bottomLeft { x0, y1, u0, v1, c.r, c.g, c.b, c.a };
    const Vertex bottomRight { x1, y1, u1, v1, c.r, c.g, c.b, c.a };

    vertices_.push_back(topLeft);
    vertices_.push_back(topRight);
    vertices_.push_back(bottomLeft);
    vertices_.push_back(topRight);
    vertices_.push_back(bottomRight);
    vertices_.push_back(bottomLeft);
}

void TextOverlay::rect(float x, float y, float w, float h, OverlayColor color)
{
    if (!ready_ || w <= 0.0f || h <= 0.0f) {
        return;
    }
    // Sample the centre of the reserved white cell so all corners read coverage 1.
    const int column = whiteCellIndex_ % columns_;
    const int row = whiteCellIndex_ / columns_;
    const float u = static_cast<float>(column * cellWidth_ + cellWidth_ / 2) / static_cast<float>(atlasWidth_);
    const float v = static_cast<float>(row * cellHeight_ + cellHeight_ / 2) / static_cast<float>(atlasHeight_);
    appendQuad(x, y, w, h, u, v, u, v, color);
}

float TextOverlay::text(float x, float y, float scale, OverlayColor color, std::string_view str)
{
    if (!ready_) {
        return 0.0f;
    }
    const float glyphW = cellWidth_ * scale;
    const float glyphH = cellHeight_ * scale;
    float cursor = x;
    for (const char rawChar : str) {
        const unsigned char ch = static_cast<unsigned char>(rawChar);
        if (ch >= firstChar_ && ch <= lastChar_ && ch != ' ') {
            const int index = ch - firstChar_;
            const int column = index % columns_;
            const int row = index / columns_;
            const float u0 = static_cast<float>(column * cellWidth_) / static_cast<float>(atlasWidth_);
            const float v0 = static_cast<float>(row * cellHeight_) / static_cast<float>(atlasHeight_);
            const float u1 = u0 + static_cast<float>(cellWidth_) / static_cast<float>(atlasWidth_);
            const float v1 = v0 + static_cast<float>(cellHeight_) / static_cast<float>(atlasHeight_);
            appendQuad(cursor, y, glyphW, glyphH, u0, v0, u1, v1, color);
        }
        cursor += glyphW;
    }
    return cursor - x;
}

bool TextOverlay::ensureVertexBuffer(std::size_t vertexCount)
{
    if (vertexBuffer_ && vertexCount <= bufferCapacity_) {
        return true;
    }
    std::size_t capacity = std::max<std::size_t>(vertexCount, bufferCapacity_ ? bufferCapacity_ * 2 : 4096);

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(capacity * sizeof(Vertex));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    vertexBuffer_.Reset();
    if (FAILED(device_->CreateBuffer(&desc, nullptr, vertexBuffer_.GetAddressOf()))) {
        return false;
    }
    bufferCapacity_ = capacity;
    return true;
}

void TextOverlay::flush(ID3D11DeviceContext* context)
{
    if (!ready_ || vertices_.empty() || !ensureVertexBuffer(vertices_.size())) {
        vertices_.clear();
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        vertices_.clear();
        return;
    }
    std::memcpy(mapped.pData, vertices_.data(), vertices_.size() * sizeof(Vertex));
    context->Unmap(vertexBuffer_.Get(), 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* buffers[] = { vertexBuffer_.Get() };
    context->IASetInputLayout(inputLayout_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context->PSSetShader(pixelShader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = { atlasView_.Get() };
    context->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* samplers[] = { sampler_.Get() };
    context->PSSetSamplers(0, 1, samplers);

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetBlendState(blendState_.Get(), blendFactor, 0xffffffff);
    context->RSSetState(rasterizerState_.Get());

    context->Draw(static_cast<UINT>(vertices_.size()), 0);

    // Restore opaque blending so the next frame's video tiles are not blended.
    ID3D11ShaderResourceView* nullViews[] = { nullptr };
    context->PSSetShaderResources(0, 1, nullViews);
    context->OMSetBlendState(nullptr, blendFactor, 0xffffffff);

    vertices_.clear();
}

} // namespace gig
