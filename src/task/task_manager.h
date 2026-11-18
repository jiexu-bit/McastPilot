#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "media/pipeline.h"
#include "task/task.h"

namespace mcast {

struct TaskView {
    TaskSpec spec;
    std::string state = "idle";
    PipelineStats stats;
    std::string last_error;
    int retries = 0;
};

nlohmann::json PipelineStatsToJson(const PipelineStats& stats);
nlohmann::json TaskViewToJson(const TaskView& view);

class TaskManager {
public:
    TaskManager(std::vector<std::string> media_roots, std::string tasks_path);
    ~TaskManager();

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    bool Load(std::string& err);
    void SetMediaRoots(std::vector<std::string> roots);

    std::vector<TaskView> List() const;
    bool Get(const std::string& id, TaskView& view) const;
    bool Create(const nlohmann::json& body, TaskView& view, std::string& err);
    bool Update(const std::string& id, const nlohmann::json& patch, TaskView& view,
                std::string& err);
    bool Delete(const std::string& id, std::string& err);
    bool Start(const std::string& id, TaskView& view, std::string& err);
    bool Stop(const std::string& id, TaskView& view, std::string& err);
    void StopAll();
    void StopWorker();
    std::vector<std::string> Logs(const std::string& id, size_t tail) const;

    // Called from the log sink with the task id carried by the pipeline thread.
    void AppendLog(const std::string& context, const std::string& line);

    void SetUpdateCallback(std::function<void(const std::string& id)> callback);
    void SetLogCallback(
        std::function<void(const std::string& id, const std::string& line)> callback);

    size_t Size() const;
    size_t RunningCount() const;
    const std::string& TasksPath() const { return tasks_path_; }

private:
    struct Entry {
        TaskSpec spec;
        std::string state = "idle";
        std::unique_ptr<Pipeline> pipeline;
        PipelineStats stats;
        std::string last_error;
        int retries = 0;
        bool restart_pending = false;
        std::chrono::steady_clock::time_point next_restart;
        std::chrono::steady_clock::time_point run_started_at;
    };

    void WorkerLoop();
    void TickLocked(std::vector<std::string>& changed);
    bool StartLocked(Entry& entry, bool manual, std::string& err);
    void StopLocked(Entry& entry);
    bool SaveLocked(std::string& err);
    Entry* FindLocked(const std::string& id);
    const Entry* FindLocked(const std::string& id) const;
    TaskView MakeViewLocked(const Entry& entry) const;
    void InvokeUpdate(const std::string& id);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::vector<std::string> media_roots_;
    std::string tasks_path_;

    std::function<void(const std::string&)> update_callback_;

    // Kept separate from mutex_: pipeline threads log while Stop() is joining
    // them under the manager lock, so log routing must never take that lock.
    mutable std::mutex log_mutex_;
    std::map<std::string, std::deque<std::string>> logs_;
    std::function<void(const std::string&, const std::string&)> log_callback_;

    std::atomic<bool> stopping_{false};
    std::condition_variable tick_cv_;
    std::thread worker_;
};

}  // namespace mcast
