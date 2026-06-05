#include "render/grid_layout.h"

#include <cstddef>

namespace gig {

GridLayout computeGridLayout(int count, int outputWidth, int outputHeight, float targetAspect)
{
    GridLayout layout;
    if (count <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        return layout;
    }
    if (targetAspect <= 0.0f) {
        targetAspect = 16.0f / 9.0f;
    }

    // Pick the column count whose cell aspect ratio is closest to targetAspect.
    // For a targetAspect video letterboxed into a cell, fill fraction == 1/ratio,
    // so minimizing this ratio maximizes how much of each cell the video covers.
    int bestColumns = 1;
    double bestScore = -1.0;
    for (int columns = 1; columns <= count; ++columns) {
        const int rows = (count + columns - 1) / columns;
        const double cellWidth = static_cast<double>(outputWidth) / columns;
        const double cellHeight = static_cast<double>(outputHeight) / rows;
        const double cellAspect = cellWidth / cellHeight;
        const double ratio = cellAspect > targetAspect
            ? cellAspect / targetAspect
            : targetAspect / cellAspect;
        if (bestScore < 0.0 || ratio <= bestScore) {
            bestScore = ratio;
            bestColumns = columns;
        }
    }

    const int columns = bestColumns;
    const int rows = (count + columns - 1) / columns;
    const float cellWidth = static_cast<float>(outputWidth) / static_cast<float>(columns);
    const float cellHeight = static_cast<float>(outputHeight) / static_cast<float>(rows);

    layout.columns = columns;
    layout.rows = rows;
    layout.tiles.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const int row = index / columns;
        const int column = index % columns;
        TileRect rect;
        rect.x = static_cast<float>(column) * cellWidth;
        rect.y = static_cast<float>(row) * cellHeight;
        rect.width = cellWidth;
        rect.height = cellHeight;
        layout.tiles.push_back(rect);
    }
    return layout;
}

} // namespace gig
