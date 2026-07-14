#include "app/activity_gate.h"

#include <algorithm>

namespace gig {

void ActivityGate::reset()
{
    shownSince_.clear();
    wasActive_.clear();
    wasConnected_ = false;
    connectedAt_ = 0.0;
}

ActivityGate::Result ActivityGate::evaluate(
    bool activityMode, bool motionCounts, bool feedConnected,
    double secondsSinceInteraction,
    const std::vector<FrigateEvents::CameraState>& states,
    int cameraCount)
{
    Result result;
    const std::size_t count = static_cast<std::size_t>(std::max(0, cameraCount));
    if (shownSince_.size() != count) {
        shownSince_.assign(count, 0.0);
        wasActive_.assign(count, false);
    }

    const bool statesUsable = states.size() == count && count > 0;
    const double now = FrigateEvents::nowSeconds();

    // /ws sends no state replay: right after ANY (re)connect the states are
    // all zeros regardless of reality, so every feedConnected rising edge
    // starts a show-all settle window (same length as the peek). Covers the
    // silent reconnects -- the feed's own backoff loop, an app auto-retry --
    // that never touch the interaction clock.
    if (feedConnected && !wasConnected_) {
        connectedAt_ = now;
    }
    wasConnected_ = feedConnected;
    const bool settling = feedConnected && (now - connectedAt_) < kPeekSeconds;

    // Wake edges fire in EVERY mode (activity wakes a dimmed wall even when it
    // shows all cameras), but only while the feed is trustworthy.
    if (statesUsable && feedConnected) {
        for (std::size_t i = 0; i < count; ++i) {
            const FrigateEvents::CameraState& s = states[i];
            const bool active = s.objectCount > 0 || (motionCounts && s.motion);
            if (active && !wasActive_[i]) {
                result.wakeEdge = true;
            }
            wasActive_[i] = active;
        }
    } else {
        std::fill(wasActive_.begin(), wasActive_.end(), false);
    }

    const bool wantFilter = activityMode && feedConnected && statesUsable;
    const bool peeking = secondsSinceInteraction < kPeekSeconds;
    if (!wantFilter || peeking || settling) {
        result.visible.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            result.visible[static_cast<std::size_t>(i)] = static_cast<int>(i);
        }
        // Show-all is not "shown by the filter": min-display bookkeeping
        // restarts when the filter re-engages, so a peek doesn't buy every
        // camera an extra kMinShowSeconds of show-all.
        std::fill(shownSince_.begin(), shownSince_.end(), 0.0);
        return result;
    }

    result.filtered = true;
    for (std::size_t i = 0; i < count; ++i) {
        const FrigateEvents::CameraState& s = states[i];
        const bool active = s.objectCount > 0 || (motionCounts && s.motion);
        const double lastActiveAt = motionCounts ? std::max(s.lastObjectAt, s.lastMotionAt)
                                                 : s.lastObjectAt;
        const bool lingering = lastActiveAt > 0.0 && (now - lastActiveAt) < kLingerSeconds;
        const bool holding = shownSince_[i] > 0.0 && (now - shownSince_[i]) < kMinShowSeconds;
        if (active || lingering || holding) {
            if (shownSince_[i] <= 0.0) {
                shownSince_[i] = now;
            }
            result.visible.push_back(static_cast<int>(i));
        } else {
            shownSince_[i] = 0.0;
        }
    }
    result.quiet = result.visible.empty();
    return result;
}

void StreamPolicy::reset()
{
    lastOnScreenAt_.clear();
    desired_.clear();
}

const std::vector<char>& StreamPolicy::evaluate(
    int cameraCount, const std::vector<int>& onScreen, bool keepHidden, double now)
{
    const std::size_t count = static_cast<std::size_t>(std::max(0, cameraCount));
    if (lastOnScreenAt_.size() != count) {
        // Fresh session (or camera-set change): everything counts as just seen,
        // so nothing tears down before a full stop-delay has really elapsed.
        lastOnScreenAt_.assign(count, now);
        desired_.assign(count, 1);
    }
    if (keepHidden) {
        std::fill(lastOnScreenAt_.begin(), lastOnScreenAt_.end(), now);
        std::fill(desired_.begin(), desired_.end(), 1);
        return desired_;
    }
    for (const int cam : onScreen) {
        if (cam >= 0 && cam < static_cast<int>(count)) {
            lastOnScreenAt_[static_cast<std::size_t>(cam)] = now;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        desired_[i] = (now - lastOnScreenAt_[i]) < kStopDelaySeconds ? 1 : 0;
    }
    return desired_;
}

} // namespace gig
