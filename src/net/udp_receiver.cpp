#include "net/udp_receiver.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "common/log.h"

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <cerrno>
#endif

namespace mcast::net {
namespace {

constexpr size_t kTsPacketSize = 188;
constexpr uint8_t kTsSyncByte = 0x47;
constexpr uint16_t kNullPid = 0x1FFF;

bool WouldBlock(int code) {
#ifdef _WIN32
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
#endif
}

int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void CloseSocket(Socket& socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
    socket = kInvalidSocket;
}

}  // namespace

UdpReceiver::~UdpReceiver() { Stop(); }

bool UdpReceiver::Start(const ReceiverConfig& config, std::string& err) {
    if (running_.load()) {
        err = "receiver is already running";
        return false;
    }
    config_ = config;
    if (config_.group.empty() || config_.port == 0) {
        err = "group and port are required";
        return false;
    }

    in_addr group_addr{};
    if (inet_pton(AF_INET, config_.group.c_str(), &group_addr) != 1) {
        err = "invalid group: " + config_.group;
        return false;
    }
    const uint32_t host = ntohl(group_addr.s_addr);
    const bool multicast = (host >> 28) == 0x0E;

    CloseSocket(socket_);
    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == kInvalidSocket) {
        err = "socket() failed: " + SocketErrorText(LastSocketError());
        return false;
    }

    int one = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));

    int rcvbuf = config_.rcvbuf > 0 ? config_.rcvbuf : 8 * 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf),
               sizeof(rcvbuf));

#ifdef _WIN32
    BOOL disable_conn_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(socket_, SIO_UDP_CONNRESET, &disable_conn_reset, sizeof(disable_conn_reset),
             nullptr, 0, &returned, nullptr, nullptr);
    DWORD timeout_ms = 500;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
#else
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(config_.port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!multicast && !config_.iface.empty()) {
        in_addr iface_addr{};
        if (inet_pton(AF_INET, config_.iface.c_str(), &iface_addr) == 1) {
            local.sin_addr = iface_addr;
        }
    }
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        err = "bind failed: " + SocketErrorText(LastSocketError());
        CloseSocket(socket_);
        return false;
    }

    if (multicast) {
        ip_mreq membership{};
        membership.imr_multiaddr = group_addr;
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!config_.iface.empty()) {
            in_addr iface_addr{};
            if (inet_pton(AF_INET, config_.iface.c_str(), &iface_addr) != 1) {
                err = "invalid iface: " + config_.iface;
                CloseSocket(socket_);
                return false;
            }
            membership.imr_interface = iface_addr;
        }
        if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0) {
            err = "IP_ADD_MEMBERSHIP failed: " + SocketErrorText(LastSocketError());
            CloseSocket(socket_);
            return false;
        }
    }

    packets_.store(0);
    bytes_.store(0);
    sync_errors_.store(0);
    cc_errors_.store(0);
    elapsed_sec_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_.clear();
    }
    started_ = std::chrono::steady_clock::now();
    running_.store(true);
    thread_ = std::thread(&UdpReceiver::Loop, this);

    LOG_INFO("monitor %s %s:%u%s%s", multicast ? "joined" : "listening on",
             config_.group.c_str(), static_cast<unsigned>(config_.port),
             config_.iface.empty() ? "" : " via ", config_.iface.c_str());
    return true;
}

void UdpReceiver::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    CloseSocket(socket_);
}

void UdpReceiver::SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
}

void UdpReceiver::Loop() {
    std::vector<uint8_t> buffer(65536);
    std::unordered_map<uint16_t, uint8_t> cc_state;

    while (running_.load(std::memory_order_relaxed)) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_)
                .count();
        elapsed_sec_.store(elapsed, std::memory_order_relaxed);
        if (config_.duration_ms > 0 && elapsed * 1000.0 >= config_.duration_ms) break;

        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()),
                               static_cast<int>(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) {
            const int code = LastSocketError();
            if (!running_.load(std::memory_order_relaxed)) break;
            if (WouldBlock(code)) continue;
            SetError("recvfrom failed: " + SocketErrorText(code));
            break;
        }

        packets_.fetch_add(1, std::memory_order_relaxed);
        bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

        const size_t size = static_cast<size_t>(n);
        if (size % kTsPacketSize != 0) {
            sync_errors_.fetch_add(1, std::memory_order_relaxed);
        }
        for (size_t offset = 0; offset + kTsPacketSize <= size; offset += kTsPacketSize) {
            if (buffer[offset] != kTsSyncByte) {
                sync_errors_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const uint16_t pid = static_cast<uint16_t>(
                ((buffer[offset + 1] & 0x1F) << 8) | buffer[offset + 2]);
            if (pid == kNullPid) continue;
            const uint8_t afc = static_cast<uint8_t>((buffer[offset + 3] >> 4) & 0x03);
            if (afc != 1 && afc != 3) continue;
            const uint8_t counter = static_cast<uint8_t>(buffer[offset + 3] & 0x0F);
            auto it = cc_state.find(pid);
            if (it != cc_state.end() && counter != ((it->second + 1) & 0x0F)) {
                cc_errors_.fetch_add(1, std::memory_order_relaxed);
            }
            cc_state[pid] = counter;
        }
    }

    elapsed_sec_.store(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count(),
        std::memory_order_relaxed);
    running_.store(false);
    // Duration expiry / fatal error ends the loop without Stop(); release the
    // port so VLC or another receiver can bind it immediately.
    CloseSocket(socket_);
}

ReceiverStats UdpReceiver::Snapshot() const {
    ReceiverStats stats;
    stats.running = running_.load(std::memory_order_relaxed);
    stats.packets = packets_.load(std::memory_order_relaxed);
    stats.bytes = bytes_.load(std::memory_order_relaxed);
    stats.sync_errors = sync_errors_.load(std::memory_order_relaxed);
    stats.cc_errors = cc_errors_.load(std::memory_order_relaxed);
    stats.elapsed_sec = stats.running
                            ? std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                            started_)
                                  .count()
                            : elapsed_sec_.load(std::memory_order_relaxed);
    if (stats.elapsed_sec > 0.0) {
        stats.bitrate_mbps =
            static_cast<double>(stats.bytes) * 8.0 / stats.elapsed_sec / 1e6;
    }
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        stats.last_error = last_error_;
    }
    return stats;
}

}  // namespace mcast::net
