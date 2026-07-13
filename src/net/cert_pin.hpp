#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

// Opaque OpenSSL forward declarations (keep OpenSSL out of this header).
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace gig {

class SettingsStore;

// An untrusted server certificate awaiting a user decision (pin or decline).
struct PendingPinDecision {
    std::string host;
    std::string spki;          // SHA-256 of the cert's SubjectPublicKeyInfo, hex
    std::string subject;
    std::string notAfter;      // human-readable expiry
    long errorCode = 0;        // X509_V_ERR_*
    std::string errorText;     // X509_verify_cert_error_string
    bool changed = false;      // a *different* cert is already pinned for this host
    std::string previousSpki;  // the prior pin, when changed
};

// Trust-on-first-use certificate pinning. Pins (SPKI-SHA256 per host) persist in
// the SettingsStore under "pins/<host>". The TLS verify callback (on network
// threads) consults this to override an untrusted-but-pinned cert and to record
// an untrusted-and-unpinned cert for the UI; the UI thread resolves the pending
// decision (pin or decline). Thread-safe.
class CertPinStore {
public:
    explicit CertPinStore(std::shared_ptr<SettingsStore> store);

    // --- network-thread side (verify callback) ---
    bool isPinned(const std::string& host, const std::string& spki) const;
    // Stage an untrusted cert for the UI to offer pinning. Deduped by host+spki;
    // ignored if the user already declined this exact cert this session.
    void recordPending(const std::string& host, const std::string& spki,
                       long errorCode, const std::string& subject, const std::string& notAfter);

    // --- UI-thread side ---
    std::optional<PendingPinDecision> takePending(); // returns + clears the slot
    void acceptPin(const PendingPinDecision& decision);  // persist the pin
    void declinePin(const PendingPinDecision& decision); // remember for this session
    // Forget session declines so a previously-declined cert can prompt again.
    // Called on an EXPLICIT user retry (Reconnect / Try Again) -- a deliberate
    // retry is a fresh trust decision; auto-reconnects must NOT call this.
    void clearSessionDeclines();

    // Drop ALL in-memory session state (staged pending decision + declines).
    // Used by Forget Settings: nothing from the pre-wipe trust session may leak
    // into the fresh onboarding (persisted pins are wiped with the store).
    void reset();

private:
    std::shared_ptr<SettingsStore> store_;
    mutable std::mutex mutex_;
    std::optional<PendingPinDecision> pending_;
    std::set<std::string> declined_; // "host|spki" the user said no to this session
};

// Process-wide registration: main owns the instance and registers it here so the
// verify callback (deep in OpenSSL, no user pointer) can reach it.
void setCertPinStore(CertPinStore* store);
CertPinStore* certPinStore();

// Install the pinning server-verify callback on ctx (no-op if no store is
// registered). Called from configureSslContext when server verification is on.
// `useSystemStore` records whether this context trusts the OS store (vs an
// explicit PEM ca): on iOS the callback consults the OS trust (SecTrust) as a
// fallback ONLY in system-store mode -- a user-configured private CA must stay
// authoritative, exactly as on desktop.
void installPinningVerify(SSL_CTX* ctx, bool useSystemStore);

// Per-connection: turn on hostname verification (SSL_set1_host) and stash the
// host so the callback can scope/label the pin. Call after SNI, before the
// handshake. Harmless under verify_none (no verification runs).
void prepareConnectionPinning(SSL* ssl, const std::string& host);

} // namespace gig
