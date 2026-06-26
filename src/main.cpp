#include "app/app_session.h"
#include "log.hpp"
#include "net/cert_pin.hpp"
#include "net/cookie_jar.hpp"
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
#include <umbra.h>
#include "ui/settings_dialog.h"
#endif

namespace {

#ifdef _WIN32
// UTF-8 -> UTF-16 for the wide-string Win32/umbra APIs (e.g. DarkMessageBox).
std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

// Ask whether to pin an untrusted / changed server certificate. "No" is the
// default button (safer). Returns true to pin.
bool promptPinDecision(void* parentHwnd, const gig::PendingPinDecision& decision)
{
    std::string message;
    if (decision.changed) {
        message = "WARNING: the TLS certificate for " + decision.host + " has CHANGED.\n\n"
            "Previously pinned (SPKI-SHA256):\n  " + decision.previousSpki + "\n"
            "Now presented:\n  " + decision.spki + "\n\n"
            "Subject: " + decision.subject + "\n"
            "Expires: " + decision.notAfter + "\n"
            "Reason:  " + decision.errorText + "\n\n"
            "This can be a normal renewal -- or an interception attempt. "
            "Pin the new certificate and trust it?";
    } else {
        message = "The TLS certificate for " + decision.host + " is not trusted.\n\n"
            "Reason:  " + decision.errorText + "\n"
            "Subject: " + decision.subject + "\n"
            "Expires: " + decision.notAfter + "\n"
            "SPKI-SHA256:\n  " + decision.spki + "\n\n"
            "Pin this certificate and trust it from now on?";
    }
    const UINT icon = decision.changed ? MB_ICONWARNING : MB_ICONQUESTION;
    const int result = umbra::DarkMessageBox(static_cast<HWND>(parentHwnd), widen(message).c_str(),
        L"gig - certificate", MB_YESNO | icon | MB_DEFBUTTON2);
    return result == IDYES;
}

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
    gig::AppSession* session = nullptr;
};

// Runs synchronously while SDL pumps messages -- including inside Windows' modal
// move/resize loop, where the normal main loop is blocked. Re-rendering here
// keeps the grid reflowing live as the window is dragged, instead of only when
// the drag ends.
bool SDLCALL liveResizeWatch(void* userdata, SDL_Event* event)
{
    auto* context = static_cast<ResizeWatchContext*>(userdata);
    if (!context || !context->renderer || !context->session) {
        return true;
    }
    switch (event->type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        context->renderer->resize();
        context->renderer->render(context->session->snapshotFrames());
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        context->renderer->render(context->session->snapshotFrames());
        break;
    default:
        break;
    }
    return true;
}

// All startup settings: the reconfigurable connection/decode set (handed to
// AppSession) plus the UI-only diagnostics overlay (applied to the renderer).
struct StartupConfig {
    gig::AppConfig session;
    bool showOverlay = true;
    LabelMode labelMode = LabelMode::ErrorOnly;
};

// Read all settings from the platform store, applying the same derivation +
// validation the ini loader did. Missing values fall back to defaults; the store
// is the sole configuration source. The password is the one DPAPI-encrypted value.
StartupConfig loadConfig(const gig::SettingsStore& store)
{
    StartupConfig cfg;
    gig::AppConfig& s = cfg.session;
    s.baseUrl = store.getString("base").value_or(std::string());
    s.url = store.getString("url").value_or(std::string());
    s.streamUrlTemplate = store.getString("stream-url").value_or(std::string());
    s.user = store.getString("user").value_or(std::string());
    s.password = store.getString("password", /*encrypted=*/true).value_or(std::string());
    s.loginRefreshSeconds = static_cast<int>(store.getInt("login-refresh").value_or(600));
    s.tls.caFile = store.getString("ca").value_or(std::string());
    s.tls.certFile = store.getString("cert").value_or(std::string());
    s.tls.keyFile = store.getString("key").value_or(std::string());
    s.softwareDecode = store.getBool("software").value_or(false);
    cfg.showOverlay = store.getBool("overlay").value_or(false); // debug tile off by default; status lives in the toolbar
    const int labelMode = static_cast<int>(store.getInt("cam-labels").value_or(1)); // default: show on error only
    cfg.labelMode = static_cast<LabelMode>((labelMode >= 0 && labelMode <= 2) ? labelMode : 1);
    s.tls.verifyServer = !store.getBool("insecure").value_or(false);
    s.pollIntervalSeconds = static_cast<int>(store.getInt("poll-interval").value_or(5));
    s.tls.rwTimeoutUs = store.getInt("rw-timeout-us").value_or(10'000'000);

    // The Windows certificate store is implicit when no PEM material is given:
    // store trust roots for server verification + a CurrentUser\MY client cert.
    s.tls.useWindowsStore = s.tls.caFile.empty()
        && s.tls.certFile.empty()
        && s.tls.keyFile.empty();

    // Frigate login needs both credential halves and a base URL to POST to.
    if (s.user.empty() != s.password.empty()) {
        gig::logWarning() << "settings: 'user' and 'password' must both be set; ignoring login auth";
        s.user.clear();
        s.password.clear();
    }
    if (!s.user.empty() && s.baseUrl.empty()) {
        gig::logWarning() << "settings: user/password needs 'base' for the login endpoint; ignoring login auth";
        s.user.clear();
        s.password.clear();
    }
    if (!s.user.empty() && s.loginRefreshSeconds < 10) {
        gig::logWarning() << "settings: login-refresh below 10s; clamping to 10";
        s.loginRefreshSeconds = 10;
    }

    return cfg;
}

// Persist the editable settings back to the store (mirror of loadConfig's keys).
// The password is DPAPI-encrypted; an empty password removes the value rather
// than storing an empty blob. useWindowsStore is derived on load, never stored.
void saveConfig(gig::SettingsStore& store, const gig::AppConfig& s, bool showOverlay, LabelMode labelMode)
{
    store.setString("base", s.baseUrl);
    store.setString("url", s.url);
    store.setString("stream-url", s.streamUrlTemplate);
    store.setString("user", s.user);
    if (s.password.empty()) {
        store.remove("password");
    } else {
        store.setString("password", s.password, /*encrypt=*/true);
    }
    store.setInt("login-refresh", s.loginRefreshSeconds);
    store.setString("ca", s.tls.caFile);
    store.setString("cert", s.tls.certFile);
    store.setString("key", s.tls.keyFile);
    store.setBool("software", s.softwareDecode);
    store.setBool("overlay", showOverlay);
    store.setInt("cam-labels", static_cast<int>(labelMode));
    store.setBool("insecure", !s.tls.verifyServer);
    store.setInt("poll-interval", s.pollIntervalSeconds);
    store.setInt("rw-timeout-us", s.tls.rwTimeoutUs);
}

// Last window placement, persisted across runs. x/y/w/h hold the *normal*
// (non-maximized) rectangle so un-maximizing returns somewhere sane.
struct WindowGeometry {
    int x = 0;
    int y = 0;
    int w = 1280;
    int h = 720;
    bool maximized = false;
    bool valid = false; // a complete record was loaded
};

WindowGeometry loadWindowGeometry(const gig::SettingsStore& store)
{
    WindowGeometry g;
    const auto x = store.getInt("window-x");
    const auto y = store.getInt("window-y");
    const auto w = store.getInt("window-w");
    const auto h = store.getInt("window-h");
    if (x && y && w && h) {
        g.x = static_cast<int>(*x);
        g.y = static_cast<int>(*y);
        g.w = static_cast<int>(*w);
        g.h = static_cast<int>(*h);
        g.maximized = store.getBool("window-maximized").value_or(false);
        g.valid = true;
    }
    return g;
}

void saveWindowGeometry(gig::SettingsStore& store, const WindowGeometry& g)
{
    store.setInt("window-x", g.x);
    store.setInt("window-y", g.y);
    store.setInt("window-w", g.w);
    store.setInt("window-h", g.h);
    store.setBool("window-maximized", g.maximized);
}

// Usable only if the size is sane AND the title bar can be grabbed on some
// currently-connected display -- guards against a monitor unplugged or a
// resolution change since last run that would strand the window off-screen.
bool geometryUsable(const WindowGeometry& g)
{
    if (g.w < 320 || g.h < 240 || g.w > 16384 || g.h > 16384) {
        return false;
    }
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays || count <= 0) {
        SDL_free(displays);
        return false;
    }
    const int grabX = g.x + g.w / 2; // title-bar center
    const int grabY = g.y + 16;      // ~title-bar height (points)
    bool onScreen = false;
    for (int i = 0; i < count && !onScreen; ++i) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(displays[i], &bounds)
            && grabX >= bounds.x && grabX < bounds.x + bounds.w
            && grabY >= bounds.y && grabY < bounds.y + bounds.h) {
            onScreen = true;
        }
    }
    SDL_free(displays);
    return onScreen;
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
#ifdef _WIN32
        // Before any window: dark title bar + dark common dialogs/message boxes
        // (so even a fatal config error below shows a dark box).
        umbra::initDarkMode();
#endif
        auto settings = gig::openSettingsStore();
        if (!settings->getInt("schema-version")) {
            settings->setInt("schema-version", 1); // first run: stamp for future migrations
        }
        StartupConfig cfg = loadConfig(*settings);

        // Cert pinning is process-wide: the TLS verify callback (deep in OpenSSL)
        // reaches this via certPinStore(). Register before any TLS connection.
        gig::CertPinStore pinStore(settings);
        gig::setCertPinStore(&pinStore);

        SdlLifetime sdl;

        // Restore the last window placement, validated against the current
        // displays. Create hidden, position, (maybe) maximize, then show -- so a
        // restored off-center window doesn't flash at the default spot first.
        WindowGeometry geom = loadWindowGeometry(*settings);
        const bool useGeom = geom.valid && geometryUsable(geom);
        if (geom.valid && !useGeom) {
            gig::logInfo() << "saved window geometry is off-screen/invalid; using default";
        }

        auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
            SDL_CreateWindow(
                "gig",
                useGeom ? geom.w : 1280,
                useGeom ? geom.h : 720,
                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN),
            SDL_DestroyWindow);

        if (!window) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

        if (useGeom) {
            SDL_SetWindowPosition(window.get(), geom.x, geom.y);
            if (geom.maximized) {
                SDL_MaximizeWindow(window.get());
            }
        }
        SDL_ShowWindow(window.get());

        // Track the live *normal* rectangle (updated on move/resize while neither
        // maximized nor minimized) so we can persist it at shutdown.
        WindowGeometry liveGeom;
        if (useGeom) {
            liveGeom = geom;
        } else {
            SDL_GetWindowPosition(window.get(), &liveGeom.x, &liveGeom.y);
            SDL_GetWindowSize(window.get(), &liveGeom.w, &liveGeom.h);
        }

        void* mainHwnd = nullptr; // for owning the reconnect message box to the window
#ifdef _WIN32
        // Own the Windows cert picker + CNG consent / key-access prompts to our
        // window so they are modal to it: a disabled main window can't be closed
        // mid-prompt, which would otherwise deadlock shutdown (join) against a
        // thread blocked in the prompt.
        if (const SDL_PropertiesID windowProps = SDL_GetWindowProperties(window.get())) {
            mainHwnd = SDL_GetPointerProperty(windowProps, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            gig::setConsentParentWindow(mainHwnd);
            if (mainHwnd) {
                umbra::setDarkWndNotifySafe(static_cast<HWND>(mainHwnd)); // dark title bar + ctl-color
            }
        }
#endif
        (void)mainHwnd;

        auto renderer = createD3D11Renderer();
        if (!renderer->initialize(window.get())) {
            return 1;
        }

        // The app-lifetime TLS resumption pool + cookie jar, shared across the
        // control plane and video and preserved across reconnects.
        auto sessionCache = std::make_shared<gig::TlsSessionCache>();
        auto cookieJar = std::make_shared<gig::CookieJar>();

        // The reconfigurable subsystem (login -> discover -> supervisor). Bring
        // it up once here; F5 (and, later, the settings dialog) re-applies it
        // live. A startup failure is fatal -- there's no dialog yet to fix it in.
        gig::AppSession session(renderer->d3d11DecodeContext(), sessionCache, cookieJar);
        gig::ApplyResult applied = session.applyConfig(cfg.session);
#ifdef _WIN32
        while (!applied.ok) {
            // A certificate-trust failure is a pin decision, not a config problem.
            if (auto pending = pinStore.takePending()) {
                if (promptPinDecision(mainHwnd, *pending)) {
                    pinStore.acceptPin(*pending);
                    gig::logInfo() << "pinned certificate for " << pending->host;
                } else {
                    pinStore.declinePin(*pending);
                    throw std::runtime_error("certificate for " + pending->host + " was not trusted");
                }
                applied = session.applyConfig(cfg.session);
                continue;
            }
            // A transient connection failure (host down, login/discovery) must not
            // trap the user in a modal: come up disconnected and let the status
            // banner + Reconnect handle it. Only a structural/local config problem
            // opens the settings dialog.
            if (applied.failure != gig::ApplyFailure::Config) {
                gig::logWarning() << "initial connect failed (" << applied.error
                                  << "); starting disconnected -- use Reconnect to retry";
                break;
            }
            // First run / unusable config: let the user fix it in the settings
            // dialog instead of dying. Keep offering until it applies or cancel.
            gig::logWarning() << "config not usable (" << applied.error << "); opening settings";
            gig::AppConfig edited = cfg.session;
            bool overlay = cfg.showOverlay;
            int labelMode = static_cast<int>(cfg.labelMode);
            if (!gig::showSettingsDialog(static_cast<HWND>(mainHwnd), edited, overlay, labelMode, applied.error)) {
                throw std::runtime_error(applied.error); // cancelled -> nothing to show
            }
            saveConfig(*settings, edited, overlay, static_cast<LabelMode>(labelMode));
            cfg = loadConfig(*settings);
            applied = session.applyConfig(cfg.session);
        }
#else
        // No settings dialog off-Windows yet: a structural config error is fatal,
        // but a transient connection failure still comes up (disconnected) so it
        // can recover via Reconnect.
        if (!applied.ok && applied.failure == gig::ApplyFailure::Config) {
            throw std::runtime_error(applied.error);
        }
#endif
        renderer->setCameraLabels(session.cameraLabels());
        renderer->setLabelMode(cfg.labelMode);

        OverlayStats initialStats;
        initialStats.showDiagnostics = cfg.showOverlay;
        if (!applied.ok) {
            // Came up without a session (transient connect failure): show the
            // disconnected banner immediately, before the first stats tick.
            initialStats.link = OverlayStats::LinkState::Disconnected;
            initialStats.statusDetail = applied.error;
            initialStats.statusHost = cfg.session.baseUrl.empty() ? cfg.session.url : cfg.session.baseUrl;
        }
        renderer->setDiagnostics(initialStats);

        // Keep the grid reflowing while the window is actively resized.
        ResizeWatchContext resizeContext { renderer.get(), &session };
        SDL_AddEventWatch(liveResizeWatch, &resizeContext);

        bool running = true;
        auto lastTitleUpdate = std::chrono::steady_clock::now();
        auto lastStatsLog = lastTitleUpdate;
        std::uint64_t lastStatsFrames = 0;
        std::uint64_t lastTitleFrames = 0;
        double lastCpuPercent = 0.0;
        // Last connection error, shown in the status banner while disconnected.
        std::string lastConnectError = applied.ok ? std::string() : applied.error;
        constexpr auto frameInterval = std::chrono::milliseconds(16);

        // Reconnect with the given config (F5 / settings-apply): rebuild the
        // session, rebind the renderer's camera set, reset the running frame
        // deltas (the new supervisor counts from 0), report a failure.
        auto applyAndReport = [&](const gig::AppConfig& connConfig) {
            renderer->setFocusedTile(-1);
            const gig::ApplyResult result = session.applyConfig(connConfig);
            renderer->setCameraLabels(session.cameraLabels());
            lastTitleFrames = 0;
            lastStatsFrames = 0;
            lastConnectError = result.ok ? std::string() : result.error;
            if (!result.ok) {
                // Surfaced non-modally via the status banner -- no messagebox to
                // dismiss. The user can adjust settings or Reconnect.
                gig::logError() << "connect failed: " << result.error;
            }
        };

#ifdef _WIN32
        // Open the settings dialog, persist, and reconnect with the new config.
        // Shared by F2 and the toolbar's Settings button.
        auto openSettings = [&]() {
            gig::AppConfig edited = cfg.session;
            bool overlay = cfg.showOverlay;
            int labelMode = static_cast<int>(cfg.labelMode);
            if (gig::showSettingsDialog(static_cast<HWND>(mainHwnd), edited, overlay, labelMode)) {
                saveConfig(*settings, edited, overlay, static_cast<LabelMode>(labelMode));
                cfg = loadConfig(*settings); // re-derive useWindowsStore + re-validate
                renderer->setLabelMode(cfg.labelMode);
                OverlayStats overlayStats;
                overlayStats.showDiagnostics = cfg.showOverlay;
                renderer->setDiagnostics(overlayStats);
                applyAndReport(cfg.session);
            }
        };
#endif

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
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5) {
                    // Reconnect live with the current config -- no restart.
                    gig::logInfo() << "reconnect requested (F5)";
                    applyAndReport(cfg.session);
                    continue;
                }
#ifdef _WIN32
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F2) {
                    gig::logInfo() << "settings dialog (F2)";
                    openSettings();
                    continue;
                }
#endif
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
                            // Match the renderer's grid (cameras + optional diagnostics
                            // cell), including the strip the toolbar reserves at the top.
                            const std::size_t cameraCount = session.cameraCount();
                            const int effective = static_cast<int>(cameraCount) + (cfg.showOverlay ? 1 : 0);
                            const int gridTop = static_cast<int>(renderer->reservedTopLogical());
                            gig::GridLayout layout = gig::computeGridLayout(effective, windowWidth, windowHeight - gridTop);
                            for (gig::TileRect& tile : layout.tiles) {
                                tile.y += static_cast<float>(gridTop);
                            }
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
                            if (!focusedOne && cfg.showOverlay && cameraCount < layout.tiles.size()
                                && inCell(layout.tiles[cameraCount])) {
                                renderer->setLogViewVisible(!renderer->logViewVisible());
                            }
                        }
                    }
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    renderer->resize();
                }

                // Remember the normal (non-maximized/minimized) window rectangle.
                if (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED) {
                    const SDL_WindowFlags wflags = SDL_GetWindowFlags(window.get());
                    if (!(wflags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
                        SDL_GetWindowPosition(window.get(), &liveGeom.x, &liveGeom.y);
                        SDL_GetWindowSize(window.get(), &liveGeom.w, &liveGeom.h);
                    }
                }
            }

#ifdef _WIN32
            // A cert that went untrusted mid-session (e.g. it changed under us) is
            // staged by the verify callback; offer to pin it, then reconnect.
            if (auto pending = pinStore.takePending()) {
                if (promptPinDecision(mainHwnd, *pending)) {
                    pinStore.acceptPin(*pending);
                    gig::logInfo() << "pinned certificate for " << pending->host;
                    applyAndReport(cfg.session);
                } else {
                    pinStore.declinePin(*pending);
                    gig::logWarning() << "declined certificate for " << pending->host;
                }
            }
#endif

            const std::vector<std::shared_ptr<VideoFrame>> frames = session.snapshotFrames();
            renderer->setTileActivity(session.tileByteCounts()); // drives the per-tile signal animation
            renderer->render(frames);

            // Toolbar buttons route through the same paths as F2 / F5.
            switch (renderer->takeToolbarAction()) {
            case VideoRenderer::ToolbarAction::Reconnect:
                gig::logInfo() << "reconnect requested (toolbar)";
                applyAndReport(cfg.session);
                break;
#ifdef _WIN32
            case VideoRenderer::ToolbarAction::Settings:
                gig::logInfo() << "settings dialog (toolbar)";
                openSettings();
                break;
#endif
            default:
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - lastTitleUpdate > std::chrono::seconds(1)) {
                const double elapsed = std::chrono::duration<double>(now - lastTitleUpdate).count();
                lastTitleUpdate = now;
                const std::uint64_t total = session.totalDecodedFrames();
                const double fps = elapsed > 0.0 ? static_cast<double>(total - lastTitleFrames) / elapsed : 0.0;
                lastTitleFrames = total;

                OverlayStats stats;
                stats.showDiagnostics = cfg.showOverlay;
                stats.camerasOnline = session.liveCameraCount();
                stats.camerasOffline = static_cast<int>(session.cameraCount()) - session.liveCameraCount();
                stats.fps = fps;
                stats.framesTotal = total;
                stats.kbps = session.ingestKbps();
                stats.cpuPercent = sampleProcessCpuPercent();

                // Derived connection status for the toolbar indicator + banner.
                // A live session whose control-plane poll is failing is "reconnecting"
                // (its decoders + poll self-heal); no session at all is "disconnected".
                const gig::ControlPlaneStatus cp = session.controlPlaneStatus();
                if (!session.running()) {
                    stats.link = OverlayStats::LinkState::Disconnected;
                    stats.statusDetail = lastConnectError;
                } else if (cp.polling && !cp.ok) {
                    stats.link = OverlayStats::LinkState::Reconnecting;
                    stats.secondsSinceData = cp.secondsSinceOk;
                } else {
                    stats.link = OverlayStats::LinkState::Ok;
                }
                stats.healthDegraded = cp.schemaError;
                stats.statusHost = cfg.session.baseUrl.empty() ? cfg.session.url : cfg.session.baseUrl;

                renderer->setDiagnostics(stats);
                lastCpuPercent = stats.cpuPercent;
                // The OS title stays "gig"; live status shows in the toolbar.
            }

            if (now - lastStatsLog > std::chrono::seconds(5)) {
                const std::uint64_t total = session.totalDecodedFrames();
                const double seconds = std::chrono::duration<double>(now - lastStatsLog).count();
                const double fps = seconds > 0.0 ? static_cast<double>(total - lastStatsFrames) / seconds : 0.0;
                gig::logInfo() << "decoded " << total << " frames total ("
                               << static_cast<int>(fps + 0.5) << "/s), live "
                               << session.liveCameraCount() << "/" << session.cameraCount()
                               << ", ingest " << session.ingestKbps() << " kbps, cpu "
                               << static_cast<int>(lastCpuPercent + 0.5) << "%";
                lastStatsLog = now;
                lastStatsFrames = total;
            }

            // Pace to ~60 fps outside any lock; the renderer uses a non-waiting
            // Present so it never blocks while holding the shared D3D11 lock.
            std::this_thread::sleep_until(frameStart + frameInterval);
        }

        SDL_RemoveEventWatch(liveResizeWatch, &resizeContext);

        // Persist window placement for next launch. Query the live rectangle when
        // not maximized; otherwise keep the tracked normal rect + the maximized flag.
        {
            const SDL_WindowFlags wflags = SDL_GetWindowFlags(window.get());
            liveGeom.maximized = (wflags & SDL_WINDOW_MAXIMIZED) != 0;
            if (!liveGeom.maximized && !(wflags & SDL_WINDOW_MINIMIZED)) {
                SDL_GetWindowPosition(window.get(), &liveGeom.x, &liveGeom.y);
                SDL_GetWindowSize(window.get(), &liveGeom.w, &liveGeom.h);
            }
            saveWindowGeometry(*settings, liveGeom);
        }

        gig::logInfo() << "shutting down";
        session.stop();
        gig::setCertPinStore(nullptr); // no more TLS; unregister before pinStore dies
        return 0;
    } catch (const std::exception& error) {
        gig::logError() << "fatal: " << error.what();
#ifdef _WIN32
        umbra::DarkMessageBox(nullptr, widen(error.what()).c_str(), L"gig", MB_ICONERROR | MB_OK);
#endif
        return 1;
    }
}
