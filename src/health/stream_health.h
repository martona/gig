#pragma once

#include "net/http_client.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace gig {

struct StreamBytes {
    std::map<std::string, std::uint64_t> bytesByStream; // summed producer bytes per go2rtc stream
    bool ok = false;
    bool schemaError = false; // producers present but no recognized byte field
    std::string error;
};

// GET /api/go2rtc/api/streams once and sum producer bytes (bytes_recv, or the
// pre-1.9 'recv') per stream. If producers exist but expose neither byte field,
// returns ok=false with schemaError=true -- a loud signal that the go2rtc JSON
// changed shape, NOT a silent "everything is offline".
StreamBytes fetchStreamBytes(HttpClient& client);

} // namespace gig
