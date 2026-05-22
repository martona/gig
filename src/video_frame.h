#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct ID3D11Texture2D;

enum class VideoFrameFormat {
    BGRA,
    NV12,
    YUV420P,
    D3D11_NV12,
};

struct VideoFrame {
    VideoFrameFormat format = VideoFrameFormat::BGRA;
    int width = 0;
    int height = 0;
    bool fullRange = false;
    std::uint64_t index = 0;
    std::array<int, 3> strides = {};
    std::array<std::vector<std::uint8_t>, 3> planes;
    ID3D11Texture2D* d3d11Texture = nullptr;
    int d3d11ArraySlice = 0;
    std::shared_ptr<std::recursive_mutex> d3d11Lock;
    std::shared_ptr<void> owner;
};
