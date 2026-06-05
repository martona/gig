#pragma once

#include "net/tls_options.h"
#include "net/tls_session_cache.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace gig {

class CookieJar;

struct HttpResponse {
    bool ok = false;
    unsigned status = 0;
    int redirects = 0;
    std::string reason;
    std::string error;
    std::string finalUrl;
    std::string body;
    bool truncated = false;
};

// Minimal persistent HTTPS/HTTP client for the Frigate control plane
// (discovery + health polling). Holds one TLS context so the shared
// TlsSessionCache resumes handshakes across calls, and persists cookies across
// requests. mTLS material comes from TlsOptions.
//
// NOT thread-safe: give each worker (discovery, health poller) its own
// HttpClient and share the TlsSessionCache (which is internally locked).
class HttpClient {
public:
    HttpClient(
        std::string baseUrl,
        TlsOptions tls,
        std::shared_ptr<TlsSessionCache> sessionCache,
        std::shared_ptr<CookieJar> cookieJar);
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // GET a path relative to the base URL (or an absolute URL), following
    // redirects and persisting cookies across calls.
    HttpResponse get(const std::string& pathOrUrl, std::size_t maxBytes = 4 * 1024 * 1024);

    const std::string& baseUrl() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gig
