#pragma once

#include <cctype>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gig {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};

inline std::string toLowerAscii(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline std::string trimTrailingSlashes(std::string value)
{
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

inline ParsedUrl parseUrl(const std::string& url)
{
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        throw std::runtime_error("URL must include http:// or https://");
    }

    ParsedUrl parsed;
    parsed.scheme = toLowerAscii(url.substr(0, schemeEnd));
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        throw std::runtime_error("only http:// and https:// URLs are supported");
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

inline bool urlUsesDefaultPort(const ParsedUrl& url)
{
    return (url.scheme == "https" && url.port == "443")
        || (url.scheme == "http" && url.port == "80");
}

inline std::string hostHeader(const ParsedUrl& url)
{
    return urlUsesDefaultPort(url) ? url.host : url.host + ":" + url.port;
}

inline std::string originForUrl(const ParsedUrl& url)
{
    return url.scheme + "://" + url.host + (urlUsesDefaultPort(url) ? "" : ":" + url.port);
}

inline std::string joinUrl(const std::string& baseUrl, const std::string& pathAndQuery)
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

inline std::string pathDirectory(const std::string& target)
{
    const std::size_t query = target.find('?');
    const std::string path = query == std::string::npos ? target : target.substr(0, query);
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return "/";
    }
    return path.substr(0, slash + 1);
}

inline std::string resolveRedirectUrl(const std::string& currentUrl, const std::string& location)
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

inline bool isRedirectStatus(unsigned status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

inline bool isUnreservedUrlChar(unsigned char value)
{
    return std::isalnum(value) || value == '-' || value == '_' || value == '.' || value == '~';
}

inline std::string urlEncode(std::string_view value)
{
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (unsigned char ch : value) {
        if (isUnreservedUrlChar(ch)) {
            output << static_cast<char>(ch);
        } else {
            output << '%' << std::setw(2) << static_cast<int>(ch);
        }
    }
    return output.str();
}

} // namespace gig
