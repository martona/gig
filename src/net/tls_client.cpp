#include "net/tls_client.hpp"

#include "log.hpp"
#include "net/cert_pin.hpp"
#include "net/cookie_jar.hpp"
#include "net/tls_context.hpp"
#include "net/tls_session_cache.hpp"
#include "net/url.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <boost/optional.hpp>
#include <openssl/ssl.h>

namespace gig {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace json = boost::json;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

constexpr int kMaxRedirects = 4;

// The go2rtc websocket-MSE hello (see MseStreamImpl). Codec strings are exact
// literals go2rtc matches (pkg/mp4/helpers.go); they are SELECTORS, not
// constraints -- any H.264/H.265 profile the camera produces is delivered.
// Audio codecs deliberately omitted: no audio track is then muxed at all.
constexpr const char* kMseHello = R"({"type":"mse","value":"avc1.640029,hvc1.1.6.L153.B0"})";

// A go2rtc websocket-MSE stream URL (Frigate's /live/mse/api/ws?src=NAME).
// Matched on the PATH suffix so HA-ingress base-path deployments still hit it.
bool isMseTarget(const std::string& target)
{
    const std::size_t query = target.find('?');
    const std::string path = query == std::string::npos ? target : target.substr(0, query);
    static constexpr std::string_view kSuffix = "/live/mse/api/ws";
    return path.size() >= kSuffix.size()
        && path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

struct HopOutcome {
    unsigned status = 0;
    std::string location;
};

} // namespace

// ---------------------------------------------------------------------------
// MediaStream::Impl -- one streaming connection (its own io_context + stream).
// Two flavors behind one interface: the plain HTTPS GET body (HttpStreamImpl,
// the bare-URL escape hatch) and the go2rtc websocket-MSE fMP4 stream
// (MseStreamImpl, the Frigate transport since 0.18 removed the generic go2rtc
// proxy that served stream.ts).
// ---------------------------------------------------------------------------
struct MediaStream::Impl {
    virtual ~Impl() = default;
    virtual int read(std::uint8_t* buf, int size) = 0;
    virtual void cancel() = 0;
};

namespace {

struct HttpStreamImpl final : MediaStream::Impl {
    HttpStreamImpl(ssl::context& context,
                   std::int64_t rwTimeoutUs,
                   const std::atomic_bool* stop,
                   std::shared_ptr<TlsSessionCache> sessionCache,
                   std::shared_ptr<CookieJar> cookieJar)
        : context_(context)
        , rwTimeoutUs_(rwTimeoutUs > 0 ? rwTimeoutUs : 10'000'000)
        , stop_(stop)
        , sessionCache_(std::move(sessionCache))
        , cookieJar_(std::move(cookieJar))
    {
    }

    // Resolve + connect + handshake + GET + read response header for one hop.
    // Leaves the connection live on a 2xx (the body is then pulled via read()).
    HopOutcome performRequest(const ParsedUrl& parsed, const std::string& origin)
    {
        buffer_.clear();
        parser_.emplace();
        parser_->body_limit(boost::none); // endless stream: no body cap
        stream_.emplace(io_, context_);

        if (!SSL_set_tlsext_host_name(stream_->native_handle(), parsed.host.c_str())) {
            throw std::runtime_error("failed to set TLS SNI host name");
        }
        prepareConnectionPinning(stream_->native_handle(), parsed.host); // hostname verify + pinning
        if (sessionCache_) {
            offerCachedSession(stream_->native_handle(), *sessionCache_);
        }

        tcp::resolver resolver(io_);
        boost::system::error_code ec;
        const auto endpoints = resolver.resolve(parsed.host, parsed.port, ec);
        if (ec) {
            throw std::runtime_error("resolve " + parsed.host + ": " + ec.message());
        }

        throwIfStopRequested();
        ec = runOne([&](auto&& complete) {
            beast::get_lowest_layer(*stream_).async_connect(
                endpoints,
                [c = std::forward<decltype(complete)>(complete)](
                    boost::system::error_code e, const tcp::endpoint&) mutable { c(e); });
        });
        if (ec) {
            // Log the resolved addresses: async_connect tried them all, so each was
            // unreachable -- surfaces an unroutable family (e.g. IPv6 with no route).
            for (const auto& entry : endpoints) {
                const auto address = entry.endpoint().address();
                logWarning() << "  " << parsed.host << " resolved to " << address.to_string()
                             << (address.is_v6() ? " [IPv6]" : " [IPv4]");
            }
            throw std::runtime_error("connect " + parsed.host + ": " + ec.message());
        }

        throwIfStopRequested();
        ec = runOne([&](auto&& complete) {
            stream_->async_handshake(
                ssl::stream_base::client,
                [c = std::forward<decltype(complete)>(complete)](boost::system::error_code e) mutable { c(e); });
        });
        if (ec) {
            throw std::runtime_error("tls handshake " + parsed.host + ": " + ec.message());
        }
        if (sessionCache_) {
            logDebug() << "video tls handshake host=" << parsed.host
                       << " reused=" << (sessionWasReused(stream_->native_handle()) ? "yes" : "no")
                       << " cached=" << sessionCache_->size();
        }

        http::request<http::empty_body> request{http::verb::get, parsed.target, 11};
        request.set(http::field::host, hostHeader(parsed));
        request.set(http::field::user_agent, "gig");
        request.set(http::field::accept, "*/*");
        if (cookieJar_) {
            const std::string cookie = cookieJar_->headerFor(origin);
            if (!cookie.empty()) {
                request.set(http::field::cookie, cookie);
            }
        }

        throwIfStopRequested();
        ec = runOne([&](auto&& complete) {
            http::async_write(
                *stream_, request,
                [c = std::forward<decltype(complete)>(complete)](
                    boost::system::error_code e, std::size_t) mutable { c(e); });
        });
        if (ec) {
            throw std::runtime_error("write request " + parsed.host + ": " + ec.message());
        }

        throwIfStopRequested();
        ec = runOne([&](auto&& complete) {
            http::async_read_header(
                *stream_, buffer_, *parser_,
                [c = std::forward<decltype(complete)>(complete)](
                    boost::system::error_code e, std::size_t) mutable { c(e); });
        });
        if (ec) {
            throw std::runtime_error("read response header " + parsed.host + ": " + ec.message());
        }

        auto& response = parser_->get();
        if (cookieJar_) {
            for (const auto& field : response.base()) {
                if (field.name() == http::field::set_cookie) {
                    cookieJar_->storeFromResponse(
                        origin, std::string_view(field.value().data(), field.value().size()));
                }
            }
        }

        HopOutcome outcome;
        outcome.status = response.result_int();
        const auto location = response.find(http::field::location);
        if (location != response.end()) {
            outcome.location = std::string(location->value());
        }
        return outcome;
    }

    int read(std::uint8_t* buf, int size) override
    {
        if (!stream_ || eof_) {
            return 0;
        }
        if (stopRequested_()) {
            return -1;
        }
        if (size <= 0) {
            return 0;
        }

        for (;;) {
            if (parser_->is_done()) {
                eof_ = true;
                return 0;
            }

            auto& body = parser_->get().body();
            body.data = buf;
            body.size = static_cast<std::size_t>(size);

            const boost::system::error_code ec = runOne([&](auto&& complete) {
                http::async_read_some(
                    *stream_, buffer_, *parser_,
                    [c = std::forward<decltype(complete)>(complete)](
                        boost::system::error_code e, std::size_t) mutable { c(e); });
            });

            const int produced = size - static_cast<int>(body.size);

            if (stopRequested_()) {
                return -1;
            }
            if (produced > 0) {
                return produced;
            }
            if (parser_->is_done()) {
                eof_ = true;
                return 0;
            }
            if (ec == asio::error::eof || ec == ssl::error::stream_truncated) {
                eof_ = true;
                return 0;
            }
            if (ec && ec != http::error::need_buffer) {
                return -1; // timeout / aborted / protocol error -> reconnect
            }
            // ec == success (or need_buffer) with no payload yet: only framing was
            // consumed this pass; read again (bounded by the per-op timeout + stop).
        }
    }

    void cancel() override
    {
        // Posting into our own io_context wakes a blocked io.run() (IOCP); closing
        // the stream aborts the outstanding async op with operation_aborted.
        asio::post(io_, [this] {
            if (stream_) {
                beast::get_lowest_layer(*stream_).close();
            }
        });
    }

private:
    bool stopRequested_() const { return stop_ && stop_->load(); }
    void throwIfStopRequested()
    {
        if (stopRequested_()) {
            throw std::runtime_error("stream open aborted (stop requested)");
        }
    }

    // Run exactly one async op to completion, bounded by the read timeout. The
    // worker blocks in io_.run() until the op's handler fires -- via a data event,
    // the one-shot timeout timer, or a posted cancel. No polling.
    template <class Initiate>
    boost::system::error_code runOne(Initiate&& initiate)
    {
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::microseconds(rwTimeoutUs_));
        boost::system::error_code result;
        initiate([&result](boost::system::error_code ec) { result = ec; });
        io_.restart();
        io_.run();
        beast::get_lowest_layer(*stream_).expires_never();
        return result;
    }

    ssl::context& context_;
    std::int64_t rwTimeoutUs_;
    const std::atomic_bool* stop_;
    std::shared_ptr<TlsSessionCache> sessionCache_;
    std::shared_ptr<CookieJar> cookieJar_;

    asio::io_context io_;
    std::optional<beast::ssl_stream<beast::tcp_stream>> stream_;
    beast::flat_buffer buffer_;
    std::optional<http::response_parser<http::buffer_body>> parser_;
    bool eof_ = false;
};

// ---------------------------------------------------------------------------
// MseStreamImpl -- go2rtc websocket-MSE (fMP4 over a beast websocket).
//
// Protocol (verified against go2rtc v1.9.2..v1.9.14, the Frigate 0.14..0.18
// envelope; internal/api/ws/ws.go + internal/mp4/ws.go):
//   C->S TEXT  {"type":"mse","value":"<codec selectors>"}   (once per socket)
//   S->C TEXT  {"type":"mse","value":"video/mp4; codecs=..."}  (negotiated)
//   S->C BIN   raw fMP4: first the init segment (ftyp+moov), then moof+mdat
//              fragments -- possibly SEVERAL per websocket message (the
//              attach-time backlog flushes as one write), so the binary
//              channel is treated strictly as a byte stream.
//   S->C TEXT  {"type":"error","value":"mse: ..."} on failure; the server
//              does NOT close the socket after an error -- the client must.
//
// The server never pings and never times out an idle client, and a stalled
// camera produces silence with no notification -- so the per-read deadline
// below doubles as the data-stall watchdog (any traffic resets it; we send
// no pings that could mask a stall). Server-side close on error paths is a
// bare TCP close (no close handshake): eof/truncated reads are normal ends.
// Reconnects get a fresh init segment with timestamps restarting near zero,
// which suits the decoder's open-demuxer-per-attempt lifecycle.
// ---------------------------------------------------------------------------
struct MseStreamImpl final : MediaStream::Impl {
    MseStreamImpl(ssl::context& context,
                  std::int64_t rwTimeoutUs,
                  const std::atomic_bool* stop,
                  std::shared_ptr<TlsSessionCache> sessionCache,
                  std::shared_ptr<CookieJar> cookieJar)
        : context_(context)
        , rwTimeoutUs_(rwTimeoutUs > 0 ? rwTimeoutUs : 10'000'000)
        , stop_(stop)
        , sessionCache_(std::move(sessionCache))
        , cookieJar_(std::move(cookieJar))
    {
    }

    // Resolve + connect + TLS + websocket upgrade + MSE hello. Throws on any
    // failure (the caller's open() surfaces it; the decoder retries).
    void connect(const ParsedUrl& parsed, const std::string& origin)
    {
        ws_.emplace(io_, context_);
        auto& sslStream = ws_->next_layer();

        if (!SSL_set_tlsext_host_name(sslStream.native_handle(), parsed.host.c_str())) {
            throw std::runtime_error("failed to set TLS SNI host name");
        }
        prepareConnectionPinning(sslStream.native_handle(), parsed.host);
        if (sessionCache_) {
            offerCachedSession(sslStream.native_handle(), *sessionCache_);
        }

        tcp::resolver resolver(io_);
        boost::system::error_code ec;
        const auto endpoints = resolver.resolve(parsed.host, parsed.port, ec);
        if (ec) {
            throw std::runtime_error("resolve " + parsed.host + ": " + ec.message());
        }

        throwIfStopRequested();
        ec = guardedRun([&](auto&& complete) {
            beast::get_lowest_layer(*ws_).async_connect(
                endpoints,
                [c = std::forward<decltype(complete)>(complete)](
                    boost::system::error_code e, const tcp::endpoint&) mutable { c(e); });
        });
        if (ec) {
            throw std::runtime_error("connect " + parsed.host + ": " + ec.message());
        }

        throwIfStopRequested();
        ec = guardedRun([&](auto&& complete) {
            sslStream.async_handshake(
                ssl::stream_base::client,
                [c = std::forward<decltype(complete)>(complete)](boost::system::error_code e) mutable { c(e); });
        });
        if (ec) {
            throw std::runtime_error("tls handshake " + parsed.host + ": " + ec.message());
        }

        // The upgrade request carries the auth cookie (frigate_token) exactly
        // like the /ws activity feed; beast's own websocket timeout machinery
        // stays OFF (it keeps a persistent timer that never lets a
        // run-to-completion io_context return) -- guardedRun's per-op deadline
        // covers every operation instead. The tcp_stream expiry must not be
        // mixed with websocket ops either, so it stays untouched (never armed).
        ws_->set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
            request.set(http::field::user_agent, "gig");
            const std::string cookie = cookieJar_ ? cookieJar_->headerFor(origin) : std::string();
            if (!cookie.empty()) {
                request.set(http::field::cookie, cookie);
            }
        }));

        throwIfStopRequested();
        ec = guardedRun([&](auto&& complete) {
            ws_->async_handshake(
                hostHeader(parsed), parsed.target,
                [c = std::forward<decltype(complete)>(complete)](boost::system::error_code e) mutable { c(e); });
        });
        if (ec) {
            throw std::runtime_error("mse websocket handshake " + parsed.host + ": " + ec.message());
        }

        // The one and only control message we send (a second would attach a
        // second consumer server-side, interleaving two fMP4 muxes).
        throwIfStopRequested();
        ws_->text(true);
        ec = guardedRun([&](auto&& complete) {
            ws_->async_write(
                asio::buffer(std::string_view(kMseHello)),
                [c = std::forward<decltype(complete)>(complete)](
                    boost::system::error_code e, std::size_t) mutable { c(e); });
        });
        if (ec) {
            throw std::runtime_error("mse hello write " + parsed.host + ": " + ec.message());
        }
    }

    int read(std::uint8_t* buf, int size) override
    {
        if (!ws_ || eof_) {
            return 0;
        }
        if (stopRequested_()) {
            return -1;
        }
        if (size <= 0) {
            return 0;
        }

        while (pendingOffset_ >= pending_.size()) {
            pending_.clear();
            pendingOffset_ = 0;

            buffer_.consume(buffer_.size());
            const boost::system::error_code ec = guardedRun([&](auto&& complete) {
                ws_->async_read(
                    buffer_,
                    [c = std::forward<decltype(complete)>(complete)](
                        boost::system::error_code e, std::size_t) mutable { c(e); });
            });

            if (stopRequested_()) {
                return -1;
            }
            if (ec == websocket::error::closed || ec == asio::error::eof
                || ec == ssl::error::stream_truncated) {
                // Clean close, or the server's bare-TCP-close error path (it
                // never sends a close frame on failure): end of stream, the
                // decoder reconnects.
                eof_ = true;
                return 0;
            }
            if (ec) {
                return -1; // timeout (data stall), aborted, or protocol error
            }

            if (ws_->got_text()) {
                if (!handleControlMessage(beast::buffers_to_string(buffer_.data()))) {
                    return -1; // server-reported error: treat as fatal
                }
                continue;
            }
            const auto data = buffer_.data();
            pending_.append(static_cast<const char*>(data.data()), data.size());
        }

        const int available = static_cast<int>(pending_.size() - pendingOffset_);
        const int produced = available < size ? available : size;
        std::memcpy(buf, pending_.data() + pendingOffset_, static_cast<std::size_t>(produced));
        pendingOffset_ += static_cast<std::size_t>(produced);
        return produced;
    }

    void cancel() override
    {
        asio::post(io_, [this] {
            if (ws_) {
                beast::get_lowest_layer(*ws_).close();
            }
        });
    }

private:
    // TEXT frames are the control channel. Returns false for a fatal
    // server-side error ({"type":"error"}); anything else is informational.
    bool handleControlMessage(const std::string& text)
    {
        boost::system::error_code parseEc;
        const json::value parsed = json::parse(text, parseEc);
        if (parseEc || !parsed.is_object()) {
            return true; // unknown chatter: ignore, like the reference client
        }
        const json::object& message = parsed.get_object();
        const json::value* type = message.if_contains("type");
        if (!type || !type->is_string()) {
            return true;
        }
        const json::value* value = message.if_contains("value");
        const std::string valueText
            = value && value->is_string() ? std::string(value->get_string()) : std::string();
        if (type->get_string() == "error") {
            logWarning() << "mse stream error: " << valueText;
            return false;
        }
        if (type->get_string() == "mse" && !loggedCodecs_) {
            loggedCodecs_ = true;
            logInfo() << "mse negotiated: " << valueText;
        }
        return true;
    }

    bool stopRequested_() const { return stop_ && stop_->load(); }
    void throwIfStopRequested()
    {
        if (stopRequested_()) {
            throw std::runtime_error("stream open aborted (stop requested)");
        }
    }

    // Run one async op to completion under a deadline. The deadline timer
    // closes the socket on expiry (aborting the op), and the op's completion
    // cancels the timer -- so io_.run() always returns with no work left.
    // Doubles as the data-stall watchdog on reads: go2rtc sends a fragment
    // per frame while healthy and total silence when the source dies.
    template <class Initiate>
    boost::system::error_code guardedRun(Initiate&& initiate)
    {
        boost::system::error_code result;
        bool timedOut = false;
        asio::steady_timer deadline(io_);
        deadline.expires_after(std::chrono::microseconds(rwTimeoutUs_));
        deadline.async_wait([&](boost::system::error_code ec) {
            if (!ec) {
                timedOut = true;
                if (ws_) {
                    beast::get_lowest_layer(*ws_).close();
                }
            }
        });
        initiate([&](boost::system::error_code ec) {
            result = ec;
            deadline.cancel();
        });
        io_.restart();
        io_.run();
        if (timedOut) {
            result = beast::error::timeout;
        }
        return result;
    }

    ssl::context& context_;
    std::int64_t rwTimeoutUs_;
    const std::atomic_bool* stop_;
    std::shared_ptr<TlsSessionCache> sessionCache_;
    std::shared_ptr<CookieJar> cookieJar_;

    asio::io_context io_;
    std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    std::string pending_; // undelivered fMP4 bytes from the last binary frame
    std::size_t pendingOffset_ = 0;
    bool eof_ = false;
    bool loggedCodecs_ = false;
};

} // namespace

MediaStream::MediaStream(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

MediaStream::~MediaStream() = default;

int MediaStream::read(std::uint8_t* buf, int size)
{
    return impl_->read(buf, size);
}

void MediaStream::cancel()
{
    impl_->cancel();
}

// ---------------------------------------------------------------------------
// TlsClient::Impl -- one shared, app-lifetime TLS context.
// ---------------------------------------------------------------------------
struct TlsClient::Impl {
    Impl(TlsOptions tls, std::shared_ptr<TlsSessionCache> sessionCache, std::shared_ptr<CookieJar> cookieJar)
        : tls_(std::move(tls))
        , sessionCache_(std::move(sessionCache))
        , cookieJar_(std::move(cookieJar))
        , context_(ssl::context::tls_client)
    {
        configureSslContext(context_, tls_);
        if (sessionCache_) {
            enableSessionCache(context_.native_handle());
        }
        if (!cookieJar_) {
            cookieJar_ = std::make_shared<CookieJar>();
        }
    }

    std::unique_ptr<MediaStream> open(const std::string& url, const std::atomic_bool& stop)
    {
        std::string currentUrl = url;
        for (int hop = 0; hop <= kMaxRedirects; ++hop) {
            if (stop.load()) {
                throw std::runtime_error("stream open aborted (stop requested)");
            }

            const ParsedUrl parsed = parseUrl(currentUrl);
            if (parsed.scheme != "https") {
                throw std::runtime_error("video stream URL must be https: " + currentUrl);
            }
            const std::string origin = originForUrl(parsed);

            // go2rtc websocket-MSE endpoint (Frigate's /live/mse/api/ws):
            // fMP4 over a websocket instead of an HTTP body. No redirects.
            if (isMseTarget(parsed.target)) {
                auto mse = std::make_unique<MseStreamImpl>(
                    context_, tls_.rwTimeoutUs, &stop, sessionCache_, cookieJar_);
                mse->connect(parsed, origin);
                return std::make_unique<MediaStream>(std::move(mse));
            }

            auto streamImpl = std::make_unique<HttpStreamImpl>(
                context_, tls_.rwTimeoutUs, &stop, sessionCache_, cookieJar_);
            const HopOutcome outcome = streamImpl->performRequest(parsed, origin);

            if (outcome.status >= 200 && outcome.status < 300) {
                return std::make_unique<MediaStream>(std::move(streamImpl));
            }
            if (isRedirectStatus(outcome.status) && !outcome.location.empty() && hop < kMaxRedirects) {
                currentUrl = resolveRedirectUrl(currentUrl, outcome.location);
                continue;
            }
            throw std::runtime_error(
                "stream GET " + currentUrl + " -> " + std::to_string(outcome.status)
                + (outcome.location.empty() ? "" : " (Location: " + outcome.location + ")"));
        }
        throw std::runtime_error("too many redirects opening stream: " + url);
    }

    TlsOptions tls_;
    std::shared_ptr<TlsSessionCache> sessionCache_;
    std::shared_ptr<CookieJar> cookieJar_;
    ssl::context context_;
};

TlsClient::TlsClient(
    TlsOptions tls,
    std::shared_ptr<TlsSessionCache> sessionCache,
    std::shared_ptr<CookieJar> cookieJar)
    : impl_(std::make_unique<Impl>(std::move(tls), std::move(sessionCache), std::move(cookieJar)))
{
}

TlsClient::~TlsClient() = default;

std::unique_ptr<MediaStream> TlsClient::open(const std::string& url, const std::atomic_bool& stopFlag)
{
    return impl_->open(url, stopFlag);
}

} // namespace gig
