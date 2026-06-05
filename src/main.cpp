#include "decode/ffmpeg_decoder.h"
#include "discovery/frigate_discovery.h"
#include "log.hpp"
#include "net/http_client.hpp"
#include "net/tls_session_cache.hpp"
#include "probe/http_probe.h"
#include "render/video_renderer.h"
#include "video_frame.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* DefaultUrl = "https://frigate.lan/security-go2rtc/api/stream.ts?src=frontgate";

enum class Command {
    Run,
    Probe,
    Discover,
};

struct ProgramOptions {
    Command command = Command::Run;
    std::string url = DefaultUrl;
    std::string streamUrlTemplate;
    bool softwareDecode = false;
    ProbeOptions probe;
    TlsOptions tls;
    bool showHelp = false;
};

void printUsage()
{
    std::cout
        << "frigate_d3d_poc [--base URL | --url URL] [--software] [--ca CA.pem] [--cert client.crt] [--key client.key] [--insecure]\n"
        << "frigate_d3d_poc probe --base URL [--src STREAM] [--stream-check] [--endpoint PATH]\n"
        << "frigate_d3d_poc discover --base URL [--stream-url TEMPLATE] [--ca CA] [--cert C] [--key K]\n"
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
    }

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

    options.probe.tls = options.tls;
    if (options.command == Command::Probe || options.command == Command::Discover) {
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
            gig::HttpClient client(options.probe.baseUrl, options.tls, sessionCache);
            const std::vector<gig::CameraStream> cameras =
                gig::discoverCameras(client, options.streamUrlTemplate);
            std::cout << "Discovered " << cameras.size() << " camera(s):\n";
            for (const gig::CameraStream& camera : cameras) {
                std::cout << "  " << camera.cameraName << "  -> " << camera.streamName
                          << "  " << camera.streamUrl << "\n";
            }
            return cameras.empty() ? 2 : 0;
        }

        SdlLifetime sdl;

        auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
            SDL_CreateWindow(
                "Frigate D3D PoC",
                1280,
                720,
                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY),
            SDL_DestroyWindow);

        if (!window) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

        auto renderer = createD3D11Renderer();
        if (!renderer->initialize(window.get())) {
            return 1;
        }

        // Resolve the camera set: discover from --base, else fall back to the
        // single --url camera (legacy/manual mode).
        auto sessionCache = std::make_shared<gig::TlsSessionCache>();
        std::vector<gig::CameraStream> cameras;
        if (!options.probe.baseUrl.empty()) {
            gig::HttpClient client(options.probe.baseUrl, options.tls, sessionCache);
            cameras = gig::discoverCameras(client, options.streamUrlTemplate);
        } else {
            cameras.push_back({ "camera", "", options.url });
            gig::logInfo() << "no --base given; single camera " << options.url;
        }
        if (cameras.empty()) {
            throw std::runtime_error("no cameras to display");
        }

        std::mutex frameMutex;
        std::vector<std::shared_ptr<VideoFrame>> latestFrames(cameras.size());
        std::atomic<std::uint64_t> decodedFrames = 0;

        std::vector<std::unique_ptr<FfmpegDecoder>> decoders;
        decoders.reserve(cameras.size());
        for (std::size_t i = 0; i < cameras.size(); ++i) {
            decoders.push_back(std::make_unique<FfmpegDecoder>(
                cameras[i].streamUrl,
                options.tls,
                renderer->d3d11DecodeContext(),
                [&frameMutex, &latestFrames, &decodedFrames, i](VideoFrame&& frame) {
                    ++decodedFrames;
                    auto sharedFrame = std::make_shared<VideoFrame>(std::move(frame));
                    std::lock_guard lock(frameMutex);
                    latestFrames[i] = std::move(sharedFrame);
                },
                options.softwareDecode));
            decoders.back()->start();
        }
        gig::logInfo() << "started " << decoders.size() << " decoder(s)";

        bool running = true;
        auto lastTitleUpdate = std::chrono::steady_clock::now();
        auto lastStatsLog = lastTitleUpdate;
        std::uint64_t lastStatsFrames = 0;
        constexpr auto frameInterval = std::chrono::milliseconds(16);

        while (running) {
            const auto frameStart = std::chrono::steady_clock::now();

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    renderer->resize();
                }
            }

            std::vector<std::shared_ptr<VideoFrame>> frames;
            {
                std::lock_guard lock(frameMutex);
                frames = latestFrames;
            }

            renderer->render(frames);

            const auto now = std::chrono::steady_clock::now();
            if (now - lastTitleUpdate > std::chrono::seconds(1)) {
                lastTitleUpdate = now;
                std::string title = "Frigate D3D PoC - " + std::to_string(cameras.size())
                    + " cams - frames " + std::to_string(decodedFrames.load());
                SDL_SetWindowTitle(window.get(), title.c_str());
            }

            if (now - lastStatsLog > std::chrono::seconds(5)) {
                const std::uint64_t total = decodedFrames.load();
                const double seconds = std::chrono::duration<double>(now - lastStatsLog).count();
                const double fps = seconds > 0.0 ? static_cast<double>(total - lastStatsFrames) / seconds : 0.0;
                gig::logInfo() << "decoded " << total << " frames total ("
                               << static_cast<int>(fps + 0.5) << "/s across " << cameras.size() << " cams)";
                lastStatsLog = now;
                lastStatsFrames = total;
            }

            // Pace to ~60 fps outside any lock; the renderer uses a non-waiting
            // Present so it never blocks while holding the shared D3D11 lock.
            std::this_thread::sleep_until(frameStart + frameInterval);
        }

        gig::logInfo() << "shutting down " << decoders.size() << " decoder(s)";
        decoders.clear();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
