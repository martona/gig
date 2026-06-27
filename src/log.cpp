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
#ifdef _WIN32
    localtime_s(&local, &seconds); // MSVC: (struct tm*, const time_t*)
#else
    localtime_r(&seconds, &local); // POSIX: (const time_t*, struct tm*) -- args swapped
#endif

    char timestamp[20] = {};
    std::snprintf(
        timestamp,
        sizeof(timestamp),
        "%02d:%02d:%02d.%03d",
        local.tm_hour,
        local.tm_min,
        local.tm_sec,
        static_cast<int>(millis.count()));

    std::string line;
    line.reserve(message.size() + 32);
    line.append(timestamp);
    line.append(" [");
    line.append(levelTag(level));
    line.append("] ");
    line.append(message.data(), message.size());

    std::lock_guard<std::mutex> lock(logMutex());
    LogBuffer::instance().push(line);
    std::cerr << line << '\n';
}

LogBuffer& LogBuffer::instance()
{
    static LogBuffer buffer;
    return buffer;
}

void LogBuffer::push(std::string line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(std::move(line));
    while (lines_.size() > maxLines_) {
        lines_.pop_front();
    }
}

void LogBuffer::snapshot(std::vector<std::string>& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    out.assign(lines_.begin(), lines_.end());
}

void LogBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
}

} // namespace gig
