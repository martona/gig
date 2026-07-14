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

#include "app/activity_gate.h"
#include "render/metal_scene.h"
#include "render/quiet_status.h"

#include "log.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
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

    // Burn-in: idle-dim ramp + chromeless timer. lastInteraction advances on any
    // tap / chrome activity; the dim factor ramps toward the target each tick.
    CFTimeInterval _lastInteraction;
    float _dimFactor;
    NSInteger _dimLevelPercent;
    NSInteger _dimDelaySeconds;
    NSInteger _orbitStepSeconds;
    CGFloat _dimPreview;  // >=0 forces the factor (settings preview); <0 = idle-driven
    BOOL _chromeHiddenFlag;

    // Dirty-render gate (the desktop run loop's on-demand rendering, ported to
    // the display-link tick): encode a pass only when a new decoded frame
    // arrived, the scene reported an animation in flight last frame (signal
    // scope / fade / zoom), or an input/layout change forced it. A static grid
    // of low-fps cameras then costs GPU work only when pixels actually change.
    std::uint64_t _lastFrameStamp;
    BOOL _lastAnimating;
    BOOL _needsRender;

    // Activity view: the gate turns the /ws feed into the visible tile subset;
    // _visibleTiles maps tile index -> camera slot (identity outside activity
    // mode). Activity wakes the dim WITHOUT counting as interaction (it must
    // not un-hide chrome or peek the full grid).
    gig::ActivityGate _gate;
    BOOL _viewModeActivity;
    BOOL _motionActivity;
    std::vector<int> _visibleTiles;
    CFTimeInterval _lastActivityWake;
    NSString *_quietText;
    CGPoint _quietPos;
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
        _lastAnimating = YES; // render until the first scene report says idle
        _needsRender = YES;
        _dimFactor = 1.0f;
        _dimLevelPercent = 60;
        _dimDelaySeconds = 600;
        _orbitStepSeconds = 40;
        _dimPreview = -1.0;
        _lastInteraction = CACurrentMediaTime();
        _lastActivityWake = _lastInteraction;
        _quietText = @"";
        _quietPos = CGPointZero;
    }
    return self;
}

- (void)noteInteraction
{
    _lastInteraction = CACurrentMediaTime();
    _needsRender = YES;
}

- (void)setDimLevelPercent:(NSInteger)levelPercent delaySeconds:(NSInteger)delaySeconds
{
    _dimLevelPercent = std::clamp<NSInteger>(levelPercent, 10, 100);
    _dimDelaySeconds = std::max<NSInteger>(delaySeconds, 0);
    _lastInteraction = CACurrentMediaTime();
}

- (void)setOrbitStepSeconds:(NSInteger)seconds
{
    _orbitStepSeconds = std::clamp<NSInteger>(seconds, 1, 600);
}

- (void)setDimPreview:(CGFloat)factor
{
    _dimPreview = factor;
    _needsRender = YES;
}

- (void)setViewModeActivity:(BOOL)activity motionCounts:(BOOL)motionCounts
{
    _viewModeActivity = activity;
    _motionActivity = motionCounts;
    _needsRender = YES;
}

- (NSString *)quietStatusText
{
    return _quietText ?: @"";
}

- (CGPoint)quietStatusPosition
{
    return _quietPos;
}

- (BOOL)chromeHidden
{
    return _chromeHiddenFlag;
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
    if (!CGSizeEqualToSize(size, _pointSize) || scale != _scale) {
        _needsRender = YES;
    }
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
    // Cap at 60: camera streams are <=30 fps, so a 120 Hz ProMotion cadence buys
    // nothing; the low bound lets the display idle down between dirty frames.
    _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(10, 60, 60);
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    _needsRender = YES; // fresh drawable after a resume
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
    [self noteInteraction]; // resets the idle-dim + chromeless timers
    _needsRender = YES; // focus change animates from the next tick
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

    // Idle-dim + chromeless timers (evaluated BEFORE the dirty gate so a lone
    // dim ramp or a chrome-hide transition still forces a render). The orbit
    // also keeps the scene changing, but on its own 40s cadence -- so we let the
    // scene's own animating flag / frame stamp drive most repaints and just make
    // sure dim/chrome transitions aren't gated out.
    const CFTimeInterval idle = CACurrentMediaTime() - _lastInteraction;
    // Chromeless de-chroming is independent of the dim setting (a "Never dim"
    // choice must still hide the bright static toolbar/status bar). The SwiftUI
    // layer additionally gates this on being connected.
    const bool chromeHide = idle >= 60.0;
    if (chromeHide != _chromeHiddenFlag) {
        _chromeHiddenFlag = chromeHide;
        _needsRender = YES;
        if (self.onOverlayChanged) {
            self.onOverlayChanged();
        }
    }
    @autoreleasepool {
        GIGEngine *engine = [GIGEngine shared];
        const GIGEngineTickSnapshot snap = [engine tickSnapshotInternal];
        if (!snap.valid) {
            return; // engine busy (connect in flight): keep the last frame + state
        }

        // Activity view: derive the visible tile subset from the /ws feed.
        // Interaction "peeks" the full grid (same idle clock as chrome-hide);
        // startup counts as interaction, which also covers /ws having no
        // state replay on connect.
        const gig::ActivityGate::Result activity = _gate.evaluate(
            _viewModeActivity == YES, _motionActivity == YES, snap.feedConnected,
            idle, snap.activity, static_cast<int>(snap.frames.size()));
        if (activity.wakeEdge) {
            _lastActivityWake = CACurrentMediaTime();
        }
        if (activity.visible != _visibleTiles) {
            // Keep focus on the same CAMERA across the reshuffle; drop it if
            // that camera left the subset (a stale out-of-range focus wedges
            // the zoom state).
            const int focused = _scene->focusedTile();
            int remapped = -1;
            if (focused >= 0 && focused < static_cast<int>(_visibleTiles.size())) {
                const int cam = _visibleTiles[static_cast<std::size_t>(focused)];
                const auto pos = std::find(activity.visible.begin(), activity.visible.end(), cam);
                if (pos != activity.visible.end()) {
                    remapped = static_cast<int>(pos - activity.visible.begin());
                }
            }
            // Immediate (no zoom transition): the animation state refers to
            // the OLD index space and would visibly zoom the wrong camera.
            _scene->setFocusedTileImmediate(remapped);
            _visibleTiles = activity.visible;
            _needsRender = YES;
        }

        // The wandering "all quiet" line lives in the SwiftUI overlay --
        // independent of the Metal dirty gate, so refresh it before the
        // early-out below (it must move once a minute on a static screen).
        NSString *quietText = @"";
        CGPoint quietPos = CGPointZero;
        if (activity.filtered && activity.quiet) {
            const std::time_t nowWall = std::time(nullptr);
            std::tm local {};
            localtime_r(&nowWall, &local);
            const std::string line = gig::quietStatusLine(local);
            float fx = 0.0f;
            float fy = 0.0f;
            gig::quietStatusPlacement(static_cast<long long>(nowWall / 60), fx, fy);
            quietText = [NSString stringWithUTF8String:line.c_str()] ?: @"";
            quietPos = CGPointMake(fx, fy);
        }
        if (![quietText isEqualToString:_quietText] || !CGPointEqualToPoint(quietPos, _quietPos)) {
            _quietText = quietText;
            _quietPos = quietPos;
            if (self.onOverlayChanged) {
                self.onOverlayChanged();
            }
        }

        // Activity wakes the display: the dim clock runs off the LEAST idle of
        // interaction vs camera activity (chrome-hide stays interaction-only).
        const CFTimeInterval dimIdle = std::min(idle, CACurrentMediaTime() - _lastActivityWake);
        float dimTarget = 1.0f;
        if (_dimPreview >= 0.0) {
            dimTarget = std::clamp(static_cast<float>(_dimPreview), 0.1f, 1.0f);
        } else if (_dimDelaySeconds > 0 && dimIdle >= static_cast<double>(_dimDelaySeconds)) {
            dimTarget = static_cast<float>(_dimLevelPercent) / 100.0f;
        }
        const bool dimming = _dimFactor != dimTarget;

        // Visible-subset frames/bytes/labels, index-aligned with the tiles.
        std::vector<std::shared_ptr<VideoFrame>> frames;
        std::vector<std::uint64_t> bytes;
        std::vector<std::string> labels;
        frames.reserve(_visibleTiles.size());
        bytes.reserve(_visibleTiles.size());
        labels.reserve(_visibleTiles.size());
        for (const int cam : _visibleTiles) {
            const auto slot = static_cast<std::size_t>(cam);
            frames.push_back(slot < snap.frames.size() ? snap.frames[slot] : nullptr);
            bytes.push_back(slot < snap.bytes.size() ? snap.bytes[slot] : 0);
            labels.push_back(slot < snap.labels.size() ? snap.labels[slot] : std::string());
        }

        // Dirty gate: skip the whole encode when nothing can have changed on
        // screen. A frameless tile's signal scope keeps the scene animating, so
        // idle here means genuinely static video (or disconnected + cleared).
        std::uint64_t stamp = static_cast<std::uint64_t>(frames.size()) * 1000003ull;
        for (const std::shared_ptr<VideoFrame> &frame : frames) {
            stamp = stamp * 31ull + (frame ? frame->index + 1ull : 0ull);
        }
        if (!_needsRender && !_lastAnimating && !dimming
            && !_scene->wantsOrbitRepaint() && stamp == _lastFrameStamp) {
            return;
        }

        if (dimming) {
            const float stepD = 0.015f;
            _dimFactor = (_dimFactor < dimTarget) ? std::min(dimTarget, _dimFactor + stepD)
                                                  : std::max(dimTarget, _dimFactor - stepD);
        }

        _scene->setTileActivity(bytes);

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
        params.dimFactor = _dimFactor;
        params.orbitStepSeconds = static_cast<float>(_orbitStepSeconds);
        const gig::MetalScene::Frame scene = _scene->render(encoder, frames, params);

        [encoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];

        _lastFrameStamp = stamp;
        _lastAnimating = scene.animating ? YES : NO;
        _needsRender = NO;

        [self updateOverlay:scene cameraCount:frames.size() labels:labels];
    }
}

// Rebuild the SwiftUI label-overlay state and notify only when it changed. The
// fingerprint keeps the 60 Hz tick cheap: labels flip rarely (signal <-> video,
// layout/count changes, zoom start/end).
- (void)updateOverlay:(const gig::MetalScene::Frame &)scene
          cameraCount:(std::size_t)cameraCount
               labels:(const std::vector<std::string> &)names
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
