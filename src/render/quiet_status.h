#pragma once

#include <ctime>
#include <string>

namespace gig {

// The empty-grid liveness line for "active cameras" view mode: a spoken-clock
// phrase ("It's ten past four and everything is quiet.") that wanders to a new
// spot every minute -- tells the user the app is alive without inviting
// burn-in. Shared by the desktop status panel and the iOS SwiftUI overlay so
// all platforms say (and place) exactly the same thing.

// camerasDown (from the /ws heartbeat liveness, NOT the streaming state, which
// the on-demand stream policy deliberately tears down) appends
// ", but 2 cameras are down" to the otherwise-reassuring line.
inline std::string quietStatusLine(const std::tm& local, int camerasDown = 0)
{
    static const char* kHours[12] = {
        "twelve", "one", "two", "three", "four", "five",
        "six", "seven", "eight", "nine", "ten", "eleven",
    };

    // Round to the nearest 5 minutes; 58..59 rolls into the next hour.
    int hour = local.tm_hour;
    int fives = (local.tm_min + 2) / 5 * 5;
    if (fives >= 60) {
        fives = 0;
        hour = (hour + 1) % 24;
    }

    auto hourWord = [&](int h24) -> std::string {
        if (h24 == 0) {
            return "midnight";
        }
        if (h24 == 12) {
            return "noon";
        }
        return kHours[h24 % 12];
    };

    std::string phrase;
    if (fives == 0) {
        const std::string word = hourWord(hour);
        phrase = (word == "midnight" || word == "noon") ? word : word + " o'clock";
    } else if (fives <= 30) {
        static const char* kPast[6] = {
            "five past", "ten past", "quarter past", "twenty past", "twenty-five past", "half past",
        };
        phrase = std::string(kPast[fives / 5 - 1]) + " " + hourWord(hour);
    } else {
        static const char* kTo[5] = {
            "twenty-five to", "twenty to", "quarter to", "ten to", "five to",
        };
        phrase = std::string(kTo[fives / 5 - 7]) + " " + hourWord((hour + 1) % 24);
    }
    std::string line = "It's " + phrase + " and everything is quiet";
    if (camerasDown == 1) {
        line += ", but one camera is down";
    } else if (camerasDown > 1) {
        line += ", but " + std::to_string(camerasDown) + " cameras are down";
    }
    return line + ".";
}

// --- "Hide offline cameras" ---------------------------------------------------
// The setting drops tiles with no displayable frame from the wall entirely.
// Shared policy bits so every platform hides (and says) exactly the same thing:

// How long after a session (re)build the hiding stays disarmed. Fresh decoders
// need a few seconds to their first keyframe; without the grace every startup
// would flash an empty wall + "N/N cameras are offline" before the first
// frames land. (A dead stream's stale frame is dropped by the supervisor after
// ~10s, so post-grace "no frame" is a truthful offline signal.)
constexpr double kHideOfflineGraceSeconds = 20.0;

// The empty-wall line when EVERY camera is offline (the only all-hidden case
// that gets a caption; a partially-hidden wall just shows the live tiles).
// Placed with quietStatusPlacement, exactly like the all-quiet line.
inline std::string offlineStatusLine(int camerasTotal)
{
    if (camerasTotal == 1) {
        return "1/1 camera is offline.";
    }
    const std::string n = std::to_string(camerasTotal);
    return n + "/" + n + " cameras are offline.";
}

// Deterministic wandering placement for the line, as fractions of the drawable
// area (top-left of the text). Feed it time(nullptr)/60 so every platform puts
// the text in the same spot and moves it once a minute.
inline void quietStatusPlacement(long long minuteIndex, float& fx, float& fy)
{
    unsigned long long h = static_cast<unsigned long long>(minuteIndex) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    fx = 0.08f + 0.68f * static_cast<float>(h & 0xFFFF) / 65535.0f;
    fy = 0.10f + 0.72f * static_cast<float>((h >> 16) & 0xFFFF) / 65535.0f;
}

} // namespace gig
