//
//  GigBridgeInternal.h
//  gig
//
//  Internal ObjC++ surface of GIGEngine for the in-process renderer host
//  (GigRenderer.mm). NOT part of the Swift bridging header -- these methods
//  return C++ types.
//
//  All three accessors are non-blocking: they try_lock the engine mutex and
//  return empty when a connect()/disconnect() is in flight on another thread,
//  so the display-link render tick never stalls the main thread behind a
//  seconds-long network operation.
//

#import "GigBridge.h"

#include "net/frigate_events.hpp"
#include "video_frame.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Everything the render tick needs, captured under ONE try_lock so the pieces
// are mutually consistent (frames/labels/activity from the same instant).
// valid == false means the engine was busy (connect in flight): render nothing
// new this tick and keep all derived state (subset, focus) untouched.
struct GIGEngineTickSnapshot {
    bool valid = false;
    // Bumped on every session rebuild (connect). The host resets its
    // per-session derived state (gate, stream policy, subset) when this
    // changes -- a fresh supervisor starts fully enabled, and stale policy
    // timestamps must not tear its decoders down moments after they start.
    std::uint64_t sessionEpoch = 0;
    std::vector<std::shared_ptr<VideoFrame>> frames; // stable camera order
    std::vector<std::uint64_t> bytes;                // signal-scope activity
    std::vector<std::string> labels;                 // display labels
    std::vector<gig::FrigateEvents::CameraState> activity; // /ws feed states
    bool feedConnected = false;                      // /ws socket is up
};

@interface GIGEngine (Internal)

// Non-blocking consistent snapshot for the display-link tick.
- (GIGEngineTickSnapshot)tickSnapshotInternal;

// Push the desired per-camera stream states (the on-demand stream policy).
// Non-blocking: silently skipped while the engine is busy -- the caller
// re-pushes every tick, so a missed application self-heals. Each unchanged
// entry is a no-op inside the supervisor.
- (void)applyStreamPolicyInternal:(const std::vector<char> &)desired;

@end
