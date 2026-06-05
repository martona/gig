#include "net/tls_context.hpp"

#include "net/cng_tls.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <boost/asio/ssl.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace gig {
namespace {

namespace ssl = boost::asio::ssl;

std::string opensslErrorString()
{
    const unsigned long error = ERR_get_error();
    if (error == 0) {
        return "unknown OpenSSL error";
    }
    char buffer[256] = {};
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return buffer;
}

std::string requireReadableFile(const std::string& path, std::string_view label)
{
    std::error_code ec;
    std::filesystem::path fsPath(path);
    std::filesystem::path absolute = std::filesystem::absolute(fsPath, ec);
    const std::string resolved = ec ? path : absolute.string();
    if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
        throw std::runtime_error(std::string(label) + " file is not readable: " + resolved);
    }
    return resolved;
}

} // namespace

void configureSslContext(ssl::context& context, const TlsOptions& tls)
{
    context.set_options(ssl::context::default_workarounds);
    if (tls.verifyServer) {
        context.set_verify_mode(ssl::verify_peer);
        if (tls.useWindowsStore) {
            // Trust roots from the Windows certificate store (our private CA lives
            // in Trusted Root). Proven via the certstore probe.
            if (SSL_CTX_load_verify_store(context.native_handle(), "org.openssl.winstore://") != 1) {
                throw std::runtime_error("failed to load Windows store trust roots: " + opensslErrorString());
            }
        } else if (!tls.caFile.empty()) {
            context.load_verify_file(requireReadableFile(tls.caFile, "CA certificate"));
        } else {
            context.set_default_verify_paths();
        }
    } else {
        context.set_verify_mode(ssl::verify_none);
    }

    if (tls.useWindowsStore) {
        // Client cert from CurrentUser\MY via the CNG signing bridge. Pops the
        // Windows consent prompt on first use; the selection is cached process-wide
        // so every context shares one picker + one consent.
        if (!useWindowsStoreClientCert(context.native_handle())) {
            throw std::runtime_error("no usable Windows store client certificate was selected");
        }
        return;
    }

    if (!tls.certFile.empty()) {
        const std::string certFile = requireReadableFile(tls.certFile, "client certificate");
        if (SSL_CTX_use_certificate_chain_file(context.native_handle(), certFile.c_str()) != 1) {
            throw std::runtime_error("failed to load client certificate: " + opensslErrorString());
        }
    }
    if (!tls.keyFile.empty()) {
        const std::string keyFile = requireReadableFile(tls.keyFile, "client private key");
        if (SSL_CTX_use_PrivateKey_file(context.native_handle(), keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw std::runtime_error("failed to load client private key: " + opensslErrorString());
        }
    }
    if (!tls.certFile.empty() || !tls.keyFile.empty()) {
        if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
            throw std::runtime_error("client certificate/private key mismatch: " + opensslErrorString());
        }
    }
}

} // namespace gig
