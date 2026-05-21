#include "decode/ffmpeg_decoder.h"
#include "render/video_renderer.h"
#include "video_frame.h"

#include <SDL.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* DefaultUrl = "https://frigate.lan/security-go2rtc/api/stream.ts?src=frontgate";

struct ProgramOptions {
    std::string url = DefaultUrl;
    TlsOptions tls;
    bool showHelp = false;
};

void printUsage()
{
    std::cout
        << "frigate_d3d_poc [--url URL] [--ca CA.pem] [--cert client.crt] [--key client.key] [--insecure]\n"
        << "\n"
        << "If --url is omitted, this default is used:\n"
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

ProgramOptions parseOptions(int argc, char** argv)
{
    ProgramOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--url") {
            options.url = requireValue(i, argc, argv, "--url");
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
            options.url = arg;
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    return options;
}

class SdlLifetime {
public:
    SdlLifetime()
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
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

        SdlLifetime sdl;

        auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
            SDL_CreateWindow(
                "Frigate D3D PoC",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                1280,
                720,
                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI),
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
                if (event.type == SDL_QUIT) {
                    running = false;
                } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (event.type == SDL_WINDOWEVENT
                    && (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_RESIZED)) {
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
