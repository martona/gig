// We knowingly use the deprecated-in-3.0 legacy RSA_METHOD / EC_KEY_METHOD APIs to
// delegate signing to the Windows CNG key. Must precede any OpenSSL header.
#define OPENSSL_SUPPRESS_DEPRECATED

#include "net/cng_tls.hpp"

#include "log.hpp"
#include "net/win_cert_store.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace gig {
namespace {

std::string subjectOf(X509* cert)
{
    char buffer[512] = {};
    X509_NAME* name = cert ? X509_get_subject_name(cert) : nullptr;
    if (name) {
        X509_NAME_oneline(name, buffer, sizeof(buffer));
    }
    return buffer[0] ? buffer : "(unknown subject)";
}

// ---- CNG client-key bridge -------------------------------------------------
// A legacy RSA/EC key whose public half comes from the leaf cert and whose private
// operation is delegated to NCryptSignHash on the Windows store key. Only the
// PKCS#1 (RSA) and raw-ECDSA paths are implemented.

int rsaCngExIndex()
{
    static const int index = RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int cngRsaSign(int type, const unsigned char* digest, unsigned int digestLen,
               unsigned char* sigret, unsigned int* siglen, const RSA* rsa)
{
    (void)type; // hash is inferred from digestLen inside the bridge
    const auto keyHandle = reinterpret_cast<std::uintptr_t>(RSA_get_ex_data(rsa, rsaCngExIndex()));
    const std::vector<std::uint8_t> signature = cngSignPkcs1(keyHandle, digest, digestLen);
    if (signature.empty()) {
        return 0;
    }
    std::memcpy(sigret, signature.data(), signature.size());
    *siglen = static_cast<unsigned int>(signature.size());
    return 1;
}

RSA_METHOD* cngRsaMethod()
{
    static RSA_METHOD* method = [] {
        RSA_METHOD* m = RSA_meth_dup(RSA_get_default_method());
        RSA_meth_set1_name(m, "gig CNG client key");
        RSA_meth_set_sign(m, cngRsaSign);
        return m;
    }();
    return method;
}

// Build an EVP_PKEY whose public half comes from `leaf` and whose signing is
// delegated to the store key. Returns nullptr on failure.
EVP_PKEY* buildCngRsaKey(const WinClientCert& cert, X509* leaf)
{
    EVP_PKEY* pub = X509_get_pubkey(leaf);
    if (!pub) {
        return nullptr;
    }
    RSA* pubRsa = EVP_PKEY_get1_RSA(pub);
    EVP_PKEY_free(pub);
    if (!pubRsa) {
        return nullptr;
    }

    const BIGNUM* n = nullptr;
    const BIGNUM* e = nullptr;
    RSA_get0_key(pubRsa, &n, &e, nullptr);

    RSA* signRsa = RSA_new();
    RSA_set0_key(signRsa, BN_dup(n), BN_dup(e), nullptr);
    RSA_free(pubRsa);

    RSA_set_method(signRsa, cngRsaMethod());
    RSA_set_ex_data(signRsa, rsaCngExIndex(), reinterpret_cast<void*>(cert.keyHandle));

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (EVP_PKEY_assign_RSA(pkey, signRsa) != 1) {
        RSA_free(signRsa);
        EVP_PKEY_free(pkey);
        return nullptr;
    }
    return pkey;
}

// EC variant. ECDSA has no padding, so the sign hook gets the bare digest and this
// works on TLS 1.2 and 1.3 alike.
int ecCngExIndex()
{
    static const int index = EC_KEY_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int cngEcSign(int type, const unsigned char* digest, int digestLen,
              unsigned char* sig, unsigned int* sigLen,
              const BIGNUM* kinv, const BIGNUM* rp, EC_KEY* eckey)
{
    (void)type;
    (void)kinv;
    (void)rp;
    const auto keyHandle = reinterpret_cast<std::uintptr_t>(EC_KEY_get_ex_data(eckey, ecCngExIndex()));
    const std::vector<std::uint8_t> raw = cngSignEcdsa(keyHandle, digest, static_cast<std::size_t>(digestLen));
    if (raw.empty() || (raw.size() % 2) != 0) {
        return 0;
    }

    const std::size_t half = raw.size() / 2;
    ECDSA_SIG* sigObj = ECDSA_SIG_new();
    if (!sigObj) {
        return 0;
    }
    BIGNUM* r = BN_bin2bn(raw.data(), static_cast<int>(half), nullptr);
    BIGNUM* s = BN_bin2bn(raw.data() + half, static_cast<int>(half), nullptr);
    if (!r || !s || ECDSA_SIG_set0(sigObj, r, s) != 1) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sigObj);
        return 0;
    }

    unsigned char* out = sig;
    const int len = i2d_ECDSA_SIG(sigObj, &out); // DER-encode r,s into the caller's buffer
    ECDSA_SIG_free(sigObj);
    if (len <= 0) {
        return 0;
    }
    *sigLen = static_cast<unsigned int>(len);
    return 1;
}

EC_KEY_METHOD* cngEcMethod()
{
    static EC_KEY_METHOD* method = [] {
        EC_KEY_METHOD* m = EC_KEY_METHOD_new(EC_KEY_get_default_method());
        int (*defaultSign)(int, const unsigned char*, int, unsigned char*, unsigned int*,
                           const BIGNUM*, const BIGNUM*, EC_KEY*) = nullptr;
        int (*defaultSetup)(EC_KEY*, BN_CTX*, BIGNUM**, BIGNUM**) = nullptr;
        ECDSA_SIG* (*defaultSignSig)(const unsigned char*, int, const BIGNUM*, const BIGNUM*, EC_KEY*) = nullptr;
        EC_KEY_METHOD_get_sign(m, &defaultSign, &defaultSetup, &defaultSignSig);
        EC_KEY_METHOD_set_sign(m, cngEcSign, defaultSetup, defaultSignSig);
        return m;
    }();
    return method;
}

EVP_PKEY* buildCngEcKey(const WinClientCert& cert, X509* leaf)
{
    EVP_PKEY* pub = X509_get_pubkey(leaf);
    if (!pub) {
        return nullptr;
    }
    EC_KEY* pubEc = EVP_PKEY_get1_EC_KEY(pub);
    EVP_PKEY_free(pub);
    if (!pubEc) {
        return nullptr;
    }

    EC_KEY* signEc = EC_KEY_new();
    if (!signEc
        || EC_KEY_set_group(signEc, EC_KEY_get0_group(pubEc)) != 1
        || EC_KEY_set_public_key(signEc, EC_KEY_get0_public_key(pubEc)) != 1) {
        EC_KEY_free(pubEc);
        EC_KEY_free(signEc);
        return nullptr;
    }
    EC_KEY_free(pubEc);

    EC_KEY_set_method(signEc, cngEcMethod());
    EC_KEY_set_ex_data(signEc, ecCngExIndex(), reinterpret_cast<void*>(cert.keyHandle));

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (EVP_PKEY_assign_EC_KEY(pkey, signEc) != 1) {
        EC_KEY_free(signEc);
        EVP_PKEY_free(pkey);
        return nullptr;
    }
    return pkey;
}

// Pick the client cert once and hold it (and its live CNG key handle) for the
// process lifetime, so a single picker + consent covers every SSL_CTX. The handle
// is released only at process exit, after all SSL_CTXs are gone.
const WinClientCert& sharedStoreCert()
{
    static WinClientCert cert = selectClientCertFromStore();
    return cert;
}

} // namespace

bool useWindowsStoreClientCert(SSL_CTX* ctx)
{
    const WinClientCert& cert = sharedStoreCert();
    if (!cert.valid()) {
        return false;
    }

    const unsigned char* der = cert.certDer.data();
    X509* leaf = d2i_X509(nullptr, &der, static_cast<long>(cert.certDer.size()));
    if (!leaf) {
        return false;
    }

    int keyType = 0;
    {
        EVP_PKEY* pub = X509_get_pubkey(leaf);
        keyType = pub ? EVP_PKEY_base_id(pub) : 0;
        EVP_PKEY_free(pub);
    }

    EVP_PKEY* pkey = nullptr;
    bool pinPkcs1 = false;
    if (keyType == EVP_PKEY_RSA) {
        pkey = buildCngRsaKey(cert, leaf);
        pinPkcs1 = true;
    } else if (keyType == EVP_PKEY_EC) {
        pkey = buildCngEcKey(cert, leaf);
    }

    const bool ok = pkey
        && SSL_CTX_use_certificate(ctx, leaf) == 1
        && SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
    if (ok && pinPkcs1) {
        // The legacy RSA bridge only does PKCS#1, so pin TLS 1.2 + PKCS#1 sig algs.
        SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set1_sigalgs_list(ctx, "RSA+SHA256:RSA+SHA384:RSA+SHA512");
    }
    if (ok) {
        logInfo() << "windows store client cert: " << subjectOf(leaf)
                  << (pinPkcs1 ? " (RSA, pinned TLS 1.2 + PKCS#1)" : " (EC, TLS 1.3)");
    }

    if (pkey) {
        EVP_PKEY_free(pkey); // the context holds its own reference
    }
    X509_free(leaf); // the context holds its own reference
    return ok;
}

} // namespace gig
