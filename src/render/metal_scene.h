#pragma once

// Shell-independent Metal "scene" for the camera grid, shared by the macOS
// renderer (SDL window + imgui chrome, metal_renderer.mm) and the iOS host
// (UIKit CAMetalLayer + SwiftUI chrome, ios/gig/Bridge/GigRenderer.mm). It owns
// everything that draws INTO the video area: per-tile texture upload (software
// BGRA/NV12/YUV420P + VideoToolbox zero-copy), the YUV->RGB draw, letterboxing,
// the procedural "signal" scope on frameless tiles, the resolve crossfade, the
// hover border, the click/tap-to-zoom animation, and the grid layout cache.
//
// The HOST owns the CAMetalLayer, command queue, drawable, render pass and
// encoder (macOS appends imgui into the same pass after the scene encodes), plus
// all chrome: toolbar, status banner, log view, labels and the diagnostics tile.
//
// ObjC++ only: include this from .mm files (it imports Metal/CoreVideo and holds
// ARC-managed ObjC objects in C++ members).

#include "render/grid_layout.h"
#include "video_frame.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

namespace gig {

class MetalScene {
public:
    struct Params {
        float pointWidth = 0.0f;       // logical points
        float pointHeight = 0.0f;
        float scale = 1.0f;            // pixels per point
        float reservedTopPoints = 0.0f; // chrome strip above the grid (grid view only)
        bool extraCell = false;        // reserve one trailing grid cell (mac diagnostics tile)
        float dimFactor = 1.0f;        // idle-dim luminance multiplier (1 = normal)
        float orbitStepSeconds = 40.0f; // burn-in pixel-orbit step interval (>= 1)
    };

    // Snapshot of what the scene drew, for the host's chrome pass. `layout` is in
    // points (tile y already offset by reservedTopPoints) and stays valid until the
    // next render(). While zooming (0 < zoomProgress < 1) hosts hide overlays that
    // can't track the in-scene animation (the SwiftUI labels on iOS).
    struct Frame {
        const GridLayout* layout = nullptr;
        bool fullyFocused = false;
        bool animating = false;        // signal/fade/zoom in flight (not host chrome)
        float zoomProgress = 0.0f;     // 0 = grid, 1 = focused
        // The rect (points) the scene actually drew content into this frame --
        // the grid area, or the focused image area. Includes the burn-in orbit
        // offset, so hosts place focused-view overlays against THIS, not the
        // window bounds.
        TileRect contentRect {};
    };

    MetalScene() = default;
    ~MetalScene();

    MetalScene(const MetalScene&) = delete;
    MetalScene& operator=(const MetalScene&) = delete;

    // `device` is created by the host (it also goes on the CAMetalLayer);
    // `pixelFormat` must match the layer's. Compiles the pipelines + CV cache.
    bool initialize(id<MTLDevice> device, MTLPixelFormat pixelFormat);

    void setFocusedTile(int index);
    int focusedTile() const { return focusedTile_; }
    void setHoveredTile(int index) { hoveredTile_ = index; }
    void setTileActivity(const std::vector<std::uint64_t>& byteCounts) { tileBytes_ = byteCounts; }

    // True while the tile draws the signal scope instead of video (the ErrorOnly
    // label rule: show the camera label only during the signal phase).
    bool tileShowingSignal(std::size_t index) const;

    // Hit-test a point-space position against the CURRENT grid layout (camera
    // cells only, diagnostics cell excluded). -1 = none. Grid view only; a
    // focused view is the host's "tap anywhere returns" case.
    int tileAt(float x, float y) const;

    // Like tileAt but over ALL grid cells (index == cameraCount is the extra
    // diagnostics cell when the host reserved one).
    int cellAt(float x, float y) const;

    // True when the burn-in orbit has stepped since the last render() -- the host
    // ORs this into its dirty check so a static image still orbits.
    bool wantsOrbitRepaint() const;

    // Encode the video scene into `encoder` (host-created, host-presented).
    Frame render(id<MTLRenderCommandEncoder> encoder,
                 const std::vector<std::shared_ptr<VideoFrame>>& frames,
                 const Params& params);

private:
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

    id<MTLRenderPipelineState> makePipeline(id<MTLFunction> vertexFn, id<MTLFunction> fragmentFn, bool blend);
    id<MTLTexture> makeTexture(MTLPixelFormat format, int width, int height);
    void ensurePlane(TileState& tile, int i, MTLPixelFormat format, int width, int height);
    static void releaseCvTextures(TileState& tile);
    void uploadVideoToolboxFrame(TileState& tile, const VideoFrame& frame);
    void uploadFrame(TileState& tile, const VideoFrame& frame);
    MTLViewport videoViewport(const TileRect& cellPts, int texW, int texH) const;
    MTLViewport cellViewport(const TileRect& cellPts) const;
    void drawTile(id<MTLRenderCommandEncoder> encoder, const TileState& tile);
    void drawSignal(id<MTLRenderCommandEncoder> encoder, const TileRect& cellPts, std::size_t index,
                    float energy, float alpha);
    void drawHover(id<MTLRenderCommandEncoder> encoder, const TileRect& cellPts);
    void drawTileContentAt(id<MTLRenderCommandEncoder> encoder, std::size_t index, const TileRect& rectPts);
    void renderGridTiles(id<MTLRenderCommandEncoder> encoder,
                         const std::vector<std::shared_ptr<VideoFrame>>& frames, const GridLayout& layout);
    void renderSingleTile(id<MTLRenderCommandEncoder> encoder, std::size_t index,
                          const std::vector<std::shared_ptr<VideoFrame>>& frames, const TileRect& full);
    void updateActivity(float dt);

    id<MTLDevice> device_ = nil;
    MTLPixelFormat pixelFormat_ = MTLPixelFormatBGRA8Unorm;
    CVMetalTextureCacheRef cvTextureCache_ = nullptr;
    id<MTLRenderPipelineState> pipelineBgra_ = nil;
    id<MTLRenderPipelineState> pipelineNv12_ = nil;
    id<MTLRenderPipelineState> pipelineYuv420_ = nil;
    id<MTLRenderPipelineState> pipelineSignal_ = nil;
    id<MTLRenderPipelineState> pipelineHover_ = nil;

    std::vector<TileState> tiles_;

    // Burn-in pixel orbit: current integer offset (points) on the slow circular
    // path, derived from wall time since the scene came up.
    void currentOrbitOffset(int& dx, int& dy) const;
    void drawDimOverlay(id<MTLRenderCommandEncoder> encoder, const Params& params);

    // Cached grid layout (recomputed when count/size/orbit-step change).
    GridLayout gridLayoutCache_;
    int gridCacheCount_ = -1;
    int gridCacheWidth_ = -1;
    int gridCacheHeight_ = -1;
    int gridCacheOrbitX_ = 0;
    int gridCacheOrbitY_ = 0;
    std::size_t lastCameraCount_ = 0;
    std::chrono::steady_clock::time_point orbitEpoch_;
    bool haveOrbitEpoch_ = false;
    double orbitStepSeconds_ = 40.0; // set each render() from Params

    std::vector<std::uint64_t> tileBytes_;

    int focusedTile_ = -1;
    int animTile_ = -1;
    float animProgress_ = 0.0f;
    int hoveredTile_ = -1;

    float scale_ = 1.0f;
    float animTime_ = 0.0f;
    float lastDt_ = 0.0f;
    std::chrono::steady_clock::time_point lastRenderTp_;
    bool haveRenderTp_ = false;
    bool sawAnimatedContent_ = false;
};

} // namespace gig
