#include "net/win_cert_store.h"

#include <utility>

#include <windows.h>

#include <wincrypt.h>
#include <cryptuiapi.h>
#include <ncrypt.h>

namespace gig {
namespace {

// Owner window for the picker + CNG UI. Set once at startup before any store op,
// read on the same (main) thread when the cert is selected; no synchronization
// needed.
HWND g_consentParent = nullptr;

} // namespace

void setConsentParentWindow(void* hwnd)
{
    g_consentParent = static_cast<HWND>(hwnd);
}

WinClientCert::~WinClientCert()
{
    if (keyHandle != 0) {
        NCryptFreeObject(static_cast<NCRYPT_KEY_HANDLE>(keyHandle));
    }
}

WinClientCert::WinClientCert(WinClientCert&& other) noexcept
    : certDer(std::move(other.certDer))
    , keyHandle(other.keyHandle)
{
    other.keyHandle = 0;
}

WinClientCert& WinClientCert::operator=(WinClientCert&& other) noexcept
{
    if (this != &other) {
        if (keyHandle != 0) {
            NCryptFreeObject(static_cast<NCRYPT_KEY_HANDLE>(keyHandle));
        }
        certDer = std::move(other.certDer);
        keyHandle = other.keyHandle;
        other.keyHandle = 0;
    }
    return *this;
}

WinClientCert selectClientCertFromStore()
{
    WinClientCert result;

    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        0,
        CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG,
        L"MY");
    if (!store) {
        return result;
    }

    PCCERT_CONTEXT cert = CryptUIDlgSelectCertificateFromStore(
        store, g_consentParent, nullptr, nullptr, CRYPTUI_SELECT_LOCATION_COLUMN, 0, nullptr);
    if (!cert) {
        CertCloseStore(store, 0);
        return result;
    }

    // Acquire the CNG key. No CRYPT_ACQUIRE_SILENT_FLAG so a UI-protected key is
    // allowed to prompt on use; ONLY_NCRYPT so we always get an NCRYPT handle.
    NCRYPT_KEY_HANDLE key = 0;
    DWORD keySpec = 0;
    BOOL callerOwnsKey = FALSE;
    if (CryptAcquireCertificatePrivateKey(
            cert,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG,
            nullptr,
            &key,
            &keySpec,
            &callerOwnsKey)
        && key != 0) {
        result.certDer.assign(cert->pbCertEncoded, cert->pbCertEncoded + cert->cbCertEncoded);
        result.keyHandle = static_cast<std::uintptr_t>(key);
        // CryptAcquireCertificatePrivateKey returns an NCRYPT handle the caller
        // owns; WinClientCert frees it. (callerOwnsKey is expected TRUE here.)

        // Own any consent / key-access UI this key shows (on whatever thread later
        // signs) to our window, so it is modal to the app and can't be closed
        // behind. Best effort: ignore failure (falls back to an unowned prompt).
        if (g_consentParent != nullptr) {
            NCryptSetProperty(
                key, NCRYPT_WINDOW_HANDLE_PROPERTY,
                reinterpret_cast<PBYTE>(&g_consentParent), sizeof(g_consentParent), 0);
        }
    }

    CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);
    return result;
}

std::vector<std::uint8_t> cngSignPkcs1(std::uintptr_t keyHandle, const std::uint8_t* digest, std::size_t digestLen)
{
    std::vector<std::uint8_t> signature;
    if (keyHandle == 0 || digest == nullptr) {
        return signature;
    }

    const wchar_t* algorithm = nullptr;
    switch (digestLen) {
    case 20: algorithm = BCRYPT_SHA1_ALGORITHM; break;
    case 32: algorithm = BCRYPT_SHA256_ALGORITHM; break;
    case 48: algorithm = BCRYPT_SHA384_ALGORITHM; break;
    case 64: algorithm = BCRYPT_SHA512_ALGORITHM; break;
    default: return signature;
    }

    BCRYPT_PKCS1_PADDING_INFO padding = {};
    padding.pszAlgId = algorithm;
    auto key = static_cast<NCRYPT_KEY_HANDLE>(keyHandle);

    DWORD signatureLen = 0;
    SECURITY_STATUS status = NCryptSignHash(
        key, &padding, const_cast<PBYTE>(digest), static_cast<DWORD>(digestLen),
        nullptr, 0, &signatureLen, NCRYPT_PAD_PKCS1_FLAG);
    if (status != ERROR_SUCCESS) {
        return signature;
    }

    signature.resize(signatureLen);
    status = NCryptSignHash(
        key, &padding, const_cast<PBYTE>(digest), static_cast<DWORD>(digestLen),
        signature.data(), signatureLen, &signatureLen, NCRYPT_PAD_PKCS1_FLAG);
    if (status != ERROR_SUCCESS) {
        signature.clear();
        return signature;
    }

    signature.resize(signatureLen);
    return signature;
}

std::vector<std::uint8_t> cngSignEcdsa(std::uintptr_t keyHandle, const std::uint8_t* digest, std::size_t digestLen)
{
    std::vector<std::uint8_t> signature;
    if (keyHandle == 0 || digest == nullptr) {
        return signature;
    }

    // ECDSA: no padding info; NCrypt returns the raw r||s.
    auto key = static_cast<NCRYPT_KEY_HANDLE>(keyHandle);
    DWORD signatureLen = 0;
    SECURITY_STATUS status = NCryptSignHash(
        key, nullptr, const_cast<PBYTE>(digest), static_cast<DWORD>(digestLen),
        nullptr, 0, &signatureLen, 0);
    if (status != ERROR_SUCCESS) {
        return signature;
    }

    signature.resize(signatureLen);
    status = NCryptSignHash(
        key, nullptr, const_cast<PBYTE>(digest), static_cast<DWORD>(digestLen),
        signature.data(), signatureLen, &signatureLen, 0);
    if (status != ERROR_SUCCESS) {
        signature.clear();
        return signature;
    }

    signature.resize(signatureLen);
    return signature;
}

} // namespace gig
