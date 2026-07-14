#pragma once

#include "d3d11_decode_context.h"
#include "net/tls_options.h"
#include "video_frame.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gig {

class TlsSessionCache;
class CookieJar;
class FrigateAuth;
class CameraSupervisor;

// The reconfigurable connection + decode settings -- everything a settings
// change can touch that requires a reconnect. UI-only settings (the diagnostics
// overlay) live outside this, applied to the renderer directly.
struct AppConfig {
    std::string baseUrl;            // Frigate control-plane base; enables discovery
    std::string url;                // single stream, used only when baseUrl is empty
    std::string user;               // login; needs both halves + baseUrl
    std::string password;
    int loginRefreshSeconds = 600;
    bool softwareDecode = false;
    int pollIntervalSeconds = 5;
    TlsOptions tls;
};

// Why an applyConfig() failed, so the caller can react without a one-size-fits-
// all modal: Config = structural/local problem (no connection set, unreadable
// TLS material) that needs the settings dialog; Auth = the server ANSWERED and
// rejected the login (4xx -- wrong credentials, an app-level problem: point the
// user at Settings, do NOT auto-retry); Transient = a network-level failure
// (host unreachable, timeout, TLS, 5xx, discovery) that is worth retrying
// automatically -- e.g. a switch port flapping under a hardwired iPad.
enum class ApplyFailure { None, Config, Auth, Transient };

struct ApplyResult {
    bool ok = false;
    std::string error;
    ApplyFailure failure = ApplyFailure::None;
};

// Control-plane (Frigate go2rtc) reachability for the status banner; safe
// defaults (polling=false, ok=true) when no session is running.
struct ControlPlaneStatus {
    bool polling = false;
    bool ok = true;
    bool schemaError = false;
    int secondsSinceOk = 0;
};

// Owns the reconfigurable subsystem -- Frigate login (FrigateAuth) + camera
// discovery + the CameraSupervisor -- behind one applyConfig() that tears the
// running session down and rebuilds it in place. This is what lets the settings
// dialog reconnect live instead of forcing a restart. The window, renderer, and
// the shared TlsSessionCache/CookieJar are app-lifetime and live outside.
//
// Not thread-safe by design: applyConfig(), stop(), and the snapshot/stat
// accessors are all called from the main/UI thread (applyConfig joins the
// worker threads, so it never races the run loop on the same thread).
class AppSession {
public:
    AppSession(std::shared_ptr<D3D11DecodeContext> decodeContext,
               std::shared_ptr<TlsSessionCache> sessionCache,
               std::shared_ptr<CookieJar> cookieJar);
    ~AppSession();

    AppSession(const AppSession&) = delete;
    AppSession& operator=(const AppSession&) = delete;

    // Stop any running session, then rebuild from cfg (login -> discover ->
    // supervisor) and start it. On failure leaves the session stopped (no
    // cameras) and returns the reason -- it never throws for config/login/
    // discovery errors, so a live reconfigure can fail without taking the app
    // down. Runs synchronously on the caller (main) thread.
    ApplyResult applyConfig(const AppConfig& cfg);

    // Tear everything down (joins the auth + supervisor threads).
    void stop();

    bool running() const { return supervisor_ != nullptr; }

    // Per-camera labels in stable camera order (empty when stopped). The caller
    // pushes these to the renderer after a successful applyConfig.
    const std::vector<std::string>& cameraLabels() const { return cameraLabels_; }

    // The raw Frigate camera names (/api/config keys) in the same order --
    // what the /ws activity topics are keyed by. May differ from the labels,
    // which prefer the go2rtc stream name.
    const std::vector<std::string>& cameraNames() const { return cameraNames_; }

    // Per-frame snapshot + live stats, passed through to the current supervisor
    // (safe zero/empty values when stopped).
    std::vector<std::shared_ptr<VideoFrame>> snapshotFrames() const;
    std::size_t cameraCount() const;
    std::uint64_t totalDecodedFrames() const;
    int liveCameraCount() const;
    int ingestKbps() const;

    // Control-plane reachability for the status UI (default/healthy when stopped).
    ControlPlaneStatus controlPlaneStatus() const;

    // Per-camera cumulative downloaded bytes (stable order) for the per-tile
    // "receiving" activity animation; empty when stopped.
    std::vector<std::uint64_t> tileByteCounts() const;

    // On-demand stream policy pass-through: tear down / reconnect one camera's
    // stream (see CameraSupervisor::setSlotEnabled). No-op when stopped; a
    // rebuilt session starts with every camera enabled.
    void setCameraStreamEnabled(std::size_t index, bool enabled);

private:
    std::shared_ptr<D3D11DecodeContext> decodeContext_;
    std::shared_ptr<TlsSessionCache> sessionCache_;
    std::shared_ptr<CookieJar> cookieJar_;

    std::unique_ptr<FrigateAuth> auth_;
    std::unique_ptr<CameraSupervisor> supervisor_;
    std::vector<std::string> cameraLabels_;
    std::vector<std::string> cameraNames_;
};

} // namespace gig
