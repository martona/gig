#pragma once

// Shared imgui welcome / connecting / error panel drawn over the (empty) grid
// area when there is no running session -- the desktop onboarding surface,
// used by both the D3D11 (Windows) and Metal (macOS) renderers. Replaces the
// old first-run modal-settings loop and the banner-on-black disconnected state.
//
// Call between ImGui::NewFrame and ImGui::Render when stats.screen != None; the
// caller maps the returned actions onto its toolbar-action/log plumbing (and
// suppresses the slim status banner while the panel is up -- the panel carries
// the message).

#include "render/video_renderer.h" // OverlayStats

namespace gig {

struct StatusPanelAction {
    bool openSettings = false;
    bool retry = false;
    bool viewLog = false;
};

// `topOffsetLogical` is the toolbar strip height (the panel fills the window
// below it). `quietScale` scales the wandering quiet-status line's text (the
// user's label-size setting; the panel's own chrome stays fixed).
StatusPanelAction buildStatusPanel(const OverlayStats& stats, float topOffsetLogical,
                                   float quietScale = 1.0f);

} // namespace gig
