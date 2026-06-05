#pragma once

#include "net/tls_options.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// Boost/OpenSSL are deliberately kept out of this header so it can be included
// from the FFmpeg decoder TU without colliding with libav / d3d11 / wincrypt
// headers (the same discipline behind win_cert_store).

namespace gig {

class TlsSessionCache;
class CookieJar;

// A live, de-chunked HTTP(S) response body fed by our own Boost.Beast TLS stack.
// One per streaming connection; created by TlsClient::open(). read() blocks until
// data, end-of-stream, the read timeout, or cancel() fires -- it never polls.
class MediaStream {
public:
    struct Impl; // opaque; defined in tls_client.cpp
    explicit MediaStream(std::unique_ptr<Impl> impl);
    ~MediaStream();

    MediaStream(const MediaStream&) = delete;
    MediaStream& operator=(const MediaStream&) = delete;

    // Pull up to `size` decoded body bytes into `buf`. Returns the byte count
    // (>0), 0 at end-of-stream, or -1 on abort / timeout / error.
    int read(std::uint8_t* buf, int size);

    // Unblock an in-flight read() promptly from another thread. Event-driven:
    // posts a socket close into this stream's own io_context, which wakes the
    // blocked io.run(). Safe to call once the owner has published the stream.
    void cancel();

private:
    std::unique_ptr<Impl> impl_;
};

// Owns one configured client TLS context for the video plane and hands out
// streaming connections that share it, the TLS session cache, and the cookie
// jar. Build one and reuse it for the whole app: a single client-cert load
// (Phase 1: one consent) plus cross-connection session resumption. This is also
// the app-lifetime "TLS holder" Phase 1 needs. open() is thread-safe.
class TlsClient {
public:
    TlsClient(
        TlsOptions tls,
        std::shared_ptr<TlsSessionCache> sessionCache,
        std::shared_ptr<CookieJar> cookieJar);
    ~TlsClient();

    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    // Open a streaming GET to `url` (https only), following up to a few
    // redirects, and return the live body stream. Throws std::runtime_error on
    // connect / TLS / HTTP failure. `stopFlag` is observed so a connect /
    // handshake / header read aborts promptly when stop is requested.
    std::unique_ptr<MediaStream> open(const std::string& url, const std::atomic_bool& stopFlag);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gig
