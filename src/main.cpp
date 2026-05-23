#include "decode/ffmpeg_decoder.h"
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

namespace {

constexpr const char* DefaultUrl = "https://frigate.lan/security-go2rtc/api/stream.ts?src=frontgate";

enum class Command {
    Run,
    Probe,
};

struct ProgramOptions {
    Command command = Command::Run;
    std::string url = DefaultUrl;
    ProbeOptions probe;
    TlsOptions tls;
    bool showHelp = false;
};

void printUsage()
{
    std::cout
        << "frigate_d3d_poc [--url URL] [--ca CA.pem] [--cert client.crt] [--key client.key] [--insecure]\n"
        << "frigate_d3d_poc probe --base URL [--src STREAM] [--stream-check] [--endpoint PATH]\n"
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
        } else if (!arg.starts_with("--")) {
            if (options.command == Command::Probe) {
                options.probe.baseUrl = arg;
            } else {
                options.url = arg;
            }
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    options.probe.tls = options.tls;
    if (options.command == Command::Probe) {
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

        std::mutex frameMutex;
        std::shared_ptr<VideoFrame> latestFrame;
        std::atomic<std::uint64_t> decodedFrames = 0;

        FfmpegDecoder decoder(
            options.url,
            options.tls,
            renderer->d3d11DecodeContext(),
            [&](VideoFrame&& frame) {
                decodedFrames = frame.index;
                auto sharedFrame = std::make_shared<VideoFrame>(std::move(frame));
                std::lock_guard lock(frameMutex);
                latestFrame = std::move(sharedFrame);
            });
        decoder.start();

        bool running = true;
        auto lastTitleUpdate = std::chrono::steady_clock::now();

        while (running) {
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

            std::shared_ptr<VideoFrame> frame;
            {
                std::lock_guard lock(frameMutex);
                frame = latestFrame;
            }

            renderer->render(frame.get());

            const auto now = std::chrono::steady_clock::now();
            if (now - lastTitleUpdate > std::chrono::seconds(1)) {
                lastTitleUpdate = now;
                std::string title = "Frigate D3D PoC - frames " + std::to_string(decodedFrames.load());
                if (frame) {
                    title += " - " + std::to_string(frame->width) + "x" + std::to_string(frame->height);
                }
                SDL_SetWindowTitle(window.get(), title.c_str());
            }

            SDL_Delay(1);
        }

        decoder.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
