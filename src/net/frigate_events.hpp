#pragma once

#include "net/tls_options.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace boost::json {
class value;
} // namespace boost::json

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
//   - NO automatic state replay on connect -- but Frigate (0.14+) answers a
//     client-published "onConnect" topic with a "camera_activity" snapshot
//     (current motion + tracked-object list per camera; the seeding its own
//     web UI does on open/refocus). We publish it right after the handshake,
//     so activity that started BEFORE we joined is known immediately.
//   - "events"/"reviews" are a verbose firehose; ignored here.
//
// SEEDING PROTOCOL -- ugly by necessity; read the WHY before touching it.
//
// The camera_activity snapshot is untrustworthy in two specific ways, both
// verified at source level against Frigate v0.15.2 + v0.16.4 (2026-07-14):
//
//   (a) It is a BROADCAST, not a reply. The dispatcher fans the response out
//       to every connected ws client (+ MQTT) with no correlation id, so
//       snapshots also arrive unsolicited whenever any OTHER viewer's UI
//       connects or refocuses a tab -- indistinguishable from ours.
//   (b) It can carry state STALER than edges already delivered to us, with
//       nothing on the wire (no seq, no version, no timestamp) to detect it.
//       Frigate's Dispatcher has no locks; handle_on_connect runs on the ws
//       thread with a copy->broadcast gap (0.15), and 0.16 additionally
//       publishes count edges BEFORE committing the snapshot's source dict
//       (activity_manager.py:121 vs :93 -- the whole camera/zone loop plus
//       synchronous socket writes sit inside that window). Because count
//       topics publish only on CHANGE, accepting a stale snapshot over a
//       fresher edge would wedge phantom state until the next real
//       transition -- unbounded on a quiet camera.
//
// Against that, the snapshot is the ONLY witness to pre-join state: after we
// join, the edge stream is a complete record of everything that CHANGES
// ("<cam>/all" carries absolute counts, not deltas). The only post-join gap
// a snapshot can fill is the per-label breakdown of objects that never
// transition again (e.g. a car parked since before we joined). The
// snapshot's "motion" field is NOT seeded at all: it is raw instantaneous
// motion (len(motion_boxes) > 0 at generation), not the debounced
// "<cam>/motion" topic state -- seeding it latched a spurious, never-
// corrected motion=true on nearly every camera (see the .cpp).
//
// The protocol that squares this (user-specified, 2026-07-14):
//   1. ACCEPTANCE WINDOW: snapshots count only within kSeedWindowSeconds of
//      our own request, and only until the seed SETTLES. Outside that, they
//      are ignored outright -- per the above they carry nothing our edge
//      stream doesn't, and every one accepted is another roll of race (b).
//   2. AMBIGUITY, per camera: a snapshot's data for a camera is applied only
//      if that camera has been edge-quiet for kSnapshotEdgeQuietSeconds
//      (the verified race windows are milliseconds; the guard is generous
//      for the NORMAL case -- a server that stalls multi-seconds inside its
//      own race window, e.g. one wedged viewer blocking 0.16's synchronous
//      broadcast loop, can still defeat it; undetectable client-side
//      without a wire seq). A camera that raced an edge is marked AMBIGUOUS
//      -- not silently dropped. Already-Clean cameras skip redundant
//      re-application. A camera an earlier snapshot marked ambiguous that a
//      later one OMITS flips to clean: whatever raced has departed (its
//      edges already told us) or the camera is role-filtered (dev builds
//      after 0.17 reshape the broadcast per recipient) and no snapshot will
//      ever carry it.
//   3. RETRY: while any camera is ambiguous, re-request the snapshot after
//      kSeedRetryDelay (logged) and restart the window; give up after
//      kSeedMaxRetries with a warning naming the stragglers. A perpetually
//      busy camera may never seed -- acceptable: its absolute counts are
//      already correct from its own edges, and only pre-join label detail /
//      motion lag until the object's next transition.
//   4. ABSENT = CLEAN: a camera missing from a snapshot (0.15 omits
//      never-active cameras; post-0.17 dev filters by viewer role) or
//      present with config only (0.16+) has nothing knowable -- it must not
//      hold the seed open. Cameras never mentioned stay Unknown and don't
//      drive retries.
//
// A pre-0.14 server never answers: no snapshot, no ambiguity, no retries --
// state stays edge-only exactly as before this protocol existed. Callers'
// show-all settle window on (re)connect remains as belt-and-braces for that
// case. Constants live next to kOnConnectFrame in the .cpp.
//
// One background thread connects, parses, and keeps a per-camera snapshot the
// render loop polls. Reconnects on its own (5s -> 60s doubling backoff); the
// steady heartbeat traffic doubles as an idle watchdog, so a wedged-but-open
// socket is detected and recycled.
class FrigateEvents {
public:
    struct CameraState {
        int objectCount = 0;       // <cam>/all: tracked objects present right now
        int activeObjectCount = 0; // <cam>/all/active: excludes STATIONARY objects
        bool motion = false;       // <cam>/motion: raw motion currently ON
        // nowSeconds() stamps of the last moment each signal was (still)
        // positive; 0 = never seen. Kept per signal so the caller can apply
        // its "does raw motion count?" / "active only?" policy to the linger
        // window too.
        double lastObjectAt = 0.0;
        double lastActiveObjectAt = 0.0;
        double lastMotionAt = 0.0;
        // Per-label object counts ("<cam>/person" and "<cam>/person/active")
        // -- feed the label reason suffix ("driveway - person"). std::map so
        // iteration (and the joined reason string) is deterministic.
        std::map<std::string, int> objects;
        std::map<std::string, int> activeObjects;
        // Labels whose count just dropped to 0, stamped at the drop edge --
        // activityReason renders them as "person (gone)" for
        // kGoneLingerSeconds instead of vanishing the instant Frigate sends
        // the off edge (detections misfire / end sooner than a human can look
        // up at the wall, and a tile whose caption just evaporated reads as
        // "why is this cam showing?"). One ledger per count map, because each
        // is its own signal: in activeOnly mode a car going STATIONARY is the
        // departure that matters. A label's >0 edge erases its ledger entry;
        // entries older than the linger are ignored at render time and
        // overwritten whenever the label next departs.
        std::map<std::string, double> goneObjects;
        std::map<std::string, double> goneActiveObjects;
        // Mirrors ActivityGate::kLingerSeconds (app layer -- net/ can't
        // include it) so in activity view the last "(gone)" tag rides exactly
        // as long as the tile itself lingers after its final drop edge.
        static constexpr double kGoneLingerSeconds = 30.0;
        // "<cam>/status/detect" heartbeat (every ~10s per camera): last
        // payload + arrival stamp. A camera counts as DOWN only after we've
        // heard from it at least once -- never-heard is unknown, not down
        // (right after a connect everything is unheard; no false alarms).
        bool statusOnline = true;
        double lastStatusAt = 0.0;
        // Last activity EDGE (count/motion topic; NOT heartbeats) for this
        // camera -- the ambiguity detector of the seeding protocol (see the
        // class comment): snapshot data for a camera with an edge fresher
        // than kSnapshotEdgeQuietSeconds is not applied, the camera is
        // marked ambiguous, and the seed is re-requested.
        double lastEdgeAt = 0.0;
        static constexpr double kSnapshotEdgeQuietSeconds = 2.0;

        // Staleness threshold DELIBERATELY beyond the socket's own idle
        // watchdog (45s idle timeout; beast's timer granularity means the
        // recycle can land up to ~67s after the last frame): if the whole
        // LINK dies silently, the watchdog must recycle it (clearing these
        // states) before any camera reads as stale -- otherwise a dead AP
        // would briefly report "N cameras are down" when zero cameras are.
        // A genuinely dead camera doesn't wait this long anyway: Frigate
        // publishes an explicit non-"online" status within ~20s.
        static constexpr double kStatusStaleSeconds = 75.0;
        bool down(double now) const
        {
            return lastStatusAt > 0.0
                && (!statusOnline || now - lastStatusAt > kStatusStaleSeconds);
        }
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
    // "camera_activity": apply a snapshot under the seeding protocol (class
    // comment) -- acceptance window, per-camera ambiguity, settle detection.
    void handleActivitySnapshot(const boost::json::value& payload);
    void clearStates();

    // --- seeding protocol (all take stateMutex_; called from the io thread) ---
    // Stamp the acceptance window: an onConnect frame just went out.
    void noteSeedRequested();
    // Any camera still ambiguous and the seed not settled? (Drives the retry
    // timer in the ws phase.)
    bool seedRetryPending() const;
    // Consume one retry: logs and returns true to send, or -- past
    // kSeedMaxRetries -- settles with a warning naming the stragglers and
    // returns false.
    bool takeSeedRetryTicket();

    enum class SeedStatus : unsigned char {
        Unknown,   // no snapshot has mentioned this camera (absent = clean-ish)
        Clean,     // seeded (or config-only: nothing to know); further copies redundant
        Ambiguous, // snapshot raced an edge; awaiting a retry
    };

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
    // Seeding-protocol state (guarded by stateMutex_, reset by clearStates):
    std::vector<SeedStatus> seedStatus_; // index-aligned with states_
    double seedRequestedAt_ = 0.0;       // nowSeconds() of the last onConnect send
    int seedRetries_ = 0;
    bool seedSettled_ = false;           // all clean, or gave up: ignore snapshots
};

// Why a camera counts as active, for the label suffix ("driveway - person"):
// the object labels joined (alphabetical -- std::map order), each tagged
// " (gone)" for kGoneLingerSeconds after its off edge ("pool - minivan
// (gone), person"), else "motion" while raw motion is ON, else empty.
// `now` is FrigateEvents::nowSeconds() -- the clock the gone stamps use.
// activeOnly picks the stationary-excluding counts so a parked car doesn't
// caption its tile. motionCounts mirrors the gate's policy
// (ActivityGate::evaluate): when the user has motion counting OFF, motion
// isn't activity -- so it can't be a caption either (a reason force-shows
// the label even in ErrorOnly mode).
inline std::string activityReason(const FrigateEvents::CameraState& state, bool motionCounts,
                                  bool activeOnly, double now)
{
    const std::map<std::string, int>& counts = activeOnly ? state.activeObjects : state.objects;
    const std::map<std::string, double>& gone = activeOnly ? state.goneActiveObjects : state.goneObjects;
    std::string reason;
    const auto append = [&reason](const std::string& label, bool departed) {
        if (!reason.empty()) {
            reason += ", ";
        }
        reason += label;
        if (departed) {
            reason += " (gone)";
        }
    };
    const auto lingering = [now](double goneAt) {
        return goneAt > 0.0 && now - goneAt < FrigateEvents::CameraState::kGoneLingerSeconds;
    };
    // Alphabetical merge of the two sorted maps. A departed label normally
    // sits in BOTH (its count entry at 0, its ledger entry stamped): the
    // present count wins, else the ledger entry captions the departure.
    auto countIt = counts.begin();
    auto goneIt = gone.begin();
    while (countIt != counts.end() || goneIt != gone.end()) {
        int order = 0;
        if (countIt == counts.end()) {
            order = 1;
        } else if (goneIt == gone.end()) {
            order = -1;
        } else {
            order = countIt->first.compare(goneIt->first);
        }
        if (order < 0) {
            if (countIt->second > 0) {
                append(countIt->first, false);
            }
            ++countIt;
        } else if (order > 0) {
            if (lingering(goneIt->second)) {
                append(goneIt->first, true);
            }
            ++goneIt;
        } else {
            if (countIt->second > 0) {
                append(countIt->first, false);
            } else if (lingering(goneIt->second)) {
                append(goneIt->first, true);
            }
            ++countIt;
            ++goneIt;
        }
    }
    if (reason.empty() && motionCounts && state.motion) {
        reason = "motion";
    }
    return reason;
}

} // namespace gig
