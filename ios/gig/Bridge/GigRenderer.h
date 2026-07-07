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
- (void)handleTapAtPoint:(CGPoint)point NS_SWIFT_NAME(handleTap(at:));

@property (nonatomic, assign, readonly) BOOL zoomed;

// Labels currently visible (empty while the zoom animation runs).
- (NSArray<GIGTileLabel *> *)visibleLabels;

// Fired on the main thread whenever visibleLabels/zoomed changed.
@property (nonatomic, copy, nullable) void (^onOverlayChanged)(void);

@end

NS_ASSUME_NONNULL_END
