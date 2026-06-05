#include "net/http_client.hpp"

#include "log.hpp"
#include "net/url.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace gig {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

using CookieJar = std::map<std::string, std::map<std::string, std::string>>;

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

std::string opensslErrorString()
{
    const unsigned long error = ERR_get_error();
    if (error == 0) {
        return "unknown OpenSSL error";
    }
    char buffer[256] = {};
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return buffer;
}

std::string requireReadableFile(const std::string& path, std::string_view label)
{
    std::error_code ec;
    std::filesystem::path fsPath(path);
    std::filesystem::path absolute = std::filesystem::absolute(fsPath, ec);
    const std::string resolved = ec ? path : absolute.string();
    if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
        throw std::runtime_error(std::string(label) + " file is not readable: " + resolved);
    }
    return resolved;
}

void configureSslContext(ssl::context& context, const TlsOptions& tls)
{
    context.set_options(ssl::context::default_workarounds);
    if (tls.verifyServer) {
        context.set_verify_mode(ssl::verify_peer);
        if (!tls.caFile.empty()) {
            context.load_verify_file(requireReadableFile(tls.caFile, "CA certificate"));
        } else {
            context.set_default_verify_paths();
        }
    } else {
        context.set_verify_mode(ssl::verify_none);
    }

    if (!tls.certFile.empty()) {
        const std::string certFile = requireReadableFile(tls.certFile, "client certificate");
        if (SSL_CTX_use_certificate_chain_file(context.native_handle(), certFile.c_str()) != 1) {
            throw std::runtime_error("failed to load client certificate: " + opensslErrorString());
        }
    }
    if (!tls.keyFile.empty()) {
        const std::string keyFile = requireReadableFile(tls.keyFile, "client private key");
        if (SSL_CTX_use_PrivateKey_file(context.native_handle(), keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw std::runtime_error("failed to load client private key: " + opensslErrorString());
        }
    }
    if (!tls.certFile.empty() || !tls.keyFile.empty()) {
        if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
            throw std::runtime_error("client certificate/private key mismatch: " + opensslErrorString());
        }
    }
}

std::string_view trimAsciiWhitespace(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

void storeSetCookie(CookieJar& cookies, const std::string& origin, std::string_view header)
{
    const std::size_t semicolon = header.find(';');
    std::string_view nameValue = semicolon == std::string_view::npos ? header : header.substr(0, semicolon);
    nameValue = trimAsciiWhitespace(nameValue);

    const std::size_t equals = nameValue.find('=');
    if (equals == std::string_view::npos) {
        return;
    }
    std::string_view name = trimAsciiWhitespace(nameValue.substr(0, equals));
    std::string_view value = trimAsciiWhitespace(nameValue.substr(equals + 1));
    if (name.empty()) {
        return;
    }
    cookies[origin][std::string(name)] = std::string(value);
}

std::string cookieHeader(const CookieJar& cookies, const std::string& origin)
{
    const auto found = cookies.find(origin);
    if (found == cookies.end() || found->second.empty()) {
        return {};
    }

    std::ostringstream header;
    bool first = true;
    for (const auto& [name, value] : found->second) {
        if (!first) {
            header << "; ";
        }
        first = false;
        header << name << '=' << value;
    }
    return header.str();
}

template <typename Stream>
void applyTimeout(Stream& stream, std::int64_t rwTimeoutUs)
{
    const auto micros = std::chrono::microseconds(rwTimeoutUs > 0 ? rwTimeoutUs : 10'000'000);
    beast::get_lowest_layer(stream).expires_after(micros);
}

template <typename Stream>
void readResponse(
    Stream& stream,
    beast::flat_buffer& buffer,
    std::size_t maxBytes,
    CookieJar& cookies,
    const std::string& origin,
    HopResult& result)
{
    http::response_parser<http::dynamic_body> parser;
    parser.body_limit(static_cast<std::uint64_t>(maxBytes));

    boost::system::error_code ec;
    http::read_header(stream, buffer, parser, ec);
    if (ec) {
        result.error = ec.message();
        return;
    }

    while (!parser.is_done()) {
        if (parser.get().body().size() >= maxBytes) {
            result.truncated = true;
            break;
        }
        http::read_some(stream, buffer, parser, ec);
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
            storeSetCookie(cookies, origin, std::string_view(field.value().data(), field.value().size()));
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
    Impl(std::string baseUrl, TlsOptions tls, std::shared_ptr<TlsSessionCache> sessionCache)
        : baseUrl(trimTrailingSlashes(std::move(baseUrl)))
        , tls(std::move(tls))
        , sessionCache(std::move(sessionCache))
        , tlsContext(ssl::context::tls_client)
    {
        configureSslContext(tlsContext, this->tls);
        if (this->sessionCache) {
            enableSessionCache(tlsContext.native_handle());
        }
    }

    HopResult fetchOnce(const std::string& url, std::size_t maxBytes)
    {
        HopResult result;
        result.finalUrl = url;
        try {
            const ParsedUrl parsed = parseUrl(url);
            const std::string origin = originForUrl(parsed);

            asio::io_context io;
            tcp::resolver resolver(io);
            const auto endpoints = resolver.resolve(parsed.host, parsed.port);

            http::request<http::empty_body> request { http::verb::get, parsed.target, 11 };
            request.set(http::field::host, hostHeader(parsed));
            request.set(http::field::user_agent, "frigate-d3d-poc");
            request.set(http::field::accept, "*/*");
            const std::string cookieValue = cookieHeader(cookies, origin);
            if (!cookieValue.empty()) {
                request.set(http::field::cookie, cookieValue);
            }

            beast::flat_buffer buffer;
            if (parsed.scheme == "https") {
                beast::ssl_stream<beast::tcp_stream> stream(io, tlsContext);
                if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
                    throw std::runtime_error("failed to set TLS SNI host name");
                }
                if (sessionCache) {
                    offerCachedSession(stream.native_handle(), *sessionCache);
                }

                applyTimeout(stream, tls.rwTimeoutUs);
                beast::get_lowest_layer(stream).connect(endpoints);
                applyTimeout(stream, tls.rwTimeoutUs);
                stream.handshake(ssl::stream_base::client);
                if (sessionCache) {
                    logDebug() << "tls handshake host=" << parsed.host
                               << " reused=" << (sessionWasReused(stream.native_handle()) ? "yes" : "no")
                               << " cached=" << sessionCache->size();
                }

                applyTimeout(stream, tls.rwTimeoutUs);
                http::write(stream, request);
                applyTimeout(stream, tls.rwTimeoutUs);
                readResponse(stream, buffer, maxBytes, cookies, origin, result);

                boost::system::error_code shutdownEc;
                stream.shutdown(shutdownEc);
                return result;
            }

            beast::tcp_stream stream(io);
            applyTimeout(stream, tls.rwTimeoutUs);
            stream.connect(endpoints);
            applyTimeout(stream, tls.rwTimeoutUs);
            http::write(stream, request);
            applyTimeout(stream, tls.rwTimeoutUs);
            readResponse(stream, buffer, maxBytes, cookies, origin, result);

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

    std::string baseUrl;
    TlsOptions tls;
    std::shared_ptr<TlsSessionCache> sessionCache;
    ssl::context tlsContext;
    CookieJar cookies;
};

HttpClient::HttpClient(std::string baseUrl, TlsOptions tls, std::shared_ptr<TlsSessionCache> sessionCache)
    : impl_(std::make_unique<Impl>(std::move(baseUrl), std::move(tls), std::move(sessionCache)))
{
}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::get(const std::string& pathOrUrl, std::size_t maxBytes)
{
    return impl_->get(pathOrUrl, maxBytes);
}

const std::string& HttpClient::baseUrl() const
{
    return impl_->baseUrl;
}

} // namespace gig
