#include "net/multicast_sender.h"

#include <cstring>

#include "common/log.h"

#ifdef _WIN32
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <cerrno>
#include <fcntl.h>
#endif

namespace mcast::net {
namespace {

constexpr int kMaxUdpPayload = 65507;

#ifdef _WIN32
bool WouldBlock(int code) { return code == WSAEWOULDBLOCK || code == WSAENOBUFS; }
int LastSocketError() { return WSAGetLastError(); }
#else
bool WouldBlock(int code) { return code == EAGAIN || code == EWOULDBLOCK || code == ENOBUFS; }
int LastSocketError() { return errno; }
#endif

}  // namespace

MulticastSender::~MulticastSender() { Close(); }

bool MulticastSender::Open(const McastConfig& config, std::string& err) {
    Close();
    config_ = config;

    if (config_.group.empty() || config_.port == 0) {
        err = "multicast group/port is empty";
        return false;
    }

    in_addr group_addr{};
    if (inet_pton(AF_INET, config_.group.c_str(), &group_addr) != 1) {
        err = "invalid multicast group: " + config_.group;
        return false;
    }
    const uint32_t host = ntohl(group_addr.s_addr);
    const bool is_multicast = (host >> 28) == 0x0E;  // 224.0.0.0/4

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == kInvalidSocket) {
        err = "socket() failed: " + SocketErrorText(LastSocketError());
        return false;
    }

#ifdef _WIN32
    // ICMP port-unreachable must not break subsequent sendto() calls.
    BOOL disable_conn_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(socket_, SIO_UDP_CONNRESET, &disable_conn_reset, sizeof(disable_conn_reset),
             nullptr, 0, &returned, nullptr, nullptr);
#else
    int on = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

    // Send buffer: report what the OS actually granted, it is often capped.
    int requested = config_.send_buffer_bytes > 0 ? config_.send_buffer_bytes : 4 * 1024 * 1024;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&requested),
                       sizeof(requested)) == 0) {
            break;
        }
        requested /= 2;
    }
    socklen_t optlen = sizeof(actual_send_buffer_);
    if (getsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&actual_send_buffer_),
                   &optlen) != 0) {
        actual_send_buffer_ = 0;
    }

    // Bind to the chosen local address so the OS cannot pick another adapter.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (!config_.source_ip.empty()) {
        in_addr src{};
        if (inet_pton(AF_INET, config_.source_ip.c_str(), &src) != 1) {
            err = "invalid source ip: " + config_.source_ip;
            Close();
            return false;
        }
        local.sin_addr = src;
    }

#ifndef _WIN32
    if (!config_.iface_name.empty()) {
        if (setsockopt(socket_, SOL_SOCKET, SO_BINDTODEVICE, config_.iface_name.c_str(),
                       static_cast<socklen_t>(config_.iface_name.size())) != 0) {
            LOG_WARN("SO_BINDTODEVICE(%s) failed: %s", config_.iface_name.c_str(),
                     SocketErrorText(LastSocketError()).c_str());
        }
    }
#endif

    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        err = "bind(" + config_.source_ip + ") failed: " + SocketErrorText(LastSocketError());
        Close();
        return false;
    }

    // Multicast egress interface. Unicast destinations follow the OS route
    // (the bind above already pins the source address when --nic is used).
    if (is_multicast) {
#ifdef _WIN32
        in_addr iface_addr{};
        iface_addr.s_addr = local.sin_addr.s_addr;
        if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<const char*>(&iface_addr), sizeof(iface_addr)) != 0) {
            LOG_WARN("IP_MULTICAST_IF(%s) failed: %s", config_.source_ip.c_str(),
                     SocketErrorText(LastSocketError()).c_str());
        }
#else
        ip_mreqn mreq{};
        mreq.imr_address = local.sin_addr;
        mreq.imr_ifindex = config_.iface_name.empty()
                               ? 0
                               : static_cast<int>(if_nametoindex(config_.iface_name.c_str()));
        if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq)) != 0) {
            LOG_WARN("IP_MULTICAST_IF(%s) failed: %s", config_.source_ip.c_str(),
                     SocketErrorText(LastSocketError()).c_str());
        }
#endif

        int ttl = config_.ttl > 0 ? config_.ttl : 1;
        if (ttl > 255) ttl = 255;
        if (setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL,
                       reinterpret_cast<const char*>(&ttl), sizeof(ttl)) != 0) {
            err = "IP_MULTICAST_TTL failed: " + SocketErrorText(LastSocketError());
            Close();
            return false;
        }

        int loop = config_.loopback ? 1 : 0;
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop),
                   sizeof(loop));
    } else {
        int ttl = config_.ttl > 0 ? config_.ttl : 64;
        setsockopt(socket_, IPPROTO_IP, IP_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    }

    if (config_.dscp > 0) {
        int tos = (config_.dscp & 0x3F) << 2;  // DSCP occupies the upper 6 bits of TOS
        setsockopt(socket_, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));
    }

#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonblocking);
#else
    int flags = fcntl(socket_, F_GETFL, 0);
    fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif

    // Discover the effective local port (useful for logs / firewalls).
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        local_port_ = ntohs(bound.sin_port);
    }

    dest_.sin_family = AF_INET;
    dest_.sin_addr = group_addr;
    dest_.sin_port = htons(config_.port);

    LOG_INFO("%s sender ready: %s:%u via %s (ttl=%d dscp=%d sndbuf=%d local_port=%d)",
             is_multicast ? "multicast" : "unicast", config_.group.c_str(), config_.port,
             config_.source_ip.empty() ? "default" : config_.source_ip.c_str(), config_.ttl,
             config_.dscp, actual_send_buffer_, local_port_);
    return true;
}

void MulticastSender::Close() {
    if (socket_ != kInvalidSocket) {
#ifdef _WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
        socket_ = kInvalidSocket;
    }
    local_port_ = 0;
    actual_send_buffer_ = 0;
}

bool MulticastSender::Send(const uint8_t* data, size_t len) {
    if (socket_ == kInvalidSocket || len == 0) return false;
    const int send_len = static_cast<int>(len > static_cast<size_t>(kMaxUdpPayload)
                                              ? kMaxUdpPayload
                                              : len);

    const int rc = ::sendto(socket_, reinterpret_cast<const char*>(data), send_len, 0,
                            reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
    if (rc == send_len) {
        bytes_.fetch_add(static_cast<uint64_t>(send_len), std::memory_order_relaxed);
        packets_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const int code = LastSocketError();
    if (WouldBlock(code)) {
        drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    errors_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = SocketErrorText(code);
    }
    return false;
}

SenderStats MulticastSender::Stats() const {
    SenderStats s;
    s.bytes = bytes_.load(std::memory_order_relaxed);
    s.packets = packets_.load(std::memory_order_relaxed);
    s.drops = drops_.load(std::memory_order_relaxed);
    s.errors = errors_.load(std::memory_order_relaxed);
    return s;
}

std::string MulticastSender::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

}  // namespace mcast::net
