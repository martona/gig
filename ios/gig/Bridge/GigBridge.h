//
//  GigBridge.h
//  gig
//
//  ObjC++ bridge between the SwiftUI app and the C++ core. Exposes:
//   - Settings: read/write the Frigate config via gig::SettingsStore.
//   - Engine:   drive gig::AppSession (login -> discover -> camera supervisor),
//               i.e. the entire net/discovery/health/decode stack — the same
//               core that runs on Windows + macOS, here linked against the
//               vcpkg-built iOS static libs.
//
//  The Metal video renderer is the remaining piece (the stub MetalVideoView
//  clears the screen for now); decoded frames already flow through AppSession.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#pragma mark - Settings

NS_SWIFT_NAME(Settings)
@interface GIGSettings : NSObject
@property (nonatomic, copy) NSString *baseURL;
@property (nonatomic, copy) NSString *user;
@property (nonatomic, copy) NSString *password;
// TLS: leave caPath empty + insecure=NO to use the OS trust store — but note iOS
// can't enumerate system roots, so for a self-signed Frigate set a PEM `ca`, or
// turn on `insecure` for testing. (Cert pinning also applies once wired to the UI.)
@property (nonatomic, copy) NSString *caPath;
@property (nonatomic, assign) BOOL insecure;
@end

NS_SWIFT_NAME(SettingsBridge)
@interface GIGSettingsBridge : NSObject
// Deliberately avoids `load` on BOTH sides of the bridge: the bare ObjC selector
// `load` is the runtime's pre-main hook (it would run the settings store + a
// Keychain read at image load), and the Swift name `load()` is ambiguous with
// NSObject's inherited class func load(). Hence loadSettings / current().
+ (GIGSettings *)loadSettings NS_SWIFT_NAME(current());
+ (void)save:(GIGSettings *)settings NS_SWIFT_NAME(save(_:));
@end

#pragma mark - Engine

NS_SWIFT_NAME(EngineStatus)
@interface GIGEngineStatus : NSObject
@property (nonatomic, assign, readonly) BOOL connected;
@property (nonatomic, assign, readonly) NSInteger cameraCount;
@property (nonatomic, assign, readonly) NSInteger liveCameraCount;
@property (nonatomic, assign, readonly) unsigned long long decodedFrames;
@property (nonatomic, assign, readonly) NSInteger ingestKbps;
@property (nonatomic, copy, readonly) NSString *message;   // "ok" or the failure reason
@end

NS_SWIFT_NAME(Engine)
@interface GIGEngine : NSObject

+ (instancetype)shared;

// Load settings from the store and bring the session up (login -> discover ->
// supervisor). Synchronous and potentially slow (network) — call off the main
// thread. Never throws; failures come back in the returned status' `message`.
- (GIGEngineStatus *)connect NS_SWIFT_NAME(connect());

// Tear the session down (joins the worker threads).
- (void)disconnect NS_SWIFT_NAME(disconnect());

// Non-blocking status poll: if the engine is busy (a connect in flight on
// another thread), returns the last known snapshot instead of waiting.
- (GIGEngineStatus *)status NS_SWIFT_NAME(status());

@end

#pragma mark - Log

// Snapshot of the in-app log ring (the same gig::LogBuffer the desktop log
// view reads) for the SwiftUI log sheet.
NS_SWIFT_NAME(LogBridge)
@interface GIGLogBridge : NSObject
+ (NSString *)snapshotText;
+ (void)clear;
@end

NS_ASSUME_NONNULL_END

// The iOS video host rides in the same bridging header (pure ObjC).
#import "GigRenderer.h"
