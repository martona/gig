#include "net/system_trust_mac.h"

#include "log.hpp"

#import <Security/Security.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

// Unlike Windows (wincrypt vs OpenSSL symbol collisions, walled off in win_cert_store),
// Apple's Security framework (CoreFoundation/SecCertificate types) and OpenSSL share no
// symbols, so both live in this one .mm directly.

namespace gig {

void loadSystemTrustRoots(SSL_CTX* context)
{
    X509_STORE* store = SSL_CTX_get_cert_store(context);
    if (!store) {
        return;
    }

    int added = 0;
    const auto addArray = [&](CFArrayRef certs) {
        if (!certs) {
            return;
        }
        const CFIndex count = CFArrayGetCount(certs);
        for (CFIndex i = 0; i < count; ++i) {
            auto cert = static_cast<SecCertificateRef>(
                const_cast<void*>(CFArrayGetValueAtIndex(certs, i)));
            if (!cert) {
                continue;
            }
            CFDataRef der = SecCertificateCopyData(cert);
            if (!der) {
                continue;
            }
            const unsigned char* bytes = CFDataGetBytePtr(der);
            X509* x509 = d2i_X509(nullptr, &bytes, CFDataGetLength(der));
            CFRelease(der);
            if (!x509) {
                continue;
            }
            // Duplicates (a root present in more than one domain) just fail to add;
            // that's fine -- the error is cleared below so it can't leak.
            if (X509_STORE_add_cert(store, x509) == 1) {
                ++added;
            }
            X509_free(x509);
        }
    };

    // Built-in system anchors (public roots), then user- and admin-domain
    // trust-settings certs (where a privately-added/trusted CA lives).
    CFArrayRef anchors = nullptr;
    if (SecTrustCopyAnchorCertificates(&anchors) == errSecSuccess) {
        addArray(anchors);
        if (anchors) {
            CFRelease(anchors);
        }
    }
    for (const SecTrustSettingsDomain domain : { kSecTrustSettingsDomainUser, kSecTrustSettingsDomainAdmin }) {
        CFArrayRef certs = nullptr;
        if (SecTrustSettingsCopyCertificates(domain, &certs) == errSecSuccess) {
            addArray(certs);
            if (certs) {
                CFRelease(certs);
            }
        }
    }

    ERR_clear_error(); // X509_STORE_add_cert flags duplicates; don't leak that state
    logInfo() << "macOS system trust: loaded " << added << " root certificate(s) into the TLS store";
}

} // namespace gig
