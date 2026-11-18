#include "common/config.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mcast {
namespace fs = std::filesystem;
namespace {

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool WriteFileAtomic(const fs::path& path, const std::string& content, std::string& err) {
    return WriteTextFileAtomic(path.string(), content, err);
}

bool StartsWithPath(const std::string& path, const std::string& root) {
    if (path == root) return true;
    if (path.size() <= root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    const char next = path[root.size()];
    return next == '/' || next == '\\';
}

std::string CanonicalString(const std::string& path) {
    std::string text = RealPathUtf8(path);
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\')) text.pop_back();
    return text;
}

}  // namespace

std::string PathToUtf8(const fs::path& path) {
    const std::u8string value = path.u8string();
    return std::string(value.begin(), value.end());
}

fs::path PathFromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string NormalizePath(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    const fs::path absolute = fs::absolute(PathFromUtf8(path), ec);
    return ec ? path : PathToUtf8(absolute);
}

std::string RealPathUtf8(const std::string& path) {
    if (path.empty()) return path;
#ifdef _WIN32
    // std::filesystem::weakly_canonical does not always resolve NTFS
    // junctions, so ask the kernel for the final path of an open handle.
    const fs::path input = PathFromUtf8(path);
    HANDLE handle = CreateFileW(input.wstring().c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(handle);
        if (length > 0 && length < buffer.size()) {
            buffer.resize(length);
            if (buffer.rfind(L"\\\\?\\UNC\\", 0) == 0) {
                buffer = L"\\\\" + buffer.substr(8);
            } else if (buffer.rfind(L"\\\\?\\", 0) == 0) {
                buffer = buffer.substr(4);
            }
            return PathToUtf8(fs::path(buffer));
        }
    }
#endif
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(PathFromUtf8(path), ec);
    if (ec) canonical = fs::absolute(PathFromUtf8(path), ec);
    return ec ? path : PathToUtf8(canonical);
}

bool WriteTextFileAtomic(const std::string& path, const std::string& content,
                         std::string& err) {
    const fs::path target(path);
    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write " + temp.string();
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            err = "cannot flush " + temp.string();
            return false;
        }
    }

#ifdef _WIN32
    // Virus scanners and the previous process may briefly hold tasks.json or
    // config.json, making the replace fail with ACCESS_DENIED. Retry briefly,
    // then fall back to remove + rename.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (MoveFileExW(temp.wstring().c_str(), target.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) {
            err = "cannot replace " + target.string() + " (error " +
                  std::to_string(code) + ")";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    std::error_code fallback_ec;
    fs::remove(target, fallback_ec);
    fs::rename(temp, target, fallback_ec);
    if (!fallback_ec) return true;
    err = "cannot replace " + target.string() + ": " + fallback_ec.message();
    return false;
#else
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        err = "cannot replace " + target.string() + ": " + ec.message();
        return false;
    }
    return true;
#endif
}

std::string DefaultDataDir() {
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
        return PathToUtf8(fs::path(local) / "Mcast");
    }
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) {
        return PathToUtf8(fs::path(profile) / "AppData" / "Local" / "Mcast");
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return PathToUtf8(fs::path(xdg) / "mcast");
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return PathToUtf8(fs::path(home) / ".local" / "share" / "mcast");
    }
#endif
    std::error_code ec;
    return PathToUtf8(fs::current_path(ec) / "mcast-data");
}

std::string GenerateToken() {
    std::random_device device;
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    static const char* hex = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (int i = 0; i < 32; ++i) {
        const unsigned int value = dist(device);
        token.push_back(hex[value >> 4]);
        token.push_back(hex[value & 0x0F]);
    }
    return token;
}

std::vector<std::string> DefaultMediaRoots() {
    std::vector<std::string> roots;
#ifdef _WIN32
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) {
        fs::path videos = fs::path(profile) / "Videos";
        std::error_code ec;
        if (fs::is_directory(videos, ec)) roots.push_back(PathToUtf8(videos));
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        fs::path videos = fs::path(home) / "Videos";
        std::error_code ec;
        if (fs::is_directory(videos, ec)) roots.push_back(PathToUtf8(videos));
    }
#endif
    return roots;
}

bool LoadConfig(const std::string& data_dir, AppConfig& config, std::string& err) {
    config = AppConfig{};
    std::error_code ec;
    config.data_dir = NormalizePath(data_dir.empty() ? DefaultDataDir() : data_dir);
    fs::create_directories(PathFromUtf8(config.data_dir) / "logs", ec);
    if (ec) {
        err = "cannot create data directory '" + config.data_dir + "': " + ec.message();
        return false;
    }

    const fs::path path = PathFromUtf8(config.data_dir) / "config.json";
    bool dirty = false;

    if (fs::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            err = "cannot read " + path.string();
            return false;
        }
        nlohmann::json json;
        try {
            in >> json;
        } catch (const std::exception& e) {
            err = "invalid config.json: " + std::string(e.what());
            return false;
        }

        config.listen = json.value("listen", config.listen);
        config.port = json.value("port", config.port);
        config.token = json.value("token", config.token);
        config.auth_enabled = json.value("auth_enabled", config.auth_enabled);
        config.vlc_path = json.value("vlc_path", config.vlc_path);
        if (json.contains("auth") && json["auth"].is_object()) {
            config.auth_enabled = json["auth"].value("enabled", config.auth_enabled);
            config.token = json["auth"].value("token", config.token);
        }
        config.web_enabled = json.value("web_enabled", config.web_enabled);
        config.log_level = json.value("log_level", config.log_level);
        config.log_max_bytes = json.value("log_max_bytes", config.log_max_bytes);
        config.log_keep = json.value("log_keep", config.log_keep);

        config.media_roots.clear();
        if (json.contains("media") && json["media"].is_object() &&
            json["media"].contains("roots") && json["media"]["roots"].is_array()) {
            for (const auto& root : json["media"]["roots"]) {
                if (root.is_string()) {
                    const std::string text = root.get<std::string>();
                    if (!text.empty()) config.media_roots.push_back(NormalizePath(text));
                }
            }
        } else {
            config.media_roots = DefaultMediaRoots();
            dirty = true;
        }
        if (json.contains("web") && json["web"].is_object() &&
            json["web"].contains("enabled")) {
            config.web_enabled = json["web"]["enabled"].get<bool>();
        }
    } else {
        config.media_roots = DefaultMediaRoots();
        dirty = true;
    }

    if (config.port <= 0 || config.port > 65535) {
        err = "config port is out of range";
        return false;
    }
    if (config.log_max_bytes < 64 * 1024) config.log_max_bytes = 64 * 1024;
    if (config.log_keep < 1) config.log_keep = 1;
    if (config.auth_enabled && config.token.empty()) {
        config.token = GenerateToken();
        dirty = true;
    }
    if (dirty) {
        if (!SaveConfig(config, err)) return false;
    }
    return true;
}

bool SaveConfig(const AppConfig& config, std::string& err) {
    nlohmann::json json;
    json["listen"] = config.listen;
    json["port"] = config.port;
    json["auth_enabled"] = config.auth_enabled;
    json["token"] = config.token;
    json["vlc_path"] = config.vlc_path;
    json["web_enabled"] = config.web_enabled;
    json["log_level"] = config.log_level;
    json["log_max_bytes"] = config.log_max_bytes;
    json["log_keep"] = config.log_keep;
    json["auth"] = {{"enabled", config.auth_enabled}, {"token", config.token}};
    std::vector<std::string> roots;
    roots.reserve(config.media_roots.size());
    for (const std::string& root : config.media_roots) {
        if (!root.empty()) roots.push_back(NormalizePath(root));
    }
    json["media"] = {{"roots", roots}};
    json["web"] = {{"enabled", config.web_enabled}};

    std::error_code ec;
    fs::create_directories(PathFromUtf8(config.data_dir), ec);
    if (ec) {
        err = "cannot create data directory: " + ec.message();
        return false;
    }
    const fs::path path = PathFromUtf8(config.data_dir) / "config.json";
    return WriteFileAtomic(path, json.dump(2) + "\n", err);
}

bool IsNetworkUrl(const std::string& input) {
    const size_t pos = input.find("://");
    if (pos == std::string::npos || pos == 0) return false;
    for (size_t i = 0; i < pos; ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return false;
    }
    const std::string scheme = ToLowerAscii(input.substr(0, pos));
    static const std::set<std::string> kAllowed = {
        "rtsp", "rtsps", "rtmp", "rtmps", "http",  "https", "udp",
        "rtp",  "srt",   "tcp",  "mms",   "mmsh", "mmst",
    };
    return kAllowed.count(scheme) > 0;
}

bool IsPathAllowed(const std::string& path, const std::vector<std::string>& roots) {
    if (path.empty()) return false;
    const std::string candidate = ToLowerAscii(CanonicalString(path));
    for (const std::string& root : roots) {
        if (root.empty()) continue;
        const std::string canonical_root = ToLowerAscii(CanonicalString(root));
        if (StartsWithPath(candidate, canonical_root)) return true;
    }
    return false;
}

bool IsAllowedInput(const std::string& input, const std::vector<std::string>& roots,
                    std::string& err) {
    if (input.empty()) {
        err = "input is empty";
        return false;
    }
    if (IsNetworkUrl(input)) return true;
    if (IsPathAllowed(input, roots)) return true;
    err = "local input is outside the configured media roots";
    return false;
}

}  // namespace mcast
