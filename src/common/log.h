#pragma once

#include <cstdarg>

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

// Thread-safe, writes one line to stderr.
void LogWrite(LogLevel level, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

}  // namespace mcast

#define MCAST_LOG(lvl, ...) ::mcast::LogWrite((lvl), __FILE__, __LINE__, __VA_ARGS__)
#define LOG_TRACE(...) MCAST_LOG(::mcast::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) MCAST_LOG(::mcast::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) MCAST_LOG(::mcast::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) MCAST_LOG(::mcast::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) MCAST_LOG(::mcast::LogLevel::Error, __VA_ARGS__)
