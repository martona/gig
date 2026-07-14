//
//  GigRenderer.h
//  gig
//
//  The iOS video host: owns a CADisplayLink and drives the shared C++ Metal
//  scene (src/render/metal_scene.mm -- the same engine the macOS renderer uses)
//  into a CAMetalLayer provided by the SwiftUI-hosted UIView. Also computes the
//  SwiftUI label-overlay state (which camera labels are visible and where),
//  since SwiftUI draws the labels natively over the Metal view.
//
//  Pure ObjC header -- safe to import from the Swift bridging header.
//

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

NS_ASSUME_NONNULL_BEGIN

// One visible camera label: `rect` is the tile's cell in view points. The
// ErrorOnly rule applies (a label shows only while its tile draws the signal
// scope instead of video), and nothing is visible mid-zoom -- SwiftUI overlays
// can't track the in-scene zoom animation, so they hide until it settles.
NS_SWIFT_NAME(TileLabel)
@interface GIGTileLabel : NSObject
@property (nonatomic, assign, readonly) NSInteger index;
@property (nonatomic, copy, readonly) NSString *text;
@property (nonatomic, assign, readonly) CGRect rect;
@end

NS_SWIFT_NAME(VideoHost)
@interface GIGVideoHost : NSObject

+ (instancetype)shared;

// Wire the view's CAMetalLayer up (creates the Metal device + scene). Safe to
// call once per app run; detach releases the layer/scene.
- (void)attachLayer:(CAMetalLayer *)layer NS_SWIFT_NAME(attach(_:));
- (void)detach;

// The hosting view's bounds (points) + display scale; call from layoutSubviews.
- (void)setViewPointSize:(CGSize)size scale:(CGFloat)scale NS_SWIFT_NAME(setViewSize(_:scale:));

// Start/stop the display-link render loop (stop on background, start on active).
- (void)start;
- (void)stop;

// A tap in view points: focused -> back to grid; grid -> zoom the tapped tile.
// Also counts as interaction (resets the idle-dim + chromeless timers).
- (void)handleTapAtPoint:(CGPoint)point NS_SWIFT_NAME(handleTap(at:));

// Register interaction that isn't a tap (e.g. chrome shown by the SwiftUI layer)
// so the idle timers reset.
- (void)noteInteraction;

// Idle-dim config (burn-in): after `delaySeconds` of no interaction the video
// ramps to `levelPercent` luminance. delaySeconds == 0 disables dimming.
- (void)setDimLevelPercent:(NSInteger)levelPercent delaySeconds:(NSInteger)delaySeconds
    NS_SWIFT_NAME(setDim(levelPercent:delaySeconds:));

// Seconds between burn-in pixel-orbit steps (>= 1; lower = more motion).
- (void)setOrbitStepSeconds:(NSInteger)seconds NS_SWIFT_NAME(setOrbitStep(seconds:));

// Live idle-dim preview: force the dim factor to `factor` (0..1) NOW, ignoring
// the idle timer, for the settings preview slider. Pass a negative value to
// resume normal idle-driven dimming.
- (void)setDimPreview:(CGFloat)factor NS_SWIFT_NAME(setDimPreview(_:));

// Activity view config (from settings): activity = show only cameras with
// current Frigate activity; motionCounts = raw motion also counts (noisier).
- (void)setViewModeActivity:(BOOL)activity motionCounts:(BOOL)motionCounts
    NS_SWIFT_NAME(setViewMode(activity:motionCounts:));

// Keep off-screen cameras streaming (default YES). NO = the on-demand stream
// policy tears hidden cameras down and reconnects them when they appear.
- (void)setKeepHiddenStreams:(BOOL)keep NS_SWIFT_NAME(setKeepHiddenStreams(_:));

@property (nonatomic, assign, readonly) BOOL zoomed;

// True once `delaySeconds` of no interaction have elapsed: the SwiftUI layer
// hides its chrome (toolbar + system status bar + home indicator) for burn-in.
@property (nonatomic, assign, readonly) BOOL chromeHidden;

// Labels currently visible (empty while the zoom animation runs).
- (NSArray<GIGTileLabel *> *)visibleLabels;

// Activity view, empty grid: the wandering "It's ten past four and everything
// is quiet." line. Empty string = hidden. Position is the text's top-left as
// FRACTIONS of the view (both 0..1); it moves once a minute.
@property (nonatomic, copy, readonly) NSString *quietStatusText;
@property (nonatomic, assign, readonly) CGPoint quietStatusPosition;

// Fired on the main thread whenever visibleLabels/zoomed/chromeHidden/
// quietStatus changed.
@property (nonatomic, copy, nullable) void (^onOverlayChanged)(void);

@end

NS_ASSUME_NONNULL_END
