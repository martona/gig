//
//  GigBridge.mm
//  gig
//
//  ObjC++ implementation. Links the C++ core directly (headers via
//  HEADER_SEARCH_PATHS = $(SRCROOT)/../src; the vcpkg static libs via
//  OTHER_LDFLAGS). The settings store is settings_store_mac.mm (shared with macOS);
//  the engine drives gig::AppSession exactly as main.cpp does on the desktop, minus
//  the renderer (AppSession takes a null decode context, as Apple already does).
//
//  Threading: connect()/disconnect() take _mutex for their whole (network-slow)
//  duration and run off the main thread. Everything the UI polls at frame rate --
//  status(), and the Internal-category snapshot accessors the display-link render
//  tick uses -- is NON-blocking (try_lock): while the engine is busy they return
//  the last known status / empty frames instead of stalling the main thread.
//

#import "GigBridge.h"
#import "GigBridgeInternal.h"

#include "app/app_session.h"
#include "log.hpp"
#include "net/cert_pin.hpp"
#include "net/cookie_jar.hpp"
#include "net/tls_session_cache.hpp"
#include "platform/settings_store.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

NSString *toNs(const std::optional<std::string> &value)
{
    if (!value) {
        return @"";
    }
    // initWithBytes: returns nil on invalid UTF-8; never hand nil to a nonnull
    // property (Swift imports them non-optional and would trap).
    return [[NSString alloc] initWithBytes:value->data()
                                   length:static_cast<NSUInteger>(value->size())
                                 encoding:NSUTF8StringEncoding] ?: @"";
}

std::string toStd(NSString *_Nullable text)
{
    if (text == nil) {
        return {};
    }
    const char *utf8 = text.UTF8String;
    return utf8 != nullptr ? std::string(utf8) : std::string();
}

// Build an AppConfig from the store. Mirrors main.cpp's loadConfig derivation for
// the keys the iOS scaffold exposes; keep in sync (or factor loadConfig into a
// shared helper) when the UI grows. The single-stream `url` / template / decode
// knobs keep their defaults here.
gig::AppConfig loadAppConfig(const gig::SettingsStore &store)
{
    gig::AppConfig s;
    s.baseUrl = store.getString("base").value_or(std::string());
    s.user = store.getString("user").value_or(std::string());
    s.password = store.getString("password", /*encrypted=*/true).value_or(std::string());
    s.loginRefreshSeconds = static_cast<int>(store.getInt("login-refresh").value_or(600));
    s.tls.caFile = store.getString("ca").value_or(std::string());
    s.tls.certFile = store.getString("cert").value_or(std::string());
    s.tls.keyFile = store.getString("key").value_or(std::string());
    s.tls.verifyServer = !store.getBool("insecure").value_or(false);
    s.pollIntervalSeconds = static_cast<int>(store.getInt("poll-interval").value_or(5));
    s.tls.rwTimeoutUs = store.getInt("rw-timeout-us").value_or(10'000'000);
    s.tls.useSystemStore = s.tls.caFile.empty() && s.tls.certFile.empty() && s.tls.keyFile.empty();

    if (s.user.empty() != s.password.empty()) {
        gig::logWarning() << "settings: 'user' and 'password' must both be set; ignoring login auth";
        s.user.clear();
        s.password.clear();
    }
    return s;
}

} // namespace

#pragma mark - Settings

@implementation GIGSettings
@end

@implementation GIGSettingsBridge

+ (GIGSettings *)loadSettings
{
    auto store = gig::openSettingsStore();
    GIGSettings *settings = [GIGSettings new];
    settings.baseURL = toNs(store->getString("base", false));
    settings.user = toNs(store->getString("user", false));
    settings.password = toNs(store->getString("password", true));
    settings.caPath = toNs(store->getString("ca", false));
    settings.insecure = store->getBool("insecure").value_or(false);
    return settings;
}

+ (void)save:(GIGSettings *)settings
{
    auto store = gig::openSettingsStore();
    store->setString("base", toStd(settings.baseURL), false);
    store->setString("user", toStd(settings.user), false);

    const std::string password = toStd(settings.password);
    if (password.empty()) {
        store->remove("password");
    } else {
        store->setString("password", password, /*encrypt=*/true);
    }

    const std::string caPath = toStd(settings.caPath);
    if (caPath.empty()) {
        store->remove("ca");
    } else {
        store->setString("ca", caPath, false);
    }
    store->setBool("insecure", settings.insecure);
}

// TODO(onboarding-project): temporary; remove with the Forget Settings UI.
+ (void)forgetAll
{
    gig::openSettingsStore()->clear();
}

@end

#pragma mark - Pending pin

@interface GIGPendingPin ()
@property (nonatomic, copy) NSString *host;
@property (nonatomic, copy) NSString *subject;
@property (nonatomic, copy) NSString *fingerprint;
@property (nonatomic, copy) NSString *expires;
@property (nonatomic, copy) NSString *reason;
@property (nonatomic, assign) BOOL changed;
@property (nonatomic, copy) NSString *previousFingerprint;
@end

@implementation GIGPendingPin
@end

#pragma mark - Engine status

// Redeclare the header's readonly properties as readwrite so the engine can fill
// them; auto-synthesis provides the ivars + accessors.
@interface GIGEngineStatus ()
@property (nonatomic, assign) BOOL connected;
@property (nonatomic, assign) NSInteger cameraCount;
@property (nonatomic, assign) NSInteger liveCameraCount;
@property (nonatomic, assign) unsigned long long decodedFrames;
@property (nonatomic, assign) NSInteger ingestKbps;
@property (nonatomic, copy) NSString *message;
@property (nonatomic, assign) BOOL configError;
@end

@implementation GIGEngineStatus
@end

#pragma mark - Engine

// Ivars live in the class extension (not the @implementation block) so the
// Internal category below can reach them from the same translation unit.
@interface GIGEngine () {
  @package
    std::mutex _mutex;
    std::shared_ptr<gig::SettingsStore> _store;
    std::shared_ptr<gig::TlsSessionCache> _sessionCache;
    std::shared_ptr<gig::CookieJar> _cookieJar;
    std::unique_ptr<gig::CertPinStore> _pinStore;
    std::unique_ptr<gig::AppSession> _session;
    // The last connect failure's classification (drives the error screen's CTA).
    bool _lastConfigError;
}
// Last successfully built status, returned by status() when the engine is busy.
// atomic: written under _mutex, read without it.
@property (atomic, strong, nullable) GIGEngineStatus *lastStatus;
@end

@implementation GIGEngine

+ (instancetype)shared
{
    static GIGEngine *instance;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ instance = [[GIGEngine alloc] init]; });
    return instance;
}

- (GIGEngineStatus *)connect
{
    std::lock_guard<std::mutex> lock(_mutex);
    return [self connectLocked];
}

// Bring the session up from the persisted config. Caller holds _mutex.
- (GIGEngineStatus *)connectLocked
{
    // Fresh session each connect (mirrors a desktop reconnect).
    if (_session) {
        _session->stop();
        _session.reset();
    }

    if (!_store) {
        _store = gig::openSettingsStore();
    }

    // App-lifetime singletons, created once (desktop parity): the pin store keeps
    // the session-declined set across reconnects (its persisted pins live in the
    // settings store either way), and the TLS session cache + cookie jar preserve
    // resumption/auth across reconnects. Cert pinning is process-wide; the OpenSSL
    // verify callback reaches it via certPinStore() — register before any TLS.
    if (!_pinStore) {
        _pinStore = std::make_unique<gig::CertPinStore>(_store);
        gig::setCertPinStore(_pinStore.get());
    }
    if (!_sessionCache) {
        _sessionCache = std::make_shared<gig::TlsSessionCache>();
    }
    if (!_cookieJar) {
        _cookieJar = std::make_shared<gig::CookieJar>();
    }
    // Null decode context: the VideoToolbox/software decode path needs nothing
    // from a renderer (same as Apple desktop — see main.cpp).
    _session = std::make_unique<gig::AppSession>(nullptr, _sessionCache, _cookieJar);

    const gig::AppConfig cfg = loadAppConfig(*_store);
    const gig::ApplyResult result = _session->applyConfig(cfg);
    _lastConfigError = !result.ok && result.failure == gig::ApplyFailure::Config;
    if (!result.ok) {
        gig::logError() << "iOS connect failed: " << result.error;
    }
    return [self statusLocked:(result.ok ? std::string("ok") : result.error)];
}

- (void)disconnect
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_session) {
        _session->stop();
        _session.reset();
    }
    // The pin store / session cache / cookie jar are app-lifetime (see
    // connectLocked) and survive a disconnect — no TLS runs while stopped.
    self.lastStatus = nil;
}

- (GIGEngineStatus *)status
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        GIGEngineStatus *last = self.lastStatus;
        if (last) {
            return last;
        }
        GIGEngineStatus *busy = [GIGEngineStatus new];
        busy.connected = NO;
        busy.message = @"working";
        return busy;
    }
    return [self statusLocked:std::string("ok")];
}

// Builds a status snapshot. Caller holds _mutex.
- (GIGEngineStatus *)statusLocked:(const std::string &)message
{
    GIGEngineStatus *out = [GIGEngineStatus new];
    const bool running = _session && _session->running();
    out.connected = running ? YES : NO;
    out.cameraCount = _session ? static_cast<NSInteger>(_session->cameraCount()) : 0;
    out.liveCameraCount = _session ? _session->liveCameraCount() : 0;
    out.decodedFrames = _session ? _session->totalDecodedFrames() : 0;
    out.ingestKbps = _session ? _session->ingestKbps() : 0;
    // Error text can carry raw server bytes (HTTP reason phrase); on invalid
    // UTF-8 fall back rather than putting nil into the nonnull property.
    out.message = [[NSString alloc] initWithBytes:message.data()
                                           length:static_cast<NSUInteger>(message.size())
                                         encoding:NSUTF8StringEncoding]
        ?: @"(non-UTF-8 error message)";
    out.configError = _lastConfigError ? YES : NO;
    self.lastStatus = out;
    return out;
}

- (void)resetDeclinedPins
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pinStore) {
        _pinStore->clearSessionDeclines();
    }
}

// TODO(onboarding-project): temporary, pairs with Forget Settings.
- (void)forgetRuntimeState
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_session) {
        _session->stop();
        _session.reset();
    }
    // The jar/cache are create-once (connectLocked); dropping them here means the
    // next connect starts from a clean slate -- no auth cookie or resumption
    // ticket from the forgotten credentials survives in memory.
    _cookieJar.reset();
    _sessionCache.reset();
    if (_pinStore) {
        _pinStore->reset(); // staged pending + session declines
    }
    self.lastStatus = nil;
}

#pragma mark Pin flow

- (nullable GIGPendingPin *)takePendingPin
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !_pinStore) {
        return nil; // busy (connect in flight) or pinning not initialized yet
    }
    std::optional<gig::PendingPinDecision> pending = _pinStore->takePending();
    if (!pending) {
        return nil;
    }

    NSString *(^ns)(const std::string &) = ^NSString *(const std::string &value) {
        return [[NSString alloc] initWithBytes:value.data()
                                       length:static_cast<NSUInteger>(value.size())
                                     encoding:NSUTF8StringEncoding] ?: @"";
    };
    GIGPendingPin *out = [GIGPendingPin new];
    out.host = ns(pending->host);
    out.subject = ns(pending->subject);
    out.fingerprint = ns(pending->spki);
    out.expires = ns(pending->notAfter);
    out.reason = ns(pending->errorText);
    out.changed = pending->changed ? YES : NO;
    out.previousFingerprint = ns(pending->previousSpki);
    return out;
}

// acceptPin/declinePin only need the identity (host + SPKI), so the decision is
// rebuilt from what the UI displayed — no state held here between take and
// resolve, and a slot re-staged mid-alert can't redirect the user's decision.

- (GIGEngineStatus *)acceptPendingPinForHost:(NSString *)host fingerprint:(NSString *)fingerprint
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pinStore && host.length > 0 && fingerprint.length > 0) {
        gig::PendingPinDecision decision;
        decision.host = toStd(host);
        decision.spki = toStd(fingerprint);
        _pinStore->acceptPin(decision);
        gig::logInfo() << "pinned certificate for " << decision.host;
    }
    return [self connectLocked]; // retry with the pin in place
}

- (void)declinePendingPinForHost:(NSString *)host fingerprint:(NSString *)fingerprint
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pinStore && host.length > 0 && fingerprint.length > 0) {
        gig::PendingPinDecision decision;
        decision.host = toStd(host);
        decision.spki = toStd(fingerprint);
        _pinStore->declinePin(decision);
        gig::logWarning() << "declined certificate for " << decision.host;
    }
}

@end

#pragma mark - Engine internals (renderer host)

@implementation GIGEngine (Internal)

- (std::vector<std::shared_ptr<VideoFrame>>)snapshotFramesInternal
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !_session) {
        return {};
    }
    return _session->snapshotFrames();
}

- (std::vector<std::uint64_t>)tileByteCountsInternal
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !_session) {
        return {};
    }
    return _session->tileByteCounts();
}

- (std::vector<std::string>)cameraLabelsInternal
{
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !_session) {
        return {};
    }
    return _session->cameraLabels();
}

@end

#pragma mark - Log

@implementation GIGLogBridge

+ (NSString *)snapshotText
{
    std::vector<std::string> lines;
    gig::LogBuffer::instance().snapshot(lines);
    std::string joined;
    for (const std::string &line : lines) {
        joined += line;
        joined.push_back('\n');
    }
    return [[NSString alloc] initWithBytes:joined.data()
                                   length:static_cast<NSUInteger>(joined.size())
                                 encoding:NSUTF8StringEncoding] ?: @"";
}

+ (void)clear
{
    gig::LogBuffer::instance().clear();
}

@end
