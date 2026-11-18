#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "api/http_server.h"
#include "common/config.h"
#include "common/file_log.h"
#include "common/log.h"
#include "net/platform.h"
#include "task/task_manager.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* kVersion = "0.3.0-m3";
std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop.store(true); }

struct CliOptions {
    std::string listen;
    int port = -1;
    std::string data_dir;
    std::string web_dir;
    std::vector<std::string> media_roots;
    bool media_roots_set = false;
    int web_enabled = -1;
    int auth_enabled = -1;
    std::string token;
    std::string vlc_path;
    std::string log_level;
    bool help = false;
    bool version = false;
};

void PrintUsage() {
    std::printf(
        "mcastd %s - McastPilot daemon\n"
        "\n"
        "Usage:\n"
        "  mcastd [options]\n"
        "\n"
        "Options:\n"
        "  --listen <addr>        Listen address (default 127.0.0.1)\n"
        "  --port <n>             HTTP port (default 8787)\n"
        "  --data-dir <dir>       Data directory (default %%LOCALAPPDATA%%\\Mcast)\n"
        "  --web-dir <dir>        Static web root (default <exe>/web)\n"
        "  --media-root <dir>     Add a media library root (repeatable)\n"
        "  --web / --no-web       Enable/disable the web UI and static files\n"
        "  --auth / --no-auth     Require a token for API/WebSocket (default off)\n"
        "  --token <text>         Custom access token (used when auth is on)\n"
        "  --vlc-path <exe>       VLC path for the \"Open in VLC\" button\n"
        "  --log-level <level>    trace|debug|info|warn|error|off\n"
        "  --console              Run in the foreground (default)\n"
        "  --version              Print version\n"
        "  --help                 Show this help\n",
        kVersion);
}

bool TakeValue(int argc, char** argv, int& index, const std::string& arg,
               const std::string& key, std::string& out) {
    const std::string prefix = "--" + key + "=";
    if (arg.rfind(prefix, 0) == 0) {
        out = arg.substr(prefix.size());
        return true;
    }
    if (arg == "--" + key) {
        if (index + 1 >= argc) return false;
        out = argv[++index];
        return true;
    }
    return false;
}

bool ParseArgs(int argc, char** argv, CliOptions& options, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--version" || arg == "-V") {
            options.version = true;
        } else if (arg == "--console") {
            // Foreground is the default; accepted for compatibility.
        } else if (arg == "--web") {
            options.web_enabled = 1;
        } else if (arg == "--no-web") {
            options.web_enabled = 0;
        } else if (arg == "--auth") {
            options.auth_enabled = 1;
        } else if (arg == "--no-auth") {
            options.auth_enabled = 0;
        } else if (TakeValue(argc, argv, i, arg, "token", value)) {
            options.token = value;
        } else if (TakeValue(argc, argv, i, arg, "vlc-path", value)) {
            options.vlc_path = value;
        } else if (TakeValue(argc, argv, i, arg, "listen", value)) {
            options.listen = value;
        } else if (TakeValue(argc, argv, i, arg, "port", value)) {
            try {
                options.port = std::stoi(value);
            } catch (...) {
                err = "invalid --port: " + value;
                return false;
            }
            if (options.port <= 0 || options.port > 65535) {
                err = "--port must be 1-65535";
                return false;
            }
        } else if (TakeValue(argc, argv, i, arg, "data-dir", value)) {
            options.data_dir = value;
        } else if (TakeValue(argc, argv, i, arg, "web-dir", value)) {
            options.web_dir = value;
        } else if (TakeValue(argc, argv, i, arg, "media-root", value)) {
            options.media_roots.push_back(value);
            options.media_roots_set = true;
        } else if (TakeValue(argc, argv, i, arg, "log-level", value)) {
            options.log_level = value;
        } else {
            err = "unknown argument: " + arg;
            return false;
        }
    }
    return true;
}

std::string ExecutableDir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0) {
        return mcast::PathToUtf8(std::filesystem::path(std::wstring(buffer, length))
                                     .parent_path());
    }
#else
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0) {
        return mcast::PathToUtf8(
            std::filesystem::path(std::string(buffer, static_cast<size_t>(length)))
                .parent_path());
    }
#endif
    return {};
}

std::string ResolveWebDir(const CliOptions& options) {
    namespace fs = std::filesystem;
    if (!options.web_dir.empty()) return options.web_dir;

    std::error_code ec;
    const std::string exe_dir = ExecutableDir();
    if (!exe_dir.empty()) {
        const fs::path candidate = fs::path(mcast::PathFromUtf8(exe_dir)) / "web";
        if (fs::is_directory(candidate, ec)) return mcast::PathToUtf8(candidate);
    }
    const fs::path cwd_candidate = fs::current_path(ec) / "web";
    if (fs::is_directory(cwd_candidate, ec)) return mcast::PathToUtf8(cwd_candidate);
    if (exe_dir.empty()) return {};
    return mcast::PathToUtf8(fs::path(mcast::PathFromUtf8(exe_dir)) / "web");
}

int RunDaemon(mcast::AppConfig& config, const CliOptions& options,
              mcast::FileLogger& file_logger) {
    const std::string tasks_path =
        mcast::PathToUtf8(std::filesystem::path(mcast::PathFromUtf8(config.data_dir)) /
                          "tasks.json");
    mcast::TaskManager task_manager(config.media_roots, tasks_path);

    mcast::SetLogSink([&](mcast::LogLevel lvl, const std::string& context,
                          const std::string& line) {
        file_logger.Write(lvl, context, line);
        if (!context.empty()) task_manager.AppendLog(context, line);
    });
    struct SinkGuard {
        ~SinkGuard() { mcast::SetLogSink(nullptr); }
    } sink_guard;

    std::string err;
    if (!task_manager.Load(err)) {
        LOG_ERROR("load tasks: %s", err.c_str());
        return 1;
    }

    const std::string web_dir = ResolveWebDir(options);
    mcast::ApiServer api(config, task_manager, web_dir);
    if (!api.Start(err)) {
        LOG_ERROR("start api: %s", err.c_str());
        return 1;
    }

    LOG_INFO("mcastd %s starting", kVersion);
    LOG_INFO("data dir: %s", config.data_dir.c_str());
    LOG_INFO("tasks: %llu | media roots: %llu",
             static_cast<unsigned long long>(task_manager.Size()),
             static_cast<unsigned long long>(config.media_roots.size()));
    LOG_INFO("auth: %s", config.auth_enabled ? "on (token required)" : "off (open access)");
    if (!config.auth_enabled && config.listen != "127.0.0.1" &&
        config.listen != "localhost") {
        LOG_WARN("auth is disabled while listening on %s; anyone on the LAN can control mcastd",
                 config.listen.c_str());
    }
    if (config.listen == "127.0.0.1" || config.listen == "localhost") {
        LOG_INFO("web UI: http://127.0.0.1:%d/ (local only)", config.port);
    } else {
        LOG_INFO("web UI: http://%s:%d/ (remote access enabled)", config.listen.c_str(),
                 config.port);
    }
    if (config.auth_enabled) {
        std::printf("\nweb token: %s\n\n", config.token.c_str());
    } else {
        std::printf("\nauth: off (no token required)\n\n");
    }
    std::fflush(stdout);

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("shutting down");
    api.Stop();
    task_manager.StopWorker();
    task_manager.StopAll();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    CliOptions options;
    std::string err;
    if (!ParseArgs(argc, argv, options, err)) {
        std::fprintf(stderr, "%s\n\n", err.c_str());
        PrintUsage();
        return 2;
    }
    if (options.help) {
        PrintUsage();
        return 0;
    }
    if (options.version) {
        std::printf("mcastd %s\n", kVersion);
        return 0;
    }

    mcast::AppConfig config;
    if (!mcast::LoadConfig(options.data_dir, config, err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 1;
    }

    bool config_dirty = false;
    if (!options.listen.empty()) {
        config.listen = options.listen;
        config_dirty = true;
    }
    if (options.port > 0) {
        config.port = options.port;
        config_dirty = true;
    }
    if (options.media_roots_set) {
        config.media_roots = options.media_roots;
        config_dirty = true;
    }
    if (options.web_enabled >= 0) {
        config.web_enabled = options.web_enabled != 0;
        config_dirty = true;
    }
    if (options.auth_enabled >= 0) {
        config.auth_enabled = options.auth_enabled != 0;
        config_dirty = true;
    }
    if (!options.token.empty()) {
        config.token = options.token;
        config_dirty = true;
    }
    if (!options.vlc_path.empty()) {
        config.vlc_path = options.vlc_path;
        config_dirty = true;
    }
    if (config.auth_enabled && config.token.empty()) {
        config.token = mcast::GenerateToken();
        config_dirty = true;
    }
    if (!options.log_level.empty()) {
        mcast::LogLevel level;
        if (!mcast::ParseLogLevel(options.log_level.c_str(), level)) {
            std::fprintf(stderr, "unknown log level: %s\n", options.log_level.c_str());
            return 2;
        }
        config.log_level = options.log_level;
        config_dirty = true;
    }
    for (std::string& root : config.media_roots) {
        root = mcast::NormalizePath(root);
    }
    if (config_dirty && !mcast::SaveConfig(config, err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 1;
    }

    mcast::LogLevel level = mcast::LogLevel::Info;
    mcast::ParseLogLevel(config.log_level.c_str(), level);
    mcast::SetLogLevel(level);

    if (!mcast::net::InitSockets(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    mcast::FileLogger file_logger;
    const std::string log_path =
        mcast::PathToUtf8(std::filesystem::path(mcast::PathFromUtf8(config.data_dir)) /
                          "logs" / "mcastd.log");
    if (!file_logger.Open(log_path, config.log_max_bytes, config.log_keep, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        mcast::net::ShutdownSockets();
        return 1;
    }

    std::signal(SIGINT, OnSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, OnSignal);
#endif

    const int result = RunDaemon(config, options, file_logger);
    mcast::net::ShutdownSockets();
    return result;
}
