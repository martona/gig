#pragma once

#include <cstdint>
#include <string>

// TLS / mTLS material shared by the FFmpeg decoder (which passes it to
// FFmpeg's own OpenSSL backend) and the Beast control-plane HTTP client.
struct TlsOptions {
    bool verifyServer = true;
    // Use the OS trust store for server verification instead of PEM files: Windows
    // cert store (+ a CurrentUser\MY client cert via the CNG bridge), macOS keychain
    // roots (built-in + user/admin-trusted), or OpenSSL's default paths elsewhere.
    // Derived, not a flag: set when no caFile/certFile/keyFile is given.
    bool useSystemStore = false;
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    std::int64_t rwTimeoutUs = 10'000'000;
};
