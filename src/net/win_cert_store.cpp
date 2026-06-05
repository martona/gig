#include "net/win_cert_store.h"

#include <utility>

#include <windows.h>

#include <wincrypt.h>
#include <cryptuiapi.h>
#include <ncrypt.h>

namespace gig {

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
        store, nullptr, nullptr, nullptr, CRYPTUI_SELECT_LOCATION_COLUMN, 0, nullptr);
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

} // namespace gig
