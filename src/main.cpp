#include "app/camera_supervisor.h"
#include "decode/ffmpeg_decoder.h"
#include "discovery/frigate_discovery.h"
#include "log.hpp"
#include "net/cookie_jar.hpp"
#include "net/http_client.hpp"
#include "net/tls_session_cache.hpp"
#include "net/win_cert_store.h"
#include "probe/cert_probe.h"
#include "probe/http_probe.h"
#include "render/grid_layout.h"
#include "render/video_renderer.h"
#include "video_frame.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr const char* DefaultUrl = "https://frigate.lan/security-go2rtc/api/stream.ts?src=frontgate";

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

enum class Command {
    Run,
    Probe,
    Discover,
    CertStore,
};

struct ProgramOptions {
    Command command = Command::Run;
    std::string url = DefaultUrl;
    std::string streamUrlTemplate;
    bool softwareDecode = false;
    int pollIntervalSeconds = 5;
    bool showOverlay = true;
    gig::ClientCertMode certMode = gig::ClientCertMode::Cng;
    ProbeOptions probe;
    TlsOptions tls;
    bool showHelp = false;
};

void printUsage()
{
    std::cout
        << "gig [--base URL | --url URL] [--software] [--no-overlay] [--ca CA.pem] [--cert client.crt] [--key client.key] [--insecure]\n"
        << "gig probe --base URL [--src STREAM] [--stream-check] [--endpoint PATH]\n"
        << "gig discover --base URL [--stream-url TEMPLATE] [--ca CA] [--cert C] [--key K]\n"
        << "gig certstore --base URL [--server-only|--capi]   (Windows cert store; default = CNG client-cert bridge)\n"
        << "\n"
        << "  With no --ca/--cert/--key, the Windows certificate store is used (store CA + CNG client\n"
        << "  cert); this pops a Windows consent prompt on first use.\n"
        << "  Defaults can be set in gig.ini next to the exe (keys: base, url, stream-url, ca, cert,\n"
        << "  key, software, overlay, insecure, poll-interval, rw-timeout-us); flags override it.\n"
        << "\n"
        << "If the viewer --url is omitted, this default is used:\n"
        << "  " << DefaultUrl << "\n";
}

std::string requireValue(int& index, int argc, char** argv, const char* option)
{
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string(option) + " requires a value.");
    }

    ++index;
    return argv[index];
}

std::string baseUrlFromStreamUrl(std::string url)
{
    const std::size_t query = url.find('?');
    if (query != std::string::npos) {
        url.erase(query);
    }

    const std::string apiMarker = "/api/";
    const std::size_t api = url.find(apiMarker);
    if (api != std::string::npos) {
        url.erase(api);
    } else if (url.ends_with("/api")) {
        url.erase(url.size() - 4);
    }

    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

// Directory containing the running executable (so gig.ini sits next to it).
std::filesystem::path exeDirectory()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer, buffer + length).parent_path();
#else
    return {};
#endif
}

std::string trimWhitespace(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string stripQuotes(std::string value)
{
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool iniTruthy(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

// Apply one gig.ini key=value. Returns false for an unrecognized key. Throws
// (caught by the caller) on a malformed numeric value.
bool applyIniSetting(ProgramOptions& options, const std::string& key, const std::string& value)
{
    if (key == "base") {
        options.probe.baseUrl = value;
    } else if (key == "url") {
        options.url = value;
    } else if (key == "stream-url" || key == "stream_url") {
        options.streamUrlTemplate = value;
    } else if (key == "ca") {
        options.tls.caFile = value;
    } else if (key == "cert") {
        options.tls.certFile = value;
    } else if (key == "key") {
        options.tls.keyFile = value;
    } else if (key == "software") {
        options.softwareDecode = iniTruthy(value);
    } else if (key == "overlay") {
        options.showOverlay = iniTruthy(value);
    } else if (key == "insecure") {
        options.tls.verifyServer = !iniTruthy(value);
    } else if (key == "poll-interval" || key == "poll_interval") {
        options.pollIntervalSeconds = std::stoi(value);
    } else if (key == "rw-timeout-us" || key == "rw_timeout_us") {
        options.tls.rwTimeoutUs = std::stoll(value);
    } else {
        return false;
    }
    return true;
}

// Layer settings from gig.ini (next to the exe) onto `options`. These are
// defaults; command-line flags parsed afterward override them. Missing file is
// silently ignored; bad lines warn but don't abort.
void applyIniConfig(ProgramOptions& options)
{
    const std::filesystem::path dir = exeDirectory();
    if (dir.empty()) {
        return;
    }
    const std::filesystem::path path = dir / "gig.ini";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return;
    }

    std::ifstream file(path);
    if (!file) {
        return;
    }

    int applied = 0;
    int lineNumber = 0;
    std::string line;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string trimmed = trimWhitespace(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';' || trimmed.front() == '[') {
            continue;
        }
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = trimWhitespace(trimmed.substr(0, equals));
        for (char& ch : key) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        const std::string value = stripQuotes(trimWhitespace(trimmed.substr(equals + 1)));
        try {
            if (applyIniSetting(options, key, value)) {
                ++applied;
            } else {
                gig::logWarning() << "gig.ini: ignoring unknown key '" << key << "' (line " << lineNumber << ")";
            }
        } catch (const std::exception& error) {
            gig::logWarning() << "gig.ini: bad value for '" << key << "' (line " << lineNumber << "): " << error.what();
        }
    }
    gig::logInfo() << "gig.ini: applied " << applied << " setting(s) from " << path.string();
}

ProgramOptions parseOptions(int argc, char** argv)
{
    ProgramOptions options;

    int firstOption = 1;
    if (argc > 1 && std::string(argv[1]) == "probe") {
        options.command = Command::Probe;
        firstOption = 2;
    } else if (argc > 1 && std::string(argv[1]) == "discover") {
        options.command = Command::Discover;
        firstOption = 2;
    } else if (argc > 1 && std::string(argv[1]) == "certstore") {
        options.command = Command::CertStore;
        firstOption = 2;
    }

    // gig.ini (next to the exe) provides defaults; the flags below override them.
    applyIniConfig(options);

    for (int i = firstOption; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--url") {
            options.url = requireValue(i, argc, argv, "--url");
            if (options.command == Command::Probe) {
                options.probe.baseUrl = options.url;
            }
        } else if (arg == "--base") {
            options.probe.baseUrl = requireValue(i, argc, argv, "--base");
        } else if (arg == "--stream-url") {
            options.streamUrlTemplate = requireValue(i, argc, argv, "--stream-url");
        } else if (arg == "--src") {
            options.probe.streamName = requireValue(i, argc, argv, "--src");
        } else if (arg == "--max-bytes") {
            const std::string value = requireValue(i, argc, argv, "--max-bytes");
            options.probe.maxBytes = static_cast<std::size_t>(std::stoull(value));
        } else if (arg == "--dump") {
            options.probe.dumpBody = true;
        } else if (arg == "--stream-check") {
            options.probe.checkStreams = true;
        } else if (arg == "--endpoint") {
            options.probe.extraEndpoints.push_back(requireValue(i, argc, argv, "--endpoint"));
        } else if (arg == "--ca") {
            options.tls.caFile = requireValue(i, argc, argv, "--ca");
        } else if (arg == "--cert") {
            options.tls.certFile = requireValue(i, argc, argv, "--cert");
        } else if (arg == "--key") {
            options.tls.keyFile = requireValue(i, argc, argv, "--key");
        } else if (arg == "--rw-timeout-us") {
            options.tls.rwTimeoutUs = std::stoll(requireValue(i, argc, argv, "--rw-timeout-us"));
        } else if (arg == "--insecure") {
            options.tls.verifyServer = false;
        } else if (arg == "--software" || arg == "--no-hwaccel") {
            options.softwareDecode = true;
        } else if (arg == "--no-overlay") {
            options.showOverlay = false;
        } else if (arg == "--server-only") {
            options.certMode = gig::ClientCertMode::None;
        } else if (arg == "--capi") {
            options.certMode = gig::ClientCertMode::Capi;
        } else if (arg == "--poll-interval") {
            options.pollIntervalSeconds = std::stoi(requireValue(i, argc, argv, "--poll-interval"));
        } else if (!arg.starts_with("--")) {
            if (options.command == Command::Probe || options.command == Command::Discover) {
                options.probe.baseUrl = arg;
            } else {
                options.url = arg;
            }
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    // The Windows certificate store is implicit when no PEM material is given:
    // store trust roots for server verification + a CurrentUser\MY client cert.
    options.tls.useWindowsStore = options.tls.caFile.empty()
        && options.tls.certFile.empty()
        && options.tls.keyFile.empty();

    options.probe.tls = options.tls;
    if (options.command == Command::Probe || options.command == Command::Discover
        || options.command == Command::CertStore) {
        if (options.probe.baseUrl.empty()) {
            options.probe.baseUrl = baseUrlFromStreamUrl(options.url);
        } else {
            options.probe.baseUrl = baseUrlFromStreamUrl(options.probe.baseUrl);
        }
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
    try {
        ProgramOptions options = parseOptions(argc, argv);
        if (options.showHelp) {
            printUsage();
            return 0;
        }
        if (options.command == Command::Probe) {
            return runProbe(options.probe);
        }
        if (options.command == Command::Discover) {
            auto sessionCache = std::make_shared<gig::TlsSessionCache>();
            auto cookieJar = std::make_shared<gig::CookieJar>();
            gig::HttpClient client(options.probe.baseUrl, options.tls, sessionCache, cookieJar);
            const std::vector<gig::CameraStream> cameras =
                gig::discoverCameras(client, options.streamUrlTemplate);
            std::cout << "Discovered " << cameras.size() << " camera(s):\n";
            for (const gig::CameraStream& camera : cameras) {
                std::cout << "  " << camera.cameraName << "  -> " << camera.streamName
                          << "  " << camera.streamUrl << "\n";
            }
            return cameras.empty() ? 2 : 0;
        }
        if (options.command == Command::CertStore) {
            return gig::runCertProbe(options.probe.baseUrl, options.certMode);
        }

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
        std::vector<gig::CameraStream> cameras;
        if (!options.probe.baseUrl.empty()) {
            gig::HttpClient client(options.probe.baseUrl, options.tls, sessionCache, cookieJar);
            cameras = gig::discoverCameras(client, options.streamUrlTemplate);
        } else {
            cameras.push_back({ "camera", "", options.url });
            gig::logInfo() << "no --base given; single camera " << options.url;
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
        supervisorConfig.baseUrl = options.probe.baseUrl;
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
                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    if (renderer->focusedTile() >= 0) {
                        renderer->setFocusedTile(-1); // first Esc leaves focus, next quits
                    } else {
                        running = false;
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                    if (renderer->focusedTile() >= 0) {
                        renderer->setFocusedTile(-1); // any click returns to the grid
                    } else {
                        int windowWidth = 0;
                        int windowHeight = 0;
                        SDL_GetWindowSize(window.get(), &windowWidth, &windowHeight);
                        if (windowWidth > 0 && windowHeight > 0) {
                            // Match the renderer's grid (cameras + optional diagnostics cell),
                            // but only the camera tiles are focusable.
                            const int effective = static_cast<int>(supervisor.cameraCount()) + (options.showOverlay ? 1 : 0);
                            const gig::GridLayout layout = gig::computeGridLayout(effective, windowWidth, windowHeight);
                            for (std::size_t t = 0; t < layout.tiles.size() && t < supervisor.cameraCount(); ++t) {
                                const gig::TileRect& cell = layout.tiles[t];
                                if (event.button.x >= cell.x && event.button.x < cell.x + cell.width
                                    && event.button.y >= cell.y && event.button.y < cell.y + cell.height) {
                                    renderer->setFocusedTile(static_cast<int>(t));
                                    break;
                                }
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
        supervisor.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
