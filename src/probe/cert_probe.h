#pragma once

#include <string>

namespace gig {

enum class ClientCertMode {
    None, // server CA only
    Capi, // OpenSSL capi engine (legacy; broken for signing on 3.x)
    Cng,  // our NCryptSignHash bridge from the Windows store
};

// Probe whether this build's OpenSSL can validate the server via the Windows
// certificate store (winstore) and present a client certificate from the store.
// Connects to `baseUrl`'s host and reports each step. Returns 0 on a successful
// handshake, non-zero otherwise.
int runCertProbe(const std::string& baseUrl, ClientCertMode mode);

} // namespace gig
