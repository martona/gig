#include "render/metal_scene.h"

#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

// The video-scene engine extracted verbatim from metal_renderer.mm so macOS
// (SDL + imgui shell) and iOS (UIKit/SwiftUI shell) share every pixel of the
// video area. See metal_scene.h for the split.

namespace gig {
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

constexpr float kZoomDuration = 0.30f;
constexpr float kFadeDur = 0.22f;

// Burn-in pixel orbit (the OLED-TV "pixel shift"): the whole scene drifts on a
// small circle in integer steps -- 1 step every kOrbitStepSeconds, one position
// per ~pixel of circumference, so a revolution takes about an hour and each
// step is an invisible 1px hop. The grid area is inset by the radius on all
// sides so the orbit never crops content.
constexpr float kOrbitRadiusPts = 16.0f;
constexpr int kOrbitPositions = 100;          // ~2*pi*16 -> ~1px per step
// The seconds-between-steps is configurable (Params.orbitStepSeconds); at the
// default 40s a full revolution is ~67 min.

// Frame.layout points here when there is nothing grid-shaped to report (early
// out, or fully-focused view), so hosts can dereference it unconditionally.
const GridLayout kEmptyLayout;

} // namespace

MetalScene::~MetalScene()
{
    for (TileState& tile : tiles_) {
        releaseCvTextures(tile);
    }
    if (cvTextureCache_) {
        CFRelease(cvTextureCache_);
        cvTextureCache_ = nullptr;
    }
}

bool MetalScene::initialize(id<MTLDevice> device, MTLPixelFormat pixelFormat)
{
    device_ = device;
    pixelFormat_ = pixelFormat;
    if (!device_) {
        logError() << "metal scene: no device";
        return false;
    }

    NSError* error = nil;
    id<MTLLibrary> library =
        [device_ newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:nil error:&error];
    if (!library) {
        logError() << "metal shader compile failed: "
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

    // VideoToolbox zero-copy: a texture cache that wraps decoded CVPixelBuffers'
    // IOSurfaces as MTLTextures with no copy. Non-fatal on failure (hw frames just
    // won't display; software decode is unaffected).
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device_, nullptr, &cvTextureCache_)
        != kCVReturnSuccess) {
        cvTextureCache_ = nullptr;
        logWarning() << "CVMetalTextureCacheCreate failed; VideoToolbox frames will not display.";
    }
    return true;
}

void MetalScene::setFocusedTile(int index)
{
    if (index == focusedTile_) {
        return;
    }
    animTile_ = (index >= 0) ? index : focusedTile_;
    focusedTile_ = index;
}

void MetalScene::setFocusedTileImmediate(int index)
{
    // No transition: used when the tile INDICES were remapped (activity-view
    // subset change), where animating via animTile_ would zoom the tile that
    // now holds a different camera.
    focusedTile_ = index;
    animTile_ = -1;
    animProgress_ = (index >= 0) ? 1.0f : 0.0f;
}

bool MetalScene::tileShowingSignal(std::size_t index) const
{
    return index < tiles_.size() && tiles_[index].showedSignal;
}

int MetalScene::tileAt(float x, float y) const
{
    const int cell = cellAt(x, y);
    return (cell >= 0 && cell < static_cast<int>(lastCameraCount_)) ? cell : -1;
}

int MetalScene::cellAt(float x, float y) const
{
    for (std::size_t i = 0; i < gridLayoutCache_.tiles.size(); ++i) {
        const TileRect& cell = gridLayoutCache_.tiles[i];
        if (x >= cell.x && x < cell.x + cell.width && y >= cell.y && y < cell.y + cell.height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool MetalScene::wantsOrbitRepaint() const
{
    if (!haveOrbitEpoch_) {
        return false;
    }
    int dx = 0;
    int dy = 0;
    currentOrbitOffset(dx, dy);
    return dx != gridCacheOrbitX_ || dy != gridCacheOrbitY_;
}

void MetalScene::currentOrbitOffset(int& dx, int& dy) const
{
    if (!haveOrbitEpoch_) {
        dx = 0;
        dy = 0;
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - orbitEpoch_).count();
    const double stepSecs = orbitStepSeconds_ >= 1.0 ? orbitStepSeconds_ : 1.0;
    const long step = static_cast<long>(elapsed / stepSecs);
    const double angle = (static_cast<double>(step % kOrbitPositions) / kOrbitPositions) * 2.0 * M_PI;
    dx = static_cast<int>(std::lround(std::cos(angle) * kOrbitRadiusPts));
    dy = static_cast<int>(std::lround(std::sin(angle) * kOrbitRadiusPts));
}

MetalScene::Frame MetalScene::render(id<MTLRenderCommandEncoder> encoder,
                                     const std::vector<std::shared_ptr<VideoFrame>>& frames,
                                     const Params& params)
{
    Frame out;
    out.layout = &kEmptyLayout;
    const int pointWidth = static_cast<int>(params.pointWidth);
    const int pointHeight = static_cast<int>(params.pointHeight);
    scale_ = params.scale;
    orbitStepSeconds_ = params.orbitStepSeconds;
    lastCameraCount_ = frames.size();
    if (!encoder || pointWidth <= 0 || pointHeight <= 0) {
        return out;
    }

    if (tiles_.size() != frames.size()) {
        // Release any VideoToolbox wrappers before tiles are dropped/realloc'd
        // (a raw CVMetalTextureRef would otherwise leak); survivors re-upload.
        for (TileState& tile : tiles_) {
            releaseCvTextures(tile);
        }
        tiles_.resize(frames.size());
    }

    const auto nowTp = std::chrono::steady_clock::now();
    if (!haveOrbitEpoch_) {
        orbitEpoch_ = nowTp;
        haveOrbitEpoch_ = true;
    }
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

    // Burn-in orbit: the whole scene drifts on a small circle in integer steps;
    // every content rect below is inset by the radius so nothing ever crops.
    int orbitX = 0;
    int orbitY = 0;
    currentOrbitOffset(orbitX, orbitY);

    // Layout in points (matches the hosts' hit-tests + chrome); video viewports
    // scale to pixels. Reserve the chrome strip in grid view only.
    const TileRect fullPts {
        kOrbitRadiusPts + static_cast<float>(orbitX),
        kOrbitRadiusPts + static_cast<float>(orbitY),
        static_cast<float>(pointWidth) - 2.0f * kOrbitRadiusPts,
        static_cast<float>(pointHeight) - 2.0f * kOrbitRadiusPts };
    const bool fullyFocused = focusedTile_ >= 0 && animProgress_ >= 1.0f
        && focusedTile_ < static_cast<int>(frames.size());

    const GridLayout* layout = &kEmptyLayout;
    if (fullyFocused) {
        renderSingleTile(encoder, static_cast<std::size_t>(focusedTile_), frames, fullPts);
        if (hoveredTile_ == focusedTile_) {
            drawHover(encoder, fullPts);
        }
        out.contentRect = fullPts;
    } else {
        const float chromeTop = params.reservedTopPoints;
        const float gridX = kOrbitRadiusPts + static_cast<float>(orbitX);
        const float gridY = chromeTop + kOrbitRadiusPts + static_cast<float>(orbitY);
        const int gridWidth = std::max(1, pointWidth - 2 * static_cast<int>(kOrbitRadiusPts));
        const int gridHeight = std::max(1, pointHeight - static_cast<int>(chromeTop)
            - 2 * static_cast<int>(kOrbitRadiusPts));
        const int effective = static_cast<int>(frames.size()) + (params.extraCell ? 1 : 0);
        // Cache the layout: it depends only on (count, size, orbit step), so
        // recompute only when one changes -- not every render.
        if (effective != gridCacheCount_ || gridWidth != gridCacheWidth_
            || gridHeight != gridCacheHeight_
            || orbitX != gridCacheOrbitX_ || orbitY != gridCacheOrbitY_) {
            gridLayoutCache_ = computeGridLayout(effective, gridWidth, gridHeight);
            for (TileRect& tile : gridLayoutCache_.tiles) {
                tile.x += gridX;
                tile.y += gridY;
            }
            gridCacheCount_ = effective;
            gridCacheWidth_ = gridWidth;
            gridCacheHeight_ = gridHeight;
            gridCacheOrbitX_ = orbitX;
            gridCacheOrbitY_ = orbitY;
        }
        layout = &gridLayoutCache_;
        renderGridTiles(encoder, frames, *layout);
        out.contentRect = TileRect { gridX, gridY,
                                     static_cast<float>(gridWidth), static_cast<float>(gridHeight) };

        if (animProgress_ > 0.0f && animTile_ >= 0 && animTile_ < static_cast<int>(frames.size())
            && animTile_ < static_cast<int>(layout->tiles.size())) {
            const TileRect& cell = layout->tiles[static_cast<std::size_t>(animTile_)];
            const float e = smoothstep01(animProgress_);
            const TileRect grown {
                std::lerp(cell.x, fullPts.x, e), std::lerp(cell.y, fullPts.y, e),
                std::lerp(cell.width, fullPts.width, e),
                std::lerp(cell.height, fullPts.height, e),
            };
            drawTileContentAt(encoder, static_cast<std::size_t>(animTile_), grown);
        }
        if (animProgress_ == 0.0f && hoveredTile_ >= 0 && hoveredTile_ < static_cast<int>(frames.size())
            && hoveredTile_ < static_cast<int>(layout->tiles.size())) {
            drawHover(encoder, layout->tiles[static_cast<std::size_t>(hoveredTile_)]);
        }
    }

    drawDimOverlay(encoder, params);

    // Record the orbit position we just drew so wantsOrbitRepaint() can clear --
    // the grid branch's layout-recompute updates these, but the focus branch
    // doesn't, so set them here for both (else focus view repaints every frame).
    gridCacheOrbitX_ = orbitX;
    gridCacheOrbitY_ = orbitY;

    const bool zoomAnimating = animProgress_ != zoomTarget;
    out.layout = layout;
    out.fullyFocused = fullyFocused;
    out.animating = sawAnimatedContent_ || zoomAnimating;
    out.zoomProgress = animProgress_;
    return out;
}

// Idle-dim: a uniform translucent black wash over the whole frame -- an exact
// luminance multiply for everything under it. Reuses the hover pipeline: a
// border wider than the quad degenerates fs_hover into a solid fill.
void MetalScene::drawDimOverlay(id<MTLRenderCommandEncoder> encoder, const Params& params)
{
    const float dim = std::clamp(params.dimFactor, 0.0f, 1.0f);
    if (dim >= 0.999f) {
        return;
    }
    const TileRect full { 0.0f, 0.0f, params.pointWidth, params.pointHeight };
    const MTLViewport vp = cellViewport(full);
    [encoder setViewport:vp];
    HoverConstants c {};
    c.sizePx[0] = static_cast<float>(vp.width);
    c.sizePx[1] = static_cast<float>(vp.height);
    c.borderPx = static_cast<float>(vp.width + vp.height); // > any edge distance -> solid
    c.color[0] = 0.0f; c.color[1] = 0.0f; c.color[2] = 0.0f;
    c.color[3] = 1.0f - dim;
    [encoder setRenderPipelineState:pipelineHover_];
    [encoder setFragmentBytes:&c length:sizeof(c) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

id<MTLRenderPipelineState> MetalScene::makePipeline(id<MTLFunction> vertexFn, id<MTLFunction> fragmentFn, bool blend)
{
    if (!vertexFn || !fragmentFn) {
        logError() << "metal: missing shader function";
        return nil;
    }
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertexFn;
    descriptor.fragmentFunction = fragmentFn;
    descriptor.colorAttachments[0].pixelFormat = pixelFormat_;
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
        logError() << "metal pipeline creation failed: "
                   << (error ? error.localizedDescription.UTF8String : "(unknown)");
    }
    return state;
}

id<MTLTexture> MetalScene::makeTexture(MTLPixelFormat format, int width, int height)
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

void MetalScene::ensurePlane(TileState& tile, int i, MTLPixelFormat format, int width, int height)
{
    if (tile.plane[i] && tile.planeW[i] == width && tile.planeH[i] == height && tile.planeFmt[i] == format) {
        return;
    }
    tile.plane[i] = makeTexture(format, width, height);
    tile.planeW[i] = width;
    tile.planeH[i] = height;
    tile.planeFmt[i] = format;
}

namespace {

void uploadPlane(id<MTLTexture> texture, int width, int height, const std::uint8_t* source, int stride)
{
    if (!texture || !source || width <= 0 || height <= 0) {
        return;
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width), static_cast<NSUInteger>(height))
               mipmapLevel:0
                 withBytes:source
               bytesPerRow:static_cast<NSUInteger>(stride)];
}

} // namespace

// Release the CVMetalTextures (and the pixel-buffer owner) a VideoToolbox tile was
// holding. Safe on a non-VT tile (the refs are null).
void MetalScene::releaseCvTextures(TileState& tile)
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
void MetalScene::uploadVideoToolboxFrame(TileState& tile, const VideoFrame& frame)
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
        logWarning() << "videotoolbox: CVMetalTextureCache create failed";
        return;
    }

    tile.cvY = yRef;
    tile.cvCbCr = cbcrRef;
    tile.gpuFrameOwner = frame.owner; // keep the CVPixelBuffer (IOSurface) alive
    tile.plane[0] = CVMetalTextureGetTexture(yRef);
    tile.plane[1] = CVMetalTextureGetTexture(cbcrRef);
}

void MetalScene::uploadFrame(TileState& tile, const VideoFrame& frame)
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
MTLViewport MetalScene::videoViewport(const TileRect& cellPts, int texW, int texH) const
{
    TileRect c { cellPts.x * scale_, cellPts.y * scale_, cellPts.width * scale_, cellPts.height * scale_ };
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

MTLViewport MetalScene::cellViewport(const TileRect& cellPts) const
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

void MetalScene::drawTile(id<MTLRenderCommandEncoder> encoder, const TileState& tile)
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

void MetalScene::drawSignal(id<MTLRenderCommandEncoder> encoder, const TileRect& cellPts, std::size_t index,
                            float energy, float alpha)
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

void MetalScene::drawHover(id<MTLRenderCommandEncoder> encoder, const TileRect& cellPts)
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

void MetalScene::drawTileContentAt(id<MTLRenderCommandEncoder> encoder, std::size_t index, const TileRect& rectPts)
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

void MetalScene::renderGridTiles(id<MTLRenderCommandEncoder> encoder,
                                 const std::vector<std::shared_ptr<VideoFrame>>& frames, const GridLayout& layout)
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
        const TileRect& cell = layout.tiles[i];

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

void MetalScene::renderSingleTile(id<MTLRenderCommandEncoder> encoder, std::size_t index,
                                  const std::vector<std::shared_ptr<VideoFrame>>& frames, const TileRect& full)
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

void MetalScene::updateActivity(float dt)
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

} // namespace gig
