#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcast {

struct ProbeStream {
    int index = 0;
    std::string type;  // video / audio / other
    std::string codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int sample_rate = 0;
    int channels = 0;
    int64_t bitrate = 0;
};

struct ProbeResult {
    std::string format;
    double duration_sec = 0.0;
    int64_t bitrate = 0;
    std::vector<ProbeStream> streams;
};

// Opens the input just far enough to read stream metadata. Works for local
// files and network URLs (rtsp/rtmp/udp/http).
bool ProbeInput(const std::string& input, ProbeResult& result, std::string& err);

}  // namespace mcast
