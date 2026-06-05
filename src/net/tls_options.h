#pragma once

#include <cstdint>
#include <string>

// TLS / mTLS material shared by the FFmpeg decoder (which passes it to
// FFmpeg's own OpenSSL backend) and the Beast control-plane HTTP client.
struct TlsOptions {
    bool verifyServer = true;
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    std::int64_t rwTimeoutUs = 10'000'000;
};
