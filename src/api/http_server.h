#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/config.h"
#include "net/udp_receiver.h"
#include "task/task_manager.h"

namespace httplib {
class Server;
namespace ws {
class WebSocket;
}  // namespace ws
}  // namespace httplib

namespace mcast {

class ApiServer {
public:
    ApiServer(AppConfig& config, TaskManager& tasks, std::string web_dir);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    bool Start(std::string& err);
    void Stop();

    void BroadcastTask(const std::string& task_id);
    void BroadcastLog(const std::string& task_id, const std::string& line);

private:
    struct WsClient;

    void SetupRoutes();
    bool Authorized(const std::string& header, const std::string& query) const;
    nlohmann::json HealthJson() const;
    nlohmann::json SettingsJson() const;
    nlohmann::json SnapshotJson() const;
    void Broadcast(const nlohmann::json& event);
    void SendTo(const std::shared_ptr<WsClient>& client, const std::string& text);

    AppConfig& config_;
    TaskManager& tasks_;
    std::string web_dir_;

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutting_down_{false};
    std::chrono::steady_clock::time_point started_;

    mutable std::mutex config_mutex_;
    std::mutex ws_mutex_;
    std::vector<std::shared_ptr<WsClient>> ws_clients_;

    std::mutex monitor_mutex_;
    std::unique_ptr<net::UdpReceiver> monitor_;
};

}  // namespace mcast
