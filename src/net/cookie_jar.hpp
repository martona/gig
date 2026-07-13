#pragma once

#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace gig {

// A small, thread-safe cookie store keyed by origin (scheme://host[:port]).
// Shared by the control-plane HttpClient and the video TlsClient so a session
// cookie obtained on one connection flows to the others. Today (mTLS) it is
// typically empty; it is the seam for native Frigate auth later.
//
// Deliberately simple: no path/expiry/secure attributes -- it mirrors the prior
// in-client behavior (name=value pairs per origin), just shared and locked.
class CookieJar {
public:
    // Record one Set-Cookie header value (the text after "Set-Cookie:") for origin.
    void storeFromResponse(const std::string& origin, std::string_view setCookieValue);

    // Build a Cookie request-header value for origin, or empty if none are stored.
    std::string headerFor(const std::string& origin) const;

    // True if a cookie with this name is stored for origin.
    bool contains(const std::string& origin, const std::string& name) const;

    // Drop every stored cookie (all origins). Used when credentials are wiped
    // (Forget Settings): a live auth token must not survive the wipe in memory.
    void clear();

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::map<std::string, std::string>> cookies_;
};

} // namespace gig
