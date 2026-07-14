#pragma once

#include "net/frigate_events.hpp"

#include <vector>

namespace gig {

// Decides which camera tiles are visible in "active cameras" view mode, given
// the FrigateEvents snapshot. Pure view logic shared by the desktop run loop
// and the iOS render tick; owns only the small per-camera bookkeeping needed
// for hysteresis (min-display) and wake-edge detection.
//
// Rules (user-approved design):
//   - not in activity mode, feed down, or camera set mismatched -> show all
//     (the feature degrades to today's behavior, never to a wrongly-empty wall)
//   - any user interaction "peeks" the full grid; it settles back into the
//     filtered view after kPeekSeconds of idle. Startup counts as an
//     interaction (also covers /ws having no state replay on connect).
//   - a camera is active when it has tracked objects (<cam>/all > 0), or --
//     only if motionCounts -- raw motion is ON. Shadows of wind-blown trees
//     trigger raw motion, hence the opt-in.
//   - hysteresis so the layout doesn't thrash: a camera lingers
//     kLingerSeconds after its last activity, and once shown stays at least
//     kMinShowSeconds even if the activity was a blip.
class ActivityGate {
public:
    static constexpr double kPeekSeconds = 60.0;
    static constexpr double kLingerSeconds = 30.0;
    static constexpr double kMinShowSeconds = 20.0;

    struct Result {
        std::vector<int> visible; // camera indices, ascending (stable order)
        bool filtered = false;    // false = showing all (mode off / peek / fallback)
        bool quiet = false;       // filtered and nothing active: draw the quiet line
        bool wakeEdge = false;    // some camera just became active (undim the display)
    };

    // states must be index-aligned with the session's cameras; pass an empty
    // vector (or a mismatched size) to force show-all.
    Result evaluate(bool activityMode, bool motionCounts, bool feedConnected,
                    double secondsSinceInteraction,
                    const std::vector<FrigateEvents::CameraState>& states,
                    int cameraCount);

    // Forget per-camera bookkeeping (session reconfigured / cameras changed).
    void reset();

private:
    std::vector<double> shownSince_; // 0 = not currently shown by the filter
    std::vector<bool> wasActive_;    // wake-edge detection
    // Settle window after the feed (re)connects: /ws has NO state replay, so
    // right after a connect the states are all zeros regardless of reality.
    // Treat the first kPeekSeconds after every feedConnected rising edge as
    // show-all -- this is what makes silent reconnects (the feed's internal
    // backoff, an auto-retry) safe, not just user-initiated ones, WITHOUT
    // touching the interaction clock (no chrome un-hide).
    bool wasConnected_ = false;
    double connectedAt_ = 0.0;
};

} // namespace gig
