#include "net/nic.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "net/platform.h"

#ifdef _WIN32
#include <iphlpapi.h>
#include <iptypes.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/if_packet.h>
#include <netpacket/packet.h>
#endif
#endif

namespace mcast::net {
namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string IpToString(const void* addr, int family) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (inet_ntop(family, addr, buf, sizeof(buf)) == nullptr) return {};
    return buf;
}

bool LooksVirtual(const NicInfo& nic) {
    const std::string text = ToLower(nic.name + " " + nic.description);
    static const char* kMarkers[] = {
        "virtual", "vmware", "virtualbox", "hyper-v", "vethernet", "tap-",
        "tap adapter", "tun", "vpn", "loopback", "bluetooth", "wan miniport",
        "pseudo", "npcap", "loopback adapter",
    };
    for (const char* marker : kMarkers) {
        if (text.find(marker) != std::string::npos) return true;
    }
    return false;
}

void SortNics(std::vector<NicInfo>& nics) {
    std::stable_sort(nics.begin(), nics.end(), [](const NicInfo& a, const NicInfo& b) {
        if (a.up != b.up) return a.up;
        if (a.virtual_adapter != b.virtual_adapter) return !a.virtual_adapter;
        if (a.ipv4.empty() != b.ipv4.empty()) return !a.ipv4.empty();
        return ToLower(a.name) < ToLower(b.name);
    });
}

}  // namespace

std::string PrimaryIpv4(const NicInfo& nic) {
    return nic.ipv4.empty() ? std::string{} : nic.ipv4.front().addr;
}

std::string NicSummary(const NicInfo& nic) {
    std::string s = nic.name;
    const std::string ip = PrimaryIpv4(nic);
    if (!ip.empty()) s += " [" + ip + "]";
    if (nic.speed_mbps > 0) s += " " + std::to_string(nic.speed_mbps) + "Mbps";
    if (!nic.up) s += " (down)";
    if (nic.virtual_adapter) s += " (virtual)";
    return s;
}

#ifdef _WIN32
namespace {

std::string ToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace

std::vector<NicInfo> EnumerateNics() {
    std::vector<NicInfo> result;

    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 8; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
    }
    if (rc != NO_ERROR) return result;

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        NicInfo nic;
        nic.id = adapter->AdapterName ? adapter->AdapterName : "";
        nic.name = ToUtf8(adapter->FriendlyName);
        nic.description = ToUtf8(adapter->Description);
        nic.ifindex = adapter->IfIndex;
        nic.up = adapter->OperStatus == IfOperStatusUp;
        nic.loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        // TransmitLinkSpeed is (ULONG64)-1 when the driver has no value.
        const uint64_t speed_bps = static_cast<uint64_t>(adapter->TransmitLinkSpeed);
        nic.speed_mbps = (speed_bps == 0 || speed_bps > 400ULL * 1000 * 1000 * 1000)
                             ? 0
                             : speed_bps / 1000000ULL;

        if (adapter->PhysicalAddressLength > 0) {
            char mac[32] = {0};
            for (ULONG i = 0; i < adapter->PhysicalAddressLength && i < 8; ++i) {
                std::snprintf(mac + i * 3, sizeof(mac) - i * 3, "%02X%s",
                              adapter->PhysicalAddress[i],
                              (i + 1 < adapter->PhysicalAddressLength) ? "-" : "");
            }
            nic.mac = mac;
        }

        for (auto* uni = adapter->FirstUnicastAddress; uni; uni = uni->Next) {
            if (!uni->Address.lpSockaddr) continue;
            const int family = uni->Address.lpSockaddr->sa_family;
            if (family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(uni->Address.lpSockaddr);
                const uint32_t host = ntohl(sin->sin_addr.s_addr);
                if ((host >> 16) == 0xA9FE) continue;  // 169.254/16 APIPA
                nic.ipv4.push_back({IpToString(&sin->sin_addr, AF_INET),
                                    static_cast<uint8_t>(uni->OnLinkPrefixLength)});
            } else if (family == AF_INET6) {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(uni->Address.lpSockaddr);
                if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
                nic.ipv6.push_back({IpToString(&sin6->sin6_addr, AF_INET6),
                                    static_cast<uint8_t>(uni->OnLinkPrefixLength)});
            }
        }

        for (auto* gw = adapter->FirstGatewayAddress; gw; gw = gw->Next) {
            if (!gw->Address.lpSockaddr) continue;
            if (gw->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(gw->Address.lpSockaddr);
            nic.gateway4 = IpToString(&sin->sin_addr, AF_INET);
            break;
        }

        nic.virtual_adapter = LooksVirtual(nic);
        result.push_back(std::move(nic));
    }

    SortNics(result);
    return result;
}

#else  // POSIX

std::vector<NicInfo> EnumerateNics() {
    std::vector<NicInfo> result;

    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return result;

    for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;

        auto it = std::find_if(result.begin(), result.end(),
                               [&](const NicInfo& n) { return n.name == ifa->ifa_name; });
        if (it == result.end()) {
            NicInfo nic;
            nic.name = ifa->ifa_name;
            nic.id = ifa->ifa_name;
            nic.ifindex = if_nametoindex(ifa->ifa_name);
            nic.up = (ifa->ifa_flags & IFF_UP) != 0;
            nic.loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
            nic.virtual_adapter = LooksVirtual(nic);
            result.push_back(std::move(nic));
            it = std::prev(result.end());
        }

        if (!ifa->ifa_addr) continue;
        const int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
            uint32_t host = ntohl(sin->sin_addr.s_addr);
            if ((host >> 16) == 0xA9FE) continue;
            it->ipv4.push_back({IpToString(&sin->sin_addr, AF_INET), 0});
        } else if (family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
            it->ipv6.push_back({IpToString(&sin6->sin6_addr, AF_INET6), 0});
        } else if (family == AF_PACKET) {
#ifdef AF_PACKET
            const auto* sll = reinterpret_cast<const sockaddr_ll*>(ifa->ifa_addr);
            if (sll->sll_halen == 6) {
                char mac[32];
                std::snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
                              sll->sll_addr[0], sll->sll_addr[1], sll->sll_addr[2],
                              sll->sll_addr[3], sll->sll_addr[4], sll->sll_addr[5]);
                it->mac = mac;
            }
#endif
        }
    }
    freeifaddrs(ifaddr);

    for (auto& nic : result) {
        const std::string base = "/sys/class/net/" + nic.name;
        if (auto f = std::fopen((base + "/speed").c_str(), "r")) {
            int speed = 0;
            if (std::fscanf(f, "%d", &speed) == 1 && speed > 0) {
                nic.speed_mbps = static_cast<uint64_t>(speed);
            }
            std::fclose(f);
        }
    }

    SortNics(result);
    return result;
}

#endif

const NicInfo* FindNic(const std::vector<NicInfo>& nics, const std::string& key) {
    if (key.empty()) return nullptr;

    // 1) IPv4 address
    for (const auto& nic : nics) {
        for (const auto& ip : nic.ipv4) {
            if (ip.addr == key) return &nic;
        }
    }

    // 2) exact id / name (case-insensitive)
    const std::string lower = ToLower(key);
    for (const auto& nic : nics) {
        if (ToLower(nic.id) == lower || ToLower(nic.name) == lower) return &nic;
    }

    // 3) description substring
    for (const auto& nic : nics) {
        if (ToLower(nic.name).find(lower) != std::string::npos ||
            ToLower(nic.description).find(lower) != std::string::npos) {
            return &nic;
        }
    }

    // 4) 1-based index
    const bool numeric = !key.empty() &&
                         std::all_of(key.begin(), key.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; });
    if (numeric) {
        const size_t index = static_cast<size_t>(std::stoul(key));
        if (index >= 1 && index <= nics.size()) return &nics[index - 1];
    }
    return nullptr;
}

}  // namespace mcast::net
