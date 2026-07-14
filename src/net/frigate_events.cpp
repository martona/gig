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
            websocket::response_type upgradeResponse;
            beast::flat_buffer buffer;
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
                                   readNext();
                               });
            const double startedAt = nowSeconds();
            io.restart();
            io.run();
            if (handshakeDone) {
                liveSeconds = nowSeconds() - startedAt;
            }

            if (stopRequested_.load() && handshakeDone) {
                // Best-effort close handshake, tightly bounded, so Frigate's
                // log doesn't fill with aborted connections on our shutdown.
                timeouts.handshake_timeout = std::chrono::seconds(2);
                ws.set_option(timeouts);
                ws.async_close(websocket::close_code::normal, [](boost::system::error_code) {});
                io.restart();
                io.run();
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
    // {"topic": "<cam>/all", "payload": 1} / {"topic": "<cam>/motion", "payload": "ON"}.
    // Everything else (events/reviews firehose, stats, heartbeats, per-label
    // counts) is deliberately ignored -- the heartbeats still count as traffic
    // for the idle watchdog just by arriving.
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
    const std::size_t slash = topic.find('/');
    if (slash == std::string_view::npos) {
        return;
    }
    const std::string camera(topic.substr(0, slash));
    const std::string_view kind = topic.substr(slash + 1);

    if (kind == "all") {
        int count = 0;
        if (payload->is_int64()) {
            count = static_cast<int>(payload->get_int64());
        } else if (payload->is_uint64()) {
            count = static_cast<int>(payload->get_uint64());
        } else if (payload->is_double()) {
            count = static_cast<int>(payload->get_double());
        } else {
            return;
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto found = cameraIndex_.find(camera);
        if (found == cameraIndex_.end()) {
            return;
        }
        CameraState& state = states_[static_cast<std::size_t>(found->second)];
        const bool wasPositive = state.objectCount > 0;
        state.objectCount = std::max(0, count);
        if (state.objectCount > 0 || wasPositive) {
            state.lastObjectAt = nowSeconds(); // linger runs from the DROP edge
        }
        return;
    }
    if (kind == "motion") {
        if (!payload->is_string()) {
            return;
        }
        const bool on = payload->get_string() == "ON";
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto found = cameraIndex_.find(camera);
        if (found == cameraIndex_.end()) {
            return;
        }
        CameraState& state = states_[static_cast<std::size_t>(found->second)];
        const bool wasOn = state.motion;
        state.motion = on;
        if (on || wasOn) {
            state.lastMotionAt = nowSeconds();
        }
    }
}

} // namespace gig
