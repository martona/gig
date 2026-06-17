#include "net/frigate_auth.hpp"

#include "log.hpp"
#include "net/cookie_jar.hpp"
#include "net/http_client.hpp"
#include "net/url.h"

#include <boost/json.hpp>

#include <cctype>
#include <stdexcept>
#include <utility>

namespace gig {
namespace {

// Cadence for retrying a failed refresh: the previous token stays valid for a
// while, so retry briskly instead of waiting out the full interval.
constexpr std::chrono::seconds RetryInterval { 30 };

// Like the health client: keep I/O timeouts short so stop() never waits long
// on an in-flight login.
TlsOptions clampLoginTimeout(TlsOptions tls)
{
    if (tls.rwTimeoutUs <= 0 || tls.rwTimeoutUs > 3'000'000) {
        tls.rwTimeoutUs = 3'000'000;
    }
    return tls;
}

// One-line, bounded view of an error-response body for logs (Frigate returns a
// short JSON message on auth failures).
std::string bodySnippet(const std::string& body)
{
    std::string snippet;
    for (const char ch : body) {
        if (snippet.size() >= 200) {
            snippet += "...";
            break;
        }
        const unsigned char uc = static_cast<unsigned char>(ch);
        snippet += std::isprint(uc) ? ch : ' ';
    }
    return snippet;
}

} // namespace

FrigateAuth::FrigateAuth(
    FrigateAuthConfig config,
    std::shared_ptr<TlsSessionCache> sessionCache,
    std::shared_ptr<CookieJar> cookieJar)
    : config_(std::move(config))
    , cookieJar_(cookieJar)
    , client_(std::make_unique<HttpClient>(
          config_.baseUrl, clampLoginTimeout(config_.tls), std::move(sessionCache), std::move(cookieJar)))
{
}

FrigateAuth::~FrigateAuth()
{
    stop();
}

bool FrigateAuth::login(std::string* error)
{
    const std::string body = boost::json::serialize(boost::json::object {
        { "user", config_.user },
        { "password", config_.password },
    });

    const HttpResponse response = client_->post("/api/login", "application/json", body, 64 * 1024);
    if (!response.ok) {
        if (error) {
            *error = response.error;
            if (!response.body.empty()) {
                *error += " (" + bodySnippet(response.body) + ")";
            }
        }
        return false;
    }

    // The jar stored any Set-Cookie already; a 200 without the token cookie
    // means something between us and Frigate swallowed it.
    const std::string origin = originForUrl(parseUrl(config_.baseUrl));
    if (!cookieJar_->contains(origin, "frigate_token")) {
        logWarning() << "frigate auth: login OK but no frigate_token cookie was set"
                     << " -- requests will go out unauthenticated";
    }
    return true;
}

void FrigateAuth::startAutoRefresh()
{
    if (thread_.joinable()) {
        return;
    }
    stopRequested_ = false;
    thread_ = std::thread([this] { refreshLoop(); });
}

void FrigateAuth::stop()
{
    stopRequested_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FrigateAuth::refreshLoop()
{
    logInfo() << "frigate auth: refreshing the login every " << config_.refreshInterval.count() << "s";
    std::chrono::seconds wait = config_.refreshInterval;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, wait, [this] { return stopRequested_.load(); })) {
                return;
            }
        }
        std::string error;
        if (login(&error)) {
            logInfo() << "frigate auth: token refreshed";
            wait = config_.refreshInterval;
        } else {
            logWarning() << "frigate auth: refresh failed (" << error << "); retrying in "
                         << RetryInterval.count() << "s";
            wait = RetryInterval;
        }
    }
}

} // namespace gig
