#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

enum class VideoFrameFormat {
    BGRA,
    NV12,
    YUV420P,
    // Zero-copy GPU frame; gpuTexture is platform-specific (a D3D11 NV12 texture on
    // Windows, a CVPixelBufferRef on macOS). Both renderers sample it as Y + CbCr.
    GPU_NV12,
};

struct VideoFrame {
    VideoFrameFormat format = VideoFrameFormat::BGRA;
    int width = 0;
    int height = 0;
    bool fullRange = false;
    std::uint64_t index = 0;
    std::array<int, 3> strides = {};

    // Per-plane read pointers the renderer uploads from; strides[] gives each row
    // stride in bytes. The backing memory is kept alive by owner: software
    // NV12/YUV420P frames *borrow* the decoded AVFrame's planes (no copy -- owner
    // is the cloned AVFrame, strides are its padded linesizes), while the
    // BGRA/swscale fallback owns its pixels in planes[] and points planeData[]
    // back into them. planeData[0] == nullptr (with a non-D3D11 format) means "no
    // displayable frame yet" (waiting for a keyframe / offline).
    std::array<const std::uint8_t*, 3> planeData = {};
    std::array<std::vector<std::uint8_t>, 3> planes;

    // Zero-copy GPU frame handle (format == GPU_NV12), interpreted by the platform
    // renderer and kept alive by owner. Windows: gpuTexture is an ID3D11Texture2D*
    // (NV12) at gpuArraySlice in the decoder's texture array, guarded by gpuLock (the
    // shared D3D11 device lock). macOS: gpuTexture is a CVPixelBufferRef (IOSurface-
    // backed NV12); gpuArraySlice is 0 and gpuLock is null. nullptr for CPU frames.
    void* gpuTexture = nullptr;
    int gpuArraySlice = 0;
    std::shared_ptr<std::recursive_mutex> gpuLock;
    std::shared_ptr<void> owner;

    // Move-only: a borrowed frame's planeData[] points into owner (or into its own
    // planes[]), so a copy would silently alias or dangle. All real usage moves.
    VideoFrame() = default;
    VideoFrame(VideoFrame&&) = default;
    VideoFrame& operator=(VideoFrame&&) = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
};
