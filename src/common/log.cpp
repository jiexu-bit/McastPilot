#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#include <io.h>
#define MCAST_ISATTY _isatty
#define MCAST_FILENO _fileno
#define strcasecmp _stricmp
#else
#include <unistd.h>
#define MCAST_ISATTY isatty
#define MCAST_FILENO fileno
#endif

namespace mcast {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_log_mutex;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
        default: return "     ";
    }
}

const char* LevelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "\x1b[90m";
        case LogLevel::Debug: return "\x1b[36m";
        case LogLevel::Info: return "\x1b[0m";
        case LogLevel::Warn: return "\x1b[33m";
        case LogLevel::Error: return "\x1b[31m";
        default: return "\x1b[0m";
    }
}

bool ColorEnabled() {
    static const bool enabled = MCAST_ISATTY(MCAST_FILENO(stderr)) != 0;
    return enabled;
}

void LocalTime(std::tm& out, std::time_t t) {
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
}

}  // namespace

void SetLogLevel(LogLevel level) { g_level.store(static_cast<int>(level)); }

LogLevel GetLogLevel() { return static_cast<LogLevel>(g_level.load()); }

bool ParseLogLevel(const char* text, LogLevel& out) {
    if (!text) return false;
    if (!strcasecmp(text, "trace")) { out = LogLevel::Trace; return true; }
    if (!strcasecmp(text, "debug")) { out = LogLevel::Debug; return true; }
    if (!strcasecmp(text, "info")) { out = LogLevel::Info; return true; }
    if (!strcasecmp(text, "warn") || !strcasecmp(text, "warning")) { out = LogLevel::Warn; return true; }
    if (!strcasecmp(text, "error")) { out = LogLevel::Error; return true; }
    if (!strcasecmp(text, "off") || !strcasecmp(text, "none")) { out = LogLevel::Off; return true; }
    return false;
}

void LogWrite(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;
    if (level == LogLevel::Off) return;

    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    LocalTime(tm, t);

    char body[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min,
                  tm.tm_sec, static_cast<int>(ms.count()));

    const char* base = file ? file : "";
    const char* slash = std::strrchr(base, '/');
    const char* bslash = std::strrchr(base, '\\');
    if (slash && bslash) base = (slash > bslash ? slash : bslash) + 1;
    else if (slash) base = slash + 1;
    else if (bslash) base = bslash + 1;

    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (ColorEnabled()) {
        std::fprintf(stderr, "%s %s%s\x1b[0m [%s:%d] %s\n", stamp, LevelColor(level),
                     LevelName(level), base, line, body);
    } else {
        std::fprintf(stderr, "%s %s [%s:%d] %s\n", stamp, LevelName(level), base, line, body);
    }
    std::fflush(stderr);
}

}  // namespace mcast
