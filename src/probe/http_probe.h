#pragma once

#include "net/tls_options.h"

#include <cstddef>
#include <string>
#include <vector>

struct ProbeOptions {
    std::string baseUrl;
    std::string streamName;
    TlsOptions tls;
    std::size_t maxBytes = 2 * 1024 * 1024;
    bool dumpBody = false;
    bool checkStreams = false;
    std::vector<std::string> extraEndpoints;
};

int runProbe(const ProbeOptions& options);
