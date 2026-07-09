#pragma once

#include <openssl/types.h> // SSL_CTX / X509_STORE_CTX forward typedefs (lightweight)

#include <string>

namespace gig {

// Load the macOS trust anchors into the SSL_CTX's X509 store so OpenSSL can verify
// server certs against the OS trust: the built-in system roots PLUS user/admin
// trust-settings certs (e.g. a private CA the user added to the login keychain and
// marked trusted). This is the mac analog of Windows' SSL_CTX_load_verify_store
// "org.openssl.winstore://" -- it keeps OpenSSL doing the verification (the locked
// decision) and only sources the roots from the platform store. macOS-only.
void loadSystemTrustRoots(SSL_CTX* context);

// iOS-only fallback, called from the pinning verify callback when OpenSSL's own
// verification failed: iOS has no API to ENUMERATE the system roots into OpenSSL
// (loadSystemTrustRoots is a no-op there), so instead route the trust DECISION
// through the OS -- build a SecTrust from the connection's presented chain with an
// SSL policy for `host` (chain + hostname) and SecTrustEvaluateWithError it against
// iOS's system + user-installed roots. Returns true if the OS trusts the chain.
// Implemented only in system_trust_ios.mm; macOS/Windows never call it (their
// OpenSSL stores hold the OS roots, so preverify is already authoritative).
bool systemTrustEvaluateChain(X509_STORE_CTX* storeCtx, const std::string& host);

} // namespace gig
