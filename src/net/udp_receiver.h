#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "net/platform.h"

namespace mcast::net {

struct ReceiverConfig {
    std::string group;   // multicast group or unicast local address
    uint16_t port = 5000;
    std::string iface;   // local IPv4 used for IP_ADD_MEMBERSHIP / bind
    int64_t duration_ms = 0;
    int rcvbuf = 8 * 1024 * 1024;
};

struct ReceiverStats {
    bool running = false;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t sync_errors = 0;
    uint64_t cc_errors = 0;
    double bitrate_mbps = 0.0;
    double elapsed_sec = 0.0;
    std::string last_error;
};

class UdpReceiver {
public:
    UdpReceiver() = default;
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    bool Start(const ReceiverConfig& config, std::string& err);
    void Stop();
    bool Running() const { return running_.load(std::memory_order_relaxed); }
    ReceiverStats Snapshot() const;

private:
    void Loop();
    void SetError(const std::string& error);

    ReceiverConfig config_;
    Socket socket_ = kInvalidSocket;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> sync_errors_{0};
    std::atomic<uint64_t> cc_errors_{0};
    std::atomic<double> elapsed_sec_{0.0};
    std::chrono::steady_clock::time_point started_;

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

}  // namespace mcast::net
