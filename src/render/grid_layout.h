#pragma once

#include <vector>

namespace gig {

struct TileRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct GridLayout {
    int columns = 1;
    int rows = 1;
    std::vector<TileRect> tiles;
};

// Lay out `count` equal cells over an outputWidth x outputHeight surface,
// choosing the column count whose cell aspect best matches targetAspect (which
// also maximizes the fraction of each cell a targetAspect video fills). Cells
// are row-major; trailing cells of the last row are simply absent.
GridLayout computeGridLayout(int count, int outputWidth, int outputHeight, float targetAspect = 16.0f / 9.0f);

} // namespace gig
