// We knowingly use the deprecated-in-3.0 ENGINE and legacy RSA_METHOD APIs.
#define OPENSSL_SUPPRESS_DEPRECATED

#include "probe/cert_probe.h"

#include "net/url.h"
#include "net/win_cert_store.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace gig {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

std::string opensslErrors()
{
    std::string out;
    unsigned long code = 0;
    char buffer[256];
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buffer, sizeof(buffer));
        out += buffer;
        out += "; ";
    }
    return out.empty() ? "(no OpenSSL error queued)" : out;
}

std::string subjectOf(X509* cert)
{
    if (!cert) {
        return "(none)";
    }
    char buffer[512] = {};
    X509_NAME* name = X509_get_subject_name(cert);
    if (name) {
        X509_NAME_oneline(name, buffer, sizeof(buffer));
    }
    return buffer[0] ? buffer : "(unparsed)";
}

// ---- CNG client-key bridge -------------------------------------------------
// A legacy RSA whose private operation is delegated to NCryptSignHash on the
// Windows store key. Only the PKCS#1 sign path is implemented, so the TLS
// context is pinned to PKCS#1 sig algs (no PSS) for these connections.

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

// EC variant. ECDSA has no padding, so the sign hook gets the bare digest and
// this works on TLS 1.2 and 1.3 alike.
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

ENGINE* loadCapiEngine()
{
    ENGINE_load_builtin_engines();
    ENGINE* capi = ENGINE_by_id("capi");
    if (!capi) {
        std::cout << "[client cert] capi engine NOT available: " << opensslErrors() << "\n";
        return nullptr;
    }
    if (ENGINE_init(capi) != 1) {
        std::cout << "[client cert] capi engine found but ENGINE_init failed: " << opensslErrors() << "\n";
        ENGINE_free(capi);
        return nullptr;
    }
    std::cout << "[client cert] capi engine loaded + initialized.\n";
    return capi;
}

} // namespace

int runCertProbe(const std::string& baseUrl, ClientCertMode mode)
{
    std::cout << "OpenSSL: " << OpenSSL_version(OPENSSL_VERSION) << "\n";

    ParsedUrl url;
    try {
        url = parseUrl(baseUrl);
    } catch (const std::exception& error) {
        std::cerr << "bad --base URL: " << error.what() << "\n";
        return 2;
    }
    if (url.scheme != "https") {
        std::cerr << "cert probe requires an https:// base URL\n";
        return 2;
    }
    std::cout << "Target: " << url.host << ":" << url.port << "\n\n";

    ssl::context ctx(ssl::context::tls_client);
    ctx.set_verify_mode(ssl::verify_peer);
    SSL_CTX* native = ctx.native_handle();

    const char* winstoreUri = "org.openssl.winstore://";
    if (SSL_CTX_load_verify_store(native, winstoreUri) == 1) {
        std::cout << "[server CA] winstore wired as trust source (" << winstoreUri << ").\n";
    } else {
        std::cout << "[server CA] SSL_CTX_load_verify_store(winstore) FAILED: " << opensslErrors() << "\n";
    }

    // Client cert source. Keep `storeCert`/`leaf` alive past the handshake: the
    // CNG key handle they own is used by the sign callback during it.
    ENGINE* capi = nullptr;
    WinClientCert storeCert;
    X509* leaf = nullptr;

    if (mode == ClientCertMode::Capi) {
        capi = loadCapiEngine();
        if (capi && SSL_CTX_set_client_cert_engine(native, capi) == 1) {
            std::cout << "[client cert] capi engine wired as client-cert source.\n";
        } else if (capi) {
            std::cout << "[client cert] SSL_CTX_set_client_cert_engine FAILED: " << opensslErrors() << "\n";
        }
    } else if (mode == ClientCertMode::Cng) {
        std::cout << "[client cert] opening Windows certificate picker (MY store)...\n";
        storeCert = selectClientCertFromStore();
        if (!storeCert.valid()) {
            std::cout << "[client cert] no certificate selected / no CNG key acquired.\n";
        } else {
            const unsigned char* der = storeCert.certDer.data();
            leaf = d2i_X509(nullptr, &der, static_cast<long>(storeCert.certDer.size()));

            int keyType = 0;
            if (leaf) {
                EVP_PKEY* pub = X509_get_pubkey(leaf);
                keyType = pub ? EVP_PKEY_base_id(pub) : 0;
                EVP_PKEY_free(pub);
            }

            EVP_PKEY* pkey = nullptr;
            const char* describe = "";
            bool pinPkcs1 = false;
            if (keyType == EVP_PKEY_RSA) {
                pkey = buildCngRsaKey(storeCert, leaf);
                pinPkcs1 = true;
                describe = "RSA (TLS 1.2, PKCS#1)";
            } else if (keyType == EVP_PKEY_EC) {
                pkey = buildCngEcKey(storeCert, leaf);
                describe = "EC (TLS 1.2/1.3, ECDSA)";
            } else if (leaf) {
                std::cout << "[client cert] unsupported key type (only RSA and EC are bridged)\n";
            }

            if (leaf && pkey
                && SSL_CTX_use_certificate(native, leaf) == 1
                && SSL_CTX_use_PrivateKey(native, pkey) == 1) {
                if (pinPkcs1) {
                    // The legacy RSA bridge only does PKCS#1, so pin TLS 1.2 + PKCS#1 sig algs.
                    SSL_CTX_set_max_proto_version(native, TLS1_2_VERSION);
                    SSL_CTX_set1_sigalgs_list(native, "RSA+SHA256:RSA+SHA384:RSA+SHA512");
                }
                std::cout << "[client cert] store cert wired via CNG bridge: " << subjectOf(leaf)
                          << " -- " << describe << "\n";
            } else if (pkey) {
                std::cout << "[client cert] failed to wire store cert: " << opensslErrors() << "\n";
            }
            if (pkey) {
                EVP_PKEY_free(pkey); // the context holds its own reference
            }
        }
    }

    std::cout << "\nConnecting (a Windows cert picker / consent prompt may appear)...\n";

    int rc = 0;
    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        const auto endpoints = resolver.resolve(url.host, url.port);

        beast::ssl_stream<beast::tcp_stream> stream(io, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
            std::cerr << "warning: failed to set SNI host name\n";
        }
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(ssl::stream_base::client);

        std::cout << "\nHandshake: OK (" << SSL_get_version(stream.native_handle()) << ", "
                  << SSL_get_cipher(stream.native_handle()) << ")\n";
        SSL* sslHandle = stream.native_handle();

        const long verify = SSL_get_verify_result(sslHandle);
        std::cout << "[server CA] verify result: " << verify << " ("
                  << X509_verify_cert_error_string(verify) << ")"
                  << (verify == X509_V_OK ? "  <-- validated via Windows store"
                                          : "  <-- NOT trusted by Windows store") << "\n";

        X509* peer = SSL_get1_peer_certificate(sslHandle);
        std::cout << "[server CA] server cert subject: " << subjectOf(peer) << "\n";
        if (peer) {
            X509_free(peer);
        }

        X509* mine = SSL_get_certificate(sslHandle);
        if (mine) {
            std::cout << "[client cert] presented to server: " << subjectOf(mine) << "  <-- store cert used\n";
        } else {
            std::cout << "[client cert] none presented\n";
        }

        boost::system::error_code shutdownEc;
        stream.shutdown(shutdownEc);
    } catch (const std::exception& error) {
        std::cout << "\nHandshake/connect FAILED: " << error.what() << "\n"
                  << "  openssl: " << opensslErrors() << "\n";
        rc = 1;
    }

    if (leaf) {
        X509_free(leaf);
    }
    if (capi) {
        ENGINE_finish(capi);
        ENGINE_free(capi);
    }
    return rc;
}

} // namespace gig
