#include "net/system_trust_mac.h"

#include "log.hpp"

#include <string>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <TargetConditionals.h>

#if TARGET_OS_IOS

#import <CoreFoundation/CoreFoundation.h>
#import <Security/Security.h>

// iOS implementations of the platform trust hooks declared in system_trust_mac.h.
//
// macOS pre-loads the OS roots into OpenSSL (SecTrustCopyAnchorCertificates,
// system_trust_mac.mm); that enumeration API does not exist on iOS, so
// loadSystemTrustRoots stays a no-op here and the OS trust is consulted the other
// way around: when OpenSSL's verification fails, systemTrustEvaluateChain (called
// from cert_pin.cpp's verify callback, network thread) hands the presented chain
// to Security and lets iOS decide against its system + user-installed roots. The
// SSL policy includes the hostname, so a mismatch fails here too and falls through
// to the normal pin-prompt path (hostname mismatch is just another pinnable error).

namespace gig {
namespace {

// DER-encode an OpenSSL X509 into a SecCertificateRef (caller releases).
SecCertificateRef certificateFromX509(X509* cert)
{
    if (!cert) {
        return nullptr;
    }
    unsigned char* der = nullptr;
    const int length = i2d_X509(cert, &der);
    if (length <= 0 || !der) {
        return nullptr;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, der, length);
    OPENSSL_free(der);
    if (!data) {
        return nullptr;
    }
    SecCertificateRef out = SecCertificateCreateWithData(kCFAllocatorDefault, data);
    CFRelease(data);
    return out;
}

} // namespace

void loadSystemTrustRoots(SSL_CTX* /*context*/)
{
    // No-op by design: iOS can't enumerate system roots into OpenSSL. The OS trust
    // is consulted via systemTrustEvaluateChain in the verify callback instead.
}

bool systemTrustEvaluateChain(X509_STORE_CTX* storeCtx, const std::string& host)
{
    if (!storeCtx) {
        return false;
    }

    // Presented chain, leaf first (SecTrustCreateWithCertificates order). The
    // untrusted stack usually repeats the leaf; skip duplicates.
    X509* leaf = X509_STORE_CTX_get0_cert(storeCtx);
    if (!leaf) {
        return false;
    }
    CFMutableArrayRef certs = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!certs) {
        return false;
    }
    bool buildOk = false;
    if (SecCertificateRef leafRef = certificateFromX509(leaf)) {
        CFArrayAppendValue(certs, leafRef);
        CFRelease(leafRef); // retained by the array
        buildOk = true;
    }
    if (buildOk) {
        STACK_OF(X509)* presented = X509_STORE_CTX_get0_untrusted(storeCtx);
        const int count = presented ? sk_X509_num(presented) : 0;
        for (int i = 0; i < count; ++i) {
            X509* cert = sk_X509_value(presented, i);
            if (!cert || X509_cmp(cert, leaf) == 0) {
                continue;
            }
            if (SecCertificateRef ref = certificateFromX509(cert)) {
                CFArrayAppendValue(certs, ref);
                CFRelease(ref);
            }
        }
    }
    if (!buildOk) {
        CFRelease(certs);
        return false;
    }

    // SSL server policy with the hostname: SecTrust checks chain AND host.
    CFStringRef hostRef = host.empty()
        ? nullptr
        : CFStringCreateWithCString(kCFAllocatorDefault, host.c_str(), kCFStringEncodingUTF8);
    SecPolicyRef policy = SecPolicyCreateSSL(true, hostRef);
    if (hostRef) {
        CFRelease(hostRef);
    }
    SecTrustRef trust = nullptr;
    const OSStatus status = SecTrustCreateWithCertificates(certs, policy, &trust);
    CFRelease(certs);
    if (policy) {
        CFRelease(policy);
    }
    if (status != errSecSuccess || !trust) {
        if (trust) {
            CFRelease(trust);
        }
        return false;
    }

    CFErrorRef error = nullptr;
    const bool trusted = SecTrustEvaluateWithError(trust, &error);
    if (!trusted && error) {
        // Expected for self-signed/private-CA servers; the pin prompt handles those.
        CFStringRef description = CFErrorCopyDescription(error);
        char buffer[256] = {};
        if (description
            && CFStringGetCString(description, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
            logInfo() << "ios system trust: " << host << " not trusted by the OS (" << buffer << ")";
        }
        if (description) {
            CFRelease(description);
        }
    }
    if (error) {
        CFRelease(error);
    }
    CFRelease(trust);
    return trusted;
}

} // namespace gig

#endif // TARGET_OS_IOS
