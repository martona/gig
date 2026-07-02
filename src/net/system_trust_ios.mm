#include "net/system_trust_mac.h"

#include "log.hpp"

#include <TargetConditionals.h>

// iOS implementation of the platform trust-root loader declared in system_trust_mac.h.
//
// macOS sources the OS roots via SecTrustCopyAnchorCertificates (system_trust_mac.mm)
// and feeds them into OpenSSL's X509 store. That *enumerate-the-anchors* API does NOT
// exist on iOS -- so we can't pre-load roots into OpenSSL the same way. This is NOT,
// however, "no system trust on iOS": the proper iOS path is to keep OpenSSL doing the
// handshake but route the trust DECISION through Security's SecTrustEvaluateWithError
// inside our verify callback (build a SecTrust from the presented chain + default SSL
// policy, evaluate against iOS's system + user-installed roots). That belongs in the
// verify path (cert_pin.cpp's installPinningVerify), not here, and is a follow-up.
//
// Until then this loader is a no-op, so `useSystemStore` connections fall through to
// the pinning/preverify path: configure server trust explicitly with a PEM `ca`, a
// trust-on-first-use cert pin, or `insecure`. Compiled into the iOS target in place
// of system_trust_mac.mm.
#if TARGET_OS_IOS

namespace gig {

void loadSystemTrustRoots(SSL_CTX* /*context*/)
{
    static bool warned = false;
    if (!warned) {
        warned = true;
        logWarning() << "iOS: can't pre-load system roots into OpenSSL "
                        "(SecTrust-in-verify-callback is the proper path, TODO); "
                        "for now set a PEM 'ca', accept a certificate pin, or use 'insecure'";
    }
}

} // namespace gig

#endif // TARGET_OS_IOS
