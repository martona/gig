#include "health/stream_health.h"

#include <string>

#include <boost/json.hpp>

namespace gig {
namespace {

namespace json = boost::json;

double asDouble(const json::value* value)
{
    if (!value) {
        return 0.0;
    }
    if (const auto* d = value->if_double()) {
        return *d;
    }
    if (const auto* i = value->if_int64()) {
        return static_cast<double>(*i);
    }
    if (const auto* u = value->if_uint64()) {
        return static_cast<double>(*u);
    }
    return 0.0;
}

} // namespace

CameraHealth fetchCameraHealth(HttpClient& client)
{
    CameraHealth result;

    const HttpResponse response = client.get("/api/stats");
    if (!response.ok) {
        result.error = "stats fetch failed: "
            + (response.error.empty() ? std::to_string(response.status) : response.error);
        return result;
    }

    boost::system::error_code ec;
    const json::value parsed = json::parse(response.body, ec);
    if (ec) {
        result.error = "stats JSON parse failed: " + ec.message();
        return result;
    }
    const json::object* root = parsed.if_object();
    const json::value* camerasValue = root ? root->if_contains("cameras") : nullptr;
    const json::object* cameras = camerasValue ? camerasValue->if_object() : nullptr;
    if (!cameras) {
        result.schemaError = true;
        result.error = "stats: no cameras object (schema change?)";
        return result;
    }

    bool sawCamera = false;
    bool sawFps = false;
    for (const auto& item : *cameras) {
        sawCamera = true;
        const std::string cameraName(item.key().data(), item.key().size());
        const json::object* camera = item.value().if_object();
        const json::value* fps = camera ? camera->if_contains("camera_fps") : nullptr;
        if (fps) {
            sawFps = true;
        }
        result.fpsByCamera[cameraName] = asDouble(fps);
    }

    // If cameras exist but none exposed camera_fps, the schema very likely
    // changed again. Surface it loudly instead of reporting every camera
    // offline (which is what blindly trusting 0.0 would do).
    if (sawCamera && !sawFps) {
        result.schemaError = true;
        result.error = "stats: cameras present but no camera_fps field (schema change?)";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace gig
