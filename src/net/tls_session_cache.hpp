#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <memory>

namespace gig {

struct SslSessionDeleter {
    void operator()(SSL_SESSION* session) const noexcept;
};

using SslSessionPtr = std::unique_ptr<SSL_SESSION, SslSessionDeleter>;

// A small thread-safe pool of resumable TLS sessions for one server.
// Ported from the hitsc project. Not host-keyed: we only ever talk to one
// Frigate origin, so any cached session is a valid resumption candidate.
class TlsSessionCache {
public:
    explicit TlsSessionCache(std::size_t maxSize = 16);
    ~TlsSessionCache();

    TlsSessionCache(TlsSessionCache&&) noexcept;
    TlsSessionCache& operator=(TlsSessionCache&&) noexcept;
    TlsSessionCache(const TlsSessionCache&) = delete;
    TlsSessionCache& operator=(const TlsSessionCache&) = delete;

    // Takes ownership of `session` on success (returns true). Expired or
    // invalid sessions are rejected (returns false; caller/OpenSSL frees).
    bool push(SSL_SESSION* session);
    SslSessionPtr pop();
    std::size_t size() const;

    // Drop every cached session. Used when credentials are wiped (Forget
    // Settings): resumption tickets bound to the old identity must not survive.
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Enable client-side session caching on an SSL_CTX. New sessions issued by the
// server are routed to whichever TlsSessionCache is attached per-connection via
// offerCachedSession(). Call once per context, before any handshake.
void enableSessionCache(SSL_CTX* context);

// Attach `cache` to this connection and, if a cached session is available,
// offer it for resumption. Call after creating the SSL stream, before connect.
void offerCachedSession(SSL* ssl, TlsSessionCache& cache);

// True if the completed handshake resumed a cached session.
bool sessionWasReused(SSL* ssl);

} // namespace gig
