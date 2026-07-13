#include "net/cert_pin.hpp"

#include "platform/settings_store.hpp"

#include <string>
#include <utility>

#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "net/system_trust_mac.h" // systemTrustEvaluateChain (iOS SecTrust fallback)
#endif
#endif

namespace gig {
namespace {

CertPinStore* g_pinStore = nullptr;

std::string declinedKey(const std::string& host, const std::string& spki)
{
    return host + "|" + spki;
}

std::string subjectOf(X509* cert)
{
    char buffer[512] = {};
    X509_NAME* name = cert ? X509_get_subject_name(cert) : nullptr;
    if (name) {
        X509_NAME_oneline(name, buffer, sizeof(buffer));
    }
    return buffer[0] ? buffer : "(unknown subject)";
}

std::string notAfterOf(X509* cert)
{
    const ASN1_TIME* time = cert ? X509_get0_notAfter(cert) : nullptr;
    if (!time) {
        return {};
    }
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return {};
    }
    std::string out;
    if (ASN1_TIME_print(bio, time)) {
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio, &data);
        if (data && length > 0) {
            out.assign(data, static_cast<std::size_t>(length));
        }
    }
    BIO_free(bio);
    return out;
}

// SHA-256 over the DER SubjectPublicKeyInfo, hex-encoded. Pinning the SPKI (not
// the whole cert) survives a same-key renewal -- only a key change trips it.
std::string spkiSha256Hex(X509* cert)
{
    if (!cert) {
        return {};
    }
    unsigned char* der = nullptr;
    const int length = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    if (length <= 0 || !der) {
        return {};
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(der, static_cast<std::size_t>(length), hash);
    OPENSSL_free(der);

    static const char* const hex = "0123456789abcdef";
    std::string out;
    out.reserve(SHA256_DIGEST_LENGTH * 2);
    for (const unsigned char byte : hash) {
        out += hex[byte >> 4];
        out += hex[byte & 0x0F];
    }
    return out;
}

// Frees the per-SSL host string when the SSL object is destroyed.
void freeHostExData(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*)
{
    delete static_cast<std::string*>(ptr);
}

int hostExIndex()
{
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, freeHostExData);
    return index;
}

// Marks an SSL_CTX as trusting the OS store (vs an explicit PEM ca). Non-null =
// system store. Read by the verify callback to gate the iOS SecTrust fallback.
int systemStoreExIndex()
{
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

// Server-cert verify callback (network thread). Pinned leaf -> trust regardless
// of chain/hostname; otherwise defer to normal verification, and on failure
// stage the cert for the UI to offer pinning.
int pinVerifyCallback(int preverifyOk, X509_STORE_CTX* storeCtx)
{
    auto* ssl = static_cast<SSL*>(
        X509_STORE_CTX_get_ex_data(storeCtx, SSL_get_ex_data_X509_STORE_CTX_idx()));
    CertPinStore* pins = certPinStore();
    auto* host = ssl ? static_cast<std::string*>(SSL_get_ex_data(ssl, hostExIndex())) : nullptr;
    if (!pins || !host) {
        return preverifyOk; // pinning not wired for this connection -> default
    }

    X509* leaf = X509_STORE_CTX_get0_cert(storeCtx);
    const std::string spki = spkiSha256Hex(leaf);
    if (!spki.empty() && pins->isPinned(*host, spki)) {
        return 1; // pinned: trust this exact cert regardless of chain/hostname
    }
    if (preverifyOk) {
        return 1; // normal verification (chain + hostname) passed
    }

#if defined(__APPLE__) && TARGET_OS_IPHONE
    // iOS, SYSTEM-STORE MODE ONLY: OpenSSL's store holds no OS roots (they can't
    // be enumerated there), so a chain failure is the NORMAL case for a publicly-
    // or user-profile-trusted cert. Ask the OS before treating it as pinnable:
    // SecTrust evaluates the presented chain + hostname against iOS's system and
    // user-installed roots. With an explicit PEM `ca` configured the fallback is
    // OFF -- the private CA is authoritative and a mismatch must land in the pin
    // prompt, exactly as on desktop (else an OS-trusted cert could silently
    // bypass the CA restriction and the changed-pin warning).
    SSL_CTX* sslCtx = SSL_get_SSL_CTX(ssl);
    const bool useSystemStore = sslCtx && SSL_CTX_get_ex_data(sslCtx, systemStoreExIndex()) != nullptr;
    if (useSystemStore && systemTrustEvaluateChain(storeCtx, *host)) {
        return 1;
    }
#endif

    const int error = X509_STORE_CTX_get_error(storeCtx);
    pins->recordPending(*host, spki, error, subjectOf(leaf), notAfterOf(leaf));
    return 0; // untrusted and unpinned -> reject (the UI may pin it, then retry)
}

} // namespace

CertPinStore::CertPinStore(std::shared_ptr<SettingsStore> store)
    : store_(std::move(store))
{
}

bool CertPinStore::isPinned(const std::string& host, const std::string& spki) const
{
    if (spki.empty() || !store_) {
        return false;
    }
    const std::optional<std::string> pinned = store_->getString("pins/" + host);
    return pinned && *pinned == spki;
}

void CertPinStore::recordPending(const std::string& host, const std::string& spki,
                                 long errorCode, const std::string& subject, const std::string& notAfter)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (declined_.count(declinedKey(host, spki))) {
        return; // user already said no this session
    }
    if (pending_ && pending_->host == host && pending_->spki == spki) {
        return; // already staged (dedupe concurrent handshakes)
    }

    PendingPinDecision decision;
    decision.host = host;
    decision.spki = spki;
    decision.errorCode = errorCode;
    decision.errorText = X509_verify_cert_error_string(errorCode);
    decision.subject = subject;
    decision.notAfter = notAfter;
    if (store_) {
        const std::optional<std::string> existing = store_->getString("pins/" + host);
        if (existing && !existing->empty() && *existing != spki) {
            decision.changed = true;
            decision.previousSpki = *existing;
        }
    }
    pending_ = std::move(decision);
}

std::optional<PendingPinDecision> CertPinStore::takePending()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<PendingPinDecision> out = pending_;
    pending_.reset();
    if (!out) {
        return std::nullopt;
    }
    // Drop stale stagings: while an accept/decline was in flight, a concurrent
    // failed handshake can have re-staged the SAME cert (recordPending's dedupe
    // only sees the current -- already-taken -- slot). Surfacing it again reads
    // as the user's decision not having worked.
    if (isPinned(out->host, out->spki) || declined_.count(declinedKey(out->host, out->spki))) {
        return std::nullopt;
    }
    return out;
}

void CertPinStore::acceptPin(const PendingPinDecision& decision)
{
    if (store_) {
        store_->setString("pins/" + decision.host, decision.spki);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    declined_.erase(declinedKey(decision.host, decision.spki));
}

void CertPinStore::declinePin(const PendingPinDecision& decision)
{
    std::lock_guard<std::mutex> lock(mutex_);
    declined_.insert(declinedKey(decision.host, decision.spki));
    // A copy of this cert re-staged while the decision was pending must not
    // resurface after the decline.
    if (pending_ && pending_->host == decision.host && pending_->spki == decision.spki) {
        pending_.reset();
    }
}

void CertPinStore::clearSessionDeclines()
{
    std::lock_guard<std::mutex> lock(mutex_);
    declined_.clear();
}

void CertPinStore::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
    declined_.clear();
}

void setCertPinStore(CertPinStore* store)
{
    g_pinStore = store;
}

CertPinStore* certPinStore()
{
    return g_pinStore;
}

void installPinningVerify(SSL_CTX* ctx, bool useSystemStore)
{
    if (certPinStore()) {
        // Non-null token = system-store mode (read by the iOS SecTrust fallback).
        SSL_CTX_set_ex_data(ctx, systemStoreExIndex(), useSystemStore ? ctx : nullptr);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, pinVerifyCallback);
    }
}

void prepareConnectionPinning(SSL* ssl, const std::string& host)
{
    if (!ssl || host.empty()) {
        return;
    }
    SSL_set1_host(ssl, host.c_str());                          // hostname verification
    SSL_set_ex_data(ssl, hostExIndex(), new std::string(host)); // freed on SSL destroy
}

} // namespace gig
