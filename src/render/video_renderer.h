#pragma once

#include "d3d11_decode_context.h"
#include "video_frame.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

struct OverlayStats {
    bool showDiagnostics = true;
    int camerasOnline = 0;
    int camerasOffline = 0;
    double fps = 0.0;
    std::uint64_t framesTotal = 0;
    int kbps = 0;
    double cpuPercent = 0.0;

    // Connection status, derived each refresh by the run loop. LinkState::Ok with
    // healthDegraded == false means nothing is wrong (no banner). Reconnecting =
    // a live session whose control-plane poll is failing (it is self-healing);
    // Disconnected = no session up (needs a Reconnect).
    enum class LinkState { Ok, Reconnecting, Disconnected };
    LinkState link = LinkState::Ok;
    bool healthDegraded = false; // go2rtc liveness unreadable (schema changed)
    int secondsSinceData = 0;    // since last good control-plane contact (Reconnecting)
    std::string statusHost;      // host/base shown in the banner
    std::string statusDetail;    // short reason (Disconnected)
};

class VideoRenderer {
public:
    // A button press in the on-screen toolbar, polled by the run loop. The log
    // toggle is handled inside the renderer; these two need the app.
    enum class ToolbarAction { None, Settings, Reconnect };

    virtual ~VideoRenderer() = default;

    virtual bool initialize(SDL_Window* window) = 0;
    virtual void resize() = 0;

    // Render one frame slot per camera into a grid. A null slot leaves its tile
    // blank (camera not yet live); slot order is the stable camera order.
    virtual void render(const std::vector<std::shared_ptr<VideoFrame>>& frames) = 0;

    // Focus a single tile so it fills the window; -1 returns to the grid.
    virtual void setFocusedTile(int index) = 0;
    virtual int focusedTile() const = 0;

    // Per-camera labels (stable camera order) and live diagnostics for the HUD.
    virtual void setCameraLabels(const std::vector<std::string>& labels) = 0;
    virtual void setDiagnostics(const OverlayStats& stats) = 0;

    // Feed an SDL event to any in-renderer UI (ImGui). Returns true if the UI
    // consumed it, so the caller should skip its own handling of that event.
    virtual bool handleEvent(const SDL_Event& event) { (void)event; return false; }

    // The full-window log view overlay (renders the captured log buffer).
    virtual void setLogViewVisible(bool visible) { (void)visible; }
    virtual bool logViewVisible() const { return false; }

    // Returns + clears the toolbar button pressed since the last call.
    virtual ToolbarAction takeToolbarAction() { return ToolbarAction::None; }

    // Vertical space (logical points) the toolbar reserves above the grid, so the
    // run loop's click hit-testing matches the rendered layout. 0 in focus view.
    virtual float reservedTopLogical() const { return 0.0f; }

    virtual std::shared_ptr<D3D11DecodeContext> d3d11DecodeContext() const { return {}; }
};

std::unique_ptr<VideoRenderer> createD3D11Renderer();
