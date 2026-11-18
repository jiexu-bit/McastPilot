#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "net/platform.h"

namespace mcast::net {

struct McastConfig {
    std::string group;      // "239.1.1.1"
    uint16_t port = 5000;
    std::string source_ip;  // local IPv4 to send from ("" = OS default route)
    std::string iface_name; // optional, Linux: SO_BINDTODEVICE
    int ttl = 16;           // 1..255
    bool loopback = false;
    int dscp = 0;           // 0..63
    int send_buffer_bytes = 4 * 1024 * 1024;
};

struct SenderStats {
    uint64_t bytes = 0;
    uint64_t packets = 0;
    uint64_t drops = 0;
    uint64_t errors = 0;
};

class MulticastSender {
public:
    MulticastSender() = default;
    ~MulticastSender();

    MulticastSender(const MulticastSender&) = delete;
    MulticastSender& operator=(const MulticastSender&) = delete;

    bool Open(const McastConfig& config, std::string& err);
    void Close();

    // Non-blocking send. Returns false when the datagram was dropped.
    bool Send(const uint8_t* data, size_t len);

    bool IsOpen() const { return socket_ != kInvalidSocket; }
    SenderStats Stats() const;
    std::string LastError() const;

    int LocalPort() const { return local_port_; }
    int ActualSendBuffer() const { return actual_send_buffer_; }

private:
    Socket socket_ = kInvalidSocket;
    McastConfig config_;
    sockaddr_in dest_{};
    int local_port_ = 0;
    int actual_send_buffer_ = 0;

    mutable std::mutex error_mutex_;
    std::string last_error_;

    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> drops_{0};
    std::atomic<uint64_t> errors_{0};
};

}  // namespace mcast::net
