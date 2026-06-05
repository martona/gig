#pragma once

#include <sstream>
#include <string_view>
#include <utility>

namespace gig {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
};

// Emit a single line to stderr. Thread-safe; lines never interleave.
void writeLog(LogLevel level, std::string_view message);

// Stream-style log line: gig::logInfo() << "opening " << url;
// The line is written when the temporary is destroyed at the end of the
// full expression. Cheap to construct, never used on per-frame hot paths.
class LogLine {
public:
    explicit LogLine(LogLevel level)
        : level_(level)
    {
    }

    LogLine(LogLine&& other) noexcept
        : level_(other.level_)
        , stream_(std::move(other.stream_))
        , active_(other.active_)
    {
        other.active_ = false;
    }

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;
    LogLine& operator=(LogLine&&) = delete;

    ~LogLine()
    {
        if (active_) {
            writeLog(level_, stream_.str());
        }
    }

    template <typename Value>
    LogLine& operator<<(Value&& value)
    {
        stream_ << std::forward<Value>(value);
        return *this;
    }

private:
    LogLevel level_;
    std::ostringstream stream_;
    bool active_ = true;
};

inline LogLine logTrace() { return LogLine(LogLevel::Trace); }
inline LogLine logDebug() { return LogLine(LogLevel::Debug); }
inline LogLine logInfo() { return LogLine(LogLevel::Info); }
inline LogLine logWarning() { return LogLine(LogLevel::Warning); }
inline LogLine logError() { return LogLine(LogLevel::Error); }

} // namespace gig
