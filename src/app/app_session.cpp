#include "app/app_session.h"

#include "app/camera_supervisor.h"
#include "discovery/frigate_discovery.h"
#include "log.hpp"
#include "net/frigate_auth.hpp"
#include "net/http_client.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace gig {

AppSession::AppSession(
    std::shared_ptr<D3D11DecodeContext> decodeContext,
    std::shared_ptr<TlsSessionCache> sessionCache,
    std::shared_ptr<CookieJar> cookieJar)
    : decodeContext_(std::move(decodeContext))
    , sessionCache_(std::move(sessionCache))
    , cookieJar_(std::move(cookieJar))
{
}

AppSession::~AppSession()
{
    stop();
}

void AppSession::stop()
{
    // Order mirrors the old main(): refresh thread first, then decoders/poll.
    if (auth_) {
        auth_->stop();
        auth_.reset();
    }
    if (supervisor_) {
        supervisor_->stop();
        supervisor_.reset();
    }
    cameraLabels_.clear();
}

ApplyResult AppSession::applyConfig(const AppConfig& cfg)
{
    stop(); // tear down any current session before rebuilding

    try {
        // 1. Login first (if credentials are set) so the shared cookie jar
        //    already carries the frigate_token before discovery's first call.
        if (!cfg.user.empty()) {
            FrigateAuthConfig authConfig;
            authConfig.baseUrl = cfg.baseUrl;
            authConfig.user = cfg.user;
            authConfig.password = cfg.password;
            authConfig.refreshInterval = std::chrono::seconds(cfg.loginRefreshSeconds);
            authConfig.tls = cfg.tls;
            auto auth = std::make_unique<FrigateAuth>(authConfig, sessionCache_, cookieJar_);
            std::string loginError;
            if (!auth->login(&loginError)) {
                // Reaching/authenticating to Frigate failed -- transient (host
                // down, blip, or wrong creds the user can fix via Reconnect).
                return { false, "login failed: " + loginError, ApplyFailure::Transient };
            }
            logInfo() << "frigate auth: logged in to " << cfg.baseUrl << " as '" << cfg.user << "'";
            auth_ = std::move(auth);
        }

        // 2. Resolve the camera set: discover from base, else the single url.
        std::vector<CameraStream> cameras;
        if (!cfg.baseUrl.empty()) {
            HttpClient client(cfg.baseUrl, cfg.tls, sessionCache_, cookieJar_);
            try {
                cameras = discoverCameras(client, cfg.streamUrlTemplate);
            } catch (const std::exception& discoveryError) {
                // A discovery throw is a connection-time failure (unreachable,
                // bad response) -- transient, not a config problem.
                stop();
                return { false, std::string("discovery failed: ") + discoveryError.what(),
                         ApplyFailure::Transient };
            }
        } else if (!cfg.url.empty()) {
            cameras.push_back({ "camera", "", cfg.url });
            logInfo() << "no base configured; single camera " << cfg.url;
        } else {
            stop();
            return { false, "no Frigate connection configured -- set a base URL (or single stream URL)",
                     ApplyFailure::Config };
        }
        if (cameras.empty()) {
            stop();
            return { false, "connected, but Frigate reported no cameras", ApplyFailure::Transient };
        }

        // 3. Labels for the renderer (stable camera order).
        cameraLabels_.clear();
        cameraLabels_.reserve(cameras.size());
        for (const CameraStream& camera : cameras) {
            cameraLabels_.push_back(camera.streamName.empty() ? camera.cameraName : camera.streamName);
        }

        // 4. Supervisor (owns the decoders + the health poll).
        SupervisorConfig supervisorConfig;
        supervisorConfig.baseUrl = cfg.baseUrl;
        supervisorConfig.tls = cfg.tls;
        supervisorConfig.softwareDecode = cfg.softwareDecode;
        supervisorConfig.pollInterval = std::chrono::seconds(cfg.pollIntervalSeconds);
        supervisor_ = std::make_unique<CameraSupervisor>(
            std::move(cameras), supervisorConfig, decodeContext_, sessionCache_, cookieJar_);
        supervisor_->start();

        // 5. Start the login refresh only once everything else is up.
        if (auth_) {
            auth_->startAutoRefresh();
        }

        return { true, {}, ApplyFailure::None };
    } catch (const std::exception& error) {
        // The only throws left here are construction-time -- unreadable PEM/TLS
        // material in an HttpClient/TlsClient/FrigateAuth ctor. Those are local
        // config the user must fix in the settings dialog (network failures take
        // the Transient returns above, not this path).
        stop();
        return { false, error.what(), ApplyFailure::Config };
    }
}

std::vector<std::shared_ptr<VideoFrame>> AppSession::snapshotFrames() const
{
    return supervisor_ ? supervisor_->snapshotFrames() : std::vector<std::shared_ptr<VideoFrame>>{};
}

std::size_t AppSession::cameraCount() const
{
    return supervisor_ ? supervisor_->cameraCount() : 0;
}

std::uint64_t AppSession::totalDecodedFrames() const
{
    return supervisor_ ? supervisor_->totalDecodedFrames() : 0;
}

int AppSession::liveCameraCount() const
{
    return supervisor_ ? supervisor_->liveCameraCount() : 0;
}

int AppSession::ingestKbps() const
{
    return supervisor_ ? supervisor_->ingestKbps() : 0;
}

std::vector<std::uint64_t> AppSession::tileByteCounts() const
{
    return supervisor_ ? supervisor_->tileByteCounts() : std::vector<std::uint64_t>{};
}

ControlPlaneStatus AppSession::controlPlaneStatus() const
{
    ControlPlaneStatus status;
    if (supervisor_) {
        const CameraSupervisor::ControlPlaneHealth health = supervisor_->controlPlaneHealth();
        status.polling = health.polling;
        status.ok = health.ok;
        status.schemaError = health.schemaError;
        status.secondsSinceOk = health.secondsSinceOk;
    }
    return status;
}

} // namespace gig
