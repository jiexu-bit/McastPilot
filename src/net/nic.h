#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcast::net {

struct IpAddress {
    std::string addr;
    uint8_t prefix = 0;
};

struct NicInfo {
    std::string id;           // stable identifier (GUID on Windows, ifname on Linux)
    std::string name;         // friendly name shown in the UI
    std::string description;
    uint32_t ifindex = 0;
    std::string mac;
    bool up = false;
    bool loopback = false;
    bool virtual_adapter = false;
    uint64_t speed_mbps = 0;
    std::vector<IpAddress> ipv4;
    std::vector<IpAddress> ipv6;
    std::string gateway4;
};

std::vector<NicInfo> EnumerateNics();

// key may be:
//   * an IPv4 address of the adapter
//   * the adapter id / friendly name (exact, case-insensitive)
//   * a substring of name or description
//   * a 1-based index into the enumerated list
const NicInfo* FindNic(const std::vector<NicInfo>& nics, const std::string& key);

// First usable IPv4 of the adapter (empty if none).
std::string PrimaryIpv4(const NicInfo& nic);

std::string NicSummary(const NicInfo& nic);

}  // namespace mcast::net
