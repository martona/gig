#pragma once

#include <openssl/types.h> // SSL_CTX forward typedef (lightweight)

namespace gig {

// Load the macOS trust anchors into the SSL_CTX's X509 store so OpenSSL can verify
// server certs against the OS trust: the built-in system roots PLUS user/admin
// trust-settings certs (e.g. a private CA the user added to the login keychain and
// marked trusted). This is the mac analog of Windows' SSL_CTX_load_verify_store
// "org.openssl.winstore://" -- it keeps OpenSSL doing the verification (the locked
// decision) and only sources the roots from the platform store. macOS-only.
void loadSystemTrustRoots(SSL_CTX* context);

} // namespace gig
