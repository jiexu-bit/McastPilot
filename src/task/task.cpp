#include "task/task.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>
#include <set>

#include "common/config.h"
#include "net/platform.h"

namespace mcast {
namespace fs = std::filesystem;
namespace {

std::string Trim(std::string text) {
    auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

bool IsMulticastAddress(const std::string& group) {
    size_t pos = group.find('.');
    if (pos == std::string::npos) return false;
    try {
        const int first = std::stoi(group.substr(0, pos));
        return first >= 224 && first <= 239;
    } catch (...) {
        return false;
    }
}

std::string DefaultName(const std::string& input) {
    if (IsNetworkUrl(input)) {
        const size_t slash = input.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < input.size()) {
            return input.substr(slash + 1);
        }
        return input;
    }
    const std::string name = fs::path(input).filename().string();
    return name.empty() ? "task" : name;
}

bool ValidIpv4(const std::string& address) {
    in_addr addr{};
    return inet_pton(AF_INET, address.c_str(), &addr) == 1;
}

}  // namespace

std::string NewTaskId() {
    std::random_device device;
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    static const char* hex = "0123456789abcdef";
    std::string id;
    id.reserve(8);
    for (int i = 0; i < 4; ++i) {
        const unsigned int value = dist(device);
        id.push_back(hex[value >> 4]);
        id.push_back(hex[value & 0x0F]);
    }
    return id;
}

bool ValidateTaskSpec(TaskSpec& spec, std::string& err) {
    spec.name = Trim(spec.name);
    if (spec.name.empty()) spec.name = DefaultName(spec.input);
    if (spec.name.size() > 200) {
        err = "name is too long";
        return false;
    }
    if (!spec.id.empty()) {
        if (spec.id.size() > 64) {
            err = "id is too long";
            return false;
        }
        for (unsigned char c : spec.id) {
            if (!std::isalnum(c) && c != '_' && c != '-') {
                err = "id may only contain letters, digits, '_' and '-'";
                return false;
            }
        }
    }
    if (spec.input.empty()) {
        err = "input is required";
        return false;
    }
    if (spec.input.size() > 4096) {
        err = "input is too long";
        return false;
    }
    if (spec.input.find("://") != std::string::npos) {
        if (!IsNetworkUrl(spec.input)) {
            err = "unsupported input URL scheme";
            return false;
        }
    } else {
        spec.input = NormalizePath(spec.input);
    }
    if (spec.group.empty()) {
        err = "group is required";
        return false;
    }
    if (!ValidIpv4(spec.group)) {
        err = "group must be a valid IPv4 address";
        return false;
    }
    if (spec.port == 0) {
        err = "port must be 1-65535";
        return false;
    }
    if (spec.extra_outputs.size() > 4) {
        err = "at most 4 extra destinations are allowed";
        return false;
    }
    std::set<std::string> destinations;
    destinations.insert(spec.group + ":" + std::to_string(spec.port));
    for (const ExtraOutput& extra : spec.extra_outputs) {
        if (!ValidIpv4(extra.group)) {
            err = "extra destination must be a valid IPv4 address: " + extra.group;
            return false;
        }
        if (extra.port == 0) {
            err = "extra destination port must be 1-65535";
            return false;
        }
        const std::string key = extra.group + ":" + std::to_string(extra.port);
        if (!destinations.insert(key).second) {
            err = "duplicate destination: " + key;
            return false;
        }
    }
    if (spec.ttl < 1 || spec.ttl > 255) {
        err = "ttl must be 1-255";
        return false;
    }
    if (spec.dscp < 0 || spec.dscp > 63) {
        err = "dscp must be 0-63";
        return false;
    }
    if (spec.pkt_size < 188 || spec.pkt_size > 65507 || spec.pkt_size % 188 != 0) {
        err = "pkt_size must be a multiple of 188 within [188, 65507]";
        return false;
    }
    if (spec.muxrate < 0) {
        err = "muxrate must be >= 0";
        return false;
    }
    if (!(spec.speed > 0.0) || spec.speed > 100.0) {
        err = "speed must be in (0, 100]";
        return false;
    }
    if (spec.buffer_bytes < 64 * 1024) spec.buffer_bytes = 64 * 1024;
    if (spec.buffer_bytes > 256ull * 1024 * 1024) spec.buffer_bytes = 256ull * 1024 * 1024;
    if (spec.max_restarts < 0) spec.max_restarts = 0;
    if (spec.max_restarts > 1000) spec.max_restarts = 1000;
    if (spec.backoff_ms < 100) spec.backoff_ms = 100;
    if (spec.backoff_ms > 60000) spec.backoff_ms = 60000;
    return true;
}

std::string TaskProtocol(const std::string& group) {
    return IsMulticastAddress(group) ? "udp-multicast" : "udp-unicast";
}

nlohmann::json TaskSpecToJson(const TaskSpec& spec) {
    nlohmann::json json;
    json["id"] = spec.id;
    json["name"] = spec.name;
    json["source"] = {
        {"type", IsNetworkUrl(spec.input) ? "url" : "file"},
        {"uri", spec.input},
        {"loop", spec.loop},
    };
    json["output"] = {
        {"protocol", TaskProtocol(spec.group)},
        {"group", spec.group},
        {"port", spec.port},
        {"nic", spec.nic},
        {"ttl", spec.ttl},
        {"dscp", spec.dscp},
        {"loopback", spec.loopback},
        {"pkt_size", spec.pkt_size},
    };
    json["output"]["extra"] = nlohmann::json::array();
    for (const ExtraOutput& extra : spec.extra_outputs) {
        json["output"]["extra"].push_back({{"group", extra.group}, {"port", extra.port}});
    }
    json["record"] = spec.record;
    json["mux"] = {
        {"format", "mpegts"},
        {"muxrate", spec.muxrate},
        {"cbr", true},
    };
    json["encode"] = {{"mode", "copy"}};
    json["restart"] = {
        {"auto", spec.auto_restart},
        {"max_retries", spec.max_restarts},
        {"backoff_ms", spec.backoff_ms},
    };
    json["realtime"] = spec.realtime;
    json["speed"] = spec.speed;
    json["buffer_kb"] = static_cast<int64_t>(spec.buffer_bytes / 1024);
    return json;
}

bool TaskSpecFromJson(const nlohmann::json& json, TaskSpec& spec, std::string& err) {
    spec = TaskSpec{};
    try {
        if (!json.is_object()) {
            err = "task must be a JSON object";
            return false;
        }

        spec.id = json.value("id", std::string{});
        spec.name = json.value("name", std::string{});

        int raw_port = 5000;
        if (json.contains("source") && json["source"].is_object()) {
            const auto& source = json["source"];
            spec.input = source.value("uri", std::string{});
            spec.loop = source.value("loop", false);
        } else {
            spec.input = json.value("input", std::string{});
            spec.loop = json.value("loop", false);
        }

        if (json.contains("output") && json["output"].is_object()) {
            const auto& output = json["output"];
            spec.group = output.value("group", std::string{});
            raw_port = output.value("port", 5000);
            spec.nic = output.value("nic", std::string{});
            spec.ttl = output.value("ttl", 16);
            spec.dscp = output.value("dscp", 0);
            spec.loopback = output.value("loopback", false);
            spec.pkt_size = output.value("pkt_size", 1316);
            if (output.contains("extra") && output["extra"].is_array()) {
                for (const auto& value : output["extra"]) {
                    if (!value.is_object()) {
                        err = "extra destination must be an object";
                        return false;
                    }
                    ExtraOutput extra;
                    extra.group = value.value("group", std::string{});
                    const int extra_port = value.value("port", 0);
                    if (extra_port < 1 || extra_port > 65535) {
                        err = "extra destination port must be 1-65535";
                        return false;
                    }
                    extra.port = static_cast<uint16_t>(extra_port);
                    spec.extra_outputs.push_back(std::move(extra));
                }
            }
        } else {
            spec.group = json.value("group", std::string{});
            raw_port = json.value("port", 5000);
            spec.nic = json.value("nic", std::string{});
            spec.ttl = json.value("ttl", 16);
            spec.dscp = json.value("dscp", 0);
            spec.loopback = json.value("loopback", false);
            spec.pkt_size = json.value("pkt_size", 1316);
        }
        spec.record = json.value("record", false);
        if (raw_port < 1 || raw_port > 65535) {
            err = "port must be 1-65535";
            return false;
        }
        spec.port = static_cast<uint16_t>(raw_port);

        if (json.contains("mux") && json["mux"].is_object()) {
            spec.muxrate = json["mux"].value("muxrate", 0);
        } else {
            spec.muxrate = json.value("muxrate", 0);
        }

        if (json.contains("restart") && json["restart"].is_object()) {
            const auto& restart = json["restart"];
            spec.auto_restart = restart.value("auto", true);
            spec.max_restarts = restart.value("max_retries", 10);
            spec.backoff_ms = restart.value("backoff_ms", 1000);
        }

        spec.realtime = json.value("realtime", true);
        spec.speed = json.value("speed", 1.0);
        const int64_t buffer_kb = json.value("buffer_kb", 4096);
        if (buffer_kb > 0) {
            spec.buffer_bytes = static_cast<size_t>(buffer_kb) * 1024;
        }
    } catch (const std::exception& e) {
        err = std::string("invalid task JSON: ") + e.what();
        return false;
    }

    return ValidateTaskSpec(spec, err);
}

}  // namespace mcast
