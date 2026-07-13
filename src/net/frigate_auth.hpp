#pragma once

#include "net/tls_options.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gig {

class CookieJar;
class HttpClient;
class TlsSessionCache;

struct FrigateAuthConfig {
    std::string baseUrl; // Frigate control-plane base; the endpoint is {base}/api/login
    std::string user;
    std::string password;
    std::chrono::seconds refreshInterval { 600 };
    TlsOptions tls;
};

// Native Frigate username/password auth. login() POSTs {base}/api/login; the
// JWT comes back as a Set-Cookie (frigate_token) that the shared CookieJar
// picks up, so every control-plane and video request carries it from then on.
// startAutoRefresh() re-logs-in every refreshInterval on a background thread
// so the token never ages out mid-run: new/restarted stream connections always
// present a fresh cookie (established streams are unaffected either way, since
// Frigate checks auth at request time). Independent of the TLS client-cert
// auth in TlsOptions -- both can be active at once.
class FrigateAuth {
public:
    FrigateAuth(
        FrigateAuthConfig config,
        std::shared_ptr<TlsSessionCache> sessionCache,
        std::shared_ptr<CookieJar> cookieJar);
    ~FrigateAuth();

    FrigateAuth(const FrigateAuth&) = delete;
    FrigateAuth& operator=(const FrigateAuth&) = delete;

    // One blocking login POST; never logs or echoes the password. On failure
    // returns false with the reason (status + a response-body snippet, or the
    // transport error) in *error. *serverRejected (optional) is set true when
    // the server ANSWERED with a 4xx -- an app-level rejection (bad creds),
    // distinct from a network-level failure the caller may auto-retry.
    bool login(std::string* error = nullptr, bool* serverRejected = nullptr);

    void startAutoRefresh();
    void stop();

private:
    void refreshLoop();

    FrigateAuthConfig config_;
    std::shared_ptr<CookieJar> cookieJar_;
    std::unique_ptr<HttpClient> client_; // used sequentially: startup login, then only the refresh thread

    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic_bool stopRequested_ { false };
    std::thread thread_;
};

} // namespace gig
