#include "common/file_log.h"

#include <filesystem>
#include <system_error>

namespace mcast {
namespace fs = std::filesystem;

bool FileLogger::Open(const std::string& path, int64_t max_bytes, int keep, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open()) out_.close();

    path_ = path;
    max_bytes_ = max_bytes;
    keep_ = keep < 1 ? 1 : keep;

    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            err = "cannot create log directory '" + parent.string() + "': " + ec.message();
            return false;
        }
    }

    out_.open(path, std::ios::binary | std::ios::app);
    if (!out_) {
        err = "cannot open log file " + path;
        return false;
    }
    const auto size = fs::file_size(path, ec);
    size_ = ec ? 0 : static_cast<int64_t>(size);
    return true;
}

void FileLogger::RotateLocked() {
    if (!out_.is_open()) return;
    out_.close();

    std::error_code ec;
    for (int i = keep_ - 1; i >= 1; --i) {
        const fs::path from = path_ + "." + std::to_string(i);
        const fs::path to = path_ + "." + std::to_string(i + 1);
        if (!fs::exists(from, ec)) continue;
        fs::remove(to, ec);
        fs::rename(from, to, ec);
    }
    const fs::path first = path_ + ".1";
    fs::remove(first, ec);
    fs::rename(path_, first, ec);

    out_.open(path_, std::ios::binary | std::ios::trunc);
    size_ = 0;
}

void FileLogger::Write(LogLevel level, const std::string& context, const std::string& line) {
    (void)level;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) return;

    std::string text;
    if (!context.empty()) {
        text = "[" + context + "] ";
    }
    text += line;
    text += '\n';

    if (max_bytes_ > 0 && size_ + static_cast<int64_t>(text.size()) > max_bytes_) {
        RotateLocked();
        if (!out_.is_open()) return;
    }

    out_.write(text.data(), static_cast<std::streamsize>(text.size()));
    out_.flush();
    if (out_) size_ += static_cast<int64_t>(text.size());
}

}  // namespace mcast
