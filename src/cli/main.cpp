#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "media/pipeline.h"
#include "media/probe.h"
#include "net/nic.h"
#include "net/platform.h"

using namespace mcast;

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop.store(true); }

struct Options {
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    std::vector<std::string> positional;

    bool Has(const std::string& key) const {
        return flags.count(key) > 0 || values.count(key) > 0;
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    int64_t GetInt(const std::string& key, int64_t fallback = 0) const {
        const std::string text = Get(key);
        if (text.empty()) return fallback;
        try {
            return std::stoll(text, nullptr, 0);
        } catch (...) {
            return fallback;
        }
    }
    double GetDouble(const std::string& key, double fallback = 0.0) const {
        const std::string text = Get(key);
        if (text.empty()) return fallback;
        try {
            return std::stod(text);
        } catch (...) {
            return fallback;
        }
    }
};

Options ParseOptions(int argc, char** argv, int start) {
    Options options;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            const size_t eq = arg.find('=');
            if (eq != std::string::npos) {
                options.values[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
                continue;
            }
            const std::string key = arg.substr(2);
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.values[key] = argv[++i];
            } else {
                options.flags.insert(key);
            }
        } else {
            options.positional.push_back(arg);
        }
    }
    return options;
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

void Usage() {
    std::printf(
        "mcastctl %s - multicast MPEG-TS pusher\n"
        "\n"
        "Usage:\n"
        "  mcastctl nics [--json] [--all]\n"
        "      List network adapters (IP / speed / virtual flag).\n"
        "\n"
        "  mcastctl probe --input <file|url>\n"
        "      Inspect the input: codecs, resolution, bitrate.\n"
        "\n"
        "  mcastctl send --input <file|url> --group <ip> --port <n> [options]\n"
        "      Push MPEG-TS over UDP (multicast 224-239.x.x.x or unicast).\n"
        "\n"
        "Send options:\n"
        "  --nic <ip|name|index>   Adapter to send from (default: OS route)\n"
        "  --ttl <1-255>           Multicast TTL (default 16)\n"
        "  --dscp <0-63>           QoS DSCP (default 0)\n"
        "  --pkt-size <bytes>      UDP payload size (default 1316 = 188*7)\n"
        "  --muxrate <bps>         CBR rate, 0 = auto from input (default 0)\n"
        "  --buffer <KB>           Send ring buffer (default 4096)\n"
        "  --loop                  Restart the file at EOF\n"
        "  --no-realtime           Push as fast as possible (bench/test)\n"
        "  --speed <factor>        Playback speed multiplier (default 1.0)\n"
        "  --duration <seconds>    Stop automatically\n"
        "  --loopback              Enable multicast loopback\n"
        "  --verbose               Debug logging\n"
        "\n"
        "Examples:\n"
        "  mcastctl nics\n"
        "  mcastctl send --input D:\\Videos\\demo.ts --loop \\\n"
        "      --group 239.1.1.1 --port 5000 --nic 192.168.1.10 --ttl 16\n"
        "  mcastctl send --input rtsp://10.0.0.5/live --group 239.1.1.2 --port 5000 \\\n"
        "      --nic 192.168.1.10 --muxrate 8000000\n",
        "0.3.0-m3");
}

int CmdNics(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv, 2);
    const bool json = options.Has("json");
    const bool show_all = options.Has("all");

    std::vector<net::NicInfo> nics = net::EnumerateNics();
    if (nics.empty()) {
        std::fprintf(stderr, "no network adapter found\n");
        return 2;
    }

    std::vector<const net::NicInfo*> visible;
    for (const auto& nic : nics) {
        if (!show_all && nic.loopback) continue;
        visible.push_back(&nic);
    }

    if (json) {
        std::printf("[");
        bool first = true;
        for (const auto* nic : visible) {
            if (!first) std::printf(",");
            first = false;
            std::printf(
                "{\"id\":\"%s\",\"name\":\"%s\",\"description\":\"%s\",\"ifindex\":%u,"
                "\"mac\":\"%s\",\"up\":%s,\"loopback\":%s,\"virtual\":%s,"
                "\"speed_mbps\":%llu,\"ipv4\":[",
                JsonEscape(nic->id).c_str(), JsonEscape(nic->name).c_str(),
                JsonEscape(nic->description).c_str(), nic->ifindex,
                JsonEscape(nic->mac).c_str(), nic->up ? "true" : "false",
                nic->loopback ? "true" : "false", nic->virtual_adapter ? "true" : "false",
                static_cast<unsigned long long>(nic->speed_mbps));
            for (size_t i = 0; i < nic->ipv4.size(); ++i) {
                if (i) std::printf(",");
                std::printf("{\"addr\":\"%s\",\"prefix\":%u}",
                            JsonEscape(nic->ipv4[i].addr).c_str(), nic->ipv4[i].prefix);
            }
            std::printf("],\"gateway4\":\"%s\"}", JsonEscape(nic->gateway4).c_str());
        }
        std::printf("]\n");
        return 0;
    }

    std::printf("%-3s %-30s %-16s %-11s %-7s %s\n", "#", "Name", "IPv4", "Speed", "Status",
                "Flags");
    std::printf("%s\n", std::string(96, '-').c_str());
    int index = 1;
    for (const auto* nic : visible) {
        const std::string ip = net::PrimaryIpv4(*nic);
        std::string speed = "--";
        if (nic->speed_mbps > 0) speed = std::to_string(nic->speed_mbps) + " Mbps";
        std::string flags;
        if (nic->virtual_adapter) flags += "virtual ";
        if (nic->loopback) flags += "loopback ";
        if (!nic->ipv4.empty() && !nic->gateway4.empty()) flags += "gw=" + nic->gateway4;

        std::printf("%-3d %-30s %-16s %-11s %-7s %s\n", index++, nic->name.c_str(),
                    ip.empty() ? "--" : ip.c_str(), speed.c_str(),
                    nic->up ? "up" : "down", flags.c_str());
    }
    std::printf("\nUse --nic <ip|name|index> to bind the multicast sender to one adapter.\n");
    return 0;
}

int CmdProbe(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv, 2);
    const std::string input = options.Get("input", options.positional.empty()
                                                       ? ""
                                                       : options.positional.front());
    if (input.empty()) {
        std::fprintf(stderr, "probe: --input is required\n");
        return 2;
    }

    ProbeResult result;
    std::string err;
    if (!ProbeInput(input, result, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }

    std::printf("input        : %s\n", input.c_str());
    std::printf("format       : %s\n", result.format.empty() ? "?" : result.format.c_str());
    std::printf("duration     : %.2f s\n", result.duration_sec);
    std::printf("input bitrate: %.3f Mbps\n",
                result.bitrate > 0 ? static_cast<double>(result.bitrate) / 1e6 : 0.0);
    std::printf("streams      : %u\n", static_cast<unsigned>(result.streams.size()));
    for (const ProbeStream& st : result.streams) {
        if (st.type == "video") {
            std::printf("  #%d video  %s %dx%d %.3f fps %lld bps\n", st.index,
                        st.codec.c_str(), st.width, st.height, st.fps,
                        static_cast<long long>(st.bitrate));
        } else if (st.type == "audio") {
            std::printf("  #%d audio  %s %d Hz %d ch %lld bps\n", st.index,
                        st.codec.c_str(), st.sample_rate, st.channels,
                        static_cast<long long>(st.bitrate));
        } else {
            std::printf("  #%d other  %s\n", st.index, st.codec.c_str());
        }
    }
    return 0;
}

int CmdSend(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv, 2);
    if (options.Has("help")) {
        Usage();
        return 0;
    }

    const std::string input = options.Get("input");
    const std::string group = options.Get("group");
    const int64_t port = options.GetInt("port", 0);
    if (input.empty() || group.empty() || port <= 0 || port > 65535) {
        std::fprintf(stderr,
                     "send: --input, --group and --port (1-65535) are required\n\n");
        Usage();
        return 2;
    }

    PipelineConfig config;
    config.input = input;
    config.loop = options.Has("loop");
    config.realtime = !options.Has("no-realtime");
    config.speed = options.GetDouble("speed", 1.0);
    config.muxrate = options.GetInt("muxrate", 0);
    config.pkt_size = static_cast<int>(options.GetInt("pkt-size", 1316));
    config.duration_ms = options.GetInt("duration", 0) * 1000;
    config.buffer_bytes = static_cast<size_t>(options.GetInt("buffer", 4096)) * 1024;

    config.mcast.group = group;
    config.mcast.port = static_cast<uint16_t>(port);
    config.mcast.ttl = static_cast<int>(options.GetInt("ttl", 16));
    config.mcast.dscp = static_cast<int>(options.GetInt("dscp", 0));
    config.mcast.loopback = options.Has("loopback");
    config.mcast.send_buffer_bytes =
        static_cast<int>(std::min<int64_t>(options.GetInt("sndbuf", 4 * 1024 * 1024), 64 * 1024 * 1024));

    const std::string nic_key = options.Get("nic");
    if (!nic_key.empty()) {
        const std::vector<net::NicInfo> nics = net::EnumerateNics();
        const net::NicInfo* nic = net::FindNic(nics, nic_key);
        if (!nic) {
            std::fprintf(stderr, "adapter not found: %s\n\nAvailable adapters:\n", nic_key.c_str());
            int index = 1;
            for (const auto& item : nics) {
                std::fprintf(stderr, "  #%-2d %s\n", index++, net::NicSummary(item).c_str());
            }
            return 2;
        }
        const std::string ip = net::PrimaryIpv4(*nic);
        if (ip.empty()) {
            std::fprintf(stderr, "adapter '%s' has no IPv4 address\n", nic->name.c_str());
            return 2;
        }
        config.mcast.source_ip = ip;
#ifndef _WIN32
        config.mcast.iface_name = nic->name;
#endif
        std::printf("adapter      : %s (%s)\n", nic->name.c_str(), ip.c_str());
    } else {
        std::printf("adapter      : OS default route (use --nic to pin one)\n");
    }

    std::printf("input        : %s%s\n", input.c_str(), config.loop ? " (loop)" : "");
    std::printf("destination  : udp://%s:%u  ttl=%d dscp=%d pkt=%d\n", group.c_str(),
                config.mcast.port, config.mcast.ttl, config.mcast.dscp, config.pkt_size);
    if (config.muxrate > 0) {
        std::printf("muxrate      : %.3f Mbps (CBR)\n", static_cast<double>(config.muxrate) / 1e6);
    } else {
        std::printf("muxrate      : auto (CBR)\n");
    }
    std::printf("\n");

    Pipeline pipeline;
    std::string err;
    if (!pipeline.Start(config, err)) {
        std::fprintf(stderr, "start failed: %s\n", err.c_str());
        return 2;
    }

    std::printf("pushing... press Ctrl+C to stop\n\n");
    auto last_print = std::chrono::steady_clock::now();
    uint64_t last_packets = 0;
    uint64_t last_muxed = 0;

    while (!g_stop.load() && pipeline.Running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print).count() < 1000) {
            continue;
        }
        last_print = now;

        const PipelineStats stats = pipeline.Snapshot();
        const uint64_t pps = stats.packets_sent - last_packets;
        last_packets = stats.packets_sent;
        const uint64_t muxed = stats.muxed_bytes - last_muxed;
        last_muxed = stats.muxed_bytes;
        const double fill = stats.buffer_capacity > 0
                                ? 100.0 * static_cast<double>(stats.buffer_bytes) /
                                      static_cast<double>(stats.buffer_capacity)
                                : 0.0;
        std::printf("[%5.1fs] tx=%6.2f Mbps  mux=%6.2f Mbps  rate=%6.2f  %7llu pkt/s  "
                    "sent=%llu  loss=%lluKB  buf=%3.0f%%  err=%llu\n",
                    stats.elapsed_sec, stats.bitrate_mbps,
                    static_cast<double>(muxed) * 8.0 / 1e6,
                    static_cast<double>(stats.muxrate_bps) / 1e6,
                    static_cast<unsigned long long>(pps),
                    static_cast<unsigned long long>(stats.packets_sent),
                    static_cast<unsigned long long>(stats.buffer_drop_bytes / 1024), fill,
                    static_cast<unsigned long long>(stats.send_errors));
        std::fflush(stdout);
    }

    pipeline.Stop();
    const PipelineStats final_stats = pipeline.Snapshot();
    std::printf("\nstopped: %.1f s, %.3f GB sent, %llu packets, %llu drops, %llu errors, %llu KB lost\n",
                final_stats.elapsed_sec,
                static_cast<double>(final_stats.output_bytes) / (1024.0 * 1024.0 * 1024.0),
                static_cast<unsigned long long>(final_stats.packets_sent),
                static_cast<unsigned long long>(final_stats.packets_dropped),
                static_cast<unsigned long long>(final_stats.send_errors),
                static_cast<unsigned long long>(final_stats.buffer_drop_bytes / 1024));
    if (!final_stats.last_error.empty()) {
        std::fprintf(stderr, "last error: %s\n", final_stats.last_error.c_str());
        return 3;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string err;
    if (!net::InitSockets(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // Log level flags are global, so handle them before dispatching.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            SetLogLevel(LogLevel::Debug);
        } else if (arg.rfind("--log-level=", 0) == 0) {
            LogLevel level;
            if (ParseLogLevel(arg.c_str() + 12, level)) SetLogLevel(level);
        } else if (arg == "--log-level" && i + 1 < argc) {
            LogLevel level;
            if (ParseLogLevel(argv[++i], level)) SetLogLevel(level);
        }
    }

    std::signal(SIGINT, OnSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, OnSignal);
#endif

    int result = 1;
    if (argc < 2) {
        Usage();
    } else {
        const std::string command = argv[1];
        if (command == "nics") {
            result = CmdNics(argc, argv);
        } else if (command == "probe") {
            result = CmdProbe(argc, argv);
        } else if (command == "send") {
            result = CmdSend(argc, argv);
        } else if (command == "--help" || command == "-h" || command == "help") {
            Usage();
            result = 0;
        } else if (command == "--version" || command == "-V") {
            std::printf("mcastctl 0.3.0-m3\n");
            result = 0;
        } else {
            std::fprintf(stderr, "unknown command: %s\n\n", command.c_str());
            Usage();
        }
    }

    net::ShutdownSockets();
    return result;
}
