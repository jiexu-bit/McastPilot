#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mcast {

// Windows filesystem::path::string() uses the ANSI code page; all paths that
// cross the API/JSON boundary are kept as UTF-8.
std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(const std::string& text);

// Converts a relative path to an absolute one (UTF-8 in/out). Network URLs are
// not passed to this helper.
std::string NormalizePath(const std::string& path);

// Resolves symlinks/junctions/reparse points when the path exists. Falls back
// to weakly_canonical/absolute for paths that do not exist yet.
std::string RealPathUtf8(const std::string& path);

struct AppConfig {
    std::string data_dir;
    std::string listen = "127.0.0.1";
    int port = 8787;
    bool auth_enabled = false;
    std::string token;
    std::vector<std::string> media_roots;
    bool web_enabled = true;
    std::string vlc_path;
    std::string log_level = "info";
    int64_t log_max_bytes = 10 * 1024 * 1024;
    int log_keep = 3;
};

std::string DefaultDataDir();
std::string GenerateToken();
std::vector<std::string> DefaultMediaRoots();

// Loads <data_dir>/config.json, creating it with safe defaults on first run.
bool LoadConfig(const std::string& data_dir, AppConfig& config, std::string& err);

// Writes <data_dir>/config.json atomically (temp file + replace).
bool SaveConfig(const AppConfig& config, std::string& err);

// Generic atomic text write used for config.json and tasks.json.
bool WriteTextFileAtomic(const std::string& path, const std::string& content,
                         std::string& err);

// True for rtsp://, rtmp://, udp://, http(s)://, srt:// ...
bool IsNetworkUrl(const std::string& input);

// Canonicalises the path and checks it is inside one of the configured roots.
bool IsPathAllowed(const std::string& path, const std::vector<std::string>& roots);

// Network URLs are always allowed as task input; local paths must be inside a
// media root. Used by both task creation and probe.
bool IsAllowedInput(const std::string& input, const std::vector<std::string>& roots,
                    std::string& err);

}  // namespace mcast
