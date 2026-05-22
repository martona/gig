#pragma once

#include "d3d11_decode_context.h"
#include "video_frame.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

struct TlsOptions {
    bool verifyServer = true;
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    std::int64_t rwTimeoutUs = 10'000'000;
};

class FfmpegDecoder {
public:
    using FrameCallback = std::function<void(VideoFrame&&)>;

    FfmpegDecoder(
        std::string url,
        TlsOptions tlsOptions,
        std::shared_ptr<D3D11DecodeContext> d3d11Context,
        FrameCallback frameCallback);
    ~FfmpegDecoder();

    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    void start();
    void stop();

private:
    void run();
    void decodeOnce();

    std::string url_;
    TlsOptions tlsOptions_;
    std::shared_ptr<D3D11DecodeContext> d3d11Context_;
    FrameCallback frameCallback_;
    std::atomic_bool stopRequested_ = false;
    std::thread worker_;
    std::uint64_t frameIndex_ = 0;
};
