#include "app/camera_supervisor.h"
#include "decode/ffmpeg_decoder.h"
#include "discovery/frigate_discovery.h"
#include "log.hpp"
#include "net/cookie_jar.hpp"
#include "net/frigate_auth.hpp"
#include "net/http_client.hpp"
#include "net/tls_session_cache.hpp"
#include "net/win_cert_store.h"
#include "platform/settings_store.hpp"
#include "render/grid_layout.h"
#include "render/video_renderer.h"
#include "video_frame.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// Process CPU usage since the previous call, normalized so 100% == all cores busy.
double sampleProcessCpuPercent()
{
    static unsigned long long lastProc = 0;
    static unsigned long long lastWall = 0;
    static const unsigned cores = [] {
        const unsigned count = std::thread::hardware_concurrency();
        return count == 0 ? 1u : count;
    }();

    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    ULARGE_INTEGER kernelTime, userTime;
    kernelTime.LowPart = kernel.dwLowDateTime;
    kernelTime.HighPart = kernel.dwHighDateTime;
    userTime.LowPart = user.dwLowDateTime;
    userTime.HighPart = user.dwHighDateTime;
    const unsigned long long proc = kernelTime.QuadPart + userTime.QuadPart;

    FILETIME nowFileTime;
    GetSystemTimeAsFileTime(&nowFileTime);
    ULARGE_INTEGER wallTime;
    wallTime.LowPart = nowFileTime.dwLowDateTime;
    wallTime.HighPart = nowFileTime.dwHighDateTime;
    const unsigned long long wall = wallTime.QuadPart;

    double percent = 0.0;
    if (lastWall != 0 && wall > lastWall) {
        percent = static_cast<double>(proc - lastProc) / static_cast<double>(wall - lastWall) / cores * 100.0;
    }
    lastProc = proc;
    lastWall = wall;
    return percent;
}
#else
double sampleProcessCpuPercent() { return 0.0; }
#endif

struct ResizeWatchContext {
    VideoRenderer* renderer = nullptr;
    gig::CameraSupervisor* supervisor = nullptr;
};

// Runs synchronously while SDL pumps messages -- including inside Windows' modal
// move/resize loop, where the normal main loop is blocked. Re-rendering here
// keeps the grid reflowing live as the window is dragged, instead of only when
// the drag ends.
bool SDLCALL liveResizeWatch(void* userdata, SDL_Event* event)
{
    auto* context = static_cast<ResizeWatchContext*>(userdata);
    if (!context || !context->renderer || !context->supervisor) {
        return true;
    }
    switch (event->type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        context->renderer->resize();
        context->renderer->render(context->supervisor->snapshotFrames());
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        context->renderer->render(context->supervisor->snapshotFrames());
        break;
    default:
        break;
    }
    return true;
}

struct ProgramOptions {
    std::string baseUrl;          // Frigate control-plane base; enables multi-camera discovery
    std::string url;              // single stream, used only when baseUrl is empty
    std::string streamUrlTemplate;
    std::string user;             // Frigate username/password login; needs both + baseUrl
    std::string password;
    int loginRefreshSeconds = 600;
    bool softwareDecode = false;
    int pollIntervalSeconds = 5;
    bool showOverlay = true;
    TlsOptions tls;
};

// Read all settings from the platform store into ProgramOptions, applying the
// same derivation + validation the ini loader did. Missing values fall back to
// defaults; the store is the sole configuration source. The password is the one
// DPAPI-encrypted value.
ProgramOptions loadConfig(const gig::SettingsStore& store)
{
    ProgramOptions options;
    options.baseUrl = store.getString("base").value_or(std::string());
    options.url = store.getString("url").value_or(std::string());
    options.streamUrlTemplate = store.getString("stream-url").value_or(std::string());
    options.user = store.getString("user").value_or(std::string());
    options.password = store.getString("password", /*encrypted=*/true).value_or(std::string());
    options.loginRefreshSeconds = static_cast<int>(store.getInt("login-refresh").value_or(600));
    options.tls.caFile = store.getString("ca").value_or(std::string());
    options.tls.certFile = store.getString("cert").value_or(std::string());
    options.tls.keyFile = store.getString("key").value_or(std::string());
    options.softwareDecode = store.getBool("software").value_or(false);
    options.showOverlay = store.getBool("overlay").value_or(true);
    options.tls.verifyServer = !store.getBool("insecure").value_or(false);
    options.pollIntervalSeconds = static_cast<int>(store.getInt("poll-interval").value_or(5));
    options.tls.rwTimeoutUs = store.getInt("rw-timeout-us").value_or(10'000'000);

    // The Windows certificate store is implicit when no PEM material is given:
    // store trust roots for server verification + a CurrentUser\MY client cert.
    options.tls.useWindowsStore = options.tls.caFile.empty()
        && options.tls.certFile.empty()
        && options.tls.keyFile.empty();

    // Frigate login needs both credential halves and a base URL to POST to.
    if (options.user.empty() != options.password.empty()) {
        gig::logWarning() << "settings: 'user' and 'password' must both be set; ignoring login auth";
        options.user.clear();
        options.password.clear();
    }
    if (!options.user.empty() && options.baseUrl.empty()) {
        gig::logWarning() << "settings: user/password needs 'base' for the login endpoint; ignoring login auth";
        options.user.clear();
        options.password.clear();
    }
    if (!options.user.empty() && options.loginRefreshSeconds < 10) {
        gig::logWarning() << "settings: login-refresh below 10s; clamping to 10";
        options.loginRefreshSeconds = 10;
    }

    return options;
}

class SdlLifetime {
public:
    SdlLifetime()
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
    }

    ~SdlLifetime()
    {
        SDL_Quit();
    }
};

} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv; // GUI app; all configuration comes from the settings store
    try {
        auto settings = gig::openSettingsStore();
        if (!settings->getInt("schema-version")) {
            settings->setInt("schema-version", 1); // first run: stamp for future migrations
        }
        const ProgramOptions options = loadConfig(*settings);

        SdlLifetime sdl;

        auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
            SDL_CreateWindow(
                "gig",
                1280,
                720,
                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY),
            SDL_DestroyWindow);

        if (!window) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

#ifdef _WIN32
        // Own the Windows cert picker + CNG consent / key-access prompts to our
        // window so they are modal to it: a disabled main window can't be closed
        // mid-prompt, which would otherwise deadlock shutdown (join) against a
        // thread blocked in the prompt.
        if (const SDL_PropertiesID windowProps = SDL_GetWindowProperties(window.get())) {
            void* hwnd = SDL_GetPointerProperty(windowProps, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            gig::setConsentParentWindow(hwnd);
        }
#endif

        auto renderer = createD3D11Renderer();
        if (!renderer->initialize(window.get())) {
            return 1;
        }

        // Resolve the camera set: discover from --base, else fall back to the
        // single --url camera (legacy/manual mode).
        auto sessionCache = std::make_shared<gig::TlsSessionCache>();
        auto cookieJar = std::make_shared<gig::CookieJar>();

        // Native Frigate login: populate the shared cookie jar before the
        // first API call so discovery already carries the frigate_token.
        // Failure is fatal, like any other startup misconfiguration.
        std::unique_ptr<gig::FrigateAuth> auth;
        if (!options.user.empty()) {
            gig::FrigateAuthConfig authConfig;
            authConfig.baseUrl = options.baseUrl;
            authConfig.user = options.user;
            authConfig.password = options.password;
            authConfig.refreshInterval = std::chrono::seconds(options.loginRefreshSeconds);
            authConfig.tls = options.tls;
            auth = std::make_unique<gig::FrigateAuth>(authConfig, sessionCache, cookieJar);
            auth->loginOrThrow();
        }

        std::vector<gig::CameraStream> cameras;
        if (!options.baseUrl.empty()) {
            gig::HttpClient client(options.baseUrl, options.tls, sessionCache, cookieJar);
            cameras = gig::discoverCameras(client, options.streamUrlTemplate);
        } else if (!options.url.empty()) {
            cameras.push_back({ "camera", "", options.url });
            gig::logInfo() << "no base configured; single camera " << options.url;
        } else {
            // Nothing configured -- fail fast with a clear message instead of
            // spinning a doomed empty-URL decoder. (M5's settings dialog will
            // replace this with an open-the-dialog-on-first-run prompt.)
            throw std::runtime_error(
                "no Frigate connection configured -- set 'base' (or 'url') under HKCU\\Software\\gig");
        }
        if (cameras.empty()) {
            throw std::runtime_error("no cameras to display");
        }

        std::vector<std::string> cameraLabels;
        cameraLabels.reserve(cameras.size());
        for (const gig::CameraStream& camera : cameras) {
            cameraLabels.push_back(camera.streamName.empty() ? camera.cameraName : camera.streamName);
        }
        renderer->setCameraLabels(cameraLabels);

        gig::SupervisorConfig supervisorConfig;
        supervisorConfig.baseUrl = options.baseUrl;
        supervisorConfig.tls = options.tls;
        supervisorConfig.softwareDecode = options.softwareDecode;
        supervisorConfig.pollInterval = std::chrono::seconds(options.pollIntervalSeconds);

        gig::CameraSupervisor supervisor(
            std::move(cameras),
            supervisorConfig,
            renderer->d3d11DecodeContext(),
            sessionCache,
            cookieJar);
        supervisor.start();
        if (auth) {
            auth->startAutoRefresh();
        }

        OverlayStats initialStats;
        initialStats.showDiagnostics = options.showOverlay;
        renderer->setDiagnostics(initialStats);

        // Keep the grid reflowing while the window is actively resized.
        ResizeWatchContext resizeContext { renderer.get(), &supervisor };
        SDL_AddEventWatch(liveResizeWatch, &resizeContext);

        bool running = true;
        auto lastTitleUpdate = std::chrono::steady_clock::now();
        auto lastStatsLog = lastTitleUpdate;
        std::uint64_t lastStatsFrames = 0;
        std::uint64_t lastTitleFrames = 0;
        double lastCpuPercent = 0.0;
        constexpr auto frameInterval = std::chrono::milliseconds(16);

        while (running) {
            const auto frameStart = std::chrono::steady_clock::now();

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                const bool imguiUsed = renderer->handleEvent(event);

                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                    continue;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    // Esc closes the log view first (even if ImGui has keyboard focus),
                    // then leaves a focused tile, then quits.
                    if (renderer->logViewVisible()) {
                        renderer->setLogViewVisible(false);
                    } else if (renderer->focusedTile() >= 0) {
                        renderer->setFocusedTile(-1);
                    } else {
                        running = false;
                    }
                    continue;
                }
                if (imguiUsed) {
                    continue; // the log view (ImGui) consumed this event
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                    if (renderer->focusedTile() >= 0) {
                        renderer->setFocusedTile(-1); // any click returns to the grid
                    } else {
                        int windowWidth = 0;
                        int windowHeight = 0;
                        SDL_GetWindowSize(window.get(), &windowWidth, &windowHeight);
                        if (windowWidth > 0 && windowHeight > 0) {
                            // Match the renderer's grid (cameras + optional diagnostics cell).
                            const std::size_t cameraCount = supervisor.cameraCount();
                            const int effective = static_cast<int>(cameraCount) + (options.showOverlay ? 1 : 0);
                            const gig::GridLayout layout = gig::computeGridLayout(effective, windowWidth, windowHeight);
                            const auto inCell = [&](const gig::TileRect& cell) {
                                return event.button.x >= cell.x && event.button.x < cell.x + cell.width
                                    && event.button.y >= cell.y && event.button.y < cell.y + cell.height;
                            };
                            bool focusedOne = false;
                            for (std::size_t t = 0; t < layout.tiles.size() && t < cameraCount; ++t) {
                                if (inCell(layout.tiles[t])) {
                                    renderer->setFocusedTile(static_cast<int>(t));
                                    focusedOne = true;
                                    break;
                                }
                            }
                            // The synthetic diagnostics tile toggles the log view.
                            if (!focusedOne && options.showOverlay && cameraCount < layout.tiles.size()
                                && inCell(layout.tiles[cameraCount])) {
                                renderer->setLogViewVisible(!renderer->logViewVisible());
                            }
                        }
                    }
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    renderer->resize();
                }
            }

            const std::vector<std::shared_ptr<VideoFrame>> frames = supervisor.snapshotFrames();
            renderer->render(frames);

            const auto now = std::chrono::steady_clock::now();
            if (now - lastTitleUpdate > std::chrono::seconds(1)) {
                const double elapsed = std::chrono::duration<double>(now - lastTitleUpdate).count();
                lastTitleUpdate = now;
                const std::uint64_t total = supervisor.totalDecodedFrames();
                const double fps = elapsed > 0.0 ? static_cast<double>(total - lastTitleFrames) / elapsed : 0.0;
                lastTitleFrames = total;

                OverlayStats stats;
                stats.showDiagnostics = options.showOverlay;
                stats.camerasOnline = supervisor.liveCameraCount();
                stats.camerasOffline = static_cast<int>(supervisor.cameraCount()) - supervisor.liveCameraCount();
                stats.fps = fps;
                stats.framesTotal = total;
                stats.kbps = supervisor.ingestKbps();
                stats.cpuPercent = sampleProcessCpuPercent();
                renderer->setDiagnostics(stats);
                lastCpuPercent = stats.cpuPercent;

                std::string title = "gig - " + std::to_string(stats.camerasOnline)
                    + "/" + std::to_string(supervisor.cameraCount()) + " live - frames "
                    + std::to_string(total);
                SDL_SetWindowTitle(window.get(), title.c_str());
            }

            if (now - lastStatsLog > std::chrono::seconds(5)) {
                const std::uint64_t total = supervisor.totalDecodedFrames();
                const double seconds = std::chrono::duration<double>(now - lastStatsLog).count();
                const double fps = seconds > 0.0 ? static_cast<double>(total - lastStatsFrames) / seconds : 0.0;
                gig::logInfo() << "decoded " << total << " frames total ("
                               << static_cast<int>(fps + 0.5) << "/s), live "
                               << supervisor.liveCameraCount() << "/" << supervisor.cameraCount()
                               << ", ingest " << supervisor.ingestKbps() << " kbps, cpu "
                               << static_cast<int>(lastCpuPercent + 0.5) << "%";
                lastStatsLog = now;
                lastStatsFrames = total;
            }

            // Pace to ~60 fps outside any lock; the renderer uses a non-waiting
            // Present so it never blocks while holding the shared D3D11 lock.
            std::this_thread::sleep_until(frameStart + frameInterval);
        }

        SDL_RemoveEventWatch(liveResizeWatch, &resizeContext);
        gig::logInfo() << "shutting down";
        if (auth) {
            auth->stop();
        }
        supervisor.stop();
        return 0;
    } catch (const std::exception& error) {
        gig::logError() << "fatal: " << error.what();
#ifdef _WIN32
        MessageBoxA(nullptr, error.what(), "gig", MB_ICONERROR | MB_OK);
#endif
        return 1;
    }
}
