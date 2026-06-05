#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace gig {

struct OverlayColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

// Immediate-mode 2D text/quad overlay for the D3D11 renderer. Bakes a monospace
// font atlas with GDI at startup, then batches alpha-blended, per-vertex-colored
// quads (glyphs sample the atlas; rect() samples a reserved white texel). Build
// a batch with begin()/text()/rect(), then flush() once per frame after the
// tiles. Pixel coordinates; origin top-left.
class TextOverlay {
public:
    bool initialize(ID3D11Device* device, int pixelHeight = 32);
    bool ready() const { return ready_; }

    void begin(int backBufferWidth, int backBufferHeight);
    void rect(float x, float y, float w, float h, OverlayColor color);
    // Draw `str` with its top-left at (x, y); returns the advance width in pixels.
    float text(float x, float y, float scale, OverlayColor color, std::string_view str);
    void flush(ID3D11DeviceContext* context);

    float textWidth(std::string_view str, float scale) const { return static_cast<float>(str.size()) * cellWidth_ * scale; }
    float lineHeight(float scale) const { return cellHeight_ * scale; }
    float glyphWidth(float scale) const { return cellWidth_ * scale; }

private:
    struct Vertex {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    bool bakeAtlas(ID3D11Device* device, int pixelHeight);
    bool createPipeline(ID3D11Device* device);
    bool ensureVertexBuffer(std::size_t vertexCount);
    void appendQuad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, OverlayColor color);

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> atlas_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> atlasView_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;

    ID3D11Device* device_ = nullptr;
    std::vector<Vertex> vertices_;
    std::size_t bufferCapacity_ = 0;

    int atlasWidth_ = 0;
    int atlasHeight_ = 0;
    int cellWidth_ = 0;
    int cellHeight_ = 0;
    static constexpr int columns_ = 16;
    static constexpr int firstChar_ = 32;
    static constexpr int lastChar_ = 126;
    static constexpr int whiteCellIndex_ = 95; // unused glyph slot, painted solid white for rect()

    float backWidth_ = 1.0f;
    float backHeight_ = 1.0f;
    bool ready_ = false;
};

} // namespace gig
