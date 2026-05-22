#pragma once

#include <array>
#include <cstdint>
#include <vector>

enum class VideoFrameFormat {
    BGRA,
    NV12,
    YUV420P,
};

struct VideoFrame {
    VideoFrameFormat format = VideoFrameFormat::BGRA;
    int width = 0;
    int height = 0;
    bool fullRange = false;
    std::uint64_t index = 0;
    std::array<int, 3> strides = {};
    std::array<std::vector<std::uint8_t>, 3> planes;
};
