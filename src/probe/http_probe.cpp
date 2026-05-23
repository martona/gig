#include "probe/http_probe.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

enum class EndpointKind {
    FrigateConfig,
    FrigateGo2rtcStreams,
    FrigateGo2rtcApiStreams,
    RawGo2rtcStreams,
    Other,
};

struct ProbeEndpoint {
    EndpointKind kind = EndpointKind::Other;
    std::string label;
    std::string pathAndQuery;
    std::size_t maxBytes = 0;
    bool partialBodyOk = false;
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};

struct FetchResult {
    bool ok = false;
    bool truncated = false;
    unsigned status = 0;
    int redirects = 0;
    std::string reason;
    std::string error;
    std::string finalUrl;
    std::string redirectLocation;
    std::vector<std::string> setCookieNames;
    std::string body;
};

using CookieJar = std::map<std::string, std::map<std::string, std::string>>;

struct StreamStatus {
    bool present = false;
    std::size_t producers = 0;
    std::size_t consumers = 0;
    std::uint64_t bytesReceived = 0;
    bool hasVideo = false;
    std::set<std::string> codecs;
};

std::string trimTrailingSlashes(std::string value)
{
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string joinUrl(const std::string& baseUrl, const std::string& pathAndQuery)
{
    const std::string base = trimTrailingSlashes(baseUrl);
    if (pathAndQuery.empty()) {
        return base;
    }
    if (pathAndQuery.front() == '/') {
        return base + pathAndQuery;
    }
    return base + "/" + pathAndQuery;
}

std::string toLower(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

ParsedUrl parseUrl(const std::string& url)
{
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        throw std::runtime_error("URL must include http:// or https://");
    }

    ParsedUrl parsed;
    parsed.scheme = toLower(url.substr(0, schemeEnd));
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        throw std::runtime_error("probe only supports http:// and https:// URLs");
    }

    const std::size_t authorityStart = schemeEnd + 3;
    const std::size_t pathStart = url.find('/', authorityStart);
    std::string authority = pathStart == std::string::npos
        ? url.substr(authorityStart)
        : url.substr(authorityStart, pathStart - authorityStart);
    parsed.target = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    if (parsed.target.empty()) {
        parsed.target = "/";
    }

    if (authority.empty()) {
        throw std::runtime_error("URL host is empty");
    }

    if (authority.front() == '[') {
        const std::size_t bracket = authority.find(']');
        if (bracket == std::string::npos) {
            throw std::runtime_error("IPv6 URL host is missing closing bracket");
        }
        parsed.host = authority.substr(1, bracket - 1);
        if (bracket + 1 < authority.size() && authority[bracket + 1] == ':') {
            parsed.port = authority.substr(bracket + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            parsed.host = authority.substr(0, colon);
            parsed.port = authority.substr(colon + 1);
        } else {
            parsed.host = authority;
        }
    }

    if (parsed.host.empty()) {
        throw std::runtime_error("URL host is empty");
    }
    if (parsed.port.empty()) {
        parsed.port = parsed.scheme == "https" ? "443" : "80";
    }
    return parsed;
}

std::string hostHeader(const ParsedUrl& url)
{
    const bool defaultPort =
        (url.scheme == "https" && url.port == "443")
        || (url.scheme == "http" && url.port == "80");
    return defaultPort ? url.host : url.host + ":" + url.port;
}

std::string originForUrl(const ParsedUrl& url)
{
    const bool defaultPort =
        (url.scheme == "https" && url.port == "443")
        || (url.scheme == "http" && url.port == "80");
    return url.scheme + "://" + url.host + (defaultPort ? "" : ":" + url.port);
}

std::string pathDirectory(const std::string& target)
{
    const std::size_t query = target.find('?');
    const std::string path = query == std::string::npos ? target : target.substr(0, query);
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return "/";
    }
    return path.substr(0, slash + 1);
}

std::string resolveRedirectUrl(const std::string& currentUrl, const std::string& location)
{
    if (location.find("://") != std::string::npos) {
        return location;
    }

    const ParsedUrl current = parseUrl(currentUrl);
    const std::string origin = originForUrl(current);
    if (location.starts_with("//")) {
        return current.scheme + ":" + location;
    }
    if (location.starts_with("/")) {
        return origin + location;
    }
    return origin + pathDirectory(current.target) + location;
}

bool isRedirectStatus(unsigned status)
{
    return status == 301
        || status == 302
        || status == 303
        || status == 307
        || status == 308;
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

std::optional<std::pair<std::string, std::string>> parseSetCookie(std::string_view header)
{
    const std::size_t semicolon = header.find(';');
    std::string_view nameValue = semicolon == std::string_view::npos
        ? header
        : header.substr(0, semicolon);
    nameValue = trimAsciiWhitespace(nameValue);

    const std::size_t equals = nameValue.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view name = trimAsciiWhitespace(nameValue.substr(0, equals));
    std::string_view value = trimAsciiWhitespace(nameValue.substr(equals + 1));
    if (name.empty()) {
        return std::nullopt;
    }

    return std::make_pair(std::string(name), std::string(value));
}

void storeSetCookie(CookieJar& cookies, const std::string& origin, std::string_view header, std::vector<std::string>& names)
{
    const std::optional<std::pair<std::string, std::string>> parsed = parseSetCookie(header);
    if (!parsed) {
        return;
    }

    cookies[origin][parsed->first] = parsed->second;
    if (std::find(names.begin(), names.end(), parsed->first) == names.end()) {
        names.push_back(parsed->first);
    }
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

void appendCookieNames(std::vector<std::string>& target, const std::vector<std::string>& source)
{
    for (const std::string& name : source) {
        if (std::find(target.begin(), target.end(), name) == target.end()) {
            target.push_back(name);
        }
    }
}

std::string joinNames(const std::vector<std::string>& names)
{
    std::ostringstream output;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            output << ", ";
        }
        output << names[i];
    }
    return output.str();
}

std::vector<std::string> storedCookieNames(const CookieJar& cookies)
{
    std::vector<std::string> names;
    for (const auto& [origin, originCookies] : cookies) {
        (void)origin;
        for (const auto& [name, value] : originCookies) {
            (void)value;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    }
    return names;
}

bool isUnreserved(unsigned char value)
{
    return std::isalnum(value) || value == '-' || value == '_' || value == '.' || value == '~';
}

std::string urlEncode(std::string_view value)
{
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (unsigned char ch : value) {
        if (isUnreserved(ch)) {
            output << static_cast<char>(ch);
        } else {
            output << '%' << std::setw(2) << static_cast<int>(ch);
        }
    }
    return output.str();
}

bool looksBinary(std::string_view body)
{
    const std::size_t scanBytes = std::min<std::size_t>(body.size(), 512);
    for (std::size_t i = 0; i < scanBytes; ++i) {
        const unsigned char ch = static_cast<unsigned char>(body[i]);
        if (ch == 0) {
            return true;
        }
        if (ch < 0x08 || (ch > 0x0D && ch < 0x20)) {
            return true;
        }
    }
    return false;
}

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

std::string normalizedPath(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    std::error_code error;
    std::filesystem::path fsPath(path);
    std::filesystem::path absolute = std::filesystem::absolute(fsPath, error);
    if (error) {
        return path;
    }
    return absolute.string();
}

std::string requireReadableFile(const std::string& path, std::string_view label)
{
    if (path.empty()) {
        return {};
    }

    const std::string absolute = normalizedPath(path);
    std::error_code error;
    const bool exists = std::filesystem::exists(absolute, error);
    if (error || !exists) {
        throw std::runtime_error(std::string(label) + " file does not exist: " + absolute);
    }
    if (!std::filesystem::is_regular_file(absolute, error) || error) {
        throw std::runtime_error(std::string(label) + " path is not a file: " + absolute);
    }
    return absolute;
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

void configureSslStreamCertificate(SSL* sslHandle, const TlsOptions& tls)
{
    if (!tls.certFile.empty()) {
        const std::string certFile = requireReadableFile(tls.certFile, "client certificate");
        if (SSL_use_certificate_chain_file(sslHandle, certFile.c_str()) != 1) {
            throw std::runtime_error("failed to attach client certificate to TLS stream: " + opensslErrorString());
        }
    }
    if (!tls.keyFile.empty()) {
        const std::string keyFile = requireReadableFile(tls.keyFile, "client private key");
        if (SSL_use_PrivateKey_file(sslHandle, keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw std::runtime_error("failed to attach client private key to TLS stream: " + opensslErrorString());
        }
    }
    if (!tls.certFile.empty() || !tls.keyFile.empty()) {
        if (SSL_check_private_key(sslHandle) != 1) {
            throw std::runtime_error("TLS stream certificate/private key mismatch: " + opensslErrorString());
        }
    }
}

void printTlsSummary(const TlsOptions& tls)
{
    std::cout << "TLS verify=" << (tls.verifyServer ? "on" : "off");
    if (!tls.caFile.empty()) {
        std::cout << " ca=" << normalizedPath(tls.caFile);
    }
    if (!tls.certFile.empty()) {
        std::cout << " cert=" << normalizedPath(tls.certFile);
    }
    if (!tls.keyFile.empty()) {
        std::cout << " key=" << normalizedPath(tls.keyFile);
    }
    std::cout << "\n";
}

template <typename Stream>
void applyTimeout(Stream& stream, const TlsOptions& tls)
{
    const auto timeout = std::chrono::microseconds(
        tls.rwTimeoutUs > 0 ? tls.rwTimeoutUs : 10'000'000);
    beast::get_lowest_layer(stream).expires_after(timeout);
}

template <typename Stream>
FetchResult readResponse(
    Stream& stream,
    beast::flat_buffer& buffer,
    const ProbeEndpoint& endpoint,
    CookieJar& cookies,
    const std::string& origin)
{
    FetchResult result;

    http::response_parser<http::dynamic_body> parser;
    const std::uint64_t bodyLimit = endpoint.partialBodyOk
        ? std::max<std::uint64_t>(64 * 1024, static_cast<std::uint64_t>(endpoint.maxBytes) * 4)
        : static_cast<std::uint64_t>(endpoint.maxBytes);
    parser.body_limit(bodyLimit);

    boost::system::error_code error;
    http::read_header(stream, buffer, parser, error);
    if (error) {
        result.error = error.message();
        return result;
    }

    while (!parser.is_done()) {
        const std::size_t bodySize = parser.get().body().size();
        if (bodySize >= endpoint.maxBytes) {
            result.truncated = true;
            break;
        }

        http::read_some(stream, buffer, parser, error);
        if (error == http::error::need_buffer) {
            continue;
        }
        if (error == asio::error::eof) {
            break;
        }
        if (error) {
            result.error = error.message();
            return result;
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
            storeSetCookie(cookies, origin, std::string_view(field.value().data(), field.value().size()), result.setCookieNames);
        }
    }
    result.body = beast::buffers_to_string(response.body().data());
    if (result.body.size() > endpoint.maxBytes) {
        result.body.resize(endpoint.maxBytes);
        result.truncated = true;
    }
    result.ok = result.status >= 200 && result.status < 300;
    if (!result.ok) {
        result.error = std::to_string(result.status) + " " + result.reason;
    }
    return result;
}

FetchResult fetchUrlOnce(const std::string& url, const TlsOptions& tls, const ProbeEndpoint& endpoint, CookieJar& cookies)
{
    try {
        const ParsedUrl parsed = parseUrl(url);
        const std::string origin = originForUrl(parsed);
        asio::io_context io;
        tcp::resolver resolver(io);
        const auto results = resolver.resolve(parsed.host, parsed.port);

        http::request<http::empty_body> request { http::verb::get, parsed.target, 11 };
        request.set(http::field::host, hostHeader(parsed));
        request.set(http::field::user_agent, "frigate-d3d-poc probe");
        request.set(http::field::accept, "*/*");
        const std::string cookiesForOrigin = cookieHeader(cookies, origin);
        if (!cookiesForOrigin.empty()) {
            request.set(http::field::cookie, cookiesForOrigin);
        }

        beast::flat_buffer buffer;
        if (parsed.scheme == "https") {
            ssl::context sslContext(ssl::context::tls_client);
            configureSslContext(sslContext, tls);

            beast::ssl_stream<beast::tcp_stream> stream(io, sslContext);
            configureSslStreamCertificate(stream.native_handle(), tls);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
                throw std::runtime_error("failed to set TLS SNI host name");
            }

            applyTimeout(stream, tls);
            beast::get_lowest_layer(stream).connect(results);
            applyTimeout(stream, tls);
            stream.handshake(ssl::stream_base::client);
            applyTimeout(stream, tls);
            http::write(stream, request);
            applyTimeout(stream, tls);
            FetchResult result = readResponse(stream, buffer, endpoint, cookies, origin);
            result.finalUrl = url;

            boost::system::error_code shutdownError;
            stream.shutdown(shutdownError);
            if (shutdownError == asio::error::eof || shutdownError == ssl::error::stream_truncated) {
                shutdownError = {};
            }
            return result;
        }

        beast::tcp_stream stream(io);
        applyTimeout(stream, tls);
        stream.connect(results);
        applyTimeout(stream, tls);
        http::write(stream, request);
        applyTimeout(stream, tls);
        FetchResult result = readResponse(stream, buffer, endpoint, cookies, origin);
        result.finalUrl = url;

        boost::system::error_code shutdownError;
        stream.socket().shutdown(tcp::socket::shutdown_both, shutdownError);
        return result;
    } catch (const std::exception& error) {
        FetchResult result;
        result.error = error.what();
        result.finalUrl = url;
        return result;
    }
}

FetchResult fetchUrl(const std::string& url, const TlsOptions& tls, const ProbeEndpoint& endpoint, CookieJar& cookies)
{
    constexpr int MaxRedirects = 8;
    std::string currentUrl = url;
    std::vector<std::string> cookieNames;

    for (int redirect = 0; redirect <= MaxRedirects; ++redirect) {
        FetchResult result = fetchUrlOnce(currentUrl, tls, endpoint, cookies);
        appendCookieNames(cookieNames, result.setCookieNames);
        result.setCookieNames = cookieNames;
        result.redirects = redirect;
        if (!isRedirectStatus(result.status)) {
            return result;
        }

        if (result.redirectLocation.empty()) {
            result.ok = false;
            result.error = std::to_string(result.status) + " " + result.reason + " without Location header";
            return result;
        }

        if (redirect == MaxRedirects) {
            result.ok = false;
            result.error = "too many redirects; last Location was " + result.redirectLocation;
            return result;
        }

        currentUrl = resolveRedirectUrl(currentUrl, result.redirectLocation);
    }

    FetchResult result;
    result.redirects = MaxRedirects;
    result.error = "too many redirects";
    result.finalUrl = currentUrl;
    result.setCookieNames = std::move(cookieNames);
    return result;
}

std::vector<ProbeEndpoint> buildEndpoints(const ProbeOptions& options)
{
    std::vector<ProbeEndpoint> endpoints = {
        { EndpointKind::FrigateConfig, "frigate config", "/api/config", options.maxBytes },
        { EndpointKind::FrigateGo2rtcStreams, "frigate go2rtc streams", "/api/go2rtc/streams", options.maxBytes },
        { EndpointKind::FrigateGo2rtcApiStreams, "frigate go2rtc api/streams proxy", "/api/go2rtc/api/streams", options.maxBytes },
        { EndpointKind::RawGo2rtcStreams, "raw go2rtc streams", "/api/streams", options.maxBytes },
    };

    if (!options.streamName.empty()) {
        const std::string pathName = urlEncode(options.streamName);
        const std::string queryName = urlEncode(options.streamName);
        endpoints.push_back({ EndpointKind::Other, "frigate go2rtc stream by name", "/api/go2rtc/streams/" + pathName, options.maxBytes });
        endpoints.push_back({ EndpointKind::Other, "frigate go2rtc api/streams by src", "/api/go2rtc/api/streams?src=" + queryName, options.maxBytes });
        endpoints.push_back({ EndpointKind::Other, "raw go2rtc streams by src", "/api/streams?src=" + queryName, options.maxBytes });

        if (options.checkStreams) {
            const std::size_t streamBytes = std::min<std::size_t>(options.maxBytes, 1880);
            endpoints.push_back({ EndpointKind::Other, "frigate go2rtc api/stream.ts bytes", "/api/go2rtc/api/stream.ts?src=" + queryName, streamBytes, true });
            endpoints.push_back({ EndpointKind::Other, "raw go2rtc stream.ts bytes", "/api/stream.ts?src=" + queryName, streamBytes, true });
        }
    }

    for (const std::string& endpoint : options.extraEndpoints) {
        endpoints.push_back({ EndpointKind::Other, "custom " + endpoint, endpoint, options.maxBytes });
    }

    return endpoints;
}

std::optional<json::value> parseJsonBody(const FetchResult& result, std::string_view label)
{
    if (!result.ok || result.body.empty() || looksBinary(result.body)) {
        return std::nullopt;
    }

    boost::system::error_code error;
    json::value parsed = json::parse(result.body, error);
    if (error) {
        std::cout << "JSON parse skipped for " << label << ": " << error.message() << "\n";
        return std::nullopt;
    }
    return parsed;
}

std::string jsonKeyToString(json::string_view key)
{
    return std::string(key.data(), key.size());
}

std::string jsonString(const json::value* value)
{
    if (!value) {
        return {};
    }
    if (const json::string* string = value->if_string()) {
        return std::string(*string);
    }
    if (const auto* integer = value->if_int64()) {
        return std::to_string(*integer);
    }
    if (const auto* integer = value->if_uint64()) {
        return std::to_string(*integer);
    }
    return {};
}

std::uint64_t jsonUInt(const json::value* value)
{
    if (!value) {
        return 0;
    }
    if (const auto* integer = value->if_uint64()) {
        return *integer;
    }
    if (const auto* integer = value->if_int64()) {
        return *integer > 0 ? static_cast<std::uint64_t>(*integer) : 0;
    }
    return 0;
}

const json::object* objectField(const json::object& object, std::string_view name)
{
    const json::value* value = object.if_contains(json::string_view(name.data(), name.size()));
    return value ? value->if_object() : nullptr;
}

const json::array* arrayField(const json::object& object, std::string_view name)
{
    const json::value* value = object.if_contains(json::string_view(name.data(), name.size()));
    return value ? value->if_array() : nullptr;
}

std::vector<std::string> configuredGo2rtcStreams(const json::object* config)
{
    std::vector<std::string> streams;
    if (!config) {
        return streams;
    }

    const json::object* go2rtc = objectField(*config, "go2rtc");
    const json::object* streamObject = go2rtc ? objectField(*go2rtc, "streams") : nullptr;
    if (!streamObject) {
        return streams;
    }

    for (const auto& item : *streamObject) {
        streams.push_back(jsonKeyToString(item.key()));
    }
    std::sort(streams.begin(), streams.end());
    return streams;
}

std::vector<std::string> cameraInputPaths(const json::object& camera)
{
    std::vector<std::string> paths;
    const json::object* ffmpeg = objectField(camera, "ffmpeg");
    const json::array* inputs = ffmpeg ? arrayField(*ffmpeg, "inputs") : nullptr;
    if (!inputs) {
        return paths;
    }

    for (const json::value& inputValue : *inputs) {
        const json::object* input = inputValue.if_object();
        if (!input) {
            continue;
        }
        std::string path = jsonString(input->if_contains("path"));
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

std::optional<std::string> localGo2rtcStreamFromInput(
    std::string_view inputPath,
    const std::set<std::string>& configuredStreams)
{
    const std::string input(inputPath);
    const std::size_t schemeEnd = input.find("://");
    if (schemeEnd == std::string::npos || toLower(input.substr(0, schemeEnd)) != "rtsp") {
        return std::nullopt;
    }

    const std::size_t authorityStart = schemeEnd + 3;
    const std::size_t pathStart = input.find('/', authorityStart);
    if (pathStart == std::string::npos) {
        return std::nullopt;
    }

    std::string authority = input.substr(authorityStart, pathStart - authorityStart);
    const std::size_t userInfoEnd = authority.rfind('@');
    if (userInfoEnd != std::string::npos) {
        authority = authority.substr(userInfoEnd + 1);
    }

    std::string host;
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t bracket = authority.find(']');
        if (bracket == std::string::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, bracket - 1);
        if (bracket + 1 < authority.size() && authority[bracket + 1] == ':') {
            port = authority.substr(bracket + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            host = authority;
        } else {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
    }

    const std::string lowerHost = toLower(host);
    const bool loopback = lowerHost == "localhost"
        || lowerHost == "127.0.0.1"
        || lowerHost == "::1";
    if (!loopback || port != "8554") {
        return std::nullopt;
    }

    std::string stream = input.substr(pathStart + 1);
    const std::size_t end = stream.find_first_of("?#");
    if (end != std::string::npos) {
        stream.resize(end);
    }
    while (!stream.empty() && stream.back() == '/') {
        stream.pop_back();
    }

    if (stream.empty() || !configuredStreams.contains(stream)) {
        return std::nullopt;
    }
    return stream;
}

std::string streamNameFromValue(const json::value& value)
{
    if (const json::string* string = value.if_string()) {
        return std::string(*string);
    }

    const json::object* object = value.if_object();
    if (!object) {
        return {};
    }

    for (std::string_view key : { "stream_name", "stream", "name", "src" }) {
        std::string stream = jsonString(object->if_contains(json::string_view(key.data(), key.size())));
        if (!stream.empty()) {
            return stream;
        }
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> cameraLiveStreams(
    const json::object& camera,
    const std::vector<std::string>& inputPaths,
    const std::set<std::string>& configuredStreams)
{
    std::vector<std::pair<std::string, std::string>> streams;
    std::vector<std::string> declaredLabels;
    const json::object* live = objectField(camera, "live");
    const json::value* liveStreams = live ? live->if_contains("streams") : nullptr;

    if (const json::object* liveObject = liveStreams ? liveStreams->if_object() : nullptr) {
        for (const auto& item : *liveObject) {
            const std::string label = jsonKeyToString(item.key());
            declaredLabels.push_back(label);
            std::string stream = streamNameFromValue(item.value());
            if (stream.empty()) {
                stream = label;
            }
            if (configuredStreams.contains(stream)) {
                streams.emplace_back(label, std::move(stream));
            }
        }
    } else if (const json::array* liveArray = liveStreams ? liveStreams->if_array() : nullptr) {
        int index = 0;
        for (const json::value& value : *liveArray) {
            std::string label = "stream " + std::to_string(++index);
            declaredLabels.push_back(label);
            std::string stream = streamNameFromValue(value);
            if (!stream.empty() && configuredStreams.contains(stream)) {
                streams.emplace_back(std::move(label), std::move(stream));
            }
        }
    }

    if (!streams.empty()) {
        return streams;
    }

    std::set<std::string> seenStreams;
    for (const std::string& input : inputPaths) {
        std::optional<std::string> stream = localGo2rtcStreamFromInput(input, configuredStreams);
        if (!stream || seenStreams.contains(*stream)) {
            continue;
        }

        const std::size_t mappedIndex = seenStreams.size();
        std::string label = mappedIndex < declaredLabels.size()
            ? declaredLabels[mappedIndex]
            : "input " + std::to_string(mappedIndex + 1);
        seenStreams.insert(*stream);
        streams.emplace_back(std::move(label), std::move(*stream));
    }
    return streams;
}

void addCodecsFromReceiver(const json::object& receiver, StreamStatus& status)
{
    const json::object* codec = objectField(receiver, "codec");
    if (!codec) {
        return;
    }

    std::string codecType = jsonString(codec->if_contains("codec_type"));
    std::string codecName = jsonString(codec->if_contains("codec_name"));
    if (!codecName.empty()) {
        status.codecs.insert(codecName);
    }
    if (codecType == "video") {
        status.hasVideo = true;
    }
}

std::map<std::string, StreamStatus> parseStreamStatuses(const json::object* streams)
{
    std::map<std::string, StreamStatus> statuses;
    if (!streams) {
        return statuses;
    }

    for (const auto& item : *streams) {
        const json::object* stream = item.value().if_object();
        if (!stream) {
            continue;
        }

        StreamStatus status;
        status.present = true;

        if (const json::array* producers = arrayField(*stream, "producers")) {
            status.producers = producers->size();
            for (const json::value& producerValue : *producers) {
                const json::object* producer = producerValue.if_object();
                if (!producer) {
                    continue;
                }

                status.bytesReceived += jsonUInt(producer->if_contains("bytes_recv"));
                if (const json::array* medias = arrayField(*producer, "medias")) {
                    for (const json::value& media : *medias) {
                        const std::string text = jsonString(&media);
                        if (text.find("video") != std::string::npos) {
                            status.hasVideo = true;
                        }
                        if (text.find("H264") != std::string::npos) {
                            status.codecs.insert("H264");
                        } else if (text.find("H265") != std::string::npos || text.find("HEVC") != std::string::npos) {
                            status.codecs.insert("H265");
                        }
                    }
                }
                if (const json::array* receivers = arrayField(*producer, "receivers")) {
                    for (const json::value& receiverValue : *receivers) {
                        if (const json::object* receiver = receiverValue.if_object()) {
                            addCodecsFromReceiver(*receiver, status);
                        }
                    }
                }
            }
        }

        if (const json::array* consumers = arrayField(*stream, "consumers")) {
            status.consumers = consumers->size();
        }
        statuses.emplace(jsonKeyToString(item.key()), std::move(status));
    }

    return statuses;
}

std::string streamStatusText(const std::string& stream, const std::map<std::string, StreamStatus>& statuses)
{
    const auto found = statuses.find(stream);
    if (found == statuses.end()) {
        return "status=not reported";
    }

    const StreamStatus& status = found->second;
    std::ostringstream output;
    output << "producers=" << status.producers
           << " consumers=" << status.consumers;
    if (status.hasVideo) {
        output << " video=yes";
    }
    if (!status.codecs.empty()) {
        output << " codecs=";
        bool first = true;
        for (const std::string& codec : status.codecs) {
            if (!first) {
                output << ",";
            }
            output << codec;
            first = false;
        }
    }
    if (status.bytesReceived > 0) {
        output << " bytes_recv=" << status.bytesReceived;
    }
    return output.str();
}

void printDiscovery(
    const std::optional<json::value>& configValue,
    const std::optional<json::value>& streamsValue)
{
    const json::object* config = configValue ? configValue->if_object() : nullptr;
    const json::object* cameras = config ? objectField(*config, "cameras") : nullptr;
    const json::object* streamObject = streamsValue ? streamsValue->if_object() : nullptr;
    const std::vector<std::string> configuredStreams = configuredGo2rtcStreams(config);
    const std::set<std::string> configuredStreamSet(configuredStreams.begin(), configuredStreams.end());
    const std::map<std::string, StreamStatus> statuses = parseStreamStatuses(streamObject);
    std::set<std::string> mappedStreams;

    std::cout << "\n== Parsed Discovery\n";
    if (cameras) {
        std::cout << "Frigate cameras: " << cameras->size() << "\n";
        for (const auto& item : *cameras) {
            const std::string cameraName = jsonKeyToString(item.key());
            const json::object* camera = item.value().if_object();
            if (!camera) {
                continue;
            }

            std::cout << "\n" << cameraName << "\n";
            const std::vector<std::string> inputs = cameraInputPaths(*camera);
            for (const std::string& input : inputs) {
                std::cout << "  input: " << input << "\n";
            }

            const auto liveStreams = cameraLiveStreams(*camera, inputs, configuredStreamSet);
            if (liveStreams.empty()) {
                std::cout << "  streams: none declared\n";
            } else {
                std::cout << "  streams:\n";
                for (const auto& [label, stream] : liveStreams) {
                    mappedStreams.insert(stream);
                    std::cout << "    " << label << " -> " << stream
                              << " [" << streamStatusText(stream, statuses) << "]\n";
                }
            }
        }
    } else {
        std::cout << "Frigate cameras: not found in /api/config\n";
    }

    if (!configuredStreams.empty()) {
        std::cout << "\nConfigured go2rtc streams: " << configuredStreams.size() << "\n";
        for (const std::string& stream : configuredStreams) {
            const bool mapped = mappedStreams.contains(stream);
            std::cout << "  " << stream
                      << " [" << streamStatusText(stream, statuses) << "]"
                      << (mapped ? "" : " unmapped")
                      << "\n";
        }
    } else if (!statuses.empty()) {
        std::cout << "\ngo2rtc streams from status API: " << statuses.size() << "\n";
        for (const auto& [stream, status] : statuses) {
            std::cout << "  " << stream << " [" << streamStatusText(stream, statuses) << "]\n";
        }
    }
}

void printResult(const ProbeEndpoint& endpoint, const std::string& url, const FetchResult& result, bool dumpBody)
{
    std::cout << "\n== " << endpoint.label << "\n"
              << "GET " << url << "\n";
    if (result.redirects > 0) {
        std::cout << "redirects=" << result.redirects << " final=" << result.finalUrl << "\n";
    }
    if (!result.setCookieNames.empty()) {
        std::cout << "set-cookie=" << joinNames(result.setCookieNames) << "\n";
    }

    if (!result.ok) {
        std::cout << "FAIL " << result.error << "\n";
        if (!result.body.empty()) {
            std::cout << "body-bytes " << result.body.size() << "\n";
        }
        return;
    }

    std::cout << "OK http=" << result.status << " bytes=" << result.body.size();
    if (result.truncated) {
        std::cout << " truncated-at=" << endpoint.maxBytes;
    }
    if (looksBinary(result.body)) {
        std::cout << " binary";
    } else {
        boost::system::error_code error;
        json::parse(result.body, error);
        if (!error) {
            std::cout << " json";
        }
    }
    std::cout << "\n";

    if (!dumpBody || result.body.empty()) {
        return;
    }

    if (looksBinary(result.body)) {
        const std::size_t bytes = std::min<std::size_t>(result.body.size(), 32);
        std::cout << "hex-preview";
        for (std::size_t i = 0; i < bytes; ++i) {
            std::cout << ' ' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(static_cast<unsigned char>(result.body[i]));
        }
        std::cout << std::dec << std::setfill(' ') << "\n";
        return;
    }

    std::cout << result.body << "\n";
}

} // namespace

int runProbe(const ProbeOptions& options)
{
    if (options.baseUrl.empty()) {
        throw std::runtime_error("probe requires --base URL or a positional URL.");
    }

    std::cout << "Probe base: " << trimTrailingSlashes(options.baseUrl) << "\n";
    printTlsSummary(options.tls);
    if (!options.streamName.empty()) {
        std::cout << "Probe stream: " << options.streamName << "\n";
    }

    int successes = 0;
    int failures = 0;
    std::optional<json::value> configJson;
    std::optional<json::value> frigateStreamsJson;
    std::optional<json::value> go2rtcApiStreamsJson;
    std::optional<json::value> rawStreamsJson;
    CookieJar cookies;

    for (const ProbeEndpoint& endpoint : buildEndpoints(options)) {
        const std::string url = joinUrl(options.baseUrl, endpoint.pathAndQuery);
        const FetchResult result = fetchUrl(url, options.tls, endpoint, cookies);
        printResult(endpoint, url, result, options.dumpBody);
        if (!result.ok) {
            ++failures;
            continue;
        }

        ++successes;
        std::optional<json::value> parsed = parseJsonBody(result, endpoint.label);
        if (!parsed) {
            continue;
        }

        switch (endpoint.kind) {
        case EndpointKind::FrigateConfig:
            configJson = std::move(parsed);
            break;
        case EndpointKind::FrigateGo2rtcStreams:
            frigateStreamsJson = std::move(parsed);
            break;
        case EndpointKind::FrigateGo2rtcApiStreams:
            go2rtcApiStreamsJson = std::move(parsed);
            break;
        case EndpointKind::RawGo2rtcStreams:
            rawStreamsJson = std::move(parsed);
            break;
        case EndpointKind::Other:
            break;
        }
    }

    const std::optional<json::value>* bestStreams = &rawStreamsJson;
    if (go2rtcApiStreamsJson) {
        bestStreams = &go2rtcApiStreamsJson;
    }
    if (frigateStreamsJson) {
        bestStreams = &frigateStreamsJson;
    }
    printDiscovery(configJson, *bestStreams);

    const std::vector<std::string> cookiesStored = storedCookieNames(cookies);
    if (!cookiesStored.empty()) {
        std::cout << "\nCookies stored: " << joinNames(cookiesStored) << "\n";
    }

    std::cout << "\nProbe complete: " << successes << " OK, " << failures << " failed endpoint(s).\n";
    return successes > 0 ? 0 : 2;
}
