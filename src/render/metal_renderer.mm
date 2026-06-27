#include "render/grid_layout.h"
#include "render/video_renderer.h"

#include "log.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl3.h>

// macOS renderer: software-frame video display (BGRA/NV12/YUV420P -> MTLTextures,
// YUV->RGB in MSL) drawn letterboxed into the shared grid_layout, plus the full
// chrome ported from the D3D11 renderer: the procedural "signal" scope on frameless
// tiles, the resolve crossfade, hover + click-to-zoom animation, dear imgui (Metal
// backend) for the toolbar / log / status banner, and camera labels + the
// diagnostics tile via imgui draw lists. On-demand rendering via isAnimating().

namespace {

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

// Signal "scope" for frameless tiles (ported from the HLSL; descending smoothsteps
// rewritten as 1 - smoothstep so edge0 < edge1, which Metal requires).
struct SignalConstants { float uTime; float uEnergy; float uSeed; float uAspect; float uAlpha; float p0; float p1; float p2; };

static inline float hash11(float n) { return fract(sin(n) * 43758.5453); }

static inline float traceWave(float x, float t, float e, float seed) {
    float smoothW = sin(x * 7.0 + t * 1.3 + seed) * 0.6 + sin(x * 3.0 - t * 0.9 + seed * 0.5) * 0.4;
    float chaosW = sin(x * 30.0 + t * 20.0 + seed * 2.3) * 0.5 + sin(x * 61.0 - t * 31.0 + seed) * 0.3
                 + (hash11(floor(x * 120.0) + floor(t * 55.0) * 7.0) - 0.5) * 1.0;
    float shape = mix(smoothW, chaosW, e);
    float a = 0.04 + e * 0.34;
    return shape * a;
}

fragment float4 fs_signal(VSOut in [[stage_in]], constant SignalConstants& c [[buffer(0)]]) {
    float2 uv = in.uv;
    float t = c.uTime;
    float e = saturate(c.uEnergy);
    float3 col = float3(0.043, 0.051, 0.071);
    col += (1.0 - smoothstep(0.0, 0.004, abs(uv.y - 0.5))) * 0.05;
    float3 cold = float3(0.88, 0.62, 0.16);
    float3 hot  = float3(0.27, 0.80, 0.66);
    float3 traceCol = mix(cold, hot, saturate(e * 1.4));
    float w = traceWave(uv.x, t, e, c.uSeed);
    float d = abs(uv.y - 0.5 - w);
    float core = 1.0 - smoothstep(0.0, 0.018, d);
    float glow = (1.0 - smoothstep(0.0, 0.085, d)) * 0.22;
    col += traceCol * (core + glow);
    float head = (1.0 - smoothstep(0.0, 0.03, distance(uv, float2(0.985, 0.5 + w)))) * e;
    col += traceCol * head;
    float coldness = 1.0 - saturate(e / 0.18);
    if (coldness > 0.001) {
        float sweepX = fract(t * 0.5 + c.uSeed * 0.37);
        float beam = 1.0 - smoothstep(0.0, 0.012, abs(uv.x - sweepX));
        float behind = sweepX - uv.x;
        float trail = (behind > 0.0) ? saturate(1.0 - behind / 0.16) * 0.16 : 0.0;
        col += cold * (beam * 0.6 + trail) * coldness;
    }
    return float4(col, c.uAlpha);
}

struct HoverConstants { float2 uSizePx; float uBorderPx; float uPad; float4 uColor; };

fragment float4 fs_hover(VSOut in [[stage_in]], constant HoverConstants& c [[buffer(0)]]) {
    float2 px = in.uv * c.uSizePx;
    float2 dEdge = min(px, c.uSizePx - px);
    float edge = min(dEdge.x, dEdge.y);
    float border = 1.0 - smoothstep(c.uBorderPx - 1.0, c.uBorderPx + 1.0, edge);
    float a = saturate(border + 0.06) * c.uColor.a;
    return float4(c.uColor.rgb, a);
}
)METAL";

struct ColorMatrix {
    float r[4];
    float g[4];
    float b[4];
};

struct SignalConstants {
    float time, energy, seed, aspect, alpha, pad0, pad1, pad2;
};

struct HoverConstants {
    float sizePx[2];
    float borderPx;
    float pad;
    float color[4];
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

float smoothstep01(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

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

    // VideoToolbox zero-copy: CVMetalTextures wrapping the current CVPixelBuffer's
    // Y + CbCr planes (plane[0]/plane[1] reference their MTLTextures), plus the frame
    // owner keeping the pixel buffer (IOSurface) alive. Released when the next frame
    // replaces them (the GPU has finished the prior draw by then).
    CVMetalTextureRef cvY = nullptr;
    CVMetalTextureRef cvCbCr = nullptr;
    std::shared_ptr<void> gpuFrameOwner;

    float signalEnergy = 0.0f;
    std::uint64_t signalLastBytes = 0;
    bool showedSignal = false;
    float frameFade = -1.0f;
};

constexpr float kToolbarLogicalHeight = 32.0f;
constexpr float kBannerLogicalHeight = 22.0f;
constexpr float kToolbarHideDelay = 2.5f;
constexpr float kZoomDuration = 0.30f;
constexpr float kFadeDur = 0.22f;

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
        device_ = MTLCreateSystemDefaultDevice();
        if (!layer_ || !device_) {
            gig::logError() << "Metal layer/device unavailable";
            return false;
        }
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        queue_ = [device_ newCommandQueue];

        NSError* error = nil;
        id<MTLLibrary> library =
            [device_ newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:nil error:&error];
        if (!library) {
            gig::logError() << "metal shader compile failed: "
                            << (error ? error.localizedDescription.UTF8String : "(unknown)");
            return false;
        }
        id<MTLFunction> vs = [library newFunctionWithName:@"vs_main"];
        pipelineBgra_ = makePipeline(vs, [library newFunctionWithName:@"fs_bgra"], false);
        pipelineNv12_ = makePipeline(vs, [library newFunctionWithName:@"fs_nv12"], false);
        pipelineYuv420_ = makePipeline(vs, [library newFunctionWithName:@"fs_yuv420"], false);
        pipelineSignal_ = makePipeline(vs, [library newFunctionWithName:@"fs_signal"], true);
        pipelineHover_ = makePipeline(vs, [library newFunctionWithName:@"fs_hover"], true);
        if (!pipelineBgra_ || !pipelineNv12_ || !pipelineYuv420_ || !pipelineSignal_ || !pipelineHover_) {
            return false;
        }

        // Initial display scale (pixels per point) for the HiDPI font bake below and
        // the video viewports. Recomputed each render; re-baking the atlas on a
        // mid-session display-scale change is a remaining item.
        {
            int pointW = 0, pointH = 0, pixelW = 0, pixelH = 0;
            SDL_GetWindowSize(window_, &pointW, &pointH);
            SDL_GetWindowSizeInPixels(window_, &pixelW, &pixelH);
            scale_ = (pointH > 0) ? static_cast<float>(pixelH) / static_cast<float>(pointH) : 1.0f;
        }

        // Toolbar glyphs (SF Symbols -> textures); nil falls back to text labels.
        iconSettings_ = makeSymbolTexture(@"gearshape");
        iconReconnect_ = makeSymbolTexture(@"arrow.clockwise");
        iconLog_ = makeSymbolTexture(@"list.bullet");

        // VideoToolbox zero-copy: a texture cache that wraps decoded CVPixelBuffers'
        // IOSurfaces as MTLTextures with no copy. Non-fatal on failure (hw frames just
        // won't display; software decode is unaffected).
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device_, nullptr, &cvTextureCache_)
            != kCVReturnSuccess) {
            cvTextureCache_ = nullptr;
            gig::logWarning() << "CVMetalTextureCacheCreate failed; VideoToolbox frames will not display.";
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        loadImguiFont();
        applyImguiStyle();
        if (ImGui_ImplSDL3_InitForMetal(window_) && ImGui_ImplMetal_Init(device_)) {
            imguiReady_ = true;
        } else {
            gig::logWarning() << "ImGui init failed; toolbar/log disabled.";
            ImGui::DestroyContext();
        }

        bakedScale_ = scale_; // the font + glyphs above were baked at this scale
        gig::logInfo() << "metal renderer ready: " << device_.name.UTF8String;
        return true;
    }

    ~MetalRenderer() override
    {
        if (imguiReady_) {
            ImGui_ImplMetal_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
        }
        if (metalView_) {
            SDL_Metal_DestroyView(metalView_);
            metalView_ = nullptr;
        }
        for (TileState& tile : tiles_) {
            releaseCvTextures(tile);
        }
        if (cvTextureCache_) {
            CFRelease(cvTextureCache_);
            cvTextureCache_ = nullptr;
        }
    }

    void resize() override {}

    void render(const std::vector<std::shared_ptr<VideoFrame>>& frames) override
    {
        if (!layer_ || !queue_) {
            return;
        }

        @autoreleasepool {
            int pointWidth = 0, pointHeight = 0, pixelWidth = 0, pixelHeight = 0;
            SDL_GetWindowSize(window_, &pointWidth, &pointHeight);
            SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight);
            if (pixelWidth <= 0 || pixelHeight <= 0 || pointWidth <= 0 || pointHeight <= 0) {
                return;
            }
            scale_ = static_cast<float>(pixelHeight) / static_cast<float>(pointHeight);
            layer_.drawableSize = CGSizeMake(pixelWidth, pixelHeight);
            applyDpiScale(); // rebake the font + glyphs if the display scale changed

            if (tiles_.size() != frames.size()) {
                // Release any VideoToolbox wrappers before tiles are dropped/realloc'd
                // (a raw CVMetalTextureRef would otherwise leak); survivors re-upload.
                for (TileState& tile : tiles_) {
                    releaseCvTextures(tile);
                }
                tiles_.resize(frames.size());
            }

            const auto nowTp = std::chrono::steady_clock::now();
            float dt = haveRenderTp_ ? std::chrono::duration<float>(nowTp - lastRenderTp_).count() : 0.0f;
            lastRenderTp_ = nowTp;
            haveRenderTp_ = true;
            dt = std::clamp(dt, 0.0f, 0.1f);
            lastDt_ = dt;
            animTime_ += dt;
            updateActivity(dt);

            const float zoomTarget = (focusedTile_ >= 0) ? 1.0f : 0.0f;
            if (animProgress_ != zoomTarget) {
                const float step = (kZoomDuration > 0.0f) ? dt / kZoomDuration : 1.0f;
                animProgress_ = (animProgress_ < zoomTarget) ? std::min(zoomTarget, animProgress_ + step)
                                                             : std::max(zoomTarget, animProgress_ - step);
            }
            sawAnimatedContent_ = false;

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

            // Layout in points (matches main.cpp's hit-test + imgui); video viewports
            // scale to pixels. Reserve the toolbar strip in grid view only.
            const gig::TileRect fullPts { 0.0f, 0.0f, static_cast<float>(pointWidth), static_cast<float>(pointHeight) };
            const bool fullyFocused = focusedTile_ >= 0 && animProgress_ >= 1.0f
                && focusedTile_ < static_cast<int>(frames.size());

            static const gig::GridLayout kEmptyLayout;
            const gig::GridLayout* layout = &kEmptyLayout;
            if (fullyFocused) {
                renderSingleTile(encoder, static_cast<std::size_t>(focusedTile_), frames, fullPts);
                if (hoveredTile_ == focusedTile_) {
                    drawHover(encoder, fullPts);
                }
            } else {
                const float toolbarTop = toolbarHeightLogical();
                const int gridHeight = std::max(1, pointHeight - static_cast<int>(toolbarTop));
                const bool showDiag = overlayStats_.showDiagnostics;
                const int effective = static_cast<int>(frames.size()) + (showDiag ? 1 : 0);
                // Cache the layout: it depends only on (count, width, height), so
                // recompute only when one changes -- not every render.
                if (effective != gridCacheCount_ || pointWidth != gridCacheWidth_
                    || gridHeight != gridCacheHeight_) {
                    gridLayoutCache_ = gig::computeGridLayout(effective, pointWidth, gridHeight);
                    for (gig::TileRect& tile : gridLayoutCache_.tiles) {
                        tile.y += toolbarTop;
                    }
                    gridCacheCount_ = effective;
                    gridCacheWidth_ = pointWidth;
                    gridCacheHeight_ = gridHeight;
                }
                layout = &gridLayoutCache_;
                renderGridTiles(encoder, frames, *layout);

                if (animProgress_ > 0.0f && animTile_ >= 0 && animTile_ < static_cast<int>(frames.size())
                    && animTile_ < static_cast<int>(layout->tiles.size())) {
                    const gig::TileRect& cell = layout->tiles[static_cast<std::size_t>(animTile_)];
                    const float e = smoothstep01(animProgress_);
                    const gig::TileRect grown {
                        std::lerp(cell.x, 0.0f, e), std::lerp(cell.y, 0.0f, e),
                        std::lerp(cell.width, static_cast<float>(pointWidth), e),
                        std::lerp(cell.height, static_cast<float>(pointHeight), e),
                    };
                    drawTileContentAt(encoder, static_cast<std::size_t>(animTile_), grown);
                }
                if (animProgress_ == 0.0f && hoveredTile_ >= 0 && hoveredTile_ < static_cast<int>(frames.size())
                    && hoveredTile_ < static_cast<int>(layout->tiles.size())) {
                    drawHover(encoder, layout->tiles[static_cast<std::size_t>(hoveredTile_)]);
                }
            }

            renderImGui(commandBuffer, encoder, pass, frames, *layout, fullyFocused, fullPts);

            const bool zoomAnimating = animProgress_ != zoomTarget;
            const bool toolbarAnimating = focusedTile_ >= 0 && toolbarIdle_ < kToolbarHideDelay;
            animating_ = sawAnimatedContent_ || zoomAnimating || toolbarAnimating;

            [encoder endEncoding];
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
    }

    bool isAnimating() const override { return animating_; }

    void setFocusedTile(int index) override
    {
        if (index == focusedTile_) {
            return;
        }
        animTile_ = (index >= 0) ? index : focusedTile_;
        focusedTile_ = index;
    }
    int focusedTile() const override { return focusedTile_; }

    void setCameraLabels(const std::vector<std::string>& labels) override { cameraLabels_ = labels; }
    void setDiagnostics(const OverlayStats& stats) override { overlayStats_ = stats; }
    void setLabelMode(LabelMode mode) override { labelMode_ = mode; }
    void setHoveredTile(int index) override { hoveredTile_ = index; }
    void setTileActivity(const std::vector<std::uint64_t>& byteCounts) override { tileBytes_ = byteCounts; }

    bool handleEvent(const SDL_Event& event) override
    {
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

    float reservedTopLogical() const override { return focusedTile_ < 0 ? kToolbarLogicalHeight : 0.0f; }

private:
    float toolbarHeightLogical() const { return kToolbarLogicalHeight; }

    id<MTLRenderPipelineState> makePipeline(id<MTLFunction> vertexFn, id<MTLFunction> fragmentFn, bool blend)
    {
        if (!vertexFn || !fragmentFn) {
            gig::logError() << "metal: missing shader function";
            return nil;
        }
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertexFn;
        descriptor.fragmentFunction = fragmentFn;
        descriptor.colorAttachments[0].pixelFormat = layer_.pixelFormat;
        if (blend) {
            descriptor.colorAttachments[0].blendingEnabled = YES;
            descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!state) {
            gig::logError() << "metal pipeline creation failed: "
                            << (error ? error.localizedDescription.UTF8String : "(unknown)");
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
        descriptor.storageMode = MTLStorageModeShared;
        return [device_ newTextureWithDescriptor:descriptor];
    }

    // Rasterize an SF Symbol to a white-on-transparent RGBA texture for the toolbar
    // (the macOS analog of the Windows Segoe MDL2 glyphs). Returns nil if the symbol
    // is unavailable, in which case buildToolbar falls back to text labels.
    id<MTLTexture> makeSymbolTexture(NSString* symbolName)
    {
        if (!device_) {
            return nil;
        }
        NSImage* image = [NSImage imageWithSystemSymbolName:symbolName accessibilityDescription:nil];
        if (!image) {
            return nil;
        }
        NSImageSymbolConfiguration* sizeCfg =
            [NSImageSymbolConfiguration configurationWithPointSize:15.0 weight:NSFontWeightRegular];
        NSImage* configured = [image imageWithSymbolConfiguration:sizeCfg];
        if (!configured) {
            configured = image;
        }
        const NSSize ptSize = configured.size;
        const int w = std::max(1, static_cast<int>(std::ceil(ptSize.width * scale_)));
        const int h = std::max(1, static_cast<int>(std::ceil(ptSize.height * scale_)));

        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:nullptr pixelsWide:w pixelsHigh:h
                        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                       colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:w * 4 bitsPerPixel:32];
        if (!rep) {
            return nil;
        }
        NSGraphicsContext* gctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = gctx;
        [[NSColor clearColor] set];
        NSRectFill(NSMakeRect(0, 0, w, h));
        [configured drawInRect:NSMakeRect(0, 0, w, h)
                      fromRect:NSZeroRect
                     operation:NSCompositingOperationSourceOver
                      fraction:1.0];
        [gctx flushGraphics];
        [NSGraphicsContext restoreGraphicsState];

        // Force white RGB and keep the rendered alpha as coverage -- straight alpha
        // for ImGui's blend (it tints by the white vertex color), independent of
        // whatever color/premultiplication the symbol drew with.
        unsigned char* bytes = static_cast<unsigned char*>(rep.bitmapData);
        if (bytes) {
            for (int i = 0; i < w * h; ++i) {
                bytes[i * 4 + 0] = 255;
                bytes[i * 4 + 1] = 255;
                bytes[i * 4 + 2] = 255;
            }
        }

        id<MTLTexture> texture = makeTexture(MTLPixelFormatRGBA8Unorm, w, h);
        [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                   mipmapLevel:0
                     withBytes:rep.bitmapData
                   bytesPerRow:static_cast<NSUInteger>(rep.bytesPerRow)];
        return texture;
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
                            const std::uint8_t* source, int stride)
    {
        if (!texture || !source || width <= 0 || height <= 0) {
            return;
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width), static_cast<NSUInteger>(height))
                   mipmapLevel:0
                     withBytes:source
                   bytesPerRow:static_cast<NSUInteger>(stride)];
    }

    // Release the CVMetalTextures (and the pixel-buffer owner) a VideoToolbox tile was
    // holding. Safe on a non-VT tile (the refs are null).
    static void releaseCvTextures(TileState& tile)
    {
        if (tile.cvY || tile.cvCbCr) {
            // plane[0]/plane[1] reference these CV textures; drop them together so a
            // stale MTLTexture can't outlive its CVMetalTexture wrapper.
            tile.plane[0] = nil;
            tile.plane[1] = nil;
        }
        if (tile.cvY) {
            CFRelease(tile.cvY);
            tile.cvY = nullptr;
        }
        if (tile.cvCbCr) {
            CFRelease(tile.cvCbCr);
            tile.cvCbCr = nullptr;
        }
        tile.gpuFrameOwner.reset();
    }

    // Wrap a decoded CVPixelBuffer's Y (R8) + CbCr (RG8) planes as MTLTextures with no
    // copy via the texture cache; plane[0]/plane[1] then feed the existing NV12 draw
    // path. The previous frame's wrappers are released first, and the frame owner pins
    // the pixel buffer (IOSurface) until the next upload replaces it.
    void uploadVideoToolboxFrame(TileState& tile, const VideoFrame& frame)
    {
        releaseCvTextures(tile);
        tile.plane[0] = nil;
        tile.plane[1] = nil;
        tile.plane[2] = nil;

        CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(frame.gpuTexture);
        if (!pixelBuffer || !cvTextureCache_) {
            return;
        }

        const int w = frame.width;
        const int h = frame.height;
        const int cw = (w + 1) / 2;
        const int ch = (h + 1) / 2;
        CVMetalTextureRef yRef = nullptr;
        CVMetalTextureRef cbcrRef = nullptr;
        const CVReturn yResult = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cvTextureCache_, pixelBuffer, nullptr,
            MTLPixelFormatR8Unorm, w, h, 0, &yRef);
        const CVReturn cbcrResult = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cvTextureCache_, pixelBuffer, nullptr,
            MTLPixelFormatRG8Unorm, cw, ch, 1, &cbcrRef);
        if (yResult != kCVReturnSuccess || cbcrResult != kCVReturnSuccess || !yRef || !cbcrRef) {
            if (yRef) {
                CFRelease(yRef);
            }
            if (cbcrRef) {
                CFRelease(cbcrRef);
            }
            gig::logWarning() << "videotoolbox: CVMetalTextureCache create failed";
            return;
        }

        tile.cvY = yRef;
        tile.cvCbCr = cbcrRef;
        tile.gpuFrameOwner = frame.owner; // keep the CVPixelBuffer (IOSurface) alive
        tile.plane[0] = CVMetalTextureGetTexture(yRef);
        tile.plane[1] = CVMetalTextureGetTexture(cbcrRef);
    }

    void uploadFrame(TileState& tile, const VideoFrame& frame)
    {
        if (tile.uploadedFrameIndex == frame.index && tile.plane[0]
            && tile.format == frame.format && tile.texW == frame.width && tile.texH == frame.height) {
            return;
        }
        if (tile.format != frame.format || tile.texW != frame.width || tile.texH != frame.height) {
            releaseCvTextures(tile); // drop any VideoToolbox wrappers from a prior format
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

        const int cw = (frame.width + 1) / 2;
        const int ch = (frame.height + 1) / 2;
        switch (frame.format) {
        case VideoFrameFormat::BGRA:
            ensurePlane(tile, 0, MTLPixelFormatBGRA8Unorm, frame.width, frame.height);
            uploadPlane(tile.plane[0], frame.width, frame.height, frame.planeData[0], frame.strides[0]);
            break;
        case VideoFrameFormat::NV12:
            ensurePlane(tile, 0, MTLPixelFormatR8Unorm, frame.width, frame.height);
            ensurePlane(tile, 1, MTLPixelFormatRG8Unorm, cw, ch);
            uploadPlane(tile.plane[0], frame.width, frame.height, frame.planeData[0], frame.strides[0]);
            uploadPlane(tile.plane[1], cw, ch, frame.planeData[1], frame.strides[1]);
            break;
        case VideoFrameFormat::YUV420P:
            ensurePlane(tile, 0, MTLPixelFormatR8Unorm, frame.width, frame.height);
            ensurePlane(tile, 1, MTLPixelFormatR8Unorm, cw, ch);
            ensurePlane(tile, 2, MTLPixelFormatR8Unorm, cw, ch);
            uploadPlane(tile.plane[0], frame.width, frame.height, frame.planeData[0], frame.strides[0]);
            uploadPlane(tile.plane[1], cw, ch, frame.planeData[1], frame.strides[1]);
            uploadPlane(tile.plane[2], cw, ch, frame.planeData[2], frame.strides[2]);
            break;
        case VideoFrameFormat::GPU_NV12:
            uploadVideoToolboxFrame(tile, frame);
            break;
        }
        tile.fullRange = frame.fullRange;
        tile.uploadedFrameIndex = frame.index;
    }

    // Letterbox a video of texW x texH into a points cell, returning a PIXEL viewport.
    MTLViewport videoViewport(const gig::TileRect& cellPts, int texW, int texH) const
    {
        gig::TileRect c { cellPts.x * scale_, cellPts.y * scale_, cellPts.width * scale_, cellPts.height * scale_ };
        MTLViewport vp {};
        vp.znear = 0.0;
        vp.zfar = 1.0;
        if (texW <= 0 || texH <= 0 || c.width <= 0.0f || c.height <= 0.0f) {
            vp.originX = c.x; vp.originY = c.y; vp.width = c.width; vp.height = c.height;
            return vp;
        }
        const float cellAspect = c.width / c.height;
        const float videoAspect = static_cast<float>(texW) / static_cast<float>(texH);
        if (cellAspect > videoAspect) {
            vp.height = c.height;
            vp.width = c.height * videoAspect;
            vp.originX = c.x + (c.width - vp.width) * 0.5;
            vp.originY = c.y;
        } else {
            vp.width = c.width;
            vp.height = c.width / videoAspect;
            vp.originX = c.x;
            vp.originY = c.y + (c.height - vp.height) * 0.5;
        }
        return vp;
    }

    MTLViewport cellViewport(const gig::TileRect& cellPts) const
    {
        MTLViewport vp {};
        vp.originX = cellPts.x * scale_;
        vp.originY = cellPts.y * scale_;
        vp.width = cellPts.width * scale_;
        vp.height = cellPts.height * scale_;
        vp.znear = 0.0;
        vp.zfar = 1.0;
        return vp;
    }

    void drawTile(id<MTLRenderCommandEncoder> encoder, const TileState& tile)
    {
        // Both matrices are constants; compute once, not per tile per frame.
        static const ColorMatrix kLimited = yuvToRgb(false);
        static const ColorMatrix kFull = yuvToRgb(true);
        const ColorMatrix& matrix = tile.fullRange ? kFull : kLimited;
        switch (tile.format) {
        case VideoFrameFormat::BGRA:
            [encoder setRenderPipelineState:pipelineBgra_];
            [encoder setFragmentTexture:tile.plane[0] atIndex:0];
            break;
        case VideoFrameFormat::NV12:
        case VideoFrameFormat::GPU_NV12: // VideoToolbox: same Y + CbCr sampling as NV12
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
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }

    void drawSignal(id<MTLRenderCommandEncoder> encoder, const gig::TileRect& cellPts, std::size_t index, float energy, float alpha)
    {
        if (cellPts.width <= 0.0f || cellPts.height <= 0.0f) {
            return;
        }
        [encoder setViewport:cellViewport(cellPts)];
        SignalConstants c {};
        c.time = animTime_;
        c.energy = energy;
        c.seed = static_cast<float>(index) * 2.39996f;
        c.aspect = cellPts.width / cellPts.height;
        c.alpha = alpha;
        [encoder setRenderPipelineState:pipelineSignal_];
        [encoder setFragmentBytes:&c length:sizeof(c) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }

    void drawHover(id<MTLRenderCommandEncoder> encoder, const gig::TileRect& cellPts)
    {
        if (cellPts.width <= 0.0f || cellPts.height <= 0.0f) {
            return;
        }
        const MTLViewport vp = cellViewport(cellPts);
        [encoder setViewport:vp];
        HoverConstants c {};
        c.sizePx[0] = static_cast<float>(vp.width);
        c.sizePx[1] = static_cast<float>(vp.height);
        c.borderPx = 2.0f * scale_;
        c.color[0] = 0.80f; c.color[1] = 0.90f; c.color[2] = 1.0f; c.color[3] = 0.6f;
        [encoder setRenderPipelineState:pipelineHover_];
        [encoder setFragmentBytes:&c length:sizeof(c) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }

    void drawTileContentAt(id<MTLRenderCommandEncoder> encoder, std::size_t index, const gig::TileRect& rectPts)
    {
        if (index >= tiles_.size()) {
            return;
        }
        const TileState& tile = tiles_[index];
        if (tile.plane[0]) {
            [encoder setViewport:videoViewport(rectPts, tile.texW, tile.texH)];
            drawTile(encoder, tile);
        } else {
            drawSignal(encoder, rectPts, index, tile.signalEnergy, 1.0f);
        }
    }

    void renderGridTiles(id<MTLRenderCommandEncoder> encoder,
                         const std::vector<std::shared_ptr<VideoFrame>>& frames, const gig::GridLayout& layout)
    {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            TileState& tile = tiles_[i];
            const VideoFrame* frame = frames[i].get();
            const bool hasFrame = frame && (frame->planeData[0] != nullptr || frame->format == VideoFrameFormat::GPU_NV12);
            if (hasFrame) {
                uploadFrame(tile, *frame);
            } else if (tile.plane[0]) {
                for (int p = 0; p < 3; ++p) { tile.plane[p] = nil; }
                releaseCvTextures(tile);
            }
            if (i >= layout.tiles.size()) {
                continue;
            }
            const gig::TileRect& cell = layout.tiles[i];

            if (!tile.plane[0]) {
                drawSignal(encoder, cell, i, tile.signalEnergy, 1.0f);
                sawAnimatedContent_ = true;
                tile.showedSignal = true;
                tile.frameFade = -1.0f;
                continue;
            }
            if (tile.showedSignal) {
                tile.frameFade = 0.0f;
                tile.showedSignal = false;
            }
            [encoder setViewport:videoViewport(cell, tile.texW, tile.texH)];
            drawTile(encoder, tile);
            if (tile.frameFade >= 0.0f) {
                sawAnimatedContent_ = true;
                const float a = 1.0f - tile.frameFade / kFadeDur;
                if (a > 0.0f) {
                    drawSignal(encoder, cell, i, tile.signalEnergy, a);
                }
                tile.frameFade += lastDt_;
                if (tile.frameFade >= kFadeDur) {
                    tile.frameFade = -1.0f;
                }
            }
        }
    }

    void renderSingleTile(id<MTLRenderCommandEncoder> encoder, std::size_t index,
                          const std::vector<std::shared_ptr<VideoFrame>>& frames, const gig::TileRect& full)
    {
        TileState& tile = tiles_[index];
        const VideoFrame* frame = frames[index].get();
        const bool hasFrame = frame && (frame->planeData[0] != nullptr || frame->format == VideoFrameFormat::GPU_NV12);
        if (hasFrame) {
            uploadFrame(tile, *frame);
        } else if (tile.plane[0]) {
            for (int p = 0; p < 3; ++p) { tile.plane[p] = nil; }
            releaseCvTextures(tile);
        }
        if (!tile.plane[0]) {
            drawSignal(encoder, full, index, tile.signalEnergy, 1.0f);
            sawAnimatedContent_ = true;
            tile.showedSignal = true;
            tile.frameFade = -1.0f;
            return;
        }
        if (tile.showedSignal) {
            tile.frameFade = 0.0f;
            tile.showedSignal = false;
        }
        [encoder setViewport:videoViewport(full, tile.texW, tile.texH)];
        drawTile(encoder, tile);
        if (tile.frameFade >= 0.0f) {
            sawAnimatedContent_ = true;
            const float a = 1.0f - tile.frameFade / kFadeDur;
            if (a > 0.0f) {
                drawSignal(encoder, full, index, tile.signalEnergy, a);
            }
            tile.frameFade += lastDt_;
            if (tile.frameFade >= kFadeDur) {
                tile.frameFade = -1.0f;
            }
        }
    }

    void updateActivity(float dt)
    {
        constexpr float kSettleTau = 0.25f;
        const float settle = std::exp(-dt / kSettleTau);
        for (std::size_t i = 0; i < tiles_.size(); ++i) {
            TileState& tile = tiles_[i];
            const std::uint64_t bytes = (i < tileBytes_.size()) ? tileBytes_[i] : tile.signalLastBytes;
            const bool gotData = bytes > tile.signalLastBytes;
            tile.signalLastBytes = bytes;
            tile.signalEnergy = gotData ? 1.0f : tile.signalEnergy * settle;
        }
    }

    std::string labelFor(std::size_t index) const
    {
        return index < cameraLabels_.size() ? cameraLabels_[index] : std::string();
    }

    bool labelVisible(std::size_t index) const
    {
        switch (labelMode_) {
        case LabelMode::Hide: return false;
        case LabelMode::Always: return true;
        case LabelMode::ErrorOnly: return index < tiles_.size() && tiles_[index].showedSignal;
        }
        return false;
    }

    // ---- ImGui (toolbar / banner / log) + camera labels + diagnostics ----------

    void renderImGui(id<MTLCommandBuffer> commandBuffer, id<MTLRenderCommandEncoder> encoder,
                     MTLRenderPassDescriptor* pass, const std::vector<std::shared_ptr<VideoFrame>>& frames,
                     const gig::GridLayout& layout, bool fullyFocused, const gig::TileRect& fullPts)
    {
        if (!imguiReady_) {
            return;
        }
        ImGui_ImplMetal_NewFrame(pass);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Camera labels + diagnostics tile via the background draw list (points; over
        // the video, under the toolbar window).
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        if (fullyFocused) {
            if (labelVisible(static_cast<std::size_t>(focusedTile_))) {
                drawLabel(bg, fullPts, labelFor(static_cast<std::size_t>(focusedTile_)), 2.0f);
            }
        } else {
            for (std::size_t i = 0; i < frames.size() && i < layout.tiles.size(); ++i) {
                if (labelVisible(i)) {
                    drawLabel(bg, layout.tiles[i], labelFor(i), 1.0f);
                }
            }
            if (overlayStats_.showDiagnostics && frames.size() < layout.tiles.size()) {
                drawDiagnostics(bg, layout.tiles[frames.size()]);
            }
        }

        buildToolbar();
        buildStatusBanner();
        if (logViewVisible_) {
            buildLogWindow();
        }

        ImGui::Render();
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, encoder);
    }

    void drawLabel(ImDrawList* dl, const gig::TileRect& cellPts, const std::string& label, float sizeScale)
    {
        if (label.empty() || cellPts.width <= 0.0f) {
            return;
        }
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * sizeScale;
        const float pad = 4.0f;
        std::string shown = label;
        while (!shown.empty()
               && font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, shown.c_str()).x + 2.0f * pad > cellPts.width) {
            shown.pop_back();
        }
        if (shown.empty()) {
            return;
        }
        const float textW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, shown.c_str()).x;
        const float stripW = std::min(cellPts.width, textW + 2.0f * pad);
        const float stripH = fontSize + 2.0f * pad;
        dl->AddRectFilled(ImVec2(cellPts.x, cellPts.y), ImVec2(cellPts.x + stripW, cellPts.y + stripH),
                          IM_COL32(0, 0, 0, 140));
        dl->AddText(font, fontSize, ImVec2(cellPts.x + pad, cellPts.y + pad), IM_COL32(230, 242, 255, 255),
                    shown.c_str());
    }

    void drawDiagnostics(ImDrawList* dl, const gig::TileRect& cellPts)
    {
        if (cellPts.width <= 0.0f || cellPts.height <= 0.0f) {
            return;
        }
        dl->AddRectFilled(ImVec2(cellPts.x, cellPts.y), ImVec2(cellPts.x + cellPts.width, cellPts.y + cellPts.height),
                          IM_COL32(10, 13, 18, 235));
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const float pad = 8.0f;
        float y = cellPts.y + pad;
        const float x = cellPts.x + pad;
        const ImU32 heading = IM_COL32(166, 199, 255, 255);
        const ImU32 body = IM_COL32(217, 230, 255, 255);
        const float lh = fontSize + 4.0f;
        char line[128] = {};

        dl->AddText(font, fontSize, ImVec2(x, y), heading, "diagnostics");
        y += lh * 1.4f;
        std::snprintf(line, sizeof(line), "cams good: %d  bad: %d", overlayStats_.camerasOnline, overlayStats_.camerasOffline);
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
        y += lh;
        std::snprintf(line, sizeof(line), "frames: %d/s, %llu total", static_cast<int>(overlayStats_.fps + 0.5),
                      static_cast<unsigned long long>(overlayStats_.framesTotal));
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
        y += lh;
        std::snprintf(line, sizeof(line), "bandwidth: %d kbps", overlayStats_.kbps);
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
        y += lh;
        std::snprintf(line, sizeof(line), "cpu: %.2f%%", overlayStats_.cpuPercent);
        dl->AddText(font, fontSize, ImVec2(x, y), body, line);
    }

    void buildToolbar()
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool active = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || ImGui::IsMouseDown(ImGuiMouseButton_Left);
        toolbarIdle_ = active ? 0.0f : toolbarIdle_ + io.DeltaTime;
        const bool focusView = focusedTile_ >= 0;
        if (focusView && toolbarIdle_ >= kToolbarHideDelay) {
            return;
        }

        const float height = kToolbarLogicalHeight;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.08f, focusView ? 0.82f : 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 0.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##toolbar", nullptr, flags)) {
            const float rowHeight = ImGui::GetFrameHeight();
            ImGui::SetCursorPosY((height - rowHeight) * 0.5f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("gig");
            ImGui::SameLine();

            const int online = overlayStats_.camerasOnline;
            const int total = overlayStats_.camerasOnline + overlayStats_.camerasOffline;
            const ImVec4 green(0.40f, 0.85f, 0.40f, 1.0f), amber(0.90f, 0.70f, 0.20f, 1.0f), red(0.90f, 0.35f, 0.35f, 1.0f);
            ImGui::AlignTextToFramePadding();
            switch (overlayStats_.link) {
            case OverlayStats::LinkState::Disconnected: ImGui::TextColored(red, "disconnected"); break;
            case OverlayStats::LinkState::Reconnecting: ImGui::TextColored(amber, "reconnecting"); break;
            case OverlayStats::LinkState::Ok: {
                const ImVec4 c = (total > 0 && online == total) ? green : (online == 0 ? red : amber);
                ImGui::TextColored(c, "%d/%d live", online, total);
                break;
            }
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%d fps   %.1f Mbps   cpu %.0f%%", static_cast<int>(overlayStats_.fps + 0.5),
                                overlayStats_.kbps / 1000.0, overlayStats_.cpuPercent);

            // Right-aligned buttons: SF Symbol glyphs when available, else text.
            const ImGuiStyle& style = ImGui::GetStyle();
            const bool icons = iconSettings_ && iconReconnect_ && iconLog_;
            if (icons) {
                const float ext = ImGui::GetFontSize();
                const ImVec2 isz(ext, ext);
                const float btnW = ext + style.FramePadding.x * 2.0f;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - (btnW * 3.0f + style.ItemSpacing.x * 2.0f));
                if (ImGui::ImageButton("##settings", (ImTextureID)(uintptr_t)(__bridge void*)iconSettings_, isz)) {
                    pendingToolbarAction_ = ToolbarAction::Settings;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Settings (F2)"); }
                ImGui::SameLine();
                if (ImGui::ImageButton("##reconnect", (ImTextureID)(uintptr_t)(__bridge void*)iconReconnect_, isz)) {
                    pendingToolbarAction_ = ToolbarAction::Reconnect;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Reconnect (F5)"); }
                ImGui::SameLine();
                if (ImGui::ImageButton("##log", (ImTextureID)(uintptr_t)(__bridge void*)iconLog_, isz)) {
                    logViewVisible_ = !logViewVisible_;
                }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Log"); }
            } else {
                const auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };
                const float buttonsWidth = buttonWidth("Settings") + buttonWidth("Reconnect") + buttonWidth("Log") + style.ItemSpacing.x * 2.0f;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonsWidth);
                if (ImGui::Button("Settings")) { pendingToolbarAction_ = ToolbarAction::Settings; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Settings (F2)"); }
                ImGui::SameLine();
                if (ImGui::Button("Reconnect")) { pendingToolbarAction_ = ToolbarAction::Reconnect; }
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Reconnect (F5)"); }
                ImGui::SameLine();
                if (ImGui::Button("Log")) { logViewVisible_ = !logViewVisible_; }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    void buildStatusBanner()
    {
        if (focusedTile_ >= 0) {
            return;
        }
        const OverlayStats& s = overlayStats_;
        if (s.link == OverlayStats::LinkState::Ok && !s.healthDegraded) {
            return;
        }
        const std::string host = s.statusHost.empty() ? std::string("Frigate") : s.statusHost;
        ImVec4 background;
        std::string message;
        if (s.link == OverlayStats::LinkState::Disconnected) {
            background = ImVec4(0.42f, 0.10f, 0.10f, 0.94f);
            message = "Not connected to " + host + "  --  press Reconnect (F5)";
            if (!s.statusDetail.empty()) {
                message += "   [" + s.statusDetail + "]";
            }
        } else if (s.link == OverlayStats::LinkState::Reconnecting) {
            background = ImVec4(0.42f, 0.28f, 0.05f, 0.94f);
            message = "Lost contact with " + host + "  --  reconnecting";
            if (s.secondsSinceData > 0) {
                message += " (last data " + std::to_string(s.secondsSinceData) + "s ago)";
            }
        } else {
            background = ImVec4(0.42f, 0.28f, 0.05f, 0.94f);
            message = "Camera health unreadable  --  go2rtc stream schema may have changed";
        }

        const float height = kBannerLogicalHeight;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + kToolbarLogicalHeight));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.95f, 0.92f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoInputs;
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
                for (const std::string& entry : logScratch_) { joined += entry; joined.push_back('\n'); }
                ImGui::SetClipboardText(joined.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) { gig::LogBuffer::instance().clear(); }
            ImGui::SameLine();
            ImGui::TextDisabled("wheel / drag to scroll, Esc or X to close");
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

    void loadImguiFont()
    {
        // HiDPI-sharp: bake the atlas at device pixels (14pt * scale) and divide back
        // with FontGlobalScale, so the displayed size stays 14pt but glyphs are crisp
        // on Retina. (imgui_impl_sdl3 sets DisplayFramebufferScale; ImGui works in points.)
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        const float pixelSize = 14.0f * scale_;
        if (io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Menlo.ttc", pixelSize) == nullptr) {
            ImFontConfig cfg;
            cfg.SizePixels = 13.0f * scale_;
            io.Fonts->AddFontDefault(&cfg);
        }
        io.FontGlobalScale = (scale_ > 0.0f) ? 1.0f / scale_ : 1.0f;
    }

    void applyImguiStyle()
    {
        // No ScaleAllSizes here (unlike the D3D11 path): on macOS ImGui is in points and
        // imgui_impl_sdl3's DisplayFramebufferScale already handles the pixel scaling.
        ImGui::GetStyle() = ImGuiStyle();
        ImGui::StyleColorsDark();
    }

    // Rebake the imgui font atlas + SF Symbol glyphs when the display scale changes
    // (e.g. the window moved to a different-DPI monitor). scale_ is recomputed each
    // render() from the backing size; bakedScale_ tracks what those were baked at. The
    // Windows analog runs from SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED; here render()
    // already recomputes scale_, so a self-detecting compare is enough.
    void applyDpiScale()
    {
        if (scale_ == bakedScale_) {
            return;
        }
        bakedScale_ = scale_;
        gig::logInfo() << "display scale changed: " << scale_;

        // SF Symbol toolbar glyphs are rasterized at device px; re-rasterize them.
        iconSettings_ = makeSymbolTexture(@"gearshape");
        iconReconnect_ = makeSymbolTexture(@"arrow.clockwise");
        iconLog_ = makeSymbolTexture(@"list.bullet");

        if (imguiReady_) {
            // imgui (1.92+) owns font-texture lifecycle: rebuilding the atlas
            // (loadImguiFont) marks it dirty and ImGui_ImplMetal_RenderDrawData
            // re-uploads it on the next frame -- no manual texture create/destroy.
            loadImguiFont();
            applyImguiStyle();
        }
    }

    SDL_Window* window_ = nullptr;
    SDL_MetalView metalView_ = nullptr;
    CAMetalLayer* layer_ = nil;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    CVMetalTextureCacheRef cvTextureCache_ = nullptr;
    id<MTLRenderPipelineState> pipelineBgra_ = nil;
    id<MTLRenderPipelineState> pipelineNv12_ = nil;
    id<MTLRenderPipelineState> pipelineYuv420_ = nil;
    id<MTLRenderPipelineState> pipelineSignal_ = nil;
    id<MTLRenderPipelineState> pipelineHover_ = nil;
    id<MTLTexture> iconSettings_ = nil;
    id<MTLTexture> iconReconnect_ = nil;
    id<MTLTexture> iconLog_ = nil;

    std::vector<TileState> tiles_;

    // Cached grid layout (recomputed only when count/width/height change).
    gig::GridLayout gridLayoutCache_;
    int gridCacheCount_ = -1;
    int gridCacheWidth_ = -1;
    int gridCacheHeight_ = -1;

    std::vector<std::uint64_t> tileBytes_;
    std::vector<std::string> cameraLabels_;
    OverlayStats overlayStats_;
    LabelMode labelMode_ = LabelMode::ErrorOnly;

    int focusedTile_ = -1;
    int animTile_ = -1;
    float animProgress_ = 0.0f;
    int hoveredTile_ = -1;

    float scale_ = 1.0f;
    float bakedScale_ = 0.0f; // display scale the imgui font + SF Symbol glyphs were baked at
    float animTime_ = 0.0f;
    float lastDt_ = 0.0f;
    std::chrono::steady_clock::time_point lastRenderTp_;
    bool haveRenderTp_ = false;
    bool animating_ = true;
    bool sawAnimatedContent_ = false;

    bool imguiReady_ = false;
    bool logViewVisible_ = false;
    std::vector<std::string> logScratch_;
    ToolbarAction pendingToolbarAction_ = ToolbarAction::None;
    float toolbarIdle_ = 0.0f;
};

} // namespace

std::unique_ptr<VideoRenderer> createRenderer()
{
    return std::make_unique<MetalRenderer>();
}
