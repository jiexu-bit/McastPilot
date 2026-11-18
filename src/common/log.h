#pragma once

#include <cstdarg>
#include <functional>
#include <string>

namespace mcast {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
bool ParseLogLevel(const char* text, LogLevel& out);

// Optional sink used by the daemon to mirror logs to a file, per-task ring
// buffers and WebSocket clients. The callback must be thread-safe and must not
// log recursively.
using LogSink = std::function<void(LogLevel, const std::string& context,
                                   const std::string& line)>;
void SetLogSink(LogSink sink);

// Task id attached to log lines emitted by the current thread. Pipeline reader
// and sender threads set this so per-task logs can be routed without changing
// every call site.
void SetThreadLogContext(const std::string& context);
std::string ThreadLogContext();

// Thread-safe, writes one line to stderr and forwards to the sink.
void LogWrite(LogLevel level, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

// For messages produced by external libraries (FFmpeg) that already contain a
// formatted line.
void LogExternal(LogLevel level, const std::string& message);

}  // namespace mcast

#define MCAST_LOG(lvl, ...) ::mcast::LogWrite((lvl), __FILE__, __LINE__, __VA_ARGS__)
#define LOG_TRACE(...) MCAST_LOG(::mcast::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) MCAST_LOG(::mcast::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) MCAST_LOG(::mcast::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) MCAST_LOG(::mcast::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) MCAST_LOG(::mcast::LogLevel::Error, __VA_ARGS__)
