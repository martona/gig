#include "app/camera_supervisor.h"

#include "health/stream_health.h"
#include "log.hpp"

#include <utility>

namespace gig {

const char* CameraSupervisor::livenessName(Liveness liveness)
{
    switch (liveness) {
    case Liveness::Unknown: return "unknown";
    case Liveness::Online:  return "online";
    case Liveness::Offline: return "offline";
    }
    return "?";
}

CameraSupervisor::CameraSupervisor(
    std::vector<CameraStream> cameras,
    SupervisorConfig config,
    std::shared_ptr<D3D11DecodeContext> decodeContext,
    std::shared_ptr<TlsSessionCache> sessionCache)
    : config_(std::move(config))
    , decodeContext_(std::move(decodeContext))
    , sessionCache_(std::move(sessionCache))
    , latestFrames_(cameras.size())
{
    slots_.reserve(cameras.size());
    for (CameraStream& camera : cameras) {
        CameraSlot slot;
        slot.info = std::move(camera);
        slots_.push_back(std::move(slot));
    }
}

CameraSupervisor::~CameraSupervisor()
{
    stop();
}

void CameraSupervisor::start()
{
    if (config_.baseUrl.empty()) {
        logInfo() << "supervisor: no base URL; running " << slots_.size()
                  << " decoder(s) without health polling";
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            startDecoder(i);
        }
        return;
    }

    // Keep the health client's I/O timeout short so shutdown can't block on an
    // in-flight poll for the decoder's full 10s read timeout.
    TlsOptions healthTls = config_.tls;
    if (healthTls.rwTimeoutUs <= 0 || healthTls.rwTimeoutUs > 3'000'000) {
        healthTls.rwTimeoutUs = 3'000'000;
    }
    healthClient_ = std::make_unique<HttpClient>(config_.baseUrl, healthTls, sessionCache_);

    stopRequested_ = false;
    pollThread_ = std::thread([this] { pollLoop(); });
}

void CameraSupervisor::stop()
{
    stopRequested_ = true;
    pollCv_.notify_all();
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].decoder) {
            stopDecoder(i);
        }
    }
}

std::vector<std::shared_ptr<VideoFrame>> CameraSupervisor::snapshotFrames() const
{
    std::lock_guard<std::mutex> lock(frameMutex_);
    return latestFrames_;
}

void CameraSupervisor::pollLoop()
{
    logInfo() << "supervisor: health polling every " << config_.pollInterval.count()
              << "s for " << slots_.size() << " camera(s)";
    while (!stopRequested_) {
        reconcile();

        std::unique_lock<std::mutex> lock(pollMutex_);
        pollCv_.wait_for(lock, config_.pollInterval, [this] { return stopRequested_.load(); });
    }
}

void CameraSupervisor::reconcile()
{
    const StreamBytes bytes = fetchStreamBytes(*healthClient_);
    if (!bytes.ok) {
        // Never flip every camera offline on a fetch/parse failure -- that is the
        // go2rtc-reshuffle trap. Log (loudly on a schema change) and leave the
        // running decoders exactly as they are.
        if (bytes.schemaError) {
            logError() << "supervisor: " << bytes.error << " -- leaving decoders unchanged";
        } else {
            logWarning() << "supervisor: health poll failed (" << bytes.error
                         << "); leaving decoders unchanged";
        }
        return;
    }

    int online = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        CameraSlot& slot = slots_[i];
        const auto found = bytes.bytesByStream.find(slot.info.streamName);
        const std::uint64_t current = (found != bytes.bytesByStream.end()) ? found->second : 0;

        Liveness newLiveness;
        if (!slot.haveByteBaseline) {
            // First sample only establishes a baseline; assume runnable until a
            // delta proves otherwise.
            newLiveness = Liveness::Unknown;
        } else if (current == 0) {
            // No producer / no bytes at all -> camera is genuinely down.
            newLiveness = Liveness::Offline;
        } else if (current > slot.lastByteCount) {
            newLiveness = Liveness::Online;
        } else if (current < slot.lastByteCount) {
            // Counter went backwards but bytes still flow -> go2rtc/producer
            // restarted. Re-baseline and keep running this cycle rather than
            // bouncing every decoder.
            newLiveness = Liveness::Unknown;
        } else {
            // current == lastByteCount (and non-zero): stalled producer.
            newLiveness = Liveness::Offline;
        }

        slot.lastByteCount = current;
        slot.haveByteBaseline = true;

        if (newLiveness != slot.liveness) {
            logInfo() << "supervisor: " << slot.info.cameraName << " "
                      << livenessName(slot.liveness) << " -> " << livenessName(newLiveness);
            slot.liveness = newLiveness;
        }

        const bool shouldRun = (slot.liveness != Liveness::Offline);
        if (shouldRun && !slot.decoder) {
            startDecoder(i);
        } else if (!shouldRun && slot.decoder) {
            stopDecoder(i);
        }

        if (slot.liveness == Liveness::Online) {
            ++online;
        }
    }
    liveCameras_.store(online, std::memory_order_relaxed);

    // Aggregate ingest bandwidth from the same byte counters. Skip the cycle on
    // a counter reset (go2rtc restart) so it never reports a huge negative blip.
    std::uint64_t totalBytes = 0;
    for (const auto& entry : bytes.bytesByStream) {
        totalBytes += entry.second;
    }
    if (haveBandwidthBaseline_ && totalBytes >= lastTotalBytes_) {
        const double seconds = config_.pollInterval.count() > 0 ? static_cast<double>(config_.pollInterval.count()) : 5.0;
        const double kbps = static_cast<double>(totalBytes - lastTotalBytes_) * 8.0 / 1000.0 / seconds;
        ingestKbps_.store(static_cast<int>(kbps + 0.5), std::memory_order_relaxed);
    }
    lastTotalBytes_ = totalBytes;
    haveBandwidthBaseline_ = true;
}

void CameraSupervisor::startDecoder(std::size_t index)
{
    CameraSlot& slot = slots_[index];
    if (slot.decoder) {
        return;
    }

    logInfo() << "supervisor: starting decoder for " << slot.info.cameraName
              << " (" << slot.info.streamName << ")";

    slot.decoder = std::make_unique<FfmpegDecoder>(
        slot.info.streamUrl,
        config_.tls,
        decodeContext_,
        [this, index](VideoFrame&& frame) {
            decodedFrames_.fetch_add(1, std::memory_order_relaxed);
            auto shared = std::make_shared<VideoFrame>(std::move(frame));
            std::lock_guard<std::mutex> lock(frameMutex_);
            latestFrames_[index] = std::move(shared);
        },
        config_.softwareDecode);
    slot.decoder->start();
}

void CameraSupervisor::stopDecoder(std::size_t index)
{
    CameraSlot& slot = slots_[index];
    if (!slot.decoder) {
        return;
    }

    logInfo() << "supervisor: stopping decoder for " << slot.info.cameraName;
    slot.decoder->stop();
    slot.decoder.reset();

    // Blank the tile so a torn-down camera does not keep showing a frozen frame.
    std::lock_guard<std::mutex> lock(frameMutex_);
    latestFrames_[index].reset();
}

} // namespace gig
