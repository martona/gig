#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>

namespace gig {
namespace {

std::mutex& logMutex()
{
    static std::mutex mutex;
    return mutex;
}

const char* levelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:   return "TRACE";
    case LogLevel::Debug:   return "DEBUG";
    case LogLevel::Info:    return "INFO ";
    case LogLevel::Warning: return "WARN ";
    case LogLevel::Error:   return "ERROR";
    }
    return "?????";
}

} // namespace

void writeLog(LogLevel level, std::string_view message)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm local {};
    localtime_s(&local, &seconds);

    char timestamp[20] = {};
    std::snprintf(
        timestamp,
        sizeof(timestamp),
        "%02d:%02d:%02d.%03d",
        local.tm_hour,
        local.tm_min,
        local.tm_sec,
        static_cast<int>(millis.count()));

    std::lock_guard<std::mutex> lock(logMutex());
    std::cerr << timestamp << " [" << levelTag(level) << "] " << message << '\n';
}

} // namespace gig
