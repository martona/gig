#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Windows certificate-store helpers. This header intentionally exposes only
// plain types: the implementation pulls in <wincrypt.h>/<ncrypt.h>, whose macros
// collide with OpenSSL's, so OpenSSL code must talk to the store only through
// this boundary.

namespace gig {

// Set the owner window for the cert picker and the CNG consent / key-access
// dialogs, so they are modal to our application window. While such a prompt is up
// the owner window is disabled, so the user can't try to close the app mid-prompt
// -- which would otherwise deadlock shutdown (a join against a thread blocked in
// the prompt). Pass the HWND as void*; nullptr (the default) means no owner (the
// CLI probes). Call once at startup, before the first store operation.
void setConsentParentWindow(void* hwnd);

// A client certificate selected from the Windows store, holding its (typically
// non-exportable) CNG private key open for signing. Move-only; releases the key
// on destruction.
class WinClientCert {
public:
    WinClientCert() = default;
    ~WinClientCert();
    WinClientCert(WinClientCert&& other) noexcept;
    WinClientCert& operator=(WinClientCert&& other) noexcept;
    WinClientCert(const WinClientCert&) = delete;
    WinClientCert& operator=(const WinClientCert&) = delete;

    bool valid() const { return !certDer.empty() && keyHandle != 0; }

    std::vector<std::uint8_t> certDer; // DER-encoded leaf certificate
    std::uintptr_t keyHandle = 0;      // NCRYPT_KEY_HANDLE, opaque to callers
};

// Show the Windows certificate picker over the Current User \ MY (Personal)
// store and acquire the chosen cert's CNG key. Returns an invalid result if the
// user cancels or no CNG key is available.
WinClientCert selectClientCertFromStore();

// PKCS#1 v1.5 signature over an already-computed digest, using the CNG key. The
// hash is inferred from digestLen (20=SHA-1, 32=SHA-256, 48=SHA-384, 64=SHA-512).
// Returns the signature, or empty on failure. This call is what triggers the
// Windows consent UI for a UI-protected key.
std::vector<std::uint8_t> cngSignPkcs1(std::uintptr_t keyHandle, const std::uint8_t* digest, std::size_t digestLen);

// ECDSA signature over an already-computed digest, using the CNG key. Returns
// the raw r||s (each half the curve's field size), or empty on failure. Also
// triggers the Windows consent UI for a UI-protected key.
std::vector<std::uint8_t> cngSignEcdsa(std::uintptr_t keyHandle, const std::uint8_t* digest, std::size_t digestLen);

} // namespace gig
