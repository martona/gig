//
//  GigRenderer.mm
//  gig
//
//  iOS video host implementation: CADisplayLink -> AppSession frame snapshot ->
//  shared gig::MetalScene encode -> present. Runs entirely on the main thread
//  (CADisplayLink on the main run loop); the engine snapshot accessors are
//  non-blocking (try_lock), so a connect() in flight on a background thread
//  just yields empty frames for a few ticks instead of stalling the UI.
//

#import "GigRenderer.h"
#import "GigBridgeInternal.h"

#include "render/metal_scene.h"

#include "log.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

@implementation GIGTileLabel {
  @public
    NSInteger _index;
    NSString *_text;
    CGRect _rect;
}
- (NSInteger)index { return _index; }
- (NSString *)text { return _text ?: @""; }
- (CGRect)rect { return _rect; }
@end

// CADisplayLink retains its target; a weak proxy breaks the cycle so the host
// (and its Metal scene) can deallocate once detached.
@interface GIGDisplayLinkProxy : NSObject
@property (nonatomic, weak) GIGVideoHost *host;
@end

@interface GIGVideoHost ()
- (void)renderTick; // display-link entry (via the weak proxy)
@end

@implementation GIGVideoHost {
    CAMetalLayer *_layer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    std::unique_ptr<gig::MetalScene> _scene;
    CADisplayLink *_displayLink;
    CGSize _pointSize;
    CGFloat _scale;

    NSArray<GIGTileLabel *> *_labels;
    std::string _overlayFingerprint;
    BOOL _zoomedFlag;
}

+ (instancetype)shared
{
    static GIGVideoHost *instance;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ instance = [[GIGVideoHost alloc] init]; });
    return instance;
}

- (instancetype)init
{
    if ((self = [super init])) {
        _labels = @[];
        _scale = 1.0;
    }
    return self;
}

- (void)attachLayer:(CAMetalLayer *)layer
{
    if (_layer == layer) {
        return;
    }
    [self detach];

    _device = MTLCreateSystemDefaultDevice();
    if (!_device) {
        gig::logError() << "ios: no Metal device";
        return;
    }
    auto scene = std::make_unique<gig::MetalScene>();
    if (!scene->initialize(_device, MTLPixelFormatBGRA8Unorm)) {
        gig::logError() << "ios: Metal scene init failed";
        _device = nil;
        return;
    }
    _scene = std::move(scene);
    _queue = [_device newCommandQueue];
    layer.device = _device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    _layer = layer;
    gig::logInfo() << "ios metal renderer ready: " << _device.name.UTF8String;
}

- (void)detach
{
    [self stop];
    _layer = nil;
    _scene.reset();
    _queue = nil;
    _device = nil;
}

- (void)setViewPointSize:(CGSize)size scale:(CGFloat)scale
{
    _pointSize = size;
    _scale = (scale > 0.0) ? scale : 1.0;
}

- (void)start
{
    if (_displayLink) {
        return;
    }
    GIGDisplayLinkProxy *proxy = [GIGDisplayLinkProxy new];
    proxy.host = self;
    _displayLink = [CADisplayLink displayLinkWithTarget:proxy selector:@selector(tick:)];
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stop
{
    [_displayLink invalidate];
    _displayLink = nil;
}

- (void)handleTapAtPoint:(CGPoint)point
{
    if (!_scene) {
        return;
    }
    if (_scene->focusedTile() >= 0) {
        _scene->setFocusedTile(-1); // any tap returns to the grid
        return;
    }
    const int index = _scene->tileAt(static_cast<float>(point.x), static_cast<float>(point.y));
    if (index >= 0) {
        _scene->setFocusedTile(index);
    }
}

- (BOOL)zoomed
{
    return _zoomedFlag;
}

- (NSArray<GIGTileLabel *> *)visibleLabels
{
    return _labels ?: @[];
}

- (void)renderTick
{
    if (!_layer || !_scene || !_queue) {
        return;
    }
    const CGFloat pixelW = _pointSize.width * _scale;
    const CGFloat pixelH = _pointSize.height * _scale;
    if (pixelW <= 0.0 || pixelH <= 0.0) {
        return;
    }

    @autoreleasepool {
        GIGEngine *engine = [GIGEngine shared];
        const std::vector<std::shared_ptr<VideoFrame>> frames = [engine snapshotFramesInternal];
        _scene->setTileActivity([engine tileByteCountsInternal]);

        _layer.drawableSize = CGSizeMake(pixelW, pixelH);
        id<CAMetalDrawable> drawable = [_layer nextDrawable];
        if (!drawable) {
            return;
        }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.01, 0.01, 0.012, 1.0);
        id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];

        gig::MetalScene::Params params;
        params.pointWidth = static_cast<float>(_pointSize.width);
        params.pointHeight = static_cast<float>(_pointSize.height);
        params.scale = static_cast<float>(_scale);
        params.reservedTopPoints = 0.0f; // the SwiftUI toolbar lives OUTSIDE the Metal view
        params.extraCell = false;        // no diagnostics tile on iOS (dropped by decision)
        const gig::MetalScene::Frame scene = _scene->render(encoder, frames, params);

        [encoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];

        [self updateOverlay:scene cameraCount:frames.size() engine:engine];
    }
}

// Rebuild the SwiftUI label-overlay state and notify only when it changed. The
// fingerprint keeps the 60 Hz tick cheap: labels flip rarely (signal <-> video,
// layout/count changes, zoom start/end).
- (void)updateOverlay:(const gig::MetalScene::Frame &)scene
          cameraCount:(std::size_t)cameraCount
               engine:(GIGEngine *)engine
{
    const bool zoomTransition = scene.zoomProgress > 0.0f && scene.zoomProgress < 1.0f;
    const int focused = _scene->focusedTile();

    std::string fingerprint;
    fingerprint.reserve(64);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "f%d z%d n%zu|", focused, zoomTransition ? 1 : 0, cameraCount);
    fingerprint += buf;

    NSMutableArray<GIGTileLabel *> *labels = [NSMutableArray array];
    if (!zoomTransition) {
        const std::vector<std::string> names = [engine cameraLabelsInternal];
        auto appendLabel = [&](std::size_t index, CGRect rect) {
            if (index >= names.size() || names[index].empty()) {
                return;
            }
            GIGTileLabel *label = [GIGTileLabel new];
            label->_index = static_cast<NSInteger>(index);
            label->_text = [NSString stringWithUTF8String:names[index].c_str()];
            label->_rect = rect;
            [labels addObject:label];
            std::snprintf(buf, sizeof(buf), "%zu:%d,%d,%d,%d|", index, (int)rect.origin.x, (int)rect.origin.y,
                          (int)rect.size.width, (int)rect.size.height);
            fingerprint += buf;
        };

        if (scene.fullyFocused) {
            // ErrorOnly rule in focus view: label the (frameless) focused camera.
            if (focused >= 0 && _scene->tileShowingSignal(static_cast<std::size_t>(focused))) {
                appendLabel(static_cast<std::size_t>(focused),
                            CGRectMake(0, 0, _pointSize.width, _pointSize.height));
            }
        } else if (scene.layout) {
            for (std::size_t i = 0; i < cameraCount && i < scene.layout->tiles.size(); ++i) {
                if (!_scene->tileShowingSignal(i)) {
                    continue; // ErrorOnly: label only while the tile shows the signal scope
                }
                const gig::TileRect &cell = scene.layout->tiles[i];
                appendLabel(i, CGRectMake(cell.x, cell.y, cell.width, cell.height));
            }
        }
    }

    const BOOL zoomed = focused >= 0;
    if (fingerprint == _overlayFingerprint && zoomed == _zoomedFlag) {
        return;
    }
    _overlayFingerprint = fingerprint;
    _zoomedFlag = zoomed;
    _labels = labels;
    if (self.onOverlayChanged) {
        self.onOverlayChanged();
    }
}

@end

@implementation GIGDisplayLinkProxy
- (void)tick:(CADisplayLink *)link
{
    (void)link;
    [self.host renderTick];
}
@end
