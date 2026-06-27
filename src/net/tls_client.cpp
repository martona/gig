#include "net/tls_client.hpp"

#include "log.hpp"
#include "net/cert_pin.hpp"
#include "net/cookie_jar.hpp"
#include "net/tls_context.hpp"
#include "net/tls_session_cache.hpp"
#include "net/url.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/optional.hpp>
#include <openssl/ssl.h>

namespace gig {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

constexpr int kMaxRedirects = 4;

struct HopOutcome {
    unsigned status = 0;
    std::string location;
};

} // namespace

// ---------------------------------------------------------------------------
// MediaStream::Impl -- one streaming connection (its own io_context + stream).
// ---------------------------------------------------------------------------
struct MediaStream::Impl {
    Impl(ssl::context& context,
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

    int read(std::uint8_t* buf, int size)
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

    void cancel()
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

            auto streamImpl = std::make_unique<MediaStream::Impl>(
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
