#pragma once

#include "net/tls_options.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gig {

class TlsSessionCache;
class CookieJar;

// Real-time per-camera activity from Frigate's /ws feed -- the same topics its
// own web UI consumes, served on the API port behind the same nginx auth
// (frigate_token cookie) and TLS we already terminate. No MQTT broker involved,
// and it works whether or not Frigate has MQTT configured.
//
// Empirical protocol notes (frigate-ws-probe capture, 2026-07-13):
//   - messages are {"topic": ..., "payload": ...}; topics have NO "frigate/"
//     prefix ("cam_frontdoor/all", "cam_doorbell/motion", "events", ...)
//   - payload is polymorphic: bare number for object counts, bare string for
//     motion ("ON"/"OFF") and the ~10s "<cam>/status/detect" heartbeat
//   - "<cam>/all" = count of tracked objects on that camera (all labels);
//     "<cam>/motion" = raw motion with a server-side off-delay (~30s+)
//   - NO state replay on connect: activity is unknown until the next edge, so
//     callers treat (re)connect like startup (show-all until things settle)
//   - "events"/"reviews" are a verbose firehose; ignored here.
//
// One background thread connects, parses, and keeps a per-camera snapshot the
// render loop polls. Reconnects on its own (5s -> 60s doubling backoff); the
// steady heartbeat traffic doubles as an idle watchdog, so a wedged-but-open
// socket is detected and recycled.
class FrigateEvents {
public:
    struct CameraState {
        int objectCount = 0;    // <cam>/all: tracked objects present right now
        bool motion = false;    // <cam>/motion: raw motion currently ON
        // nowSeconds() stamps of the last moment each signal was (still)
        // positive; 0 = never seen. Kept per signal so the caller can apply
        // its "does raw motion count?" policy to the linger window too.
        double lastObjectAt = 0.0;
        double lastMotionAt = 0.0;
    };

    FrigateEvents(std::string baseUrl, TlsOptions tls,
                  std::shared_ptr<TlsSessionCache> sessionCache,
                  std::shared_ptr<CookieJar> cookieJar);
    ~FrigateEvents();

    FrigateEvents(const FrigateEvents&) = delete;
    FrigateEvents& operator=(const FrigateEvents&) = delete;

    // Begin the feed for these FRIGATE camera names (the /api/config keys --
    // NOT the display labels, which may be go2rtc stream names). Order is
    // stable and index-aligned with snapshot(). No-op if already started.
    void start(std::vector<std::string> cameraNames);

    // Stop the thread and drop all state. Safe to call repeatedly.
    void stop();

    // True while the websocket is up. While false, callers should fall back
    // to showing everything rather than trusting an empty activity set.
    bool connected() const { return connected_.load(); }

    // Per-camera activity, index-aligned with the start() names.
    std::vector<CameraState> snapshot() const;

    // Monotonic seconds on the clock the CameraState stamps use.
    static double nowSeconds();

private:
    void runLoop();
    // One connect -> read-until-error cycle; returns a description for the log
    // (empty when cancelled). Sets liveSeconds to how long the socket was up.
    std::string runOnce(double& liveSeconds);
    void handleMessage(const std::string& text);
    void clearStates();

    const std::string baseUrl_;
    const TlsOptions tls_;
    const std::shared_ptr<TlsSessionCache> sessionCache_;
    const std::shared_ptr<CookieJar> cookieJar_;

    std::thread thread_;
    std::atomic_bool stopRequested_ { false };
    std::atomic_bool connected_ { false };
    std::condition_variable cv_;        // interruptible backoff sleep
    std::mutex cvMutex_;

    // Wakes a blocked websocket read from stop() (io_context::stop is one of
    // the few thread-safe asio members); same pattern as HttpClient::cancel.
    std::mutex ioMutex_;
    void* activeIo_ = nullptr;          // boost::asio::io_context*, type-erased

    mutable std::mutex stateMutex_;
    std::vector<std::string> cameraNames_;
    std::unordered_map<std::string, int> cameraIndex_;
    std::vector<CameraState> states_;
};

} // namespace gig
