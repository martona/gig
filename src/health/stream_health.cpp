#include "health/stream_health.h"

#include <string>

#include <boost/json.hpp>

namespace gig {
namespace {

namespace json = boost::json;

std::uint64_t asUInt(const json::value* value)
{
    if (!value) {
        return 0;
    }
    if (const auto* u = value->if_uint64()) {
        return *u;
    }
    if (const auto* i = value->if_int64()) {
        return *i > 0 ? static_cast<std::uint64_t>(*i) : 0;
    }
    if (const auto* d = value->if_double()) {
        return *d > 0.0 ? static_cast<std::uint64_t>(*d) : 0;
    }
    return 0;
}

} // namespace

StreamBytes fetchStreamBytes(HttpClient& client)
{
    StreamBytes result;

    const HttpResponse response = client.get("/api/go2rtc/api/streams");
    if (!response.ok) {
        result.error = "streams fetch failed: " + (response.error.empty() ? std::to_string(response.status) : response.error);
        return result;
    }

    boost::system::error_code ec;
    const json::value parsed = json::parse(response.body, ec);
    if (ec) {
        result.error = "streams JSON parse failed: " + ec.message();
        return result;
    }
    const json::object* streams = parsed.if_object();
    if (!streams) {
        result.error = "streams response is not a JSON object";
        return result;
    }

    bool sawProducer = false;
    bool sawByteField = false;

    for (const auto& item : *streams) {
        const std::string streamName(item.key().data(), item.key().size());
        std::uint64_t total = 0;

        const json::object* stream = item.value().if_object();
        const json::value* producersValue = stream ? stream->if_contains("producers") : nullptr;
        const json::array* producers = producersValue ? producersValue->if_array() : nullptr;
        if (producers) {
            for (const json::value& producerValue : *producers) {
                const json::object* producer = producerValue.if_object();
                if (!producer) {
                    continue;
                }
                sawProducer = true;
                // go2rtc 1.9+ renamed 'recv' to 'bytes_recv'.
                const json::value* bytes = producer->if_contains("bytes_recv");
                if (!bytes) {
                    bytes = producer->if_contains("recv");
                }
                if (bytes) {
                    sawByteField = true;
                    total += asUInt(bytes);
                }
            }
        }

        result.bytesByStream[streamName] = total;
    }

    // If producers exist but none exposed a known byte field, the schema very
    // likely changed again. Surface it loudly instead of reporting every camera
    // offline (which is what blindly trusting a 0 sum would do).
    if (sawProducer && !sawByteField) {
        result.schemaError = true;
        result.error = "go2rtc streams: producers present but no bytes_recv/recv field (schema change?)";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace gig
