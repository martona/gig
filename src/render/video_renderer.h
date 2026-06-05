#pragma once

#include "d3d11_decode_context.h"
#include "video_frame.h"

#include <memory>
#include <vector>

#include <SDL3/SDL.h>

class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    virtual bool initialize(SDL_Window* window) = 0;
    virtual void resize() = 0;

    // Render one frame slot per camera into a grid. A null slot leaves its tile
    // blank (camera not yet live); slot order is the stable camera order.
    virtual void render(const std::vector<std::shared_ptr<VideoFrame>>& frames) = 0;

    // Focus a single tile so it fills the window; -1 returns to the grid.
    virtual void setFocusedTile(int index) = 0;
    virtual int focusedTile() const = 0;

    virtual std::shared_ptr<D3D11DecodeContext> d3d11DecodeContext() const { return {}; }
};

std::unique_ptr<VideoRenderer> createD3D11Renderer();
