#pragma once

#include "video_frame.h"

#include <memory>

#include <SDL3/SDL.h>

class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    virtual bool initialize(SDL_Window* window) = 0;
    virtual void resize() = 0;
    virtual void render(const VideoFrame* frame) = 0;
};

std::unique_ptr<VideoRenderer> createD3D11Renderer();
