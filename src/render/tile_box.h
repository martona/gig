#pragma once

#include <string>
#include <vector>

namespace gig {

// One detection box on a camera tile, in video-frame-normalized coordinates
// (0..1 across the displayed video; x1 < x2, y1 < y2). Fed per tile by the
// run loop from the /ws activity feed (see activityBoxes in
// net/frigate_events.hpp), subset-aligned like the labels/reasons.
//
// `id` is Frigate's tracked-object id -- stable for the object's lifetime, so
// renderers key their position easing on it (boxes GLIDE to each ~350ms
// update instead of teleporting). `gone` switches the lingering-departure
// style (blue) over the live-detection style (red); `fade` is a 0..1 opacity
// multiplier the policy layer uses to ease out the end of the linger.
struct TileBox {
    std::string id;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    bool gone = false;
    float fade = 1.0f;

    // The run loop pushes box lists only on change; memberwise equality is
    // exactly that change test.
    bool operator==(const TileBox&) const = default;
};

using TileBoxList = std::vector<TileBox>;

} // namespace gig
