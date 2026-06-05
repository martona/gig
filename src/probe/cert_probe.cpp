// We knowingly use the deprecated-in-3.0 ENGINE API for the (dead) capi probe path.
#define OPENSSL_SUPPRESS_DEPRECATED

#include "probe/cert_probe.h"

#include "net/cng_tls.hpp"
#include "net/url.h"

#include <chrono>
#include <iostream>
#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/crypto.h>
#include <openssl/engine.h>
#include <openssl/err.h>
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

    // Client cert source. The CNG path's cert + live key handle are owned (for the
    // process lifetime) inside cng_tls, so nothing to keep alive here.
    ENGINE* capi = nullptr;

    if (mode == ClientCertMode::Capi) {
        capi = loadCapiEngine();
        if (capi && SSL_CTX_set_client_cert_engine(native, capi) == 1) {
            std::cout << "[client cert] capi engine wired as client-cert source.\n";
        } else if (capi) {
            std::cout << "[client cert] SSL_CTX_set_client_cert_engine FAILED: " << opensslErrors() << "\n";
        }
    } else if (mode == ClientCertMode::Cng) {
        std::cout << "[client cert] opening Windows certificate picker (MY store)...\n";
        if (useWindowsStoreClientCert(native)) {
            std::cout << "[client cert] store cert wired via CNG bridge "
                         "(RSA -> TLS 1.2 / PKCS#1, EC -> TLS 1.3).\n";
        } else {
            std::cout << "[client cert] no usable store cert selected / wiring failed: "
                      << opensslErrors() << "\n";
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

    if (capi) {
        ENGINE_finish(capi);
        ENGINE_free(capi);
    }
    return rc;
}

} // namespace gig
