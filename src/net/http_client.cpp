#include "net/http_client.hpp"

#include "log.hpp"
#include "net/cookie_jar.hpp"
#include "net/tls_context.hpp"
#include "net/url.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

namespace gig {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

struct HopResult {
    bool ok = false;
    bool truncated = false;
    unsigned status = 0;
    std::string reason;
    std::string error;
    std::string redirectLocation;
    std::string finalUrl;
    std::string body;
};

template <typename Stream>
void applyTimeout(Stream& stream, std::int64_t rwTimeoutUs)
{
    const auto micros = std::chrono::microseconds(rwTimeoutUs > 0 ? rwTimeoutUs : 10'000'000);
    beast::get_lowest_layer(stream).expires_after(micros);
}

// Run exactly one async op to completion, bounded by the rw timeout. Beast's
// tcp_stream timer only fires for ASYNC operations (sync calls silently ignore
// expires_after), so every I/O step goes through here -- this is what makes
// rwTimeoutUs real. Same idiom as MediaStream::runOne in tls_client.cpp.
template <typename Stream, typename Initiate>
boost::system::error_code runStep(
    asio::io_context& io, Stream& stream, std::int64_t rwTimeoutUs, Initiate&& initiate)
{
    applyTimeout(stream, rwTimeoutUs);
    boost::system::error_code result;
    initiate([&result](boost::system::error_code ec) { result = ec; });
    io.restart();
    io.run();
    beast::get_lowest_layer(stream).expires_never();
    return result;
}

template <typename Stream>
void readResponse(
    asio::io_context& io,
    Stream& stream,
    std::int64_t rwTimeoutUs,
    beast::flat_buffer& buffer,
    std::size_t maxBytes,
    CookieJar& cookies,
    const std::string& origin,
    HopResult& result)
{
    http::response_parser<http::dynamic_body> parser;
    parser.body_limit(static_cast<std::uint64_t>(maxBytes));

    boost::system::error_code ec = runStep(io, stream, rwTimeoutUs, [&](auto&& complete) {
        http::async_read_header(
            stream, buffer, parser,
            [c = std::forward<decltype(complete)>(complete)](
                boost::system::error_code e, std::size_t) mutable { c(e); });
    });
    if (ec) {
        result.error = ec.message();
        return;
    }

    while (!parser.is_done()) {
        if (parser.get().body().size() >= maxBytes) {
            result.truncated = true;
            break;
        }
        ec = runStep(io, stream, rwTimeoutUs, [&](auto&& complete) {
            http::async_read_some(
                stream, buffer, parser,
                [c = std::forward<decltype(complete)>(complete)](
                    boost::system::error_code e, std::size_t) mutable { c(e); });
        });
        if (ec == http::error::need_buffer) {
            continue;
        }
        if (ec == asio::error::eof) {
            break;
        }
        if (ec) {
            result.error = ec.message();
            return;
        }
    }

    auto response = parser.release();
    result.status = response.result_int();
    result.reason = std::string(response.reason());
    const auto location = response.find(http::field::location);
    if (location != response.end()) {
        result.redirectLocation = std::string(location->value());
    }
    for (const auto& field : response.base()) {
        if (field.name() == http::field::set_cookie) {
            cookies.storeFromResponse(origin, std::string_view(field.value().data(), field.value().size()));
        }
    }
    result.body = beast::buffers_to_string(response.body().data());
    if (result.body.size() > maxBytes) {
        result.body.resize(maxBytes);
        result.truncated = true;
    }
    result.ok = result.status >= 200 && result.status < 300;
}

} // namespace

struct HttpClient::Impl {
    Impl(std::string baseUrl, TlsOptions tls, std::shared_ptr<TlsSessionCache> sessionCache,
         std::shared_ptr<CookieJar> cookieJar)
        : baseUrl(trimTrailingSlashes(std::move(baseUrl)))
        , tls(std::move(tls))
        , sessionCache(std::move(sessionCache))
        , cookieJar(std::move(cookieJar))
        , tlsContext(ssl::context::tls_client)
    {
        configureSslContext(tlsContext, this->tls);
        if (this->sessionCache) {
            enableSessionCache(tlsContext.native_handle());
        }
        if (!this->cookieJar) {
            this->cookieJar = std::make_shared<CookieJar>();
        }
    }

    HopResult fetchOnce(
        const std::string& url,
        std::size_t maxBytes,
        http::verb method = http::verb::get,
        const std::string& contentType = {},
        const std::string& body = {})
    {
        HopResult result;
        result.finalUrl = url;
        try {
            const ParsedUrl parsed = parseUrl(url);
            const std::string origin = originForUrl(parsed);

            asio::io_context io;
            tcp::resolver resolver(io);
            const auto endpoints = resolver.resolve(parsed.host, parsed.port);

            http::request<http::string_body> request { method, parsed.target, 11 };
            request.set(http::field::host, hostHeader(parsed));
            request.set(http::field::user_agent, "frigate-d3d-poc");
            request.set(http::field::accept, "*/*");
            if (!contentType.empty()) {
                request.set(http::field::content_type, contentType);
            }
            const std::string cookieValue = cookieJar->headerFor(origin);
            if (!cookieValue.empty()) {
                request.set(http::field::cookie, cookieValue);
            }
            request.body() = body;
            request.prepare_payload();

            beast::flat_buffer buffer;
            boost::system::error_code ec;
            if (parsed.scheme == "https") {
                beast::ssl_stream<beast::tcp_stream> stream(io, tlsContext);
                if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
                    throw std::runtime_error("failed to set TLS SNI host name");
                }
                if (sessionCache) {
                    offerCachedSession(stream.native_handle(), *sessionCache);
                }

                ec = runStep(io, stream, tls.rwTimeoutUs, [&](auto&& complete) {
                    beast::get_lowest_layer(stream).async_connect(
                        endpoints,
                        [c = std::forward<decltype(complete)>(complete)](
                            boost::system::error_code e, const tcp::endpoint&) mutable { c(e); });
                });
                if (ec) {
                    result.error = "connect " + parsed.host + ": " + ec.message();
                    return result;
                }

                ec = runStep(io, stream, tls.rwTimeoutUs, [&](auto&& complete) {
                    stream.async_handshake(
                        ssl::stream_base::client,
                        [c = std::forward<decltype(complete)>(complete)](
                            boost::system::error_code e) mutable { c(e); });
                });
                if (ec) {
                    result.error = "tls handshake " + parsed.host + ": " + ec.message();
                    return result;
                }
                if (sessionCache) {
                    logDebug() << "tls handshake host=" << parsed.host
                               << " reused=" << (sessionWasReused(stream.native_handle()) ? "yes" : "no")
                               << " cached=" << sessionCache->size();
                }

                ec = runStep(io, stream, tls.rwTimeoutUs, [&](auto&& complete) {
                    http::async_write(
                        stream, request,
                        [c = std::forward<decltype(complete)>(complete)](
                            boost::system::error_code e, std::size_t) mutable { c(e); });
                });
                if (ec) {
                    result.error = "write request " + parsed.host + ": " + ec.message();
                    return result;
                }

                readResponse(io, stream, tls.rwTimeoutUs, buffer, maxBytes, *cookieJar, origin, result);

                runStep(io, stream, tls.rwTimeoutUs, [&](auto&& complete) {
                    stream.async_shutdown(
                        [c = std::forward<decltype(complete)>(complete)](
                            boost::system::error_code e) mutable { c(e); });
                }); // best effort; bounded so a peer that never closes can't stall us
                return result;
            }

            beast::tcp_stream stream(io);
            ec = runStep(io, stream, tls.rwTimeoutUs, [&](auto&& complete) {
                stream.async_connect(
                    endpoints,
                    [c = std::forward<decltype(complete)>(complete)](
                        boost::system::error_code e, const tcp::endpoint&) mutable { c(e); });
            });
            if (ec) {
                result.error = "connect " + parsed.host + ": " + ec.message();
                return result;
            }

            ec = runStep(io, stream, tls.rwTimeoutUs, [&](auto&& complete) {
                http::async_write(
                    stream, request,
                    [c = std::forward<decltype(complete)>(complete)](
                        boost::system::error_code e, std::size_t) mutable { c(e); });
            });
            if (ec) {
                result.error = "write request " + parsed.host + ": " + ec.message();
                return result;
            }

            readResponse(io, stream, tls.rwTimeoutUs, buffer, maxBytes, *cookieJar, origin, result);

            boost::system::error_code shutdownEc;
            stream.socket().shutdown(tcp::socket::shutdown_both, shutdownEc);
            return result;
        } catch (const std::exception& error) {
            result.error = error.what();
            return result;
        }
    }

    HttpResponse get(const std::string& pathOrUrl, std::size_t maxBytes)
    {
        const std::string startUrl = pathOrUrl.find("://") != std::string::npos
            ? pathOrUrl
            : joinUrl(baseUrl, pathOrUrl);

        constexpr int MaxRedirects = 8;
        std::string currentUrl = startUrl;
        HttpResponse out;

        for (int redirect = 0; redirect <= MaxRedirects; ++redirect) {
            HopResult hop = fetchOnce(currentUrl, maxBytes);
            out.status = hop.status;
            out.reason = hop.reason;
            out.body = std::move(hop.body);
            out.truncated = hop.truncated;
            out.finalUrl = hop.finalUrl;
            out.error = hop.error;
            out.redirects = redirect;

            if (hop.status == 0) {
                out.ok = false;
                if (out.error.empty()) {
                    out.error = "request failed";
                }
                return out;
            }
            if (!isRedirectStatus(hop.status)) {
                out.ok = hop.ok;
                if (!out.ok && out.error.empty()) {
                    out.error = std::to_string(hop.status) + " " + hop.reason;
                }
                return out;
            }
            if (hop.redirectLocation.empty()) {
                out.ok = false;
                out.error = std::to_string(hop.status) + " " + hop.reason + " without Location header";
                return out;
            }
            if (redirect == MaxRedirects) {
                out.ok = false;
                out.error = "too many redirects; last Location was " + hop.redirectLocation;
                return out;
            }
            currentUrl = resolveRedirectUrl(currentUrl, hop.redirectLocation);
        }
        return out;
    }

    HttpResponse post(
        const std::string& pathOrUrl,
        const std::string& contentType,
        const std::string& body,
        std::size_t maxBytes)
    {
        const std::string url = pathOrUrl.find("://") != std::string::npos
            ? pathOrUrl
            : joinUrl(baseUrl, pathOrUrl);

        HopResult hop = fetchOnce(url, maxBytes, http::verb::post, contentType, body);

        HttpResponse out;
        out.ok = hop.ok;
        out.status = hop.status;
        out.reason = hop.reason;
        out.body = std::move(hop.body);
        out.truncated = hop.truncated;
        out.finalUrl = hop.finalUrl;
        out.error = hop.error;
        if (hop.status == 0) {
            out.ok = false;
            if (out.error.empty()) {
                out.error = "request failed";
            }
        } else if (isRedirectStatus(hop.status)) {
            // Following would replay toward a target the endpoint should not
            // have; surface the misconfiguration instead.
            out.ok = false;
            out.error = std::to_string(hop.status) + " " + hop.reason + " redirect to "
                + (hop.redirectLocation.empty() ? "<missing Location>" : hop.redirectLocation);
        } else if (!out.ok && out.error.empty()) {
            out.error = std::to_string(hop.status) + " " + hop.reason;
        }
        return out;
    }

    std::string baseUrl;
    TlsOptions tls;
    std::shared_ptr<TlsSessionCache> sessionCache;
    std::shared_ptr<CookieJar> cookieJar;
    ssl::context tlsContext;
};

HttpClient::HttpClient(
    std::string baseUrl,
    TlsOptions tls,
    std::shared_ptr<TlsSessionCache> sessionCache,
    std::shared_ptr<CookieJar> cookieJar)
    : impl_(std::make_unique<Impl>(
        std::move(baseUrl), std::move(tls), std::move(sessionCache), std::move(cookieJar)))
{
}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::get(const std::string& pathOrUrl, std::size_t maxBytes)
{
    return impl_->get(pathOrUrl, maxBytes);
}

HttpResponse HttpClient::post(
    const std::string& pathOrUrl,
    const std::string& contentType,
    const std::string& body,
    std::size_t maxBytes)
{
    return impl_->post(pathOrUrl, contentType, body, maxBytes);
}

const std::string& HttpClient::baseUrl() const
{
    return impl_->baseUrl;
}

} // namespace gig
