#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

#include "common/log.h"

namespace mcast {

// Size-based rolling file logger used by mcastd. Thread-safe; never logs about
// its own failures to avoid recursion.
class FileLogger {
public:
    bool Open(const std::string& path, int64_t max_bytes, int keep, std::string& err);
    void Write(LogLevel level, const std::string& context, const std::string& line);

private:
    void RotateLocked();

    std::mutex mutex_;
    std::ofstream out_;
    std::string path_;
    int64_t max_bytes_ = 0;
    int keep_ = 1;
    int64_t size_ = 0;
};

}  // namespace mcast
