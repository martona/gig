#pragma once

#include <cstdint>
#include <string>

// TLS / mTLS material shared by the FFmpeg decoder (which passes it to
// FFmpeg's own OpenSSL backend) and the Beast control-plane HTTP client.
struct TlsOptions {
    bool verifyServer = true;
    // Use the Windows certificate store (store trust roots for server verification
    // + a CurrentUser\MY client cert signed via the CNG bridge) instead of PEM
    // files. Derived, not a flag: set when no caFile/certFile/keyFile is given.
    bool useWindowsStore = false;
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    std::int64_t rwTimeoutUs = 10'000'000;
};
