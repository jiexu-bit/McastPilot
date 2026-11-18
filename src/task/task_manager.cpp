#include "task/task_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#include "common/config.h"
#include "common/log.h"
#include "net/nic.h"

namespace mcast {
namespace fs = std::filesystem;

nlohmann::json PipelineStatsToJson(const PipelineStats& stats) {
    const double fill =
        stats.buffer_capacity > 0
            ? 100.0 * static_cast<double>(stats.buffer_bytes) /
                  static_cast<double>(stats.buffer_capacity)
            : 0.0;
    return {
        {"running", stats.running},
        {"bitrate_mbps", stats.bitrate_mbps},
        {"muxed_bytes", stats.muxed_bytes},
        {"output_bytes", stats.output_bytes},
        {"packets_sent", stats.packets_sent},
        {"packets_dropped", stats.packets_dropped},
        {"buffer_drop_bytes", stats.buffer_drop_bytes},
        {"send_errors", stats.send_errors},
        {"frames", stats.frames},
        {"recorded_bytes", stats.recorded_bytes},
        {"destinations", stats.destinations},
        {"buffer_bytes", stats.buffer_bytes},
        {"buffer_capacity", stats.buffer_capacity},
        {"buffer_fill", fill},
        {"muxrate_bps", stats.muxrate_bps},
        {"elapsed_sec", stats.elapsed_sec},
        {"last_error", stats.last_error},
    };
}

nlohmann::json TaskViewToJson(const TaskView& view) {
    nlohmann::json json = TaskSpecToJson(view.spec);
    json["state"] = view.state;
    json["stats"] = PipelineStatsToJson(view.stats);
    json["last_error"] = view.last_error;
    json["retries"] = view.retries;
    return json;
}

TaskManager::TaskManager(std::vector<std::string> media_roots, std::string tasks_path)
    : media_roots_(std::move(media_roots)), tasks_path_(std::move(tasks_path)) {
    worker_ = std::thread(&TaskManager::WorkerLoop, this);
}

TaskManager::~TaskManager() {
    StopWorker();

    std::vector<Entry> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.swap(entries_);
    }
    for (Entry& entry : entries) {
        if (entry.pipeline) entry.pipeline->Stop();
    }
}

void TaskManager::StopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
        if (entry.pipeline || entry.state != "idle") StopLocked(entry);
    }
}

void TaskManager::StopWorker() {
    stopping_.store(true);
    tick_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool TaskManager::Load(std::string& err) {
    std::error_code ec;
    if (!fs::exists(tasks_path_, ec)) {
        std::lock_guard<std::mutex> lock(mutex_);
        return SaveLocked(err);
    }

    std::ifstream in(tasks_path_, std::ios::binary);
    if (!in) {
        err = "cannot read " + tasks_path_;
        return false;
    }
    nlohmann::json document;
    try {
        in >> document;
    } catch (const std::exception& e) {
        err = "invalid tasks.json: " + std::string(e.what());
        return false;
    }

    const nlohmann::json* list = nullptr;
    if (document.is_array()) {
        list = &document;
    } else if (document.is_object() && document.contains("tasks") &&
               document["tasks"].is_array()) {
        list = &document["tasks"];
    } else {
        err = "tasks.json must contain an array or an object with a tasks array";
        return false;
    }

    std::vector<Entry> loaded;
    std::set<std::string> ids;
    int skipped = 0;
    bool repaired = document.is_array();
    for (const auto& item : *list) {
        if (loaded.size() >= 100) {
            ++skipped;
            repaired = true;
            continue;
        }
        TaskSpec spec;
        std::string parse_err;
        if (!TaskSpecFromJson(item, spec, parse_err)) {
            ++skipped;
            repaired = true;
            continue;
        }
        if (spec.id.empty() || ids.count(spec.id) > 0) {
            do {
                spec.id = NewTaskId();
            } while (ids.count(spec.id) > 0);
            repaired = true;
        }
        ids.insert(spec.id);

        Entry entry;
        entry.spec = std::move(spec);
        entry.state = "idle";
        loaded.push_back(std::move(entry));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_ = std::move(loaded);
        if (repaired && !SaveLocked(err)) return false;
    }
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        logs_.clear();
    }

    LOG_INFO("loaded %llu task(s) from %s (skipped %d)",
             static_cast<unsigned long long>(ids.size()), tasks_path_.c_str(), skipped);
    return true;
}

void TaskManager::SetMediaRoots(std::vector<std::string> roots) {
    std::lock_guard<std::mutex> lock(mutex_);
    media_roots_ = std::move(roots);
}

std::vector<TaskView> TaskManager::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TaskView> views;
    views.reserve(entries_.size());
    for (const Entry& entry : entries_) views.push_back(MakeViewLocked(entry));
    return views;
}

bool TaskManager::Get(const std::string& id, TaskView& view) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* entry = FindLocked(id);
    if (!entry) return false;
    view = MakeViewLocked(*entry);
    return true;
}

bool TaskManager::Create(const nlohmann::json& body, TaskView& view, std::string& err) {
    TaskSpec spec;
    if (!body.is_object()) {
        err = "task body must be a JSON object";
        return false;
    }
    if (!TaskSpecFromJson(body, spec, err)) return false;

    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots = media_roots_;
    }
    if (!IsAllowedInput(spec.input, roots, err)) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (spec.id.empty()) {
            do {
                spec.id = NewTaskId();
            } while (FindLocked(spec.id) != nullptr);
        } else if (FindLocked(spec.id) != nullptr) {
            err = "task id already exists";
            return false;
        }

        Entry entry;
        entry.spec = spec;
        entry.state = "idle";
        entries_.push_back(std::move(entry));
        if (!SaveLocked(err)) {
            entries_.pop_back();
            return false;
        }
        view = MakeViewLocked(entries_.back());
    }

    InvokeUpdate(spec.id);
    return true;
}

bool TaskManager::Update(const std::string& id, const nlohmann::json& patch, TaskView& view,
                         std::string& err) {
    if (!patch.is_object()) {
        err = "patch body must be a JSON object";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = FindLocked(id);
        if (!entry) {
            err = "task not found";
            return false;
        }
        if (entry->state != "idle" && entry->state != "error") {
            err = "task must be stopped before editing";
            return false;
        }

        nlohmann::json merged = TaskSpecToJson(entry->spec);
        merged.merge_patch(patch);

        TaskSpec updated;
        if (!TaskSpecFromJson(merged, updated, err)) return false;
        updated.id = entry->spec.id;
        if (!IsAllowedInput(updated.input, media_roots_, err)) return false;

        TaskSpec previous = entry->spec;
        entry->spec = std::move(updated);
        if (!SaveLocked(err)) {
            entry->spec = std::move(previous);
            return false;
        }
        view = MakeViewLocked(*entry);
    }

    InvokeUpdate(id);
    return true;
}

bool TaskManager::Delete(const std::string& id, std::string& err) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& entry) { return entry.spec.id == id; });
        if (it == entries_.end()) {
            err = "task not found";
            return false;
        }
        if (it->state != "idle" && it->state != "error") {
            err = "task must be stopped before deleting";
            return false;
        }

        Entry removed = std::move(*it);
        const size_t index = static_cast<size_t>(it - entries_.begin());
        entries_.erase(it);
        if (!SaveLocked(err)) {
            entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(index),
                            std::move(removed));
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        logs_.erase(id);
    }

    InvokeUpdate(id);
    return true;
}

bool TaskManager::Start(const std::string& id, TaskView& view, std::string& err) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = FindLocked(id);
        if (!entry) {
            err = "task not found";
            return false;
        }
        if (entry->state == "running") {
            view = MakeViewLocked(*entry);
            return true;
        }
        if (entry->state == "starting" || entry->state == "stopping") {
            err = "task is busy";
            return false;
        }
        ok = StartLocked(*entry, true, err);
        view = MakeViewLocked(*entry);
    }

    InvokeUpdate(id);
    return ok;
}

bool TaskManager::Stop(const std::string& id, TaskView& view, std::string& err) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = FindLocked(id);
        if (!entry) {
            err = "task not found";
            return false;
        }
        if (entry->state == "stopping") {
            view = MakeViewLocked(*entry);
            return true;
        }
        StopLocked(*entry);
        view = MakeViewLocked(*entry);
    }

    InvokeUpdate(id);
    return true;
}

std::vector<std::string> TaskManager::Logs(const std::string& id, size_t tail) const {
    if (tail == 0) tail = 200;
    if (tail > 1000) tail = 1000;

    std::lock_guard<std::mutex> lock(log_mutex_);
    auto it = logs_.find(id);
    if (it == logs_.end()) return {};

    const std::deque<std::string>& lines = it->second;
    const size_t available = lines.size();
    const size_t first = available > tail ? available - tail : 0;
    return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(first),
                                    lines.end());
}

void TaskManager::AppendLog(const std::string& context, const std::string& line) {
    if (context.empty()) return;

    std::function<void(const std::string&, const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        auto& lines = logs_[context];
        lines.push_back(line);
        if (lines.size() > 500) lines.pop_front();
        callback = log_callback_;
    }

    if (callback) callback(context, line);
}

void TaskManager::SetUpdateCallback(std::function<void(const std::string& id)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    update_callback_ = std::move(callback);
}

void TaskManager::SetLogCallback(
    std::function<void(const std::string& id, const std::string& line)> callback) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_callback_ = std::move(callback);
}

size_t TaskManager::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t TaskManager::RunningCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(
        std::count_if(entries_.begin(), entries_.end(),
                      [](const Entry& entry) { return entry.state == "running"; }));
}

void TaskManager::WorkerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_.load()) {
        tick_cv_.wait_for(lock, std::chrono::seconds(1),
                          [this] { return stopping_.load(); });
        if (stopping_.load()) break;

        std::vector<std::string> changed;
        TickLocked(changed);
        auto callback = update_callback_;
        lock.unlock();
        if (callback) {
            for (const std::string& id : changed) callback(id);
        }
        lock.lock();
    }
}

void TaskManager::TickLocked(std::vector<std::string>& changed) {
    const auto now = std::chrono::steady_clock::now();

    for (Entry& entry : entries_) {
        if (entry.state == "running" && entry.pipeline) {
            entry.stats = entry.pipeline->Snapshot();
            if (now - entry.run_started_at > std::chrono::seconds(60)) entry.retries = 0;

            if (!entry.pipeline->Running()) {
                const PipelineStats final_stats = entry.stats;
                entry.pipeline->Stop();
                entry.pipeline.reset();
                entry.stats = final_stats;

                const std::string reason = final_stats.last_error.empty()
                                               ? "input ended"
                                               : final_stats.last_error;
                entry.last_error = reason;

                if (entry.spec.auto_restart && entry.retries < entry.spec.max_restarts) {
                    ++entry.retries;
                    entry.restart_pending = true;
                    entry.state = "starting";
                    int64_t delay = entry.spec.backoff_ms;
                    const int shift = std::min(entry.retries - 1, 5);
                    for (int i = 0; i < shift; ++i) {
                        delay *= 2;
                        if (delay >= 30000) {
                            delay = 30000;
                            break;
                        }
                    }
                    entry.next_restart = now + std::chrono::milliseconds(delay);
                } else {
                    entry.state = "error";
                    entry.restart_pending = false;
                }
                changed.push_back(entry.spec.id);
            } else {
                changed.push_back(entry.spec.id);
            }
        } else if (entry.state == "starting" && entry.restart_pending &&
                   now >= entry.next_restart) {
            std::string restart_err;
            if (StartLocked(entry, false, restart_err)) {
                changed.push_back(entry.spec.id);
            } else {
                if (entry.spec.auto_restart && entry.retries < entry.spec.max_restarts) {
                    ++entry.retries;
                    int64_t delay = entry.spec.backoff_ms;
                    const int shift = std::min(entry.retries - 1, 5);
                    for (int i = 0; i < shift; ++i) {
                        delay *= 2;
                        if (delay >= 30000) {
                            delay = 30000;
                            break;
                        }
                    }
                    entry.next_restart = now + std::chrono::milliseconds(delay);
                    entry.restart_pending = true;
                    entry.state = "starting";
                } else {
                    entry.state = "error";
                    entry.restart_pending = false;
                }
                changed.push_back(entry.spec.id);
            }
        }
    }
}

bool TaskManager::StartLocked(Entry& entry, bool manual, std::string& err) {
    entry.restart_pending = false;

    if (!IsAllowedInput(entry.spec.input, media_roots_, err)) {
        entry.state = "error";
        entry.last_error = err;
        if (manual) entry.retries = 0;
        return false;
    }

    net::McastConfig mcast;
    mcast.group = entry.spec.group;
    mcast.port = entry.spec.port;
    mcast.ttl = entry.spec.ttl;
    mcast.dscp = entry.spec.dscp;
    mcast.loopback = entry.spec.loopback;
    mcast.send_buffer_bytes = 4 * 1024 * 1024;

    if (!entry.spec.nic.empty()) {
        const std::vector<net::NicInfo> nics = net::EnumerateNics();
        const net::NicInfo* nic = net::FindNic(nics, entry.spec.nic);
        if (!nic) {
            err = "adapter not found: " + entry.spec.nic;
            entry.state = "error";
            entry.last_error = err;
            if (manual) entry.retries = 0;
            return false;
        }
        const std::string ip = net::PrimaryIpv4(*nic);
        if (ip.empty()) {
            err = "adapter '" + nic->name + "' has no IPv4 address";
            entry.state = "error";
            entry.last_error = err;
            if (manual) entry.retries = 0;
            return false;
        }
        mcast.source_ip = ip;
#ifndef _WIN32
        mcast.iface_name = nic->name;
#endif
    }

    PipelineConfig config;
    config.input = entry.spec.input;
    config.loop = entry.spec.loop;
    config.realtime = entry.spec.realtime;
    config.speed = entry.spec.speed;
    config.muxrate = entry.spec.muxrate;
    config.pkt_size = entry.spec.pkt_size;
    config.duration_ms = 0;
    config.buffer_bytes = entry.spec.buffer_bytes;
    config.log_tag = entry.spec.id;
    config.mcast = mcast;
    for (const ExtraOutput& extra : entry.spec.extra_outputs) {
        net::McastConfig extra_mcast = mcast;
        extra_mcast.group = extra.group;
        extra_mcast.port = extra.port;
        config.extra_mcasts.push_back(extra_mcast);
    }
    if (entry.spec.record) {
        std::error_code ec;
        const std::filesystem::path recordings =
            std::filesystem::path(PathFromUtf8(tasks_path_)).parent_path() / "recordings";
        std::filesystem::create_directories(recordings, ec);
        if (ec) {
            err = "cannot create recordings directory: " + ec.message();
            entry.state = "error";
            entry.last_error = err;
            if (manual) entry.retries = 0;
            return false;
        }
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        config.record_path =
            PathToUtf8(recordings / (entry.spec.id + "_" + std::to_string(stamp) + ".ts"));
    }
    config.encode_mode = entry.spec.encode_mode;
    config.video_bitrate = entry.spec.video_bitrate;
    config.audio_bitrate = entry.spec.audio_bitrate;
    config.preset = entry.spec.preset;
    config.gop = entry.spec.gop;

    auto pipeline = std::make_unique<Pipeline>();
    entry.state = "starting";
    std::string start_err;
    if (!pipeline->Start(config, start_err)) {
        entry.state = "error";
        entry.last_error = start_err;
        err = start_err;
        if (manual) entry.retries = 0;
        return false;
    }

    entry.pipeline = std::move(pipeline);
    entry.state = "running";
    entry.stats = entry.pipeline->Snapshot();
    entry.last_error.clear();
    entry.run_started_at = std::chrono::steady_clock::now();
    if (manual) entry.retries = 0;
    return true;
}

void TaskManager::StopLocked(Entry& entry) {
    entry.restart_pending = false;
    if (entry.pipeline) {
        entry.state = "stopping";
        entry.pipeline->Stop();
        entry.stats = entry.pipeline->Snapshot();
        entry.pipeline.reset();
    }
    entry.state = "idle";
    entry.last_error.clear();
    entry.retries = 0;
}

bool TaskManager::SaveLocked(std::string& err) {
    nlohmann::json document;
    document["version"] = 1;
    document["tasks"] = nlohmann::json::array();
    for (const Entry& entry : entries_) {
        document["tasks"].push_back(TaskSpecToJson(entry.spec));
    }
    return WriteTextFileAtomic(tasks_path_, document.dump(2) + "\n", err);
}

TaskManager::Entry* TaskManager::FindLocked(const std::string& id) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const Entry& entry) { return entry.spec.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

const TaskManager::Entry* TaskManager::FindLocked(const std::string& id) const {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const Entry& entry) { return entry.spec.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

TaskView TaskManager::MakeViewLocked(const Entry& entry) const {
    TaskView view;
    view.spec = entry.spec;
    view.state = entry.state;
    view.stats = entry.stats;
    view.last_error = entry.last_error;
    view.retries = entry.retries;
    return view;
}

void TaskManager::InvokeUpdate(const std::string& id) {
    std::function<void(const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = update_callback_;
    }
    if (callback) callback(id);
}

}  // namespace mcast
