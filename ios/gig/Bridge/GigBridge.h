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

// One saved Frigate server (the connections/ registry in the settings store).
// The editable fields are what the UI exposes; `storedID` is the leaf the
// entry was loaded from ("" for a brand-new one) -- carry it through edits so
// saveConnections can preserve the no-UI ride-alongs (bare-stream url, PEM
// paths, login-refresh) and reparent a URL edit onto its new identity leaf.
NS_SWIFT_NAME(Connection)
@interface GIGConnection : NSObject
@property (nonatomic, copy) NSString *baseURL;
@property (nonatomic, copy) NSString *user;
@property (nonatomic, copy) NSString *password;
// Skip server-cert verification for THIS server (testing; disables pinning).
@property (nonatomic, assign) BOOL insecure;
@property (nonatomic, copy) NSString *storedID;
// "host:port  (user)" -- the row/chooser label (identical on every platform).
@property (nonatomic, copy, readonly) NSString *listLabel;
// URL-derived identity hash; equal keys = the same server (duplicate checks).
@property (nonatomic, copy, readonly) NSString *identityKey;
@end

// Global (per-device) settings. Connection fields moved into GIGConnection /
// the connections registry below.
NS_SWIFT_NAME(Settings)
@interface GIGSettings : NSObject
// Burn-in idle dim: ramp video to this luminance (10..100 %) after `dimDelaySeconds`
// of no interaction (0 = never dim).
@property (nonatomic, assign) NSInteger dimLevelPercent;
@property (nonatomic, assign) NSInteger dimDelaySeconds;
// Seconds between burn-in pixel-orbit steps (>= 1; default 40).
@property (nonatomic, assign) NSInteger orbitStepSeconds;
// Activity view: show only cameras with current activity (Frigate /ws feed);
// the grid stays empty when nothing is happening. Any tap peeks the full wall.
@property (nonatomic, assign) BOOL activityView;
// Whether raw motion counts as activity (noisy: wind-blown shadows trigger
// it); tracked objects always count.
@property (nonatomic, assign) BOOL motionActivity;
// Ignore STATIONARY objects: a parked car or a package settled on the
// doorstep stops counting as activity ~10s after it stops moving.
@property (nonatomic, assign) BOOL activeOnly;
// Draw detection bounding boxes over the video (default on): pulsing red for
// a live tracked object, blue while it lingers "(gone)".
@property (nonatomic, assign) BOOL showBoxes;
// Tile-label / quiet-status text size: 0 normal, 1 large (1.5x), 2 larger
// (2x) -- a wall watched from across the room needs bigger type.
@property (nonatomic, assign) NSInteger labelSize;
// Keep off-screen cameras streaming (default). Off = tear a hidden camera's
// stream down and reconnect when it appears (saves decode power; costs ~1-2s
// + the scope animation on wake).
@property (nonatomic, assign) BOOL keepHiddenStreams;
// Hide cameras with no incoming video (default off): a down camera disappears
// from the wall instead of showing an error tile; when every camera is down,
// a wandering status line says so ("10/10 cameras are offline").
@property (nonatomic, assign) BOOL hideOfflineCameras;
@end

NS_SWIFT_NAME(SettingsBridge)
@interface GIGSettingsBridge : NSObject
// Deliberately avoids `load` on BOTH sides of the bridge: the bare ObjC selector
// `load` is the runtime's pre-main hook (it would run the settings store + a
// Keychain read at image load), and the Swift name `load()` is ambiguous with
// NSObject's inherited class func load(). Hence loadSettings / current().
+ (GIGSettings *)loadSettings NS_SWIFT_NAME(current());
+ (void)save:(GIGSettings *)settings NS_SWIFT_NAME(save(_:));

// --- Multi-connection registry (connections/ in the settings store) ---
// All index-taking/returning methods use ONE canonical order: the loadable
// entries sorted by leaf id (what `connections`/`connectionLabels` return).

// Any usable connection saved? (Gates welcome/onboarding, replaces the old
// "baseURL is empty" check.)
+ (BOOL)isConfigured;
// Full entries, secrets included -- call when OPENING the settings UI, not
// per-frame (each entry is a Keychain read).
+ (NSArray<GIGConnection *> *)connections;
// Labels only, no secret reads -- cheap enough for periodic UI refreshes.
+ (NSArray<NSString *> *)connectionLabels;
// The active entry's index (-1 = none); the getter repairs a dangling pointer.
+ (NSInteger)activeConnectionIndex;
+ (void)setActiveConnectionIndex:(NSInteger)index;
// Commit a STAGED edit of the whole list (the settings sheet's working copy):
// saves new/changed entries, deletes vanished ones, points the active pointer
// at items[activeIndex]. Mirrors the desktop dialogs' commit-on-Save.
+ (void)saveConnections:(NSArray<GIGConnection *> *)items
            activeIndex:(NSInteger)activeIndex NS_SWIFT_NAME(saveConnections(_:activeIndex:));

// TODO(onboarding-project): temporary Forget Settings affordance; remove when
// done. Wipes EVERYTHING (config, cert pins, secrets) so onboarding restarts.
+ (void)forgetAll;
@end

#pragma mark - Engine

// An untrusted server certificate staged by the TLS layer, awaiting the user's
// trust-on-first-use decision (the iOS analog of the desktop pin prompt).
// `changed` means a DIFFERENT certificate is already pinned for this host --
// a renewal, or interception; the UI warns louder.
NS_SWIFT_NAME(PendingPin)
@interface GIGPendingPin : NSObject
@property (nonatomic, copy, readonly) NSString *host;
@property (nonatomic, copy, readonly) NSString *subject;
@property (nonatomic, copy, readonly) NSString *fingerprint;         // SPKI-SHA256, hex
@property (nonatomic, copy, readonly) NSString *expires;             // cert notAfter
@property (nonatomic, copy, readonly) NSString *reason;              // X509 error text
@property (nonatomic, assign, readonly) BOOL changed;
@property (nonatomic, copy, readonly) NSString *previousFingerprint; // when changed
@end

NS_SWIFT_NAME(EngineStatus)
@interface GIGEngineStatus : NSObject
@property (nonatomic, assign, readonly) BOOL connected;
@property (nonatomic, assign, readonly) NSInteger cameraCount;
@property (nonatomic, assign, readonly) NSInteger liveCameraCount;
@property (nonatomic, assign, readonly) unsigned long long decodedFrames;
@property (nonatomic, assign, readonly) NSInteger ingestKbps;
@property (nonatomic, copy, readonly) NSString *message;   // "ok" or the failure reason
// The last connect failure is one Settings fixes (bad/missing config OR a
// server-rejected login) rather than a network transient (host unreachable ->
// retry, and gig auto-retries). So !configError on a failure == transient.
@property (nonatomic, assign, readonly) BOOL configError;
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

// Certificate-pin flow: poll after a failed connect (and on the status ticker,
// for a mid-session cert change). Non-blocking (nil when busy). Resolve the
// decision with exactly one of the two calls below, passing back the identity
// (host + fingerprint) of the pin the user actually saw — the staged slot can
// be re-staged/replaced while an alert is up, so the decision must not depend
// on whatever is staged at resolve time.
- (nullable GIGPendingPin *)takePendingPin;

// Persist the pin and reconnect (slow, network — call off the main thread).
- (GIGEngineStatus *)acceptPendingPinForHost:(NSString *)host
                                 fingerprint:(NSString *)fingerprint
    NS_SWIFT_NAME(acceptPendingPin(host:fingerprint:));

// Decline: remembered for the session (no re-prompt for this exact cert).
- (void)declinePendingPinForHost:(NSString *)host
                     fingerprint:(NSString *)fingerprint
    NS_SWIFT_NAME(declinePendingPin(host:fingerprint:));

// Forget session pin declines. Call before a USER-initiated retry (a deliberate
// retry is a fresh trust decision); auto-reconnects must not.
- (void)resetDeclinedPins;

// TODO(onboarding-project): temporary, pairs with Forget Settings. Tears the
// session down AND drops the in-memory runtime state a settings wipe must not
// leak through: the auth cookie + TLS resumption tickets (they embody the
// forgotten credentials) and the pin store's staged/declined session state.
- (void)forgetRuntimeState;

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
