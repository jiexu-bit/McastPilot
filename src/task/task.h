#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcast {

struct ExtraOutput {
    std::string group;
    uint16_t port = 5000;
};

struct TaskSpec {
    std::string id;
    std::string name;
    std::string input;
    bool loop = false;

    std::string group;
    uint16_t port = 5000;
    std::vector<ExtraOutput> extra_outputs;
    bool record = false;
    std::string nic;
    int ttl = 16;
    int dscp = 0;
    bool loopback = false;
    int pkt_size = 1316;

    int64_t muxrate = 0;
    bool realtime = true;
    double speed = 1.0;
    size_t buffer_bytes = 4 * 1024 * 1024;

    bool auto_restart = true;
    int max_restarts = 10;
    int backoff_ms = 1000;
};

std::string NewTaskId();

// Fills defaults (name, clamp values) and validates required fields.
bool ValidateTaskSpec(TaskSpec& spec, std::string& err);

// Design-style nested JSON:
// { id, name, source{type,uri,loop}, output{protocol,group,port,nic,ttl,
//   dscp,loopback,pkt_size}, mux{format,muxrate,cbr}, encode{mode},
//   restart{auto,max_retries,backoff_ms}, realtime, speed, buffer_kb }
nlohmann::json TaskSpecToJson(const TaskSpec& spec);
bool TaskSpecFromJson(const nlohmann::json& json, TaskSpec& spec, std::string& err);

std::string TaskProtocol(const std::string& group);

}  // namespace mcast
