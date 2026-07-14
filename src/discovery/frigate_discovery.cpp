#include "discovery/frigate_discovery.h"

#include "log.hpp"
#include "net/url.h"

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <boost/json.hpp>

namespace gig {
namespace {

namespace json = boost::json;

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

std::set<std::string> configuredGo2rtcStreams(const json::object& config)
{
    std::set<std::string> streams;
    const json::object* go2rtc = objectField(config, "go2rtc");
    const json::object* streamObject = go2rtc ? objectField(*go2rtc, "streams") : nullptr;
    if (!streamObject) {
        return streams;
    }
    for (const auto& item : *streamObject) {
        streams.insert(jsonKeyToString(item.key()));
    }
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

// Map a Frigate ffmpeg input path of the form rtsp://localhost:8554/<stream>
// back to its go2rtc stream name, when it matches a configured stream.
std::optional<std::string> localGo2rtcStreamFromInput(
    std::string_view inputPath,
    const std::set<std::string>& configuredStreams)
{
    const std::string input(inputPath);
    const std::size_t schemeEnd = input.find("://");
    if (schemeEnd == std::string::npos || toLowerAscii(input.substr(0, schemeEnd)) != "rtsp") {
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

    const std::string lowerHost = toLowerAscii(host);
    const bool loopback = lowerHost == "localhost" || lowerHost == "127.0.0.1" || lowerHost == "::1";
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

// Resolve a camera's primary go2rtc stream: prefer the live.streams mapping,
// fall back to the loopback ffmpeg input. Returns empty if none is configured.
std::string primaryStreamForCamera(const json::object& camera, const std::set<std::string>& configuredStreams)
{
    const json::object* live = objectField(camera, "live");
    const json::value* liveStreams = live ? live->if_contains("streams") : nullptr;

    if (const json::object* liveObject = liveStreams ? liveStreams->if_object() : nullptr) {
        for (const auto& item : *liveObject) {
            std::string stream = streamNameFromValue(item.value());
            if (stream.empty()) {
                stream = jsonKeyToString(item.key());
            }
            if (configuredStreams.contains(stream)) {
                return stream;
            }
        }
    } else if (const json::array* liveArray = liveStreams ? liveStreams->if_array() : nullptr) {
        for (const json::value& value : *liveArray) {
            std::string stream = streamNameFromValue(value);
            if (!stream.empty() && configuredStreams.contains(stream)) {
                return stream;
            }
        }
    }

    for (const std::string& input : cameraInputPaths(camera)) {
        if (std::optional<std::string> stream = localGo2rtcStreamFromInput(input, configuredStreams)) {
            return *stream;
        }
    }
    return {};
}

std::string buildStreamUrl(const std::string& templ, const std::string& streamName)
{
    const std::string placeholder = "{src}";
    const std::string encoded = urlEncode(streamName);
    std::string url = templ;
    std::size_t pos = url.find(placeholder);
    while (pos != std::string::npos) {
        url.replace(pos, placeholder.size(), encoded);
        pos = url.find(placeholder, pos + encoded.size());
    }
    return url;
}

} // namespace

std::vector<CameraStream> discoverCameras(HttpClient& client)
{
    const std::string streamTemplate =
        trimTrailingSlashes(client.baseUrl()) + "/api/go2rtc/api/stream.ts?src={src}";

    logInfo() << "discovery: GET " << trimTrailingSlashes(client.baseUrl()) << "/api/config";
    const HttpResponse response = client.get("/api/config");
    if (!response.ok) {
        throw std::runtime_error("discovery: /api/config failed: " + response.error);
    }

    boost::system::error_code ec;
    const json::value parsed = json::parse(response.body, ec);
    if (ec) {
        throw std::runtime_error("discovery: /api/config JSON parse failed: " + ec.message());
    }
    const json::object* config = parsed.if_object();
    if (!config) {
        throw std::runtime_error("discovery: /api/config is not a JSON object");
    }

    const std::set<std::string> configuredStreams = configuredGo2rtcStreams(*config);
    const json::object* cameras = objectField(*config, "cameras");
    if (!cameras) {
        throw std::runtime_error("discovery: no 'cameras' object in /api/config");
    }

    std::vector<CameraStream> result;
    for (const auto& item : *cameras) {
        const std::string cameraName = jsonKeyToString(item.key());
        const json::object* camera = item.value().if_object();
        if (!camera) {
            continue;
        }

        const std::string stream = primaryStreamForCamera(*camera, configuredStreams);
        if (stream.empty()) {
            logWarning() << "discovery: no go2rtc stream resolved for camera " << cameraName << " (skipped)";
            continue;
        }

        CameraStream cameraStream { cameraName, stream, buildStreamUrl(streamTemplate, stream) };
        logInfo() << "discovery: " << cameraName << " -> " << stream << "  " << cameraStream.streamUrl;
        result.push_back(std::move(cameraStream));
    }

    logInfo() << "discovery: resolved " << result.size() << " camera(s)";
    return result;
}

} // namespace gig
