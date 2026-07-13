#include "net/cookie_jar.hpp"

#include <cctype>
#include <sstream>

namespace gig {
namespace {

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

} // namespace

void CookieJar::storeFromResponse(const std::string& origin, std::string_view header)
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

    std::lock_guard<std::mutex> lock(mutex_);
    cookies_[origin][std::string(name)] = std::string(value);
}

bool CookieJar::contains(const std::string& origin, const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = cookies_.find(origin);
    return found != cookies_.end() && found->second.count(name) != 0;
}

std::string CookieJar::headerFor(const std::string& origin) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = cookies_.find(origin);
    if (found == cookies_.end() || found->second.empty()) {
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

void CookieJar::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cookies_.clear();
}

} // namespace gig
