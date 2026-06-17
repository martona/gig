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
    std::string streamUrlTemplate;
    std::string user;               // login; needs both halves + baseUrl
    std::string password;
    int loginRefreshSeconds = 600;
    bool softwareDecode = false;
    int pollIntervalSeconds = 5;
    TlsOptions tls;
};

struct ApplyResult {
    bool ok = false;
    std::string error;
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

    // Per-frame snapshot + live stats, passed through to the current supervisor
    // (safe zero/empty values when stopped).
    std::vector<std::shared_ptr<VideoFrame>> snapshotFrames() const;
    std::size_t cameraCount() const;
    std::uint64_t totalDecodedFrames() const;
    int liveCameraCount() const;
    int ingestKbps() const;

private:
    std::shared_ptr<D3D11DecodeContext> decodeContext_;
    std::shared_ptr<TlsSessionCache> sessionCache_;
    std::shared_ptr<CookieJar> cookieJar_;

    std::unique_ptr<FrigateAuth> auth_;
    std::unique_ptr<CameraSupervisor> supervisor_;
    std::vector<std::string> cameraLabels_;
};

} // namespace gig
