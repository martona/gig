#pragma once

#include "d3d11_decode_context.h"
#include "video_frame.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gig {
class TlsClient;
class MediaStream;
}

class FfmpegDecoder {
public:
    using FrameCallback = std::function<void(VideoFrame&&)>;

    FfmpegDecoder(
        std::string url,
        std::shared_ptr<gig::TlsClient> tlsClient,
        std::shared_ptr<D3D11DecodeContext> d3d11Context,
        FrameCallback frameCallback,
        bool softwareOnly = false,
        std::chrono::milliseconds startupDelay = std::chrono::milliseconds(0),
        // Optional sink the AVIO read path adds downloaded byte counts to, so the
        // UI can show live "receiving data" activity before the first keyframe.
        // Owned by the caller (the supervisor) and outlives the decoder.
        std::atomic<std::uint64_t>* byteSink = nullptr);
    ~FfmpegDecoder();

    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    void start();
    void stop();

private:
    void run();
    void decodeOnce();

    std::string url_;
    std::shared_ptr<gig::TlsClient> tlsClient_;
    std::shared_ptr<D3D11DecodeContext> d3d11Context_;
    FrameCallback frameCallback_;
    bool softwareOnly_ = false;
    std::chrono::milliseconds startupDelay_ { 0 };
    std::atomic<std::uint64_t>* byteSink_ = nullptr;
    std::atomic_bool stopRequested_ = false;
    std::thread worker_;
    std::uint64_t frameIndex_ = 0;
    // Set when a hardware decoder repeatedly fails to produce a frame (e.g. a
    // VideoToolbox stream it can't sync to); makes the next decodeOnce open a
    // software codec instead. Worker-thread only.
    bool forceSoftware_ = false;

    // The connection FFmpeg is currently reading from, so stop() can cancel an
    // in-flight read promptly. Guarded by streamMutex_.
    std::mutex streamMutex_;
    gig::MediaStream* activeStream_ = nullptr;

    // Interruptible wait for the initial-connect stagger; stop() wakes it.
    std::mutex startupMutex_;
    std::condition_variable startupCv_;
};
