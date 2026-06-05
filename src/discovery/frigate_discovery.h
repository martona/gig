#pragma once

#include "net/http_client.hpp"

#include <string>
#include <vector>

namespace gig {

struct CameraStream {
    std::string cameraName; // Frigate camera key, e.g. "cam_frontgate"
    std::string streamName; // go2rtc stream name, e.g. "frontgate"
    std::string streamUrl;  // full stream.ts URL handed to FFmpeg
};

// Fetch /api/config through `client` and map every camera to its primary
// go2rtc live stream, in config order (stable slot order).
//
// `streamUrlTemplate` is a stream URL containing a "{src}" placeholder that is
// replaced with each camera's go2rtc stream name. If empty, it defaults to
// `<client base>/api/go2rtc/api/stream.ts?src={src}`.
//
// Throws std::runtime_error on fetch or parse failure. Cameras with no
// resolvable go2rtc stream are logged and skipped.
std::vector<CameraStream> discoverCameras(HttpClient& client, const std::string& streamUrlTemplate);

} // namespace gig
