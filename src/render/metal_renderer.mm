#include "render/grid_layout.h"
#include "render/video_renderer.h"

#include "log.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// Phase C1 of the macOS renderer: software-frame video display. It uploads the
// decoder's CPU planes (BGRA / NV12 / YUV420P) to MTLTextures and draws each camera
// letterboxed into the shared grid_layout, with YUV->RGB in MSL fragment shaders.
// Click-to-focus renders one tile fullscreen. Still TODO (C2-C4): the procedural
// "signal" scope on frameless tiles, hover + zoom animation (and isAnimating()),
// the text overlay (labels + diagnostics), and imgui_impl_metal (toolbar/log/banner).

namespace {

// MSL: a full-screen quad generated from vertex_id (no vertex buffer), plus one
// fragment per pixel format. NV12/YUV420 do the YUV->RGB matrix the same way the
// D3D11 shaders do (saturate(dot(row, [y,u,v,1]))).
const char* const kShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut vs_main(uint vid [[vertex_id]]) {
    const float2 pos[4] = { float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0) };
    const float2 uv[4]  = { float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0), float2(1.0, 1.0) };
    VSOut out;
    out.position = float4(pos[vid], 0.0, 1.0);
    out.uv = uv[vid];
    return out;
}

constexpr sampler kSampler(filter::linear, address::clamp_to_edge);

fragment float4 fs_bgra(VSOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
    return tex.sample(kSampler, in.uv);
}

struct ColorMatrix { float4 r; float4 g; float4 b; };

fragment float4 fs_nv12(VSOut in [[stage_in]],
                        texture2d<float> ytex [[texture(0)]],
                        texture2d<float> uvtex [[texture(1)]],
                        constant ColorMatrix& m [[buffer(0)]]) {
    float y = ytex.sample(kSampler, in.uv).r;
    float2 uv = uvtex.sample(kSampler, in.uv).rg;
    float4 px = float4(y, uv.x, uv.y, 1.0);
    return float4(saturate(dot(m.r, px)), saturate(dot(m.g, px)), saturate(dot(m.b, px)), 1.0);
}

fragment float4 fs_yuv420(VSOut in [[stage_in]],
                          texture2d<float> ytex [[texture(0)]],
                          texture2d<float> utex [[texture(1)]],
                          texture2d<float> vtex [[texture(2)]],
                          constant ColorMatrix& m [[buffer(0)]]) {
    float y = ytex.sample(kSampler, in.uv).r;
    float u = utex.sample(kSampler, in.uv).r;
    float v = vtex.sample(kSampler, in.uv).r;
    float4 px = float4(y, u, v, 1.0);
    return float4(saturate(dot(m.r, px)), saturate(dot(m.g, px)), saturate(dot(m.b, px)), 1.0);
}
)METAL";

// 3 rows (r,g,b) of the YUV->RGB matrix; matches d3d11_renderer's constants.
struct ColorMatrix {
    float r[4];
    float g[4];
    float b[4];
};

ColorMatrix yuvToRgb(bool fullRange)
{
    if (fullRange) {
        return {
            { 1.0f, 0.0f, 1.5748f, -0.7874f },
            { 1.0f, -0.1873f, -0.4681f, 0.3277f },
            { 1.0f, 1.8556f, 0.0f, -0.9278f },
        };
    }
    return {
        { 1.164383f, 0.0f, 1.792741f, -0.969430f },
        { 1.164383f, -0.213249f, -0.532909f, 0.300047f },
        { 1.164383f, 2.112402f, 0.0f, -1.129253f },
    };
}

// One camera's GPU state: a texture per plane plus the identity of the last upload,
// so a tile re-uploads only when its frame index changes.
struct TileState {
    id<MTLTexture> plane[3] = { nil, nil, nil };
    int planeW[3] = { 0, 0, 0 };
    int planeH[3] = { 0, 0, 0 };
    MTLPixelFormat planeFmt[3] = { MTLPixelFormatInvalid, MTLPixelFormatInvalid, MTLPixelFormatInvalid };
    VideoFrameFormat format = VideoFrameFormat::BGRA;
    int texW = 0;
    int texH = 0;
    bool fullRange = false;
    std::uint64_t uploadedFrameIndex = 0;
};

class MetalRenderer final : public VideoRenderer {
public:
    bool initialize(SDL_Window* window) override
    {
        window_ = window;

        metalView_ = SDL_Metal_CreateView(window);
        if (!metalView_) {
            gig::logError() << "SDL_Metal_CreateView failed: " << SDL_GetError();
            return false;
        }
        layer_ = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(metalView_);
        if (!layer_) {
            gig::logError() << "SDL_Metal_GetLayer returned null";
            return false;
        }

        device_ = MTLCreateSystemDefaultDevice();
        if (!device_) {
            gig::logError() << "MTLCreateSystemDefaultDevice failed";
            return false;
        }
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        queue_ = [device_ newCommandQueue];
        if (!queue_) {
            gig::logError() << "Metal command queue creation failed";
            return false;
        }

        NSError* error = nil;
        id<MTLLibrary> library =
            [device_ newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:nil error:&error];
        if (!library) {
            gig::logError() << "metal shader compile failed: "
                            << (error.localizedDescription.UTF8String ?: "(unknown)");
            return false;
        }
        id<MTLFunction> vs = [library newFunctionWithName:@"vs_main"];
        pipelineBgra_ = makePipeline(vs, [library newFunctionWithName:@"fs_bgra"]);
        pipelineNv12_ = makePipeline(vs, [library newFunctionWithName:@"fs_nv12"]);
        pipelineYuv420_ = makePipeline(vs, [library newFunctionWithName:@"fs_yuv420"]);
        if (!pipelineBgra_ || !pipelineNv12_ || !pipelineYuv420_) {
            return false;
        }

        gig::logInfo() << "metal renderer ready: " << device_.name.UTF8String;
        return true;
    }

    void resize() override
    {
        // drawableSize is refreshed from the window's pixel size each render().
    }

    void render(const std::vector<std::shared_ptr<VideoFrame>>& frames) override
    {
        if (!layer_ || !queue_) {
            return;
        }

        @autoreleasepool {
            int pixelWidth = 0;
            int pixelHeight = 0;
            SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight);
            if (pixelWidth <= 0 || pixelHeight <= 0) {
                return;
            }
            layer_.drawableSize = CGSizeMake(pixelWidth, pixelHeight);

            if (tiles_.size() != frames.size()) {
                tiles_.resize(frames.size());
            }

            id<CAMetalDrawable> drawable = [layer_ nextDrawable];
            if (!drawable) {
                return;
            }

            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.01, 0.01, 0.012, 1.0);

            id<MTLCommandBuffer> commandBuffer = [queue_ commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];

            const bool focused = focusedTile_ >= 0 && focusedTile_ < static_cast<int>(frames.size());
            if (focused) {
                const gig::TileRect full {
                    0.0f, 0.0f, static_cast<float>(pixelWidth), static_cast<float>(pixelHeight)
                };
                drawOne(encoder, static_cast<std::size_t>(focusedTile_), frames, full);
            } else {
                const gig::GridLayout layout =
                    gig::computeGridLayout(static_cast<int>(frames.size()), pixelWidth, pixelHeight);
                for (std::size_t i = 0; i < frames.size() && i < layout.tiles.size(); ++i) {
                    drawOne(encoder, i, frames, layout.tiles[i]);
                }
            }

            [encoder endEncoding];
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
    }

    // No animation in C1: the run loop draws on new frames / input. (C2 adds the
    // signal scope + zoom/hover and flips this true while they're in flight.)
    bool isAnimating() const override { return false; }

    void setFocusedTile(int index) override { focusedTile_ = index; }
    int focusedTile() const override { return focusedTile_; }

    void setCameraLabels(const std::vector<std::string>&) override {}
    void setDiagnostics(const OverlayStats&) override {}

    ~MetalRenderer() override
    {
        if (metalView_) {
            SDL_Metal_DestroyView(metalView_);
            metalView_ = nullptr;
        }
    }

private:
    id<MTLRenderPipelineState> makePipeline(id<MTLFunction> vertexFn, id<MTLFunction> fragmentFn)
    {
        if (!vertexFn || !fragmentFn) {
            gig::logError() << "metal: missing shader function";
            return nil;
        }
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertexFn;
        descriptor.fragmentFunction = fragmentFn;
        descriptor.colorAttachments[0].pixelFormat = layer_.pixelFormat;

        NSError* error = nil;
        id<MTLRenderPipelineState> state =
            [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!state) {
            gig::logError() << "metal pipeline creation failed: "
                            << (error.localizedDescription.UTF8String ?: "(unknown)");
        }
        return state;
    }

    id<MTLTexture> makeTexture(MTLPixelFormat format, int width, int height)
    {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                               width:static_cast<NSUInteger>(width)
                                                              height:static_cast<NSUInteger>(height)
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeShared; // CPU upload via replaceRegion (unified memory)
        return [device_ newTextureWithDescriptor:descriptor];
    }

    void ensurePlane(TileState& tile, int i, MTLPixelFormat format, int width, int height)
    {
        if (tile.plane[i] && tile.planeW[i] == width && tile.planeH[i] == height && tile.planeFmt[i] == format) {
            return;
        }
        tile.plane[i] = makeTexture(format, width, height);
        tile.planeW[i] = width;
        tile.planeH[i] = height;
        tile.planeFmt[i] = format;
    }

    static void uploadPlane(id<MTLTexture> texture, int width, int height,
                            const std::vector<std::uint8_t>& source, int stride)
    {
        if (!texture || source.empty() || width <= 0 || height <= 0) {
            return;
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width), static_cast<NSUInteger>(height))
                   mipmapLevel:0
                     withBytes:source.data()
                   bytesPerRow:static_cast<NSUInteger>(stride)];
    }

    void uploadFrame(TileState& tile, const VideoFrame& frame)
    {
        if (tile.uploadedFrameIndex == frame.index && tile.plane[0]
            && tile.format == frame.format && tile.texW == frame.width && tile.texH == frame.height) {
            return;
        }
        if (tile.format != frame.format || tile.texW != frame.width || tile.texH != frame.height) {
            for (int i = 0; i < 3; ++i) {
                tile.plane[i] = nil;
                tile.planeW[i] = 0;
                tile.planeH[i] = 0;
                tile.planeFmt[i] = MTLPixelFormatInvalid;
            }
            tile.format = frame.format;
            tile.texW = frame.width;
            tile.texH = frame.height;
        }

        const int chromaWidth = (frame.width + 1) / 2;
        const int chromaHeight = (frame.height + 1) / 2;
        switch (frame.format) {
        case VideoFrameFormat::BGRA:
            ensurePlane(tile, 0, MTLPixelFormatBGRA8Unorm, frame.width, frame.height);
            uploadPlane(tile.plane[0], frame.width, frame.height, frame.planes[0], frame.strides[0]);
            break;
        case VideoFrameFormat::NV12:
            ensurePlane(tile, 0, MTLPixelFormatR8Unorm, frame.width, frame.height);
            ensurePlane(tile, 1, MTLPixelFormatRG8Unorm, chromaWidth, chromaHeight);
            uploadPlane(tile.plane[0], frame.width, frame.height, frame.planes[0], frame.strides[0]);
            uploadPlane(tile.plane[1], chromaWidth, chromaHeight, frame.planes[1], frame.strides[1]);
            break;
        case VideoFrameFormat::YUV420P:
            ensurePlane(tile, 0, MTLPixelFormatR8Unorm, frame.width, frame.height);
            ensurePlane(tile, 1, MTLPixelFormatR8Unorm, chromaWidth, chromaHeight);
            ensurePlane(tile, 2, MTLPixelFormatR8Unorm, chromaWidth, chromaHeight);
            uploadPlane(tile.plane[0], frame.width, frame.height, frame.planes[0], frame.strides[0]);
            uploadPlane(tile.plane[1], chromaWidth, chromaHeight, frame.planes[1], frame.strides[1]);
            uploadPlane(tile.plane[2], chromaWidth, chromaHeight, frame.planes[2], frame.strides[2]);
            break;
        case VideoFrameFormat::D3D11_NV12:
            return; // never produced on macOS (hw path is Windows-only)
        }

        tile.fullRange = frame.fullRange;
        tile.uploadedFrameIndex = frame.index;
    }

    static MTLViewport computeVideoViewport(const gig::TileRect& cell, int textureWidth, int textureHeight)
    {
        MTLViewport viewport {};
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        if (textureWidth <= 0 || textureHeight <= 0 || cell.width <= 0.0f || cell.height <= 0.0f) {
            viewport.originX = cell.x;
            viewport.originY = cell.y;
            viewport.width = cell.width;
            viewport.height = cell.height;
            return viewport;
        }

        const float cellAspect = cell.width / cell.height;
        const float videoAspect = static_cast<float>(textureWidth) / static_cast<float>(textureHeight);
        if (cellAspect > videoAspect) {
            viewport.height = cell.height;
            viewport.width = cell.height * videoAspect;
            viewport.originX = cell.x + (cell.width - viewport.width) * 0.5;
            viewport.originY = cell.y;
        } else {
            viewport.width = cell.width;
            viewport.height = cell.width / videoAspect;
            viewport.originX = cell.x;
            viewport.originY = cell.y + (cell.height - viewport.height) * 0.5;
        }
        return viewport;
    }

    void drawTile(id<MTLRenderCommandEncoder> encoder, const TileState& tile)
    {
        ColorMatrix matrix = yuvToRgb(tile.fullRange);
        switch (tile.format) {
        case VideoFrameFormat::BGRA:
            [encoder setRenderPipelineState:pipelineBgra_];
            [encoder setFragmentTexture:tile.plane[0] atIndex:0];
            break;
        case VideoFrameFormat::NV12:
            [encoder setRenderPipelineState:pipelineNv12_];
            [encoder setFragmentTexture:tile.plane[0] atIndex:0];
            [encoder setFragmentTexture:tile.plane[1] atIndex:1];
            [encoder setFragmentBytes:&matrix length:sizeof(matrix) atIndex:0];
            break;
        case VideoFrameFormat::YUV420P:
            [encoder setRenderPipelineState:pipelineYuv420_];
            [encoder setFragmentTexture:tile.plane[0] atIndex:0];
            [encoder setFragmentTexture:tile.plane[1] atIndex:1];
            [encoder setFragmentTexture:tile.plane[2] atIndex:2];
            [encoder setFragmentBytes:&matrix length:sizeof(matrix) atIndex:0];
            break;
        case VideoFrameFormat::D3D11_NV12:
            return;
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }

    void drawOne(id<MTLRenderCommandEncoder> encoder, std::size_t index,
                 const std::vector<std::shared_ptr<VideoFrame>>& frames, const gig::TileRect& cell)
    {
        TileState& tile = tiles_[index];
        const VideoFrame* frame = frames[index].get();
        const bool hasFrame = frame && !frame->planes[0].empty();
        if (hasFrame) {
            uploadFrame(tile, *frame);
        }
        if (!tile.plane[0]) {
            return; // no frame yet -> black cell in C1 (the signal scope arrives in C2)
        }
        [encoder setViewport:computeVideoViewport(cell, tile.texW, tile.texH)];
        drawTile(encoder, tile);
    }

    SDL_Window* window_ = nullptr;
    SDL_MetalView metalView_ = nullptr;
    CAMetalLayer* layer_ = nil;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLRenderPipelineState> pipelineBgra_ = nil;
    id<MTLRenderPipelineState> pipelineNv12_ = nil;
    id<MTLRenderPipelineState> pipelineYuv420_ = nil;
    std::vector<TileState> tiles_;
    int focusedTile_ = -1;
};

} // namespace

std::unique_ptr<VideoRenderer> createRenderer()
{
    return std::make_unique<MetalRenderer>();
}
