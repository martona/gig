#pragma once

#include "d3d11_decode_context.h"
#include "decode/ffmpeg_decoder.h"
#include "discovery/frigate_discovery.h"
#include "net/http_client.hpp"
#include "net/tls_options.h"
#include "net/tls_session_cache.hpp"
#include "video_frame.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gig {

struct SupervisorConfig {
    std::string baseUrl; // Frigate control-plane base for the health poll; empty disables polling
    TlsOptions tls;
    bool softwareDecode = false;
    std::chrono::seconds pollInterval { 5 };
    // Per-camera spread on initial connect so earlier handshakes seed the shared
    // TLS session cache before later cameras connect (more resumption).
    std::chrono::milliseconds startupStagger { 50 };
};

// Owns one decoder per camera and the authoritative liveness signal. A health
// thread polls go2rtc byte counters every pollInterval and reconciles: a camera
// whose bytes advance should be running; one that stalls is torn down; one that
// resumes is brought back. FFmpeg handles its own transient reconnects in
// between. Layout/order is fixed (stable slots); liveness only changes contents.
class CameraSupervisor {
public:
    CameraSupervisor(
        std::vector<CameraStream> cameras,
        SupervisorConfig config,
        std::shared_ptr<D3D11DecodeContext> decodeContext,
        std::shared_ptr<TlsSessionCache> sessionCache,
        std::shared_ptr<CookieJar> cookieJar);
    ~CameraSupervisor();

    CameraSupervisor(const CameraSupervisor&) = delete;
    CameraSupervisor& operator=(const CameraSupervisor&) = delete;

    void start();
    void stop();

    // Latest frame per camera in stable camera order (null = no current frame).
    std::vector<std::shared_ptr<VideoFrame>> snapshotFrames() const;

    std::size_t cameraCount() const { return slots_.size(); }
    std::uint64_t totalDecodedFrames() const { return decodedFrames_.load(); }
    int liveCameraCount() const { return liveCameras_.load(); }
    // Aggregate go2rtc ingest bandwidth (camera->go2rtc) across all streams.
    int ingestKbps() const { return ingestKbps_.load(); }

    // Control-plane (go2rtc health poll) reachability for the status UI. Updated
    // by the poll thread each cycle; safe to read from the UI thread.
    struct ControlPlaneHealth {
        bool polling = false;     // health poll active (baseUrl configured)
        bool ok = true;           // last poll succeeded
        bool schemaError = false; // producers present but no known byte field
        int secondsSinceOk = 0;   // since last successful poll (0 if ok / not polling)
    };
    ControlPlaneHealth controlPlaneHealth() const;

    // Per-camera cumulative downloaded bytes (stable slot order) for the UI's
    // "receiving" activity animation. Advances while a decoder is pulling data,
    // stalls when it isn't -- so the renderer can tell "alive, no keyframe yet"
    // from "actually stuck". Read from the UI thread; bumped by decoder threads.
    std::vector<std::uint64_t> tileByteCounts() const;

    // The on-demand stream policy: a disabled slot's decoder is torn down (its
    // tile blanks); re-enabling reconnects it. All slots start enabled. With
    // the health poll running the change is applied BY the poll thread (kicked
    // immediately, so a woken camera connects right away and the caller never
    // blocks on a join); without polling (single-url mode) it applies inline
    // on the caller thread, which owns slot lifecycle in that mode. A disabled
    // slot keeps its LAST liveness verdict (its go2rtc producer may idle
    // precisely because we stopped consuming it, so its byte counter proves
    // nothing) -- a policy-hidden camera never reads as a down camera; health
    // is re-assessed on re-enable. Safe to call from the UI thread; no-op when
    // unchanged.
    void setSlotEnabled(std::size_t index, bool enabled);

private:
    enum class Liveness { Unknown, Online, Offline };
    static const char* livenessName(Liveness liveness);

    struct CameraSlot {
        CameraStream info;
        std::unique_ptr<FfmpegDecoder> decoder;
        std::uint64_t lastByteCount = 0;
        bool haveByteBaseline = false;
        Liveness liveness = Liveness::Unknown;
        bool wasEnabled = true; // poll-thread only: enable-edge detection
    };

    void pollLoop();
    void reconcile();
    void startDecoder(std::size_t index);
    void stopDecoder(std::size_t index);

    SupervisorConfig config_;
    std::shared_ptr<D3D11DecodeContext> decodeContext_;
    std::shared_ptr<TlsSessionCache> sessionCache_;
    std::shared_ptr<CookieJar> cookieJar_;
    std::shared_ptr<TlsClient> videoTls_; // one shared TLS holder for all video connections

    std::vector<CameraSlot> slots_; // lifecycle owned by the poll thread (or start() when not polling)
    // One byte counter per slot, owned here so it outlives any decoder and the UI
    // never touches a decoder being torn down. Bumped by the AVIO read path.
    std::unique_ptr<std::atomic<std::uint64_t>[]> slotBytes_;
    // Desired stream state per slot (setSlotEnabled), written by the UI thread,
    // consumed by reconcile() -- all slots_ mutation stays on the poll thread.
    std::unique_ptr<std::atomic<bool>[]> slotEnabled_;

    mutable std::mutex frameMutex_;
    std::vector<std::shared_ptr<VideoFrame>> latestFrames_; // guarded by frameMutex_

    std::unique_ptr<HttpClient> healthClient_;

    std::mutex pollMutex_;
    std::condition_variable pollCv_;
    std::atomic_bool stopRequested_ { false };
    // Breaks the poll wait early (its predicate would otherwise sleep out the
    // full interval on a bare notify): a slot-enable change must reconcile NOW.
    std::atomic_bool reconcileRequested_ { false };
    std::thread pollThread_;

    std::atomic<std::uint64_t> decodedFrames_ { 0 };
    std::atomic<int> liveCameras_ { 0 };
    std::atomic<int> ingestKbps_ { 0 };
    std::uint64_t lastTotalBytes_ = 0;   // poll-thread only
    bool haveBandwidthBaseline_ = false; // poll-thread only

    // Control-plane health, written by the poll thread, read by the UI thread.
    mutable std::mutex healthMutex_;
    bool healthPolling_ = false;
    bool healthOk_ = true;
    bool healthSchemaError_ = false;
    std::chrono::steady_clock::time_point healthLastOk_;
};

} // namespace gig
