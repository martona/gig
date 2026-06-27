#include "render/video_renderer.h"

#include "log.hpp"

#include <memory>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace {

// First-light macOS renderer STUB. It stands up a CAMetalLayer on the SDL window
// and clears it each frame -- it deliberately draws NO video yet. It exists so the
// macOS build links and runs end to end (window + event loop + software decoders
// all live, "live N/N" in the log) before the real Metal renderer is written.
//
// The real renderer (the "in earnest" port work) replaces this: upload the
// software VideoFrame planes (BGRA / NV12 / YUV420P) to MTLTextures, port the
// YUV->RGB + signal-scope + hover shaders to MSL, and wire imgui_impl_metal for
// the toolbar/log. It implements just enough of VideoRenderer to run.
class MetalStubRenderer final : public VideoRenderer {
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
            gig::logError() << "MTLCreateSystemDefaultDevice failed (no Metal device)";
            return false;
        }
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;

        queue_ = [device_ newCommandQueue];
        if (!queue_) {
            gig::logError() << "Metal command queue creation failed";
            return false;
        }

        gig::logInfo() << "metal stub renderer ready: " << device_.name.UTF8String
                       << " (clear-only; no video yet)";
        return true;
    }

    void resize() override
    {
        // The CAMetalLayer tracks the view's size; the clear-only stub needs nothing
        // here. The real renderer will set layer_.drawableSize from the backbuffer.
    }

    void render(const std::vector<std::shared_ptr<VideoFrame>>& frames) override
    {
        (void)frames; // not drawn yet -- proves the pipeline, not the picture
        if (!layer_ || !queue_) {
            return;
        }

        @autoreleasepool {
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
            [encoder endEncoding];
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
    }

    // Nothing animates in the clear-only stub, so the run loop draws only on new
    // frames / input (still proves decoders are producing frames).
    bool isAnimating() const override { return false; }

    void setFocusedTile(int index) override { focusedTile_ = index; }
    int focusedTile() const override { return focusedTile_; }

    void setCameraLabels(const std::vector<std::string>&) override {}
    void setDiagnostics(const OverlayStats&) override {}

    ~MetalStubRenderer() override
    {
        if (metalView_) {
            SDL_Metal_DestroyView(metalView_);
            metalView_ = nullptr;
        }
    }

private:
    SDL_Window* window_ = nullptr;
    SDL_MetalView metalView_ = nullptr;
    CAMetalLayer* layer_ = nil;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    int focusedTile_ = -1;
};

} // namespace

std::unique_ptr<VideoRenderer> createRenderer()
{
    return std::make_unique<MetalStubRenderer>();
}
