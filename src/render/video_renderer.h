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

    // Full-window status screen over the (empty) grid area when there is no
    // running session -- the onboarding/error surface. None = normal video UI.
    // Welcome: nothing configured yet (first run / after Forget Settings).
    // Connecting: a connect is in flight (drawn before the blocking apply).
    // Error: the last connect failed; statusDetail carries the reason and
    // errorIsConfig picks the primary CTA (Settings vs Try Again).
    enum class StatusScreen { None, Welcome, Connecting, Error };
    StatusScreen screen = StatusScreen::None;
    bool errorIsConfig = false;

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

// When to draw the per-tile camera label. ErrorOnly shows it only while a tile
// has no live frame (the connecting / reconnecting "signal" phase), where it's
// useful to identify which camera; it stays out of the way once video is up.
enum class LabelMode { Hide = 0, ErrorOnly = 1, Always = 2 };

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

    // Whether the last render had time-driven animation in flight: the zoom
    // transition, the procedural "signal" scope on a frameless tile (or its
    // resolve fade), or the focus-view toolbar still counting down to auto-hide.
    // The run loop renders on demand -- it keeps drawing while this is true so
    // those animations stay smooth, and otherwise draws only when a new decoded
    // frame arrives or input/stats change. Default true = always render (safe for
    // a renderer that doesn't track this yet).
    virtual bool isAnimating() const { return true; }

    // Focus a single tile so it fills the window; -1 returns to the grid.
    virtual void setFocusedTile(int index) = 0;
    virtual int focusedTile() const = 0;

    // Per-camera labels (stable camera order) and live diagnostics for the HUD.
    virtual void setCameraLabels(const std::vector<std::string>& labels) = 0;
    virtual void setDiagnostics(const OverlayStats& stats) = 0;

    // When to draw the per-tile labels. Changes rarely (settings only), so it's a
    // setter rather than per-frame state.
    virtual void setLabelMode(LabelMode mode) { (void)mode; }

    // The camera tile under the mouse (-1 = none), for the hover affordance. Fed
    // from the run loop's mouse-motion handling.
    virtual void setHoveredTile(int index) { (void)index; }

    // Per-camera cumulative downloaded bytes (stable order), pushed each frame so
    // the renderer can animate a data-driven "receiving / reconnecting" signal on
    // tiles with no displayable frame yet. The renderer smooths the deltas into a
    // per-tile activity level; flat deltas read as cold (stuck), rising as alive.
    virtual void setTileActivity(const std::vector<std::uint64_t>& byteCounts) { (void)byteCounts; }

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

// Platform renderer factory: D3D11 on Windows, Metal on macOS (a clear-only stub
// for now). Exactly one definition is compiled per platform.
std::unique_ptr<VideoRenderer> createRenderer();
