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

#include "video_frame.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

@interface GIGEngine (Internal)

// Per-camera frame snapshot in stable camera order (empty when stopped/busy).
- (std::vector<std::shared_ptr<VideoFrame>>)snapshotFramesInternal;

// Per-camera cumulative downloaded bytes (drives the signal-scope animation).
- (std::vector<std::uint64_t>)tileByteCountsInternal;

// Per-camera labels in stable camera order.
- (std::vector<std::string>)cameraLabelsInternal;

@end
