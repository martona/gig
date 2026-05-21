#pragma once

#include <cstdint>
#include <vector>

struct VideoFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint64_t index = 0;
    std::vector<std::uint8_t> bgra;
};
