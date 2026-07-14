#include "app/activity_gate.h"
#include "app/app_session.h"
#include "log.hpp"
#include "net/cert_pin.hpp"
#include "net/cookie_jar.hpp"
#include "net/frigate_events.hpp"
#include "net/tls_session_cache.hpp"
#include "net/win_cert_store.h"
#include "platform/settings_store.hpp"
#include "render/grid_layout.h"
#include "render/quiet_status.h"
#include "render/video_renderer.h"
#include "video_frame.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <umbra.h>
#else
#include <sys/resource.h> // getrusage (process CPU sampler)
#include <sys/time.h>     // gettimeofday
#endif
#if defined(_WIN32) || defined(__APPLE__)
#include "ui/settings_dialog.h" // native settings dialog (Win32 dialog / AppKit window)
#include "ui/pin_prompt.h"      // cert-trust prompt (Win32 message box / AppKit NSAlert)
#include "ui/app_menu.h"        // macOS application menu (About / Preferences / Quit)
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
// POSIX process CPU since the previous call, normalized so 100% == all cores busy
// (mirrors the Windows GetProcessTimes sampler). getrusage gives user+system time
// as timevals (unambiguous us); wall from gettimeofday.
double sampleProcessCpuPercent()
{
    static std::uint64_t lastProcUs = 0;
    static std::uint64_t lastWallUs = 0;
    static const unsigned cores = [] {
        const unsigned count = std::thread::hardware_concurrency();
        return count == 0 ? 1u : count;
    }();

    rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    const std::uint64_t procUs =
        static_cast<std::uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1'000'000ull
        + static_cast<std::uint64_t>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);

    timeval now {};
    gettimeofday(&now, nullptr);
    const std::uint64_t wallUs =
        static_cast<std::uint64_t>(now.tv_sec) * 1'000'000ull + static_cast<std::uint64_t>(now.tv_usec);

    double percent = 0.0;
    if (lastWallUs != 0 && wallUs > lastWallUs) {
        percent = static_cast<double>(procUs - lastProcUs) / static_cast<double>(wallUs - lastWallUs) / cores * 100.0;
    }
    lastProcUs = procUs;
    lastWallUs = wallUs;
    return percent;
}
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
    LabelMode labelMode = LabelMode::ErrorOnly;
    // Burn-in idle dim: after dimDelaySeconds without interaction, the video
    // ramps to dimLevelPercent luminance (0 delay = never dim).
    int dimLevelPercent = 60;
    int dimDelaySeconds = 600;
    int orbitStepSeconds = 40; // burn-in pixel-orbit step interval
    // View mode: 0 = all cameras (the classic wall), 1 = active cameras only
    // (tiles appear/disappear with Frigate's real-time activity feed).
    int viewMode = 0;
    // Whether raw motion counts as activity (opt-in: wind-blown shadows
    // trigger it); tracked objects (<cam>/all) always count.
    bool motionActivity = false;
    // Ignore STATIONARY objects (default ON -- the less annoying path):
    // activity uses Frigate's /active counters, so a parked car or a package
    // settled on the doorstep stops counting ~10s after it stops moving.
    // Opt out for presence semantics (a motionless loiterer stays visible).
    bool activeOnly = true;
    // Keep off-screen cameras streaming (default). Off = tear a hidden
    // camera's stream down after a short delay and reconnect when it appears
    // (saves decode power; costs ~1-2s + the scope animation on wake).
    bool keepHiddenStreams = true;
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
    s.user = store.getString("user").value_or(std::string());
    s.password = store.getString("password", /*encrypted=*/true).value_or(std::string());
    s.loginRefreshSeconds = static_cast<int>(store.getInt("login-refresh").value_or(600));
    s.tls.caFile = store.getString("ca").value_or(std::string());
    s.tls.certFile = store.getString("cert").value_or(std::string());
    s.tls.keyFile = store.getString("key").value_or(std::string());
    s.softwareDecode = store.getBool("software").value_or(false);
    const int labelMode = static_cast<int>(store.getInt("cam-labels").value_or(1)); // default: show on error only
    cfg.labelMode = static_cast<LabelMode>((labelMode >= 0 && labelMode <= 2) ? labelMode : 1);
    cfg.dimLevelPercent = std::clamp(static_cast<int>(store.getInt("dim-level").value_or(60)), 10, 100);
    cfg.dimDelaySeconds = std::max(0, static_cast<int>(store.getInt("dim-delay").value_or(600)));
    cfg.orbitStepSeconds = std::clamp(static_cast<int>(store.getInt("orbit-step").value_or(40)), 1, 600);
    cfg.viewMode = std::clamp(static_cast<int>(store.getInt("view-mode").value_or(0)), 0, 1);
    cfg.motionActivity = store.getBool("motion-activity").value_or(false);
    cfg.activeOnly = store.getBool("active-only").value_or(true);
    cfg.keepHiddenStreams = store.getBool("stream-hidden").value_or(true);
    s.tls.verifyServer = !store.getBool("insecure").value_or(false);
    s.pollIntervalSeconds = static_cast<int>(store.getInt("poll-interval").value_or(5));
    s.tls.rwTimeoutUs = store.getInt("rw-timeout-us").value_or(10'000'000);

    // Use the OS trust store implicitly when no PEM material is given: Windows cert
    // store, macOS keychain roots (built-in + user/admin-trusted CAs), or OpenSSL's
    // default verify paths elsewhere. The platform-specific loading lives in
    // configureSslContext; this is just the derivation.
    s.tls.useSystemStore = s.tls.caFile.empty()
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
// than storing an empty blob. useSystemStore is derived on load, never stored.
void saveConfig(gig::SettingsStore& store, const gig::AppConfig& s, LabelMode labelMode,
                int dimLevelPercent, int dimDelaySeconds, int orbitStepSeconds,
                int viewMode, bool motionActivity, bool activeOnly, bool keepHiddenStreams)
{
    store.setInt("dim-level", dimLevelPercent);
    store.setInt("dim-delay", dimDelaySeconds);
    store.setInt("orbit-step", orbitStepSeconds);
    store.setInt("view-mode", viewMode);
    store.setBool("motion-activity", motionActivity);
    store.setBool("active-only", activeOnly);
    store.setBool("stream-hidden", keepHiddenStreams);
    store.setString("base", s.baseUrl);
    store.setString("url", s.url);
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
// The "all quiet" liveness line for the current wall-clock minute.
std::string quietLineNow(int camerasDown)
{
    const std::time_t now = std::time(nullptr);
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return gig::quietStatusLine(local, camerasDown);
}

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

        SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#ifdef __APPLE__
        windowFlags |= SDL_WINDOW_METAL; // the Metal renderer attaches a CAMetalLayer via SDL_Metal_CreateView
#endif
        auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
            SDL_CreateWindow(
                "gig",
                useGeom ? geom.w : 1280,
                useGeom ? geom.h : 720,
                windowFlags),
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

        auto renderer = createRenderer();
        if (!renderer->initialize(window.get())) {
            return 1;
        }

#ifdef __APPLE__
        // Native macOS menu bar: About / Preferences (Cmd-,) / Quit (Cmd-Q). The
        // Preferences item pushes this SDL event; the run loop opens the dialog below.
        const Uint32 prefsEventType = installAppMenu();
#endif

        // The app-lifetime TLS resumption pool + cookie jar, shared across the
        // control plane and video and preserved across reconnects.
        auto sessionCache = std::make_shared<gig::TlsSessionCache>();
        auto cookieJar = std::make_shared<gig::CookieJar>();

        // The reconfigurable subsystem (login -> discover -> supervisor). Bring it
        // up once here; F5 / the settings dialog re-apply it live. There is no
        // modal-settings loop anymore: first run (nothing configured) lands on the
        // welcome screen, any connect failure lands on the error screen -- both
        // drawn full-window by the renderer's status panel, with Settings / Try
        // Again CTAs. Only the certificate pin decision stays a modal prompt.
        gig::AppSession session(renderer->d3d11DecodeContext(), sessionCache, cookieJar);
        auto configEmpty = [&]() { return cfg.session.baseUrl.empty() && cfg.session.url.empty(); };
        bool lastFailureWasConfig = false;
        bool lastFailureWasAuth = false;

        // Auto-reconnect for network-transient failures (a flapping switch port
        // must not need a human): doubling backoff 5s -> 60s while the error
        // screen is up. Config errors and server-rejected logins never auto-retry.
        bool autoRetryArmed = false;
        int autoRetryDelaySeconds = 0;
        auto autoRetryAt = std::chrono::steady_clock::time_point{};
        auto scheduleAutoRetry = [&]() {
            autoRetryDelaySeconds = autoRetryDelaySeconds == 0 ? 5 : std::min(autoRetryDelaySeconds * 2, 60);
            autoRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(autoRetryDelaySeconds);
            autoRetryArmed = true;
            gig::logInfo() << "auto-reconnect in " << autoRetryDelaySeconds << "s";
        };

        // Activity view: the /ws feed + the gate that turns it into a visible
        // tile subset. visibleTiles is what the renderer currently shows
        // (camera indices, all of them outside activity mode); it doubles as
        // the map from renderer tile index -> camera slot for hit-testing.
        gig::ActivityGate activityGate;
        gig::StreamPolicy streamPolicy;
        std::unique_ptr<gig::FrigateEvents> activityFeed;
        std::vector<int> visibleTiles;
        std::vector<std::string> tileReasons; // last pushed, subset-aligned
        auto restartActivityFeed = [&]() {
            activityFeed.reset();
            activityGate.reset();
            streamPolicy.reset(); // a rebuilt supervisor starts fully enabled
            visibleTiles.clear(); // force a re-derive (and label re-push) next tick
            tileReasons.clear();
            if (session.running() && !cfg.session.baseUrl.empty()) {
                activityFeed = std::make_unique<gig::FrigateEvents>(
                    cfg.session.baseUrl, cfg.session.tls, sessionCache, cookieJar);
                activityFeed->start(session.cameraNames());
            }
        };

        gig::ApplyResult applied;
        if (!configEmpty()) {
            applied = session.applyConfig(cfg.session);
#if defined(_WIN32) || defined(__APPLE__)
            while (!applied.ok) {
                // A certificate-trust failure is a pin decision, not a config problem.
                if (auto pending = pinStore.takePending()) {
                    if (gig::promptPinDecision(mainHwnd, *pending)) {
                        pinStore.acceptPin(*pending);
                        gig::logInfo() << "pinned certificate for " << pending->host;
                        applied = session.applyConfig(cfg.session);
                        continue;
                    }
                    // Declined: come up on the error screen (Try Again offers a
                    // fresh decision -- it clears the session declines).
                    pinStore.declinePin(*pending);
                    applied.error = "certificate for " + pending->host + " was not trusted";
                    applied.failure = gig::ApplyFailure::Transient;
                }
                break;
            }
#endif
            if (!applied.ok) {
                gig::logWarning() << "initial connect failed (" << applied.error
                                  << "); starting on the error screen";
                lastFailureWasConfig = (applied.failure == gig::ApplyFailure::Config);
                lastFailureWasAuth = (applied.failure == gig::ApplyFailure::Auth);
                if (applied.failure == gig::ApplyFailure::Transient) {
                    scheduleAutoRetry();
                }
            }
        }
        restartActivityFeed();
        renderer->setCameraLabels(session.cameraLabels());
        renderer->setLabelMode(cfg.labelMode);

        // Derive the full-window status screen (status panel) from session/config
        // state. None while a session is up -- the slim banner handles in-session
        // degradation; the panel only replaces dead air.
        auto applyScreenState = [&](OverlayStats& stats) {
            if (session.running()) {
                stats.screen = OverlayStats::StatusScreen::None;
            } else if (configEmpty()) {
                stats.screen = OverlayStats::StatusScreen::Welcome;
            } else {
                stats.screen = OverlayStats::StatusScreen::Error;
                stats.errorIsConfig = lastFailureWasConfig;
                stats.errorIsAuth = lastFailureWasAuth;
                stats.autoRetryPending = autoRetryArmed;
            }
        };

        OverlayStats initialStats;
        if (!applied.ok) {
            // Came up without a session: welcome or error screen, immediately.
            initialStats.link = OverlayStats::LinkState::Disconnected;
            initialStats.statusDetail = applied.error;
            initialStats.statusHost = cfg.session.baseUrl.empty() ? cfg.session.url : cfg.session.baseUrl;
        }
        applyScreenState(initialStats);
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

        // On-demand rendering: only draw when something visibly changed -- a new
        // decoded frame, an animation in flight (renderer->isAnimating()), recent
        // input (so ImGui hover/tooltips settle), or a stats refresh -- and never
        // while minimized. Steady live video advances totalDecodedFrames() every
        // tick, so a full grid still renders ~as before; the savings are in focus
        // view, idle/offline tiles, and a minimized/occluded window (where the
        // decoders keep running for instant restore but nothing is presented).
        constexpr auto inputRenderTail = std::chrono::milliseconds(1000);
        std::uint64_t lastRenderedFrames = 0;
        auto lastInteraction = std::chrono::steady_clock::now();
        // A camera becoming active wakes a dimmed wall (that's the point of a
        // security monitor at 3am) -- but deliberately does NOT count as user
        // interaction, or every event would also un-hide chrome and peek the
        // full grid.
        auto lastActivityWake = lastInteraction;

        // Idle-dim (burn-in): ramp the video luminance to the configured level
        // after the configured idle delay; snap back to full on any interaction.
        float currentDim = 1.0f;

        // Reconnect with the given config: user-initiated (F5 / settings-apply /
        // Try Again -- fresh backoff + fresh trust decision) or an automatic
        // retry (keeps the backoff, keeps session pin declines).
        auto applyAndReport = [&](const gig::AppConfig& connConfig, bool userInitiated = true) {
            renderer->setFocusedTile(-1);
            if (userInitiated) {
                // An explicit retry is a fresh trust decision: a previously
                // declined certificate may prompt again. And a fresh backoff.
                pinStore.clearSessionDeclines();
                autoRetryDelaySeconds = 0;
            }
            autoRetryArmed = false;
            {
                // applyConfig blocks this thread (login/discovery); put the
                // connecting screen up for the duration by rendering one frame.
                OverlayStats connecting;
                connecting.screen = OverlayStats::StatusScreen::Connecting;
                connecting.statusHost = connConfig.baseUrl.empty() ? connConfig.url : connConfig.baseUrl;
                renderer->setDiagnostics(connecting);
                renderer->render(session.snapshotFrames());
            }
            const gig::ApplyResult result = session.applyConfig(connConfig);
            restartActivityFeed();
            renderer->setCameraLabels(session.cameraLabels());
            lastTitleFrames = 0;
            lastStatsFrames = 0;
            lastConnectError = result.ok ? std::string() : result.error;
            lastFailureWasConfig = !result.ok && result.failure == gig::ApplyFailure::Config;
            lastFailureWasAuth = !result.ok && result.failure == gig::ApplyFailure::Auth;
            if (!result.ok) {
                // Surfaced non-modally via the error screen -- no messagebox.
                gig::logError() << "connect failed: " << result.error;
                if (result.failure == gig::ApplyFailure::Transient) {
                    scheduleAutoRetry(); // network-level: keep trying on our own
                }
            }
            // Reflect the outcome immediately (don't wait for the 1s stats tick).
            OverlayStats after;
            after.link = session.running() ? OverlayStats::LinkState::Ok
                                           : OverlayStats::LinkState::Disconnected;
            after.statusDetail = lastConnectError;
            after.statusHost = cfg.session.baseUrl.empty() ? cfg.session.url : cfg.session.baseUrl;
            applyScreenState(after);
            renderer->setDiagnostics(after);
        };

        // Camera tile under a window-space point, or -1. The renderer owns the
        // laid-out rects (which include the burn-in orbit offset only it knows),
        // so hit-testing asks it.
        // NOTE: returns the renderer TILE index (an index into visibleTiles),
        // which equals the camera slot only outside activity mode.
        auto cameraTileAt = [&](float x, float y) -> int {
            const int cell = renderer->hitTestCell(x, y);
            return (cell >= 0 && cell < static_cast<int>(visibleTiles.size())) ? cell : -1;
        };

        // True fullscreen (borderless desktop), distinct from maximization.
        // Enter/exit: F11 or the toolbar button; Esc also exits (after closing
        // the log view / leaving a focused tile).
        auto isFullscreen = [&]() {
            return (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_FULLSCREEN) != 0;
        };
        auto toggleFullscreen = [&]() {
            if (!SDL_SetWindowFullscreen(window.get(), !isFullscreen())) {
                gig::logWarning() << "fullscreen toggle failed: " << SDL_GetError();
            }
        };

#if defined(_WIN32) || defined(__APPLE__)
        // Open the settings dialog, persist, and reconnect with the new config.
        // Shared by F2, the toolbar's Settings button, and the status panel's CTAs.
        auto openSettings = [&]() {
            gig::AppConfig edited = cfg.session;
            int labelMode = static_cast<int>(cfg.labelMode);
            int dimLevel = cfg.dimLevelPercent;
            int dimDelay = cfg.dimDelaySeconds;
            int orbitStep = cfg.orbitStepSeconds;
            int viewMode = cfg.viewMode;
            bool motionActivity = cfg.motionActivity;
            bool activeOnly = cfg.activeOnly;
            bool keepHiddenStreams = cfg.keepHiddenStreams;
            bool forget = false;
            // Live idle-dim preview: while the slider moves, apply the previewed
            // luminance to the main view behind the modal dialog by rendering one
            // frame (the run loop is suspended during the modal, so we drive it).
            auto onDimPreview = [&](int pct) {
                currentDim = std::clamp(pct, 10, 100) / 100.0f;
                renderer->setDimFactor(currentDim);
                renderer->render(session.snapshotFrames());
            };
            if (gig::showSettingsDialog(mainHwnd, edited, labelMode, dimLevel, dimDelay,
                                        orbitStep, viewMode, motionActivity, activeOnly,
                                        keepHiddenStreams, forget, lastConnectError, onDimPreview)) {
                saveConfig(*settings, edited, static_cast<LabelMode>(labelMode),
                           dimLevel, dimDelay, orbitStep, viewMode, motionActivity, activeOnly,
                           keepHiddenStreams);
                cfg = loadConfig(*settings); // re-derive useSystemStore + re-validate
                renderer->setLabelMode(cfg.labelMode);
                applyAndReport(cfg.session);
            } else if (forget) {
                // TODO(onboarding-project): temporary. Wipe everything and restart
                // first-run onboarding on the welcome screen. "Everything" includes
                // the in-memory runtime state a wipe must not leak through: the
                // auth cookie + TLS resumption tickets (they embody the forgotten
                // credentials), the pin store's staged/declined session state, the
                // renderer's focus, and the frame-delta latches (a dead session's
                // counters would wrap the next fps calculation).
                gig::logWarning() << "forgetting all settings (user request)";
                activityFeed.reset();
                activityGate.reset();
                visibleTiles.clear();
                session.stop();
                settings->clear();
                settings->setInt("schema-version", 1);
                cookieJar->clear();
                sessionCache->clear();
                pinStore.reset();
                cfg = loadConfig(*settings);
                renderer->setFocusedTile(-1);
                renderer->setHoveredTile(-1);
                renderer->setCameraLabels(session.cameraLabels());
                renderer->setLabelMode(cfg.labelMode);
                lastConnectError.clear();
                lastFailureWasConfig = false;
                lastTitleFrames = 0;
                lastStatsFrames = 0;
                lastRenderedFrames = 0;
                OverlayStats welcome;
                applyScreenState(welcome);
                renderer->setDiagnostics(welcome);
            }
        };
#endif

        while (running) {
            const auto frameStart = std::chrono::steady_clock::now();

            bool sawEvent = false;
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                sawEvent = true;
                const bool imguiUsed = renderer->handleEvent(event);

                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                    continue;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    // Esc unwinds innermost-out: log view, focused tile,
                    // fullscreen, then quit.
                    if (renderer->logViewVisible()) {
                        renderer->setLogViewVisible(false);
                    } else if (renderer->focusedTile() >= 0) {
                        renderer->setFocusedTile(-1);
                    } else if (isFullscreen()) {
                        toggleFullscreen();
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
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11) {
                    toggleFullscreen(); // browser convention
                    continue;
                }
#if defined(_WIN32) || defined(__APPLE__)
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F2) {
                    gig::logInfo() << "settings dialog (F2)";
                    openSettings();
                    continue;
                }
#endif
#ifdef __APPLE__
                if (prefsEventType != 0 && event.type == prefsEventType) {
                    gig::logInfo() << "settings dialog (menu)";
                    openSettings();
                    continue;
                }
#endif
                if (imguiUsed) {
                    if (event.type == SDL_EVENT_MOUSE_MOTION) {
                        renderer->setHoveredTile(-1); // pointer is over the toolbar / log view
                    }
                    continue; // ImGui consumed this event
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                    if (renderer->focusedTile() >= 0) {
                        renderer->setFocusedTile(-1); // any click returns to the grid
                    } else if (const int idx = cameraTileAt(event.button.x, event.button.y); idx >= 0) {
                        renderer->setFocusedTile(idx);
                    }
                } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    // Hover affordance: in focus view the whole image is the hot
                    // element; in the grid it's the camera tile under the pointer.
                    renderer->setHoveredTile(renderer->focusedTile() >= 0
                        ? renderer->focusedTile()
                        : cameraTileAt(event.motion.x, event.motion.y));
                } else if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
                    renderer->setHoveredTile(-1);
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    renderer->resize();
                }

                // Remember the normal (non-maximized/minimized/fullscreen) rectangle.
                if (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED) {
                    const SDL_WindowFlags wflags = SDL_GetWindowFlags(window.get());
                    if (!(wflags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN))) {
                        SDL_GetWindowPosition(window.get(), &liveGeom.x, &liveGeom.y);
                        SDL_GetWindowSize(window.get(), &liveGeom.w, &liveGeom.h);
                    }
                }
            }
            if (sawEvent) {
                // Recent input: keep rendering briefly so ImGui hover/tooltip state
                // and the focus-view toolbar settle even without further events.
                lastInteraction = frameStart;
            }

#if defined(_WIN32) || defined(__APPLE__)
            // A cert that went untrusted mid-session (e.g. it changed under us) is
            // staged by the verify callback; offer to pin it, then reconnect.
            if (auto pending = pinStore.takePending()) {
                if (gig::promptPinDecision(mainHwnd, *pending)) {
                    pinStore.acceptPin(*pending);
                    gig::logInfo() << "pinned certificate for " << pending->host;
                    applyAndReport(cfg.session);
                } else {
                    pinStore.declinePin(*pending);
                    gig::logWarning() << "declined certificate for " << pending->host;
                }
            }
#endif

            // Activity view: derive the visible tile subset from the /ws feed.
            // Startup and reconnects count as interaction (lastInteraction
            // begins "now"), so the wall opens showing everything and settles
            // into the filtered view after the peek window -- which also
            // papers over /ws having no state replay on connect.
            const double sinceInteraction =
                std::chrono::duration<double>(frameStart - lastInteraction).count();
            const bool feedUp = activityFeed && activityFeed->connected();
            const std::vector<gig::FrigateEvents::CameraState> feedStates =
                activityFeed ? activityFeed->snapshot()
                             : std::vector<gig::FrigateEvents::CameraState> {};
            const gig::ActivityGate::Result activity = activityGate.evaluate(
                cfg.viewMode == 1, cfg.motionActivity, cfg.activeOnly, feedUp,
                sinceInteraction, feedStates, static_cast<int>(session.cameraCount()));
            if (activity.wakeEdge) {
                lastActivityWake = frameStart;
            }
            const bool visibleChanged = activity.visible != visibleTiles;
            if (visibleChanged) {
                // Keep focus on the same CAMERA across the reshuffle; drop it
                // if that camera left the subset (a stale focused index past
                // the new tile count wedges the zoom state).
                const int focused = renderer->focusedTile();
                int remapped = -1;
                if (focused >= 0 && focused < static_cast<int>(visibleTiles.size())) {
                    const int cam = visibleTiles[static_cast<std::size_t>(focused)];
                    const auto pos = std::find(activity.visible.begin(), activity.visible.end(), cam);
                    if (pos != activity.visible.end()) {
                        remapped = static_cast<int>(pos - activity.visible.begin());
                    }
                }
                // Immediate (no zoom transition): the animation state refers to
                // the OLD index space and would visibly zoom the wrong camera.
                renderer->setFocusedTileImmediate(remapped);
                renderer->setHoveredTile(-1);
                visibleTiles = activity.visible;
                // Re-run the stats tick next loop so quietStatus (and the
                // banner counts) track the subset change within a frame.
                lastTitleUpdate = frameStart - std::chrono::seconds(2);
                // Labels must track the subset so tile text matches what's shown.
                const std::vector<std::string>& labels = session.cameraLabels();
                std::vector<std::string> shownLabels;
                shownLabels.reserve(visibleTiles.size());
                for (const int cam : visibleTiles) {
                    shownLabels.push_back(cam < static_cast<int>(labels.size())
                        ? labels[static_cast<std::size_t>(cam)]
                        : std::string());
                }
                renderer->setCameraLabels(shownLabels);
            }

            // Activity-reason suffixes ("driveway - person"): rebuilt each tick
            // from the feed (a reason can change while the subset doesn't) and
            // pushed only on change. The renderer appends them to the labels
            // and force-shows the label while a reason is active.
            bool reasonsChanged = false;
            {
                std::vector<std::string> reasons(visibleTiles.size());
                if (feedUp) {
                    for (std::size_t i = 0; i < visibleTiles.size(); ++i) {
                        const int cam = visibleTiles[i];
                        if (cam >= 0 && cam < static_cast<int>(feedStates.size())) {
                            reasons[i] = gig::activityReason(
                                feedStates[static_cast<std::size_t>(cam)],
                                cfg.motionActivity, cfg.activeOnly);
                        }
                    }
                }
                if (reasons != tileReasons) {
                    tileReasons = std::move(reasons);
                    renderer->setTileReasons(tileReasons);
                    reasonsChanged = true;
                }
            }

            // On-demand stream policy: what's RENDERED must stream; everything
            // else winds down after the stop delay (unless the keep setting is
            // on). setCameraStreamEnabled is a no-op when unchanged, so pushing
            // the whole vector every tick is cheap and self-healing.
            {
                std::vector<int> onScreen;
                const int focused = renderer->focusedTile();
                if (focused >= 0 && focused < static_cast<int>(visibleTiles.size())) {
                    onScreen.push_back(visibleTiles[static_cast<std::size_t>(focused)]);
                } else {
                    onScreen = visibleTiles;
                }
                const std::vector<char>& desired = streamPolicy.evaluate(
                    static_cast<int>(session.cameraCount()), onScreen,
                    cfg.keepHiddenStreams,
                    gig::FrigateEvents::nowSeconds());
                for (std::size_t i = 0; i < desired.size(); ++i) {
                    session.setCameraStreamEnabled(i, desired[i] != 0);
                }
            }

            // Refresh the toolbar/banner stats first (1s cadence), before the draw
            // decision, so a stats change can itself mark this tick dirty -- that
            // keeps the live fps/cpu numbers moving even when no new frame arrives
            // (e.g. a single focused low-fps camera).
            const auto now = std::chrono::steady_clock::now();
            bool statsRefreshed = false;
            if (now - lastTitleUpdate > std::chrono::seconds(1)) {
                const double elapsed = std::chrono::duration<double>(now - lastTitleUpdate).count();
                lastTitleUpdate = now;
                const std::uint64_t total = session.totalDecodedFrames();
                const double fps = elapsed > 0.0 ? static_cast<double>(total - lastTitleFrames) / elapsed : 0.0;
                lastTitleFrames = total;

                OverlayStats stats;
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
                applyScreenState(stats);
                if (activity.filtered && activity.quiet) {
                    // Down = the /ws heartbeat says so (explicit non-online or
                    // 35s stale) -- NOT our streaming state, which the
                    // on-demand stream policy tears down on purpose.
                    int camerasDown = 0;
                    const double nowSeconds = gig::FrigateEvents::nowSeconds();
                    for (const gig::FrigateEvents::CameraState& s : feedStates) {
                        camerasDown += s.down(nowSeconds) ? 1 : 0;
                    }
                    stats.quietStatus = quietLineNow(camerasDown);
                }

                renderer->setDiagnostics(stats);
                lastCpuPercent = stats.cpuPercent;
                statsRefreshed = true;
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

            // Fire a scheduled auto-reconnect when its backoff elapses (a network
            // transient recovering on its own -- e.g. the switch port came back).
            if (autoRetryArmed && now >= autoRetryAt) {
                gig::logInfo() << "auto-reconnect";
                applyAndReport(cfg.session, /*userInitiated=*/false);
            }

            // Idle-dim ramp. Target = full while a status screen is up, recently
            // interacted, or dimming disabled; else the configured level after the
            // delay. Ramp ~1.5%/frame so it's a gentle fade, not a snap.
            const auto sinceDimWake = std::min(frameStart - lastInteraction, frameStart - lastActivityWake);
            const bool idleForDim = cfg.dimDelaySeconds > 0
                && sinceDimWake >= std::chrono::seconds(cfg.dimDelaySeconds)
                && session.running();
            const float dimTarget = idleForDim ? static_cast<float>(cfg.dimLevelPercent) / 100.0f : 1.0f;
            const bool dimming = currentDim != dimTarget;
            if (dimming) {
                const float stepD = 0.015f;
                currentDim = (currentDim < dimTarget) ? std::min(dimTarget, currentDim + stepD)
                                                      : std::max(dimTarget, currentDim - stepD);
            }
            renderer->setDimFactor(currentDim);
            renderer->setOrbitStepSeconds(static_cast<float>(cfg.orbitStepSeconds));

            // Draw on demand. While minimized there's nothing to present, so skip
            // the render (and the shared D3D11 lock it holds) entirely -- decoders
            // keep running so restore is instant. Otherwise draw when a new frame
            // arrived, an animation is in flight, input was recent, the stats just
            // refreshed, the log view is open, or the dim level is ramping.
            const bool minimized = (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) != 0;
            const std::uint64_t decodedNow = session.totalDecodedFrames();
            const bool newFrame = decodedNow != lastRenderedFrames;
            const bool recentInput = (frameStart - lastInteraction) < inputRenderTail;
            const bool dirty = !minimized
                && (newFrame || statsRefreshed || recentInput || dimming || visibleChanged
                    || reasonsChanged || renderer->isAnimating() || renderer->logViewVisible()
                    || renderer->wantsRepaint()); // burn-in orbit step

            if (dirty) {
                // Render only the visible subset; frames/byte-counts must stay
                // index-aligned with the tiles (and with the labels pushed on
                // the last subset change).
                const std::vector<std::shared_ptr<VideoFrame>> allFrames = session.snapshotFrames();
                const std::vector<std::uint64_t> allBytes = session.tileByteCounts();
                std::vector<std::shared_ptr<VideoFrame>> frames;
                std::vector<std::uint64_t> bytes;
                frames.reserve(visibleTiles.size());
                bytes.reserve(visibleTiles.size());
                for (const int cam : visibleTiles) {
                    frames.push_back(cam < static_cast<int>(allFrames.size())
                        ? allFrames[static_cast<std::size_t>(cam)]
                        : nullptr);
                    bytes.push_back(cam < static_cast<int>(allBytes.size())
                        ? allBytes[static_cast<std::size_t>(cam)]
                        : 0);
                }
                renderer->setTileActivity(bytes); // drives the per-tile signal animation
                renderer->render(frames);
                lastRenderedFrames = decodedNow;

                // Toolbar buttons route through the same paths as F2 / F5 / F11.
                switch (renderer->takeToolbarAction()) {
                case VideoRenderer::ToolbarAction::Reconnect:
                    gig::logInfo() << "reconnect requested (toolbar)";
                    applyAndReport(cfg.session);
                    break;
                case VideoRenderer::ToolbarAction::ToggleFullscreen:
                    toggleFullscreen();
                    break;
#if defined(_WIN32) || defined(__APPLE__)
                case VideoRenderer::ToolbarAction::Settings:
                    gig::logInfo() << "settings dialog (toolbar)";
                    openSettings();
                    break;
#endif
                default:
                    break;
                }
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
            if (!liveGeom.maximized
                && !(wflags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN))) {
                SDL_GetWindowPosition(window.get(), &liveGeom.x, &liveGeom.y);
                SDL_GetWindowSize(window.get(), &liveGeom.w, &liveGeom.h);
            }
            saveWindowGeometry(*settings, liveGeom);
        }

        gig::logInfo() << "shutting down";
        activityFeed.reset();
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
