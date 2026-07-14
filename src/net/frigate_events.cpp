#include "net/frigate_events.hpp"

#include "log.hpp"
#include "net/cert_pin.hpp"
#include "net/cookie_jar.hpp"
#include "net/tls_context.hpp"
#include "net/tls_session_cache.hpp"
#include "net/url.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <openssl/ssl.h>

namespace gig {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;

// Heartbeats ("<cam>/status/detect") arrive every ~10s per camera, so a link
// silent for this long is dead even if TCP still ACKs; beast pings at half
// this and errors out at the full interval.
constexpr auto kIdleTimeout = std::chrono::seconds(45);
constexpr auto kHandshakeTimeout = std::chrono::seconds(15);

// Published right after the websocket handshake: Frigate's dispatcher (0.14+)
// answers with a "camera_activity" state snapshot -- /ws has no replay, and
// without this a camera active since BEFORE we joined stays invisible until
// its next edge. Exact shape Frigate's own web UI sends on open/refocus.
// Static storage: the async write completes alongside the reads.
const std::string kOnConnectFrame =
    R"({"topic":"onConnect","payload":"onConnect","retain":false})";

// Seeding-protocol tuning (the SEEDING PROTOCOL block in the header is the
// documentation; these are the knobs it names). Window and delay are
// user-chosen (2026-07-14): 15s comfortably covers any response latency
// without leaving the race-acceptance window open all session; 10s between
// retries lets a transition burst pass instead of re-rolling into it.
constexpr double kSeedWindowSeconds = 15.0;
constexpr auto kSeedRetryDelay = std::chrono::seconds(10);
constexpr int kSeedMaxRetries = 5;

// Frigate suffixes a tracked object's label when a sub_label is verified
// ("person-verified"); fold those into the base label for the reason text.
std::string normalizedLabel(std::string_view label)
{
    constexpr std::string_view kVerified = "-verified";
    if (label.size() > kVerified.size()
        && label.substr(label.size() - kVerified.size()) == kVerified) {
        label.remove_suffix(kVerified.size());
    }
    return std::string(label);
}

} // namespace

FrigateEvents::FrigateEvents(std::string baseUrl, TlsOptions tls,
                             std::shared_ptr<TlsSessionCache> sessionCache,
                             std::shared_ptr<CookieJar> cookieJar)
    : baseUrl_(trimTrailingSlashes(std::move(baseUrl)))
    , tls_(std::move(tls))
    , sessionCache_(std::move(sessionCache))
    , cookieJar_(std::move(cookieJar))
{
}

FrigateEvents::~FrigateEvents()
{
    stop();
}

double FrigateEvents::nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FrigateEvents::start(std::vector<std::string> cameraNames)
{
    if (thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        cameraNames_ = std::move(cameraNames);
        cameraIndex_.clear();
        for (int i = 0; i < static_cast<int>(cameraNames_.size()); ++i) {
            cameraIndex_[cameraNames_[static_cast<std::size_t>(i)]] = i;
        }
        states_.assign(cameraNames_.size(), CameraState {});
        seedStatus_.assign(cameraNames_.size(), SeedStatus::Unknown);
    }
    stopRequested_ = false;
    thread_ = std::thread([this] { runLoop(); });
}

void FrigateEvents::stop()
{
    stopRequested_ = true;
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (activeIo_) {
            static_cast<asio::io_context*>(activeIo_)->stop();
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    connected_ = false;
    clearStates();
}

std::vector<FrigateEvents::CameraState> FrigateEvents::snapshot() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return states_;
}

void FrigateEvents::clearStates()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::fill(states_.begin(), states_.end(), CameraState {});
    // Every (re)connect runs the seeding protocol from scratch.
    std::fill(seedStatus_.begin(), seedStatus_.end(), SeedStatus::Unknown);
    seedRequestedAt_ = 0.0;
    seedRetries_ = 0;
    seedSettled_ = false;
}

void FrigateEvents::noteSeedRequested()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    seedRequestedAt_ = nowSeconds();
}

bool FrigateEvents::seedRetryPending() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return !seedSettled_
        && std::find(seedStatus_.begin(), seedStatus_.end(), SeedStatus::Ambiguous)
        != seedStatus_.end();
}

bool FrigateEvents::takeSeedRetryTicket()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (seedSettled_) {
        return false;
    }
    if (seedRetries_ >= kSeedMaxRetries) {
        // Step 3's escape hatch: a perpetually busy camera never presents an
        // edge-quiet window. Cheap to live with -- its ABSOLUTE counts are
        // already current from its own edges; only pre-join label detail and
        // the motion flag lag until the object's next transition.
        std::string stragglers;
        for (std::size_t i = 0; i < seedStatus_.size(); ++i) {
            if (seedStatus_[i] == SeedStatus::Ambiguous) {
                if (!stragglers.empty()) {
                    stragglers += ", ";
                }
                stragglers += cameraNames_[i];
            }
        }
        seedSettled_ = true;
        logWarning() << "activity seed: giving up after " << kSeedMaxRetries
                     << " attempts (" << stragglers << " kept racing edges); counts are "
                     << "current from the live feed, pre-join label detail may lag";
        return false;
    }
    ++seedRetries_;
    logInfo() << "activity seed: snapshot raced an edge; re-requesting (attempt "
              << seedRetries_ << "/" << kSeedMaxRetries << ")";
    return true;
}

void FrigateEvents::runLoop()
{
    logInfo() << "activity feed: watching " << baseUrl_ << "/ws";
    int backoffSeconds = 0;
    while (!stopRequested_.load()) {
        double liveSeconds = 0.0;
        const std::string error = runOnce(liveSeconds);
        connected_ = false;
        // No replay on reconnect: stale "active" latches would mis-show tiles.
        clearStates();
        if (stopRequested_.load()) {
            break;
        }
        // A connection that held for a while earns a fresh backoff.
        if (liveSeconds > 60.0) {
            backoffSeconds = 0;
        }
        backoffSeconds = backoffSeconds == 0 ? 5 : std::min(backoffSeconds * 2, 60);
        logWarning() << "activity feed: " << (error.empty() ? "connection closed" : error)
                     << "; retrying in " << backoffSeconds << "s";
        std::unique_lock<std::mutex> lock(cvMutex_);
        cv_.wait_for(lock, std::chrono::seconds(backoffSeconds),
                     [this] { return stopRequested_.load(); });
    }
    logInfo() << "activity feed: stopped";
}

std::string FrigateEvents::runOnce(double& liveSeconds)
{
    liveSeconds = 0.0;
    try {
        const ParsedUrl parsed = parseUrl(baseUrl_);
        const std::string origin = originForUrl(parsed);
        const std::string wsTarget =
            (parsed.target == "/" ? std::string() : trimTrailingSlashes(parsed.target)) + "/ws";

        asio::io_context io;
        {
            std::lock_guard<std::mutex> lock(ioMutex_);
            activeIo_ = &io;
        }
        struct IoGuard {
            FrigateEvents* self;
            ~IoGuard()
            {
                std::lock_guard<std::mutex> lock(self->ioMutex_);
                self->activeIo_ = nullptr;
            }
        } ioGuard { this };

        tcp::resolver resolver(io);
        const auto endpoints = resolver.resolve(parsed.host, parsed.port);

        // One bounded async step (connect / TLS handshake). The websocket
        // phase below runs the io_context continuously instead, because
        // beast's websocket timeout machinery keeps its own timer pending.
        auto runStep = [&](auto&& initiate) -> boost::system::error_code {
            if (stopRequested_.load()) {
                return asio::error::operation_aborted;
            }
            boost::system::error_code result;
            initiate([&result](boost::system::error_code ec) { result = ec; });
            io.restart();
            io.run();
            if (stopRequested_.load() && !result) {
                result = asio::error::operation_aborted;
            }
            return result;
        };

        // The whole websocket phase, generic over plain vs TLS stream.
        // Returns the human-readable exit reason ("" for a cancelled stop).
        auto wsPhase = [&](auto& ws) -> std::string {
            ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& request) {
                request.set(http::field::user_agent, "gig");
                const std::string cookie = cookieJar_ ? cookieJar_->headerFor(origin) : std::string();
                if (!cookie.empty()) {
                    request.set(http::field::cookie, cookie);
                }
            }));
            websocket::stream_base::timeout timeouts {};
            timeouts.handshake_timeout = kHandshakeTimeout;
            timeouts.idle_timeout = kIdleTimeout;
            timeouts.keep_alive_pings = true;
            ws.set_option(timeouts);
            // beast: the websocket timeout machinery replaces the tcp_stream
            // expiry; both at once produces spurious timeouts.
            beast::get_lowest_layer(ws).expires_never();

            boost::system::error_code exitEc;
            bool handshakeDone = false;
            bool writeInFlight = false;
            asio::steady_timer seedTimer(io);
            bool seedTimerArmed = false;
            websocket::response_type upgradeResponse;
            beast::flat_buffer buffer;
            // Publish the onConnect frame (one concurrent read + one write is
            // legal on a beast websocket; writes never overlap -- the next one
            // is a retry >= kSeedRetryDelay later). Best-effort: a write error
            // surfaces through the read loop, and a pre-0.14 server just
            // ignores the topic. writeInFlight gates close and retries.
            std::function<void()> sendSeedRequest = [&] {
                writeInFlight = true;
                ws.text(true);
                ws.async_write(asio::buffer(kOnConnectFrame),
                               [&](boost::system::error_code, std::size_t) {
                                   writeInFlight = false;
                               });
                noteSeedRequested();
            };
            // Step 3 of the seeding protocol (header): while any camera is
            // ambiguous, re-request after a delay. Armed from the read loop
            // whenever a snapshot leaves ambiguity behind; the ticket call
            // does the retry accounting (and the giving-up).
            std::function<void()> armSeedRetry = [&] {
                if (seedTimerArmed || !seedRetryPending()) {
                    return;
                }
                seedTimerArmed = true;
                seedTimer.expires_after(kSeedRetryDelay);
                seedTimer.async_wait([&](boost::system::error_code ec) {
                    seedTimerArmed = false;
                    if (ec || stopRequested_.load() || !seedRetryPending()) {
                        return;
                    }
                    if (writeInFlight) {
                        // Pathological (a write outlives a 10s timer only if
                        // the peer wedged, which the idle watchdog will kill);
                        // don't burn a ticket, just try again later.
                        armSeedRetry();
                        return;
                    }
                    if (takeSeedRetryTicket()) {
                        sendSeedRequest();
                    }
                });
            };
            std::function<void()> readNext = [&] {
                ws.async_read(buffer, [&](boost::system::error_code ec, std::size_t) {
                    if (ec) {
                        exitEc = ec;
                        io.stop();
                        return;
                    }
                    if (ws.got_text()) {
                        handleMessage(beast::buffers_to_string(buffer.data()));
                    }
                    buffer.consume(buffer.size());
                    if (stopRequested_.load()) {
                        io.stop();
                        return;
                    }
                    armSeedRetry();
                    readNext();
                });
            };
            ws.async_handshake(upgradeResponse, hostHeader(parsed), wsTarget,
                               [&](boost::system::error_code ec) {
                                   if (ec) {
                                       exitEc = ec;
                                       io.stop();
                                       return;
                                   }
                                   handshakeDone = true;
                                   connected_ = true;
                                   logInfo() << "activity feed: connected";
                                   sendSeedRequest();
                                   readNext();
                               });
            const double startedAt = nowSeconds();
            io.restart();
            io.run();
            if (handshakeDone) {
                liveSeconds = nowSeconds() - startedAt;
            }
            // A pending retry timer is io_context "work": left alive, the
            // close-phase io.run() below would block until it fires (up to
            // 10s of shutdown hang). Kill it before any further run.
            seedTimer.cancel();

            if (stopRequested_.load() && handshakeDone) {
                // Best-effort close handshake, tightly bounded, so Frigate's
                // log doesn't fill with aborted connections on our shutdown.
                // Skipped in the sliver where an onConnect write is still in
                // flight: beast 1.91 serializes close against an in-flight
                // write internally, but the docs are ambiguous enough that we
                // stay conservative -- an abortive close is fine on that path.
                if (!writeInFlight) {
                    timeouts.handshake_timeout = std::chrono::seconds(2);
                    ws.set_option(timeouts);
                    ws.async_close(websocket::close_code::normal, [](boost::system::error_code) {});
                    io.restart();
                    io.run();
                }
                return {};
            }
            if (stopRequested_.load()) {
                return {};
            }
            if (!handshakeDone) {
                std::string reason = "websocket handshake failed: "
                    + (exitEc ? exitEc.message() : std::string("no response"));
                if (upgradeResponse.result_int() != 0) {
                    reason += " (HTTP " + std::to_string(upgradeResponse.result_int()) + ")";
                }
                return reason;
            }
            if (exitEc == websocket::error::closed) {
                return "closed by server";
            }
            return exitEc ? exitEc.message() : std::string("connection ended");
        };

        if (parsed.scheme == "https") {
            asio::ssl::context tlsContext(asio::ssl::context::tls_client);
            configureSslContext(tlsContext, tls_);
            if (sessionCache_) {
                enableSessionCache(tlsContext.native_handle());
            }
            websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(io, tlsContext);
            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), parsed.host.c_str())) {
                return "failed to set TLS SNI host name";
            }
            prepareConnectionPinning(ws.next_layer().native_handle(), parsed.host);
            if (sessionCache_) {
                offerCachedSession(ws.next_layer().native_handle(), *sessionCache_);
            }

            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
            boost::system::error_code ec = runStep([&](auto&& complete) {
                beast::get_lowest_layer(ws).async_connect(
                    endpoints,
                    [c = std::forward<decltype(complete)>(complete)](
                        boost::system::error_code e, const tcp::endpoint&) mutable { c(e); });
            });
            if (ec) {
                return "connect " + parsed.host + ": " + ec.message();
            }
            ec = runStep([&](auto&& complete) {
                ws.next_layer().async_handshake(
                    asio::ssl::stream_base::client,
                    [c = std::forward<decltype(complete)>(complete)](
                        boost::system::error_code e) mutable { c(e); });
            });
            if (ec) {
                return "tls handshake " + parsed.host + ": " + ec.message();
            }
            return wsPhase(ws);
        }

        websocket::stream<beast::tcp_stream> ws(io);
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
        boost::system::error_code ec = runStep([&](auto&& complete) {
            beast::get_lowest_layer(ws).async_connect(
                endpoints,
                [c = std::forward<decltype(complete)>(complete)](
                    boost::system::error_code e, const tcp::endpoint&) mutable { c(e); });
        });
        if (ec) {
            return "connect " + parsed.host + ": " + ec.message();
        }
        return wsPhase(ws);
    } catch (const std::exception& error) {
        return error.what();
    }
}

void FrigateEvents::handleMessage(const std::string& text)
{
    // Consumed topics (everything else -- events/reviews firehose, stats,
    // set/state control echoes -- is ignored, though it still counts as
    // traffic for the idle watchdog just by arriving):
    //   "camera_activity"      per-camera state snapshot; answers our
    //                          "onConnect" publish (and any other client's)
    //   "<cam>/all"            bare int, tracked objects (the activity trigger)
    //   "<cam>/all/active"     bare int, EXCLUDES stationary objects (parked
    //                          cars, delivered packages) -- the active-only mode
    //   "<cam>/motion"         "ON"/"OFF"
    //   "<cam>/<label>"        bare int per object label (reason suffix)
    //   "<cam>/<label>/active" bare int, active objects of that label
    //   "<cam>/status/detect"  "online" heartbeat every ~10s (down detection)
    boost::system::error_code parseEc;
    const boost::json::value parsed = boost::json::parse(text, parseEc);
    if (parseEc || !parsed.is_object()) {
        return;
    }
    const boost::json::object& message = parsed.get_object();
    const boost::json::value* topicValue = message.if_contains("topic");
    const boost::json::value* payload = message.if_contains("payload");
    if (!topicValue || !topicValue->is_string() || !payload) {
        return;
    }
    const std::string_view topic = topicValue->get_string();
    if (topic == "camera_activity") {
        handleActivitySnapshot(*payload);
        return;
    }
    const std::size_t slash = topic.find('/');
    if (slash == std::string_view::npos) {
        return;
    }
    const std::string camera(topic.substr(0, slash));
    const std::string_view kind = topic.substr(slash + 1);

    int numeric = 0;
    bool isNumeric = false;
    if (payload->is_int64()) {
        numeric = static_cast<int>(payload->get_int64());
        isNumeric = true;
    } else if (payload->is_uint64()) {
        numeric = static_cast<int>(payload->get_uint64());
        isNumeric = true;
    } else if (payload->is_double()) {
        numeric = static_cast<int>(payload->get_double());
        isNumeric = true;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto found = cameraIndex_.find(camera);
    if (found == cameraIndex_.end()) {
        return;
    }
    CameraState& state = states_[static_cast<std::size_t>(found->second)];

    if (kind == "all") {
        if (!isNumeric) {
            return;
        }
        const bool wasPositive = state.objectCount > 0;
        state.objectCount = std::max(0, numeric);
        if (state.objectCount > 0 || wasPositive) {
            state.lastObjectAt = nowSeconds(); // linger runs from the DROP edge
        }
        state.lastEdgeAt = nowSeconds();
        return;
    }
    if (kind == "all/active") {
        if (!isNumeric) {
            return;
        }
        const bool wasPositive = state.activeObjectCount > 0;
        state.activeObjectCount = std::max(0, numeric);
        if (state.activeObjectCount > 0 || wasPositive) {
            state.lastActiveObjectAt = nowSeconds();
        }
        state.lastEdgeAt = nowSeconds();
        return;
    }
    if (kind == "motion") {
        if (!payload->is_string()) {
            return;
        }
        const bool on = payload->get_string() == "ON";
        const bool wasOn = state.motion;
        state.motion = on;
        if (on || wasOn) {
            state.lastMotionAt = nowSeconds();
        }
        state.lastEdgeAt = nowSeconds();
        return;
    }
    if (kind == "status/detect") {
        if (!payload->is_string()) {
            return;
        }
        state.statusOnline = payload->get_string() == "online";
        state.lastStatusAt = nowSeconds();
        return;
    }
    // "<cam>/<label>" and "<cam>/<label>/active" object-count topics (person,
    // car, dog, ...): a single label segment with a numeric payload. The
    // numeric check alone filters the string-payload siblings (review_status,
    // snapshots, ...); multi-segment paths like "x/state" fail both shapes.
    if (!isNumeric) {
        return;
    }
    const std::size_t kindSlash = kind.find('/');
    if (kindSlash == std::string_view::npos) {
        state.objects[std::string(kind)] = std::max(0, numeric);
        state.lastEdgeAt = nowSeconds();
    } else if (kind.substr(kindSlash + 1) == "active"
               && kind.substr(0, kindSlash).find('/') == std::string_view::npos) {
        state.activeObjects[std::string(kind.substr(0, kindSlash))] = std::max(0, numeric);
        state.lastEdgeAt = nowSeconds();
    }
}

void FrigateEvents::handleActivitySnapshot(const boost::json::value& payload)
{
    // {"<cam>": {"motion": bool, "objects": [{"label", "stationary", ...}],
    //            "config": {...}}, ...} -- structured /ws payloads arrive
    // JSON-encoded as a string (double-parse, like "events"); accept a bare
    // object too in case a future version stops re-encoding.
    boost::json::value decoded;
    if (payload.is_string()) {
        boost::system::error_code parseEc;
        decoded = boost::json::parse(payload.get_string(), parseEc);
        if (parseEc) {
            return;
        }
    } else {
        decoded = payload;
    }
    if (!decoded.is_object()) {
        return;
    }

    const double now = nowSeconds();
    std::lock_guard<std::mutex> lock(stateMutex_);
    // Acceptance gate (steps 1 of the seeding protocol -- see the header):
    // once settled, snapshots carry nothing our edge stream doesn't, and
    // outside the window after OUR request they're another viewer's
    // broadcast. Either way, applying one is only another chance to lose the
    // snapshot-staler-than-edges race, for zero information gain.
    if (seedSettled_ || seedRequestedAt_ == 0.0
        || now - seedRequestedAt_ > kSeedWindowSeconds) {
        return;
    }
    std::vector<bool> mentioned(states_.size(), false);
    for (const auto& entry : decoded.get_object()) {
        const auto found = cameraIndex_.find(std::string(entry.key()));
        if (found == cameraIndex_.end() || !entry.value().is_object()) {
            continue;
        }
        const boost::json::object& cam = entry.value().get_object();
        const auto index = static_cast<std::size_t>(found->second);
        CameraState& state = states_[index];
        mentioned[index] = true;

        // Already seeded: this copy is redundant (and re-applying it would
        // re-roll the race for no gain).
        if (seedStatus_[index] == SeedStatus::Clean) {
            continue;
        }
        // Step 2: a camera that raced one of our own edges MAY be staler
        // than what we already know (verified server-side windows; header).
        // Mark ambiguous for the retry loop instead of applying.
        if (now - state.lastEdgeAt < CameraState::kSnapshotEdgeQuietSeconds) {
            seedStatus_[index] = SeedStatus::Ambiguous;
            continue;
        }

        // A camera that has never reported activity appears with "config"
        // only (0.16+) or not at all (0.15): no "objects"/"motion" keys means
        // the server has nothing authoritative to say -- leave the state
        // alone. An empty objects ARRAY, by contrast, authoritatively means
        // "nothing tracked right now" and zeroes us out.
        const boost::json::value* objectsValue = cam.if_contains("objects");
        if (objectsValue && objectsValue->is_array()) {
            int total = 0;
            int active = 0;
            std::map<std::string, int> objects;
            std::map<std::string, int> activeObjects;
            for (const boost::json::value& item : objectsValue->get_array()) {
                if (!item.is_object()) {
                    continue;
                }
                const boost::json::object& object = item.get_object();
                std::string label;
                if (const boost::json::value* labelValue = object.if_contains("label");
                    labelValue && labelValue->is_string()) {
                    label = normalizedLabel(labelValue->get_string());
                }
                const boost::json::value* stationaryValue = object.if_contains("stationary");
                const bool stationary = stationaryValue && stationaryValue->is_bool()
                    && stationaryValue->get_bool();
                ++total;
                if (!label.empty()) {
                    ++objects[label];
                }
                if (!stationary) {
                    ++active;
                    if (!label.empty()) {
                        ++activeObjects[label];
                    }
                }
            }
            // Same stamp semantics as the edge topics: the linger window runs
            // from the drop edge as well as from activity.
            if (total > 0 || state.objectCount > 0) {
                state.lastObjectAt = now;
            }
            if (active > 0 || state.activeObjectCount > 0) {
                state.lastActiveObjectAt = now;
            }
            state.objectCount = total;
            state.activeObjectCount = active;
            state.objects = std::move(objects);
            state.activeObjects = std::move(activeObjects);
        }

        if (const boost::json::value* motionValue = cam.if_contains("motion");
            motionValue && motionValue->is_bool()) {
            const bool on = motionValue->get_bool();
            if (on || state.motion) {
                state.lastMotionAt = now;
            }
            state.motion = on;
        }

        // Step 4: config-only entries land here too -- the server had
        // nothing to say about this camera, which is itself an answer.
        seedStatus_[index] = SeedStatus::Clean;
    }

    // A camera marked Ambiguous by an EARLIER snapshot that this one omits
    // is done: 0.15 omits cameras with no activity (so whatever raced has
    // since departed, and its departure edge already told us), and post-0.17
    // dev role-filtering omits cameras we can never see (so no snapshot will
    // ever seed them). Either way there's nothing left to fetch -- without
    // this, such a camera would burn every retry and then be falsely named
    // in the give-up warning.
    for (std::size_t i = 0; i < seedStatus_.size(); ++i) {
        if (seedStatus_[i] == SeedStatus::Ambiguous && !mentioned[i]) {
            seedStatus_[i] = SeedStatus::Clean;
        }
    }

    // Settle when nothing is ambiguous. Cameras no snapshot ever mentioned
    // stay Unknown and must not hold the seed open (0.15 omits never-active
    // cameras entirely). Settling closes the acceptance gate for good --
    // from here on, the edge stream is the sole source of truth.
    if (std::find(seedStatus_.begin(), seedStatus_.end(), SeedStatus::Ambiguous)
        == seedStatus_.end()) {
        seedSettled_ = true;
        if (seedRetries_ > 0) {
            logInfo() << "activity seed: settled after " << seedRetries_
                      << (seedRetries_ == 1 ? " retry" : " retries");
        }
    }
}

} // namespace gig
