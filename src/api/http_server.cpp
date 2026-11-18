#include "api/http_server.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include <httplib.h>

#ifdef _WIN32
#include <shellapi.h>
#endif

#include "common/log.h"
#include "media/probe.h"
#include "net/nic.h"

namespace mcast {
namespace fs = std::filesystem;

struct ApiServer::WsClient {
    std::mutex mutex;
    httplib::ws::WebSocket* ws = nullptr;
    bool alive = true;
};

namespace {

constexpr const char* kVersion = "0.3.0-m3";

std::string DumpJson(const nlohmann::json& json) {
    return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void SetJson(httplib::Response& res, const nlohmann::json& json, int status = 200) {
    res.status = status;
    res.set_content(DumpJson(json), "application/json; charset=utf-8");
}

void SetError(httplib::Response& res, int status, const std::string& message) {
    SetJson(res, {{"error", message}}, status);
}

bool ParseBody(const httplib::Request& req, nlohmann::json& json, std::string& err) {
    if (req.body.empty()) {
        err = "empty request body";
        return false;
    }
    try {
        json = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        err = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!json.is_object()) {
        err = "request body must be a JSON object";
        return false;
    }
    return true;
}

int StatusForError(const std::string& err) {
    if (err.find("not found") != std::string::npos) return 404;
    if (err.find("busy") != std::string::npos ||
        err.find("must be stopped") != std::string::npos) {
        return 409;
    }
    if (err.find("outside the configured media roots") != std::string::npos) return 403;
    return 400;
}

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    size_t diff = a.size() ^ b.size();
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<size_t>(ca ^ cb);
    }
    return diff == 0;
}

std::string LowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

nlohmann::json NicJson(const net::NicInfo& nic) {
    nlohmann::json item = {
        {"id", nic.id},
        {"name", nic.name},
        {"description", nic.description},
        {"ifindex", nic.ifindex},
        {"mac", nic.mac},
        {"up", nic.up},
        {"loopback", nic.loopback},
        {"virtual", nic.virtual_adapter},
        {"speed_mbps", nic.speed_mbps},
        {"gateway4", nic.gateway4},
    };
    item["ipv4"] = nlohmann::json::array();
    for (const auto& address : nic.ipv4) {
        item["ipv4"].push_back({{"addr", address.addr}, {"prefix", address.prefix}});
    }
    return item;
}

nlohmann::json ProbeJson(const ProbeResult& probe) {
    nlohmann::json json = {
        {"format", probe.format},
        {"duration_sec", probe.duration_sec},
        {"bitrate", probe.bitrate},
    };
    json["streams"] = nlohmann::json::array();
    for (const ProbeStream& stream : probe.streams) {
        json["streams"].push_back({
            {"index", stream.index},
            {"type", stream.type},
            {"codec", stream.codec},
            {"width", stream.width},
            {"height", stream.height},
            {"fps", stream.fps},
            {"sample_rate", stream.sample_rate},
            {"channels", stream.channels},
            {"bitrate", stream.bitrate},
        });
    }
    return json;
}

nlohmann::json ReceiverStatsJson(const net::ReceiverStats& stats) {
    return {
        {"running", stats.running},
        {"packets", stats.packets},
        {"bytes", stats.bytes},
        {"sync_errors", stats.sync_errors},
        {"cc_errors", stats.cc_errors},
        {"bitrate_mbps", stats.bitrate_mbps},
        {"elapsed_sec", stats.elapsed_sec},
        {"last_error", stats.last_error},
    };
}

bool IsLoopbackRemote(const std::string& address) {
    return address == "127.0.0.1" || address == "::1" ||
           address == "::ffff:127.0.0.1";
}

bool IsLocalIpv4(const std::string& address) {
    if (address.rfind("::ffff:", 0) == 0) return IsLocalIpv4(address.substr(7));
    if (address.rfind("127.", 0) == 0) return true;
    for (const auto& nic : net::EnumerateNics()) {
        for (const auto& ip : nic.ipv4) {
            if (ip.addr == address) return true;
        }
    }
    return false;
}

bool IsMulticastIpv4(const std::string& address) {
    const size_t dot = address.find('.');
    if (dot == std::string::npos) return false;
    try {
        const int first = std::stoi(address.substr(0, dot));
        return first >= 224 && first <= 239;
    } catch (...) {
        return false;
    }
}

std::string FindVlc(const std::string& configured) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (!configured.empty()) candidates.push_back(PathFromUtf8(configured));
#ifdef _WIN32
    if (const char* pf = std::getenv("ProgramFiles"); pf && *pf) {
        candidates.push_back(PathFromUtf8(pf) / "VideoLAN" / "VLC" / "vlc.exe");
    }
    if (const char* pf86 = std::getenv("ProgramFiles(x86)"); pf86 && *pf86) {
        candidates.push_back(PathFromUtf8(pf86) / "VideoLAN" / "VLC" / "vlc.exe");
    }
#endif
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate, ec)) return PathToUtf8(candidate);
    }
    return {};
}

bool LaunchVlc(const std::string& vlc_path, const std::string& url, std::string& err) {
#ifdef _WIN32
    const std::wstring exe = PathFromUtf8(vlc_path).wstring();
    const std::wstring args = PathFromUtf8(url).wstring();
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(),
                                           nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        err = "cannot launch VLC (ShellExecute error " +
              std::to_string(reinterpret_cast<INT_PTR>(result)) + ")";
        return false;
    }
    return true;
#else
    (void)vlc_path;
    (void)url;
    err = "open in VLC is only supported on Windows";
    return false;
#endif
}

bool IsMediaExtension(const std::string& extension) {
    static const std::set<std::string> kAllowed = {
        ".ts", ".m2ts", ".mts", ".trp", ".mp4", ".m4v", ".mkv", ".mov",
        ".flv", ".avi", ".wmv", ".mp3", ".aac", ".m4a", ".wav",
    };
    return kAllowed.count(LowerAscii(extension)) > 0;
}

const char* MimeFor(const std::string& path) {
    const std::string ext = LowerAscii(PathFromUtf8(path).extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json" || ext == ".map") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    return "application/octet-stream";
}

bool ReadFileUtf8(const fs::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    content = buffer.str();
    return true;
}

size_t ParseTail(const httplib::Request& req, size_t fallback) {
    const std::string text = req.get_param_value("tail");
    if (text.empty()) return fallback;
    try {
        const unsigned long value = std::stoul(text);
        if (value == 0) return fallback;
        return std::min<size_t>(value, 1000);
    } catch (...) {
        return fallback;
    }
}

struct MediaEntry {
    bool is_dir = false;
    std::string sort_name;
    nlohmann::json json;
};

}  // namespace

ApiServer::ApiServer(AppConfig& config, TaskManager& tasks, std::string web_dir)
    : config_(config),
      tasks_(tasks),
      web_dir_(std::move(web_dir)),
      started_(std::chrono::steady_clock::now()) {
    tasks_.SetUpdateCallback([this](const std::string& id) { BroadcastTask(id); });
    tasks_.SetLogCallback(
        [this](const std::string& id, const std::string& line) { BroadcastLog(id, line); });
}

ApiServer::~ApiServer() { Stop(); }

bool ApiServer::Start(std::string& err) {
    if (running_.load()) {
        err = "api server is already running";
        return false;
    }
    shutting_down_.store(false);
    started_ = std::chrono::steady_clock::now();

    server_ = std::make_unique<httplib::Server>();
    SetupRoutes();

    if (!server_->bind_to_port(config_.listen, config_.port)) {
        err = "cannot bind " + config_.listen + ":" + std::to_string(config_.port);
        server_.reset();
        return false;
    }

    thread_ = std::thread([this] { server_->listen_after_bind(); });
    running_.store(true);
    LOG_INFO("api listening on http://%s:%d (web=%s, dir=%s)", config_.listen.c_str(),
             config_.port, config_.web_enabled ? "on" : "off", web_dir_.c_str());
    return true;
}

void ApiServer::Stop() {
    if (shutting_down_.exchange(true)) return;

    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();

    tasks_.SetUpdateCallback(nullptr);
    tasks_.SetLogCallback(nullptr);

    std::vector<std::shared_ptr<WsClient>> clients;
    {
        std::lock_guard<std::mutex> lock(ws_mutex_);
        clients.swap(ws_clients_);
    }
    for (const auto& client : clients) {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->alive = false;
        client->ws = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        if (monitor_) {
            monitor_->Stop();
            monitor_.reset();
        }
    }

    server_.reset();
    running_.store(false);
}

bool ApiServer::Authorized(const std::string& header, const std::string& query) const {
    bool auth_enabled = false;
    std::string expected;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        auth_enabled = config_.auth_enabled;
        expected = config_.token;
    }
    if (!auth_enabled) return true;
    if (expected.empty()) return false;

    std::string token = header;
    if (token.rfind("Bearer ", 0) == 0) token = token.substr(7);
    if (token.empty()) token = query;
    return ConstantTimeEquals(token, expected);
}

nlohmann::json ApiServer::HealthJson() const {
    bool web = false;
    bool auth = false;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        web = config_.web_enabled;
        auth = config_.auth_enabled;
    }
    const double uptime =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    return {
        {"name", "mcastd"},
        {"version", kVersion},
        {"uptime_sec", uptime},
        {"tasks", tasks_.Size()},
        {"running", tasks_.RunningCount()},
        {"web", web},
        {"auth", auth},
    };
}

nlohmann::json ApiServer::SettingsJson() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string hint;
    if (!config_.token.empty()) {
        if (config_.token.size() <= 8) {
            hint = config_.token;
        } else {
            hint = config_.token.substr(0, 4) + "..." +
                   config_.token.substr(config_.token.size() - 4);
        }
    }
    return {
        {"listen", config_.listen},
        {"port", config_.port},
        {"web_enabled", config_.web_enabled},
        {"auth_enabled", config_.auth_enabled},
        {"token_set", !config_.token.empty()},
        {"token_hint", hint},
        {"vlc_path", config_.vlc_path},
        {"media_roots", config_.media_roots},
        {"log_level", config_.log_level},
        {"log_max_bytes", config_.log_max_bytes},
        {"log_keep", config_.log_keep},
        {"data_dir", config_.data_dir},
    };
}

nlohmann::json ApiServer::SnapshotJson() const {
    nlohmann::json tasks = nlohmann::json::array();
    for (const TaskView& view : tasks_.List()) tasks.push_back(TaskViewToJson(view));
    return {
        {"type", "snapshot"},
        {"health", HealthJson()},
        {"tasks", tasks},
    };
}

void ApiServer::Broadcast(const nlohmann::json& event) {
    if (shutting_down_.load()) return;

    const std::string text = DumpJson(event);
    std::vector<std::shared_ptr<WsClient>> clients;
    {
        std::lock_guard<std::mutex> lock(ws_mutex_);
        clients = ws_clients_;
    }
    for (const auto& client : clients) SendTo(client, text);
}

void ApiServer::BroadcastTask(const std::string& task_id) {
    TaskView view;
    if (tasks_.Get(task_id, view)) {
        Broadcast({{"type", "task"}, {"task", TaskViewToJson(view)}});
    } else {
        Broadcast({{"type", "deleted"}, {"task_id", task_id}});
    }
}

void ApiServer::BroadcastLog(const std::string& task_id, const std::string& line) {
    std::string text = line;
    if (text.size() > 2000) text.resize(2000);
    Broadcast({{"type", "log"}, {"task_id", task_id}, {"line", text}});
}

void ApiServer::SendTo(const std::shared_ptr<WsClient>& client, const std::string& text) {
    std::lock_guard<std::mutex> lock(client->mutex);
    if (!client->alive || !client->ws) return;
    if (!client->ws->is_open() || !client->ws->send(text)) client->alive = false;
}

void ApiServer::SetupRoutes() {
    server_->set_payload_max_length(1024 * 1024);
    server_->set_read_timeout(30, 0);
    server_->set_write_timeout(30, 0);

    server_->set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res) {
            const bool is_api = req.path.rfind("/api/", 0) == 0;
            if (is_api) {
                if (req.path == "/api/v1/health") {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                if (!Authorized(req.get_header_value("Authorization"),
                                req.get_param_value("token"))) {
                    SetError(res, 401, "missing or invalid token");
                    return httplib::Server::HandlerResponse::Handled;
                }
            } else {
                bool web = false;
                {
                    std::lock_guard<std::mutex> lock(config_mutex_);
                    web = config_.web_enabled;
                }
                if (!web) {
                    SetError(res, 404, "web UI is disabled");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

    server_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.body.empty()) {
            const int status = res.status == 0 ? 404 : res.status;
            SetJson(res, {{"error", status == 404 ? "not found" : "request failed"}}, status);
        }
    });

    server_->set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            std::string message = "internal server error";
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    message = e.what();
                } catch (...) {
                }
            }
            SetError(res, 500, message);
        });

    server_->Get("/api/v1/health",
                 [this](const httplib::Request&, httplib::Response& res) {
                     SetJson(res, HealthJson());
                 });

    server_->Get("/api/v1/nics",
                 [](const httplib::Request&, httplib::Response& res) {
                     nlohmann::json nics = nlohmann::json::array();
                     for (const auto& nic : net::EnumerateNics()) nics.push_back(NicJson(nic));
                     SetJson(res, {{"nics", nics}});
                 });

    server_->Get("/api/v1/media/roots",
                 [this](const httplib::Request&, httplib::Response& res) {
                     std::vector<std::string> roots;
                     {
                         std::lock_guard<std::mutex> lock(config_mutex_);
                         roots = config_.media_roots;
                     }
                     SetJson(res, {{"roots", roots}});
                 });

    server_->Get("/api/v1/media/list",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     std::vector<std::string> roots;
                     {
                         std::lock_guard<std::mutex> lock(config_mutex_);
                         roots = config_.media_roots;
                     }
                     const std::string requested = req.get_param_value("path");
                     if (requested.empty()) {
                         nlohmann::json entries = nlohmann::json::array();
                         for (const std::string& root : roots) {
                             const std::string name = PathToUtf8(PathFromUtf8(root).filename());
                             entries.push_back({
                                 {"name", name.empty() ? root : name},
                                 {"path", root},
                                 {"type", "dir"},
                                 {"root", true},
                             });
                         }
                         SetJson(res, {{"path", ""}, {"parent", ""}, {"entries", entries}});
                         return;
                     }

                     if (!IsPathAllowed(requested, roots)) {
                         SetError(res, 403, "path is outside the configured media roots");
                         return;
                     }

                     std::error_code ec;
                     const std::string canonical_text = RealPathUtf8(requested);
                     if (!IsPathAllowed(canonical_text, roots)) {
                         SetError(res, 403, "path resolves outside the configured media roots");
                         return;
                     }
                     const fs::path canonical = PathFromUtf8(canonical_text);
                     if (!fs::is_directory(canonical, ec)) {
                         SetError(res, 404, "directory not found");
                         return;
                     }

                     std::vector<MediaEntry> items;
                     fs::directory_iterator iterator(canonical,
                                                     fs::directory_options::skip_permission_denied,
                                                     ec);
                     if (ec) {
                         SetError(res, 403, "cannot read directory: " + ec.message());
                         return;
                     }
                     for (const auto& entry : iterator) {
                         if (items.size() >= 2000) break;
                         std::error_code entry_ec;
                         const bool is_dir = entry.is_directory(entry_ec);
                         const bool is_file = entry.is_regular_file(entry_ec);
                         if (!is_dir && !is_file) continue;

                         const std::string path = PathToUtf8(entry.path());
                         const std::string name = PathToUtf8(entry.path().filename());
                         const std::string extension = PathToUtf8(entry.path().extension());
                         if (is_file && !IsMediaExtension(extension)) continue;

                         MediaEntry item;
                         item.is_dir = is_dir;
                         item.sort_name = LowerAscii(name);
                         item.json = {
                             {"name", name},
                             {"path", path},
                             {"type", is_dir ? "dir" : "file"},
                             {"size", is_file ? static_cast<int64_t>(entry.file_size(entry_ec))
                                              : 0},
                             {"ext", LowerAscii(extension)},
                         };
                         items.push_back(std::move(item));
                     }

                     std::sort(items.begin(), items.end(),
                               [](const MediaEntry& a, const MediaEntry& b) {
                                   if (a.is_dir != b.is_dir) return a.is_dir;
                                   return a.sort_name < b.sort_name;
                               });

                     nlohmann::json entries = nlohmann::json::array();
                     for (MediaEntry& item : items) entries.push_back(std::move(item.json));

                     std::string parent;
                     const fs::path up = canonical.parent_path();
                     if (!up.empty()) {
                         const std::string up_text = PathToUtf8(up);
                         if (IsPathAllowed(up_text, roots)) parent = up_text;
                     }

                     SetJson(res, {{"path", PathToUtf8(canonical)},
                                   {"parent", parent},
                                   {"entries", entries}});
                 });

    server_->Get("/api/v1/tasks",
                 [this](const httplib::Request&, httplib::Response& res) {
                     nlohmann::json tasks = nlohmann::json::array();
                     for (const TaskView& view : tasks_.List()) {
                         tasks.push_back(TaskViewToJson(view));
                     }
                     SetJson(res, {{"tasks", tasks}});
                 });

    server_->Post("/api/v1/tasks",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      nlohmann::json body;
                      std::string err;
                      if (!ParseBody(req, body, err)) {
                          SetError(res, 400, err);
                          return;
                      }
                      TaskView view;
                      if (!tasks_.Create(body, view, err)) {
                          SetError(res, StatusForError(err), err);
                          return;
                      }
                      SetJson(res, {{"task", TaskViewToJson(view)}}, 201);
                  });

    server_->Get(R"(/api/v1/tasks/([A-Za-z0-9_-]+))",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     const std::string id = req.matches[1].str();
                     TaskView view;
                     if (!tasks_.Get(id, view)) {
                         SetError(res, 404, "task not found");
                         return;
                     }
                     SetJson(res, {{"task", TaskViewToJson(view)}});
                 });

    server_->Patch(R"(/api/v1/tasks/([A-Za-z0-9_-]+))",
                   [this](const httplib::Request& req, httplib::Response& res) {
                       nlohmann::json patch;
                       std::string err;
                       if (!ParseBody(req, patch, err)) {
                           SetError(res, 400, err);
                           return;
                       }
                       const std::string id = req.matches[1].str();
                       TaskView view;
                       if (!tasks_.Update(id, patch, view, err)) {
                           SetError(res, StatusForError(err), err);
                           return;
                       }
                       SetJson(res, {{"task", TaskViewToJson(view)}});
                   });

    server_->Delete(R"(/api/v1/tasks/([A-Za-z0-9_-]+))",
                    [this](const httplib::Request& req, httplib::Response& res) {
                        const std::string id = req.matches[1].str();
                        std::string err;
                        if (!tasks_.Delete(id, err)) {
                            SetError(res, StatusForError(err), err);
                            return;
                        }
                        SetJson(res, {{"ok", true}});
                    });

    server_->Post(R"(/api/v1/tasks/([A-Za-z0-9_-]+)/start)",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      const std::string id = req.matches[1].str();
                      TaskView view;
                      std::string err;
                      if (!tasks_.Start(id, view, err)) {
                          SetError(res, StatusForError(err), err);
                          return;
                      }
                      SetJson(res, {{"task", TaskViewToJson(view)}});
                  });

    server_->Post(R"(/api/v1/tasks/([A-Za-z0-9_-]+)/stop)",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      const std::string id = req.matches[1].str();
                      TaskView view;
                      std::string err;
                      if (!tasks_.Stop(id, view, err)) {
                          SetError(res, StatusForError(err), err);
                          return;
                      }
                      SetJson(res, {{"task", TaskViewToJson(view)}});
                  });

    server_->Post(R"(/api/v1/tasks/([A-Za-z0-9_-]+)/open-vlc)",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      if (!IsLoopbackRemote(req.remote_addr) && !IsLocalIpv4(req.remote_addr)) {
                          SetError(res, 403, "open in VLC is only available on the server machine");
                          return;
                      }
                      const std::string id = req.matches[1].str();
                      TaskView view;
                      if (!tasks_.Get(id, view)) {
                          SetError(res, 404, "task not found");
                          return;
                      }
                      if (view.state != "running") {
                          SetError(res, 409, "task is not running");
                          return;
                      }

                      const std::string& group = view.spec.group;
                      if (!IsMulticastIpv4(group) && !IsLocalIpv4(group)) {
                          SetError(res, 400, "unicast destination is not an address of this machine");
                          return;
                      }

                      std::string configured;
                      {
                          std::lock_guard<std::mutex> lock(config_mutex_);
                          configured = config_.vlc_path;
                      }
                      const std::string vlc_path = FindVlc(configured);
                      if (vlc_path.empty()) {
                          SetError(res, 404,
                                   "VLC not found; set vlc_path in settings or --vlc-path");
                          return;
                      }

                      const std::string url =
                          "udp://@" + group + ":" + std::to_string(view.spec.port);
                      std::string err;
                      if (!LaunchVlc(vlc_path, url, err)) {
                          SetError(res, 500, err);
                          return;
                      }
                      SetJson(res, {{"ok", true}, {"url", url}, {"vlc", vlc_path}});
                  });

    server_->Get(R"(/api/v1/tasks/([A-Za-z0-9_-]+)/stats)",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     const std::string id = req.matches[1].str();
                     TaskView view;
                     if (!tasks_.Get(id, view)) {
                         SetError(res, 404, "task not found");
                         return;
                     }
                     SetJson(res, {{"task_id", id},
                                   {"state", view.state},
                                   {"stats", PipelineStatsToJson(view.stats)},
                                   {"last_error", view.last_error},
                                   {"retries", view.retries}});
                 });

    server_->Get(R"(/api/v1/tasks/([A-Za-z0-9_-]+)/logs)",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     const std::string id = req.matches[1].str();
                     TaskView view;
                     if (!tasks_.Get(id, view)) {
                         SetError(res, 404, "task not found");
                         return;
                     }
                     const std::vector<std::string> lines = tasks_.Logs(id, ParseTail(req, 200));
                     SetJson(res, {{"task_id", id}, {"lines", lines}});
                 });

    server_->Post("/api/v1/probe",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      nlohmann::json body;
                      std::string err;
                      if (!ParseBody(req, body, err)) {
                          SetError(res, 400, err);
                          return;
                      }
                      const std::string input = body.value("input", std::string{});
                      std::vector<std::string> roots;
                      {
                          std::lock_guard<std::mutex> lock(config_mutex_);
                          roots = config_.media_roots;
                      }
                      if (!IsAllowedInput(input, roots, err)) {
                          SetError(res, 403, err);
                          return;
                      }
                      ProbeResult probe;
                      if (!ProbeInput(input, probe, err)) {
                          SetError(res, 400, err);
                          return;
                      }
                      SetJson(res, {{"input", input}, {"probe", ProbeJson(probe)}});
                  });

    server_->Get("/api/v1/settings",
                 [this](const httplib::Request&, httplib::Response& res) {
                     SetJson(res, {{"settings", SettingsJson()}});
                 });

    server_->Patch("/api/v1/settings",
                   [this](const httplib::Request& req, httplib::Response& res) {
                       nlohmann::json body;
                       std::string err;
                       if (!ParseBody(req, body, err)) {
                           SetError(res, 400, err);
                           return;
                       }
                       if (body.contains("listen") || body.contains("port")) {
                           SetError(res, 409, "listen/port changes require a server restart");
                           return;
                       }

                       std::vector<std::string> roots;
                       bool media_changed = false;
                       bool log_changed = false;
                       bool auth_after = false;
                       std::string token_after;
                       LogLevel new_level = GetLogLevel();
                       {
                           std::lock_guard<std::mutex> lock(config_mutex_);
                           AppConfig updated = config_;

                           if (body.contains("auth_enabled")) {
                               if (!body["auth_enabled"].is_boolean()) {
                                   SetError(res, 400, "auth_enabled must be a boolean");
                                   return;
                               }
                               updated.auth_enabled = body["auth_enabled"].get<bool>();
                           }
                           if (body.contains("token")) {
                               if (!body["token"].is_string()) {
                                   SetError(res, 400, "token must be a string");
                                   return;
                               }
                               const std::string text = body["token"].get<std::string>();
                               if (text.size() > 128) {
                                   SetError(res, 400, "token is too long (max 128)");
                                   return;
                               }
                               if (!text.empty()) updated.token = text;
                           }
                           if (body.contains("vlc_path")) {
                               if (!body["vlc_path"].is_string()) {
                                   SetError(res, 400, "vlc_path must be a string");
                                   return;
                               }
                               updated.vlc_path = body["vlc_path"].get<std::string>();
                           }
                           if (updated.auth_enabled && updated.token.empty()) {
                               updated.token = GenerateToken();
                           }

                           if (body.contains("web_enabled")) {
                               if (!body["web_enabled"].is_boolean()) {
                                   SetError(res, 400, "web_enabled must be a boolean");
                                   return;
                               }
                               updated.web_enabled = body["web_enabled"].get<bool>();
                           }
                           if (body.contains("media_roots")) {
                               if (!body["media_roots"].is_array()) {
                                   SetError(res, 400, "media_roots must be an array");
                                   return;
                               }
                               std::vector<std::string> normalized;
                               for (const auto& root : body["media_roots"]) {
                                   if (!root.is_string()) {
                                       SetError(res, 400, "media_roots entries must be strings");
                                       return;
                                   }
                                   const std::string text = root.get<std::string>();
                                   if (text.empty()) continue;
                                   if (normalized.size() >= 32) {
                                       SetError(res, 400, "at most 32 media roots are allowed");
                                       return;
                                   }
                                   normalized.push_back(NormalizePath(text));
                               }
                               updated.media_roots = std::move(normalized);
                               media_changed = true;
                           }
                           if (body.contains("log_level")) {
                               if (!body["log_level"].is_string()) {
                                   SetError(res, 400, "log_level must be a string");
                                   return;
                               }
                               const std::string text = body["log_level"].get<std::string>();
                               if (!ParseLogLevel(text.c_str(), new_level)) {
                                   SetError(res, 400, "unknown log level");
                                   return;
                               }
                               updated.log_level = text;
                               log_changed = true;
                           }

                           if (!SaveConfig(updated, err)) {
                               SetError(res, 500, err);
                               return;
                           }
                           config_ = std::move(updated);
                           roots = config_.media_roots;
                           auth_after = config_.auth_enabled;
                           token_after = config_.token;
                       }
                       if (log_changed) SetLogLevel(new_level);
                       if (media_changed) tasks_.SetMediaRoots(roots);
                       nlohmann::json response = {
                           {"settings", SettingsJson()},
                           {"restart_required", false},
                       };
                       if (auth_after) response["token"] = token_after;
                       SetJson(res, response);
                   });

    server_->Get("/api/v1/monitor",
                 [this](const httplib::Request&, httplib::Response& res) {
                     std::lock_guard<std::mutex> lock(monitor_mutex_);
                     net::ReceiverStats stats;
                     if (monitor_) stats = monitor_->Snapshot();
                     SetJson(res, {{"monitor", ReceiverStatsJson(stats)}});
                 });

    server_->Post("/api/v1/monitor/start",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      nlohmann::json body;
                      std::string err;
                      if (!ParseBody(req, body, err)) {
                          SetError(res, 400, err);
                          return;
                      }

                      net::ReceiverConfig config;
                      try {
                          config.group = body.value("group", std::string{});
                          const int port = body.value("port", 0);
                          if (config.group.empty()) {
                              SetError(res, 400, "group is required");
                              return;
                          }
                          if (port < 1 || port > 65535) {
                              SetError(res, 400, "port must be 1-65535");
                              return;
                          }
                          config.port = static_cast<uint16_t>(port);
                          config.iface = body.value("iface", std::string{});
                          if (!config.iface.empty()) {
                              in_addr address{};
                              if (inet_pton(AF_INET, config.iface.c_str(), &address) != 1) {
                                  SetError(res, 400, "iface must be an IPv4 address");
                                  return;
                              }
                          }
                          const double duration = body.value("duration_sec", 0.0);
                          if (duration < 0.0 || duration > 3600.0) {
                              SetError(res, 400, "duration_sec must be 0-3600");
                              return;
                          }
                          config.duration_ms = static_cast<int64_t>(duration * 1000.0);
                          const int rcvbuf = body.value("rcvbuf", 8 * 1024 * 1024);
                          config.rcvbuf = std::clamp(rcvbuf, 64 * 1024, 64 * 1024 * 1024);
                      } catch (const std::exception& e) {
                          SetError(res, 400, std::string("invalid monitor JSON: ") + e.what());
                          return;
                      }

                      std::lock_guard<std::mutex> lock(monitor_mutex_);
                      if (monitor_ && monitor_->Running()) {
                          SetError(res, 409, "a monitor is already running");
                          return;
                      }
                      if (monitor_) {
                          monitor_->Stop();
                          monitor_.reset();
                      }
                      auto receiver = std::make_unique<net::UdpReceiver>();
                      if (!receiver->Start(config, err)) {
                          SetError(res, 400, err);
                          return;
                      }
                      monitor_ = std::move(receiver);
                      SetJson(res, {{"monitor", ReceiverStatsJson(monitor_->Snapshot())}});
                  });

    server_->Post("/api/v1/monitor/stop",
                  [this](const httplib::Request&, httplib::Response& res) {
                      std::lock_guard<std::mutex> lock(monitor_mutex_);
                      net::ReceiverStats stats;
                      if (monitor_) {
                          stats = monitor_->Snapshot();
                          monitor_->Stop();
                          stats = monitor_->Snapshot();
                          monitor_.reset();
                      }
                      SetJson(res, {{"monitor", ReceiverStatsJson(stats)}});
                  });

    server_->WebSocket(
        "/api/v1/events",
        [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
            if (!Authorized(req.get_header_value("Authorization"),
                            req.get_param_value("token"))) {
                ws.close(httplib::ws::CloseStatus::PolicyViolation, "invalid token");
                return;
            }

            auto client = std::make_shared<WsClient>();
            client->ws = &ws;
            {
                std::lock_guard<std::mutex> lock(ws_mutex_);
                if (shutting_down_.load()) {
                    ws.close(httplib::ws::CloseStatus::GoingAway, "server stopping");
                    return;
                }
                ws_clients_.push_back(client);
            }

            SendTo(client, DumpJson(SnapshotJson()));

            // Wake up periodically so a connected browser cannot block
            // graceful shutdown inside a long read.
            ws.set_read_timeout(1);
            std::string message;
            while (!shutting_down_.load()) {
                const auto result = ws.read(message);
                if (result == httplib::ws::ReadResult::Fail) break;
                if (result == httplib::ws::ReadResult::Text && message == "ping") {
                    SendTo(client, R"({"type":"pong"})");
                }
            }

            {
                std::lock_guard<std::mutex> lock(ws_mutex_);
                ws_clients_.erase(
                    std::remove(ws_clients_.begin(), ws_clients_.end(), client),
                    ws_clients_.end());
            }
            {
                std::lock_guard<std::mutex> lock(client->mutex);
                client->alive = false;
                client->ws = nullptr;
            }
            ws.close(httplib::ws::CloseStatus::Normal);
        });

    server_->Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0) {
            SetError(res, 404, "not found");
            return;
        }
        if (web_dir_.empty()) {
            res.status = 404;
            res.set_content("web UI is not available\n", "text/plain; charset=utf-8");
            return;
        }

        std::string relative = req.path;
        if (relative.empty() || relative == "/") relative = "/index.html";
        if (relative.find('\0') != std::string::npos) {
            res.status = 404;
            res.set_content("not found\n", "text/plain; charset=utf-8");
            return;
        }

        std::error_code ec;
        fs::path target = PathFromUtf8(web_dir_) / PathFromUtf8(relative.substr(1));
        const std::string canonical_text = RealPathUtf8(PathToUtf8(target));
        if (!IsPathAllowed(canonical_text, {web_dir_})) {
            res.status = 404;
            res.set_content("not found\n", "text/plain; charset=utf-8");
            return;
        }
        fs::path canonical = PathFromUtf8(canonical_text);
        if (fs::is_directory(canonical, ec)) canonical /= "index.html";

        std::string content;
        if (!ReadFileUtf8(canonical, content)) {
            res.status = 404;
            res.set_content("not found\n", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-cache");
        res.set_content(content, MimeFor(PathToUtf8(canonical)));
    });
}

}  // namespace mcast
