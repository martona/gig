#pragma once

#include "net/http_client.hpp"

#include <string>
#include <vector>

namespace gig {

struct CameraStream {
    std::string cameraName; // Frigate camera key, e.g. "cam_frontgate"
    std::string streamName; // go2rtc stream name, e.g. "frontgate"
    std::string streamUrl;  // full stream.ts URL handed to FFmpeg
    // Detect-stream resolution (cameras.<name>.detect.width/height) -- the
    // coordinate space of /ws "events" bounding boxes. 0 = not reported;
    // that camera gets no detection-box overlay.
    int detectWidth = 0;
    int detectHeight = 0;
};

// Fetch /api/config through `client` and map every camera to its primary
// go2rtc live stream, in config order (stable slot order). Each camera's video
// URL is Frigate's authenticated restream endpoint,
// `<client base>/api/go2rtc/api/stream.ts?src=<stream>`.
//
// Throws std::runtime_error on fetch or parse failure. Cameras with no
// resolvable go2rtc stream are logged and skipped.
std::vector<CameraStream> discoverCameras(HttpClient& client);

} // namespace gig
