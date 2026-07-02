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

#import "GigBridge.h"

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

namespace {

NSString *toNs(const std::optional<std::string> &value)
{
    if (!value) {
        return @"";
    }
    return [[NSString alloc] initWithBytes:value->data()
                                   length:static_cast<NSUInteger>(value->size())
                                 encoding:NSUTF8StringEncoding];
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

+ (GIGSettings *)load
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
@end

@implementation GIGEngineStatus
@end

#pragma mark - Engine

@implementation GIGEngine {
    std::mutex _mutex;
    std::shared_ptr<gig::SettingsStore> _store;
    std::shared_ptr<gig::TlsSessionCache> _sessionCache;
    std::shared_ptr<gig::CookieJar> _cookieJar;
    std::unique_ptr<gig::CertPinStore> _pinStore;
    std::unique_ptr<gig::AppSession> _session;
}

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

    // Fresh session each connect (mirrors a desktop reconnect).
    if (_session) {
        _session->stop();
        _session.reset();
    }

    _store = gig::openSettingsStore();

    // Cert pinning is process-wide; the OpenSSL verify callback reaches it via
    // certPinStore(). Register before any TLS connection. (Interactive pin prompts
    // aren't wired into the iOS UI yet — a pending pin surfaces as a connect
    // failure for now.)
    _pinStore = std::make_unique<gig::CertPinStore>(_store);
    gig::setCertPinStore(_pinStore.get());

    _sessionCache = std::make_shared<gig::TlsSessionCache>();
    _cookieJar = std::make_shared<gig::CookieJar>();
    // Null decode context: the VideoToolbox/software decode path needs nothing
    // from a renderer (same as Apple desktop — see main.cpp).
    _session = std::make_unique<gig::AppSession>(nullptr, _sessionCache, _cookieJar);

    const gig::AppConfig cfg = loadAppConfig(*_store);
    const gig::ApplyResult result = _session->applyConfig(cfg);
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
    gig::setCertPinStore(nullptr);
    _pinStore.reset();
}

- (GIGEngineStatus *)status
{
    std::lock_guard<std::mutex> lock(_mutex);
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
    out.message = [[NSString alloc] initWithBytes:message.data()
                                           length:static_cast<NSUInteger>(message.size())
                                         encoding:NSUTF8StringEncoding];
    return out;
}

@end
