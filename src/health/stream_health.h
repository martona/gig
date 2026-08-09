#pragma once

#include "net/http_client.hpp"

#include <map>
#include <string>

namespace gig {

struct CameraHealth {
    std::map<std::string, double> fpsByCamera; // Frigate camera name -> camera_fps
    bool ok = false;
    bool schemaError = false; // cameras present but no recognized fps field
    std::string error;
};

// GET /api/stats once and read cameras.<name>.camera_fps -- the capture
// process's fps gauge, a PRODUCER-side liveness signal that is independent of
// whether gig is consuming the stream (unlike the old go2rtc byte counters,
// whose generic proxy endpoint Frigate 0.18 removed -- GHSA-mgh5-cr9h-g6hr).
// If cameras exist but none exposes camera_fps, returns ok=false with
// schemaError=true -- a loud signal that the stats JSON changed shape, NOT a
// silent "everything is offline".
CameraHealth fetchCameraHealth(HttpClient& client);

} // namespace gig
