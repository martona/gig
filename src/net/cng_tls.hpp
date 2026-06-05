#pragma once

#include <openssl/ssl.h>

namespace gig {

// Select a client certificate from the Windows store (CurrentUser\MY) and wire it
// into `ctx` via our CNG signing bridge -- the private key never leaves the store;
// signing is delegated to NCryptSignHash. RSA keys are pinned to TLS 1.2 + PKCS#1
// (the legacy RSA_METHOD path only gets the bare digest); EC keys work on TLS 1.3.
//
// The picker is shown once and the chosen cert (with its live CNG key handle) is
// cached for the process lifetime, so a single picker + consent covers every
// SSL_CTX. The first signing use pops the Windows consent prompt. Returns false if
// no usable certificate was selected (e.g. the user cancelled, or an unsupported
// key type).
bool useWindowsStoreClientCert(SSL_CTX* ctx);

} // namespace gig
