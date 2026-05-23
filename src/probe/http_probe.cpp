#include "probe/http_probe.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace {

std::once_flag ffmpegNetworkInitOnce;

struct AvioContextDeleter {
    void operator()(AVIOContext* context) const
    {
        if (context) {
            avio_closep(&context);
        }
    }
};

using AvioContextPtr = std::unique_ptr<AVIOContext, AvioContextDeleter>;

struct ProbeEndpoint {
    std::string label;
    std::string pathAndQuery;
    std::size_t maxBytes = 0;
};

struct FetchResult {
    bool ok = false;
    bool truncated = false;
    int errorCode = 0;
    std::string error;
    std::vector<std::uint8_t> body;
};

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void setOption(AVDictionary** options, const char* key, const std::string& value)
{
    if (!value.empty()) {
        av_dict_set(options, key, value.c_str(), 0);
    }
}

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

bool looksBinary(const std::vector<std::uint8_t>& body)
{
    const std::size_t scanBytes = std::min<std::size_t>(body.size(), 512);
    for (std::size_t i = 0; i < scanBytes; ++i) {
        const unsigned char ch = body[i];
        if (ch == 0) {
            return true;
        }
        if (ch < 0x08 || (ch > 0x0D && ch < 0x20)) {
            return true;
        }
    }
    return false;
}

std::string textPreview(const std::vector<std::uint8_t>& body, std::size_t maxBytes)
{
    const std::size_t previewBytes = std::min(maxBytes, body.size());
    std::string output;
    output.reserve(previewBytes);
    for (std::size_t i = 0; i < previewBytes; ++i) {
        const char ch = static_cast<char>(body[i]);
        if (ch == '\r') {
            continue;
        }
        output.push_back(ch);
    }
    return output;
}

FetchResult fetchUrl(const std::string& url, const TlsOptions& tls, std::size_t maxBytes)
{
    std::call_once(ffmpegNetworkInitOnce, [] {
        avformat_network_init();
    });

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rw_timeout", std::to_string(tls.rwTimeoutUs).c_str(), 0);
    av_dict_set(&options, "tls_verify", tls.verifyServer ? "1" : "0", 0);
    setOption(&options, "ca_file", tls.caFile);
    setOption(&options, "cert_file", tls.certFile);
    setOption(&options, "key_file", tls.keyFile);

    AVIOContext* rawContext = nullptr;
    const int openResult = avio_open2(&rawContext, url.c_str(), AVIO_FLAG_READ, nullptr, &options);
    av_dict_free(&options);
    if (openResult < 0) {
        return {
            false,
            false,
            openResult,
            ffmpegError(openResult),
            {},
        };
    }

    AvioContextPtr context(rawContext);
    FetchResult result;
    result.ok = true;

    std::array<std::uint8_t, 4096> buffer = {};
    while (result.body.size() < maxBytes) {
        const std::size_t remaining = maxBytes - result.body.size();
        const int requested = static_cast<int>(std::min<std::size_t>(buffer.size(), remaining));
        const int read = avio_read(context.get(), buffer.data(), requested);
        if (read == AVERROR_EOF || read == 0) {
            break;
        }
        if (read < 0) {
            result.ok = false;
            result.errorCode = read;
            result.error = ffmpegError(read);
            break;
        }

        result.body.insert(result.body.end(), buffer.begin(), buffer.begin() + read);
    }

    result.truncated = result.body.size() == maxBytes;
    return result;
}

std::vector<ProbeEndpoint> buildEndpoints(const ProbeOptions& options)
{
    std::vector<ProbeEndpoint> endpoints = {
        { "frigate config", "/api/config", options.maxBytes },
        { "frigate go2rtc streams", "/api/go2rtc/streams", options.maxBytes },
        { "frigate go2rtc api/streams proxy", "/api/go2rtc/api/streams", options.maxBytes },
        { "raw go2rtc streams", "/api/streams", options.maxBytes },
    };

    if (!options.streamName.empty()) {
        const std::string pathName = urlEncode(options.streamName);
        const std::string queryName = urlEncode(options.streamName);
        endpoints.push_back({ "frigate go2rtc stream by name", "/api/go2rtc/streams/" + pathName, options.maxBytes });
        endpoints.push_back({ "frigate go2rtc api/streams by src", "/api/go2rtc/api/streams?src=" + queryName, options.maxBytes });
        endpoints.push_back({ "raw go2rtc streams by src", "/api/streams?src=" + queryName, options.maxBytes });

        if (options.checkStreams) {
            const std::size_t streamBytes = std::min<std::size_t>(options.maxBytes, 1880);
            endpoints.push_back({ "frigate go2rtc api/stream.ts bytes", "/api/go2rtc/api/stream.ts?src=" + queryName, streamBytes });
            endpoints.push_back({ "raw go2rtc stream.ts bytes", "/api/stream.ts?src=" + queryName, streamBytes });
        }
    }

    for (const std::string& endpoint : options.extraEndpoints) {
        endpoints.push_back({ "custom " + endpoint, endpoint, options.maxBytes });
    }

    return endpoints;
}

void printResult(const ProbeEndpoint& endpoint, const std::string& url, const FetchResult& result, bool dumpBody)
{
    std::cout << "\n== " << endpoint.label << "\n"
              << "GET " << url << "\n";

    if (!result.ok) {
        std::cout << "FAIL " << result.error << " (" << result.errorCode << ")\n";
        if (!result.body.empty()) {
            std::cout << "partial-bytes " << result.body.size() << "\n";
        }
        return;
    }

    std::cout << "OK bytes=" << result.body.size();
    if (result.truncated) {
        std::cout << " truncated-at=" << endpoint.maxBytes;
    }
    if (looksBinary(result.body)) {
        std::cout << " binary";
    }
    std::cout << "\n";

    if (result.body.empty()) {
        return;
    }

    if (looksBinary(result.body)) {
        const std::size_t bytes = std::min<std::size_t>(result.body.size(), 32);
        std::cout << "hex-preview";
        for (std::size_t i = 0; i < bytes; ++i) {
            std::cout << ' ' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(result.body[i]);
        }
        std::cout << std::dec << std::setfill(' ') << "\n";
        return;
    }

    const std::size_t previewBytes = dumpBody ? result.body.size() : std::min<std::size_t>(result.body.size(), 1600);
    std::cout << textPreview(result.body, previewBytes);
    if (!dumpBody && result.body.size() > previewBytes) {
        std::cout << "\n... body preview clipped; pass --dump to print all fetched bytes";
    }
    std::cout << "\n";
}

} // namespace

int runProbe(const ProbeOptions& options)
{
    if (options.baseUrl.empty()) {
        throw std::runtime_error("probe requires --base URL or a positional URL.");
    }

    std::cout << "Probe base: " << trimTrailingSlashes(options.baseUrl) << "\n";
    if (!options.streamName.empty()) {
        std::cout << "Probe stream: " << options.streamName << "\n";
    }

    int successes = 0;
    int failures = 0;
    for (const ProbeEndpoint& endpoint : buildEndpoints(options)) {
        const std::string url = joinUrl(options.baseUrl, endpoint.pathAndQuery);
        const FetchResult result = fetchUrl(url, options.tls, endpoint.maxBytes);
        printResult(endpoint, url, result, options.dumpBody);
        if (!result.ok) {
            ++failures;
        } else {
            ++successes;
        }
    }

    std::cout << "\nProbe complete: " << successes << " OK, " << failures << " failed endpoint(s).\n";
    return successes > 0 ? 0 : 2;
}
