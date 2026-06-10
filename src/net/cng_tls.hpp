#pragma once

#include <openssl/ssl.h>

namespace gig {

// Install a lazy client-certificate callback on `ctx`: the Windows store picker
// (CurrentUser\MY) and CNG consent prompt appear only if a server's handshake
// actually requests a client certificate -- connections to servers that never
// ask stay prompt-free. The chosen cert (with its live CNG key handle) is
// cached for the process lifetime, so one picker + consent covers every
// connection and SSL_CTX.
//
// Signing is delegated to NCryptSignHash via the CNG bridge; the private key
// never leaves the store. EC certs work on any TLS version. RSA certs only
// work when the connection negotiated <= TLS 1.2 (the legacy RSA_METHOD bridge
// is PKCS#1-only and TLS 1.3 mandates RSA-PSS) -- on a TLS 1.3 connection an
// RSA pick logs an error and the certificate request is declined.
void installWindowsStoreClientCertCallback(SSL_CTX* ctx);

} // namespace gig
