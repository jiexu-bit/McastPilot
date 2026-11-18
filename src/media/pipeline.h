#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/byte_ring.h"
#include "net/multicast_sender.h"

struct AVFormatContext;
struct AVIOContext;

namespace mcast {

class Transcoder;

struct PipelineConfig {
    std::string input;          // file path or URL (rtsp://, udp://, ...)
    bool loop = false;          // restart at EOF (files)
    bool realtime = true;       // pace input by DTS, like `ffmpeg -re`
    double speed = 1.0;         // 1.0 = realtime
    int64_t muxrate = 0;        // bits/s, 0 = auto from input bitrate
    int pkt_size = 1316;        // UDP payload size (188 * 7)
    int64_t duration_ms = 0;    // 0 = until stopped
    size_t buffer_bytes = 4 * 1024 * 1024;
    std::string log_tag;        // task id; attached to per-task log lines
    net::McastConfig mcast;
    std::vector<net::McastConfig> extra_mcasts;
    std::string record_path;    // empty = no recording
    std::string encode_mode = "copy";
    int video_bitrate = 4000000;
    int audio_bitrate = 128000;
    std::string preset = "veryfast";
    int gop = 0;
};

struct PipelineStats {
    bool running = false;
    uint64_t muxed_bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t packets_sent = 0;
    uint64_t packets_dropped = 0;
    uint64_t buffer_drop_bytes = 0;
    uint64_t send_errors = 0;
    uint64_t frames = 0;
    uint64_t recorded_bytes = 0;
    size_t destinations = 0;
    size_t buffer_bytes = 0;
    size_t buffer_capacity = 0;
    int64_t muxrate_bps = 0;
    double bitrate_mbps = 0.0;
    double elapsed_sec = 0.0;
    std::string last_error;
};

class Pipeline {
public:
    Pipeline();
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    bool Start(const PipelineConfig& config, std::string& err);
    void Stop();
    bool Running() const { return running_.load(std::memory_order_relaxed); }

    PipelineStats Snapshot() const;

private:
    void ReaderLoop();
    void SenderLoop();
    void SleepInterruptible(int64_t microseconds);

    static int WriteCallback(void* opaque, const uint8_t* buf, int size);
    static int InterruptCallback(void* opaque);

    void SetError(const std::string& message);

    PipelineConfig config_;
    std::unique_ptr<ByteRing> ring_;
    std::vector<std::unique_ptr<net::MulticastSender>> senders_;
    std::unique_ptr<Transcoder> transcoder_;
    std::ofstream record_file_;

    std::thread reader_thread_;
    std::thread sender_thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> muxed_bytes_{0};
    std::atomic<uint64_t> recorded_bytes_{0};

    std::atomic<int64_t> rate_bps_{0};
    int64_t started_us_ = 0;

    mutable std::mutex error_mutex_;
    std::string last_error_;

    mutable std::atomic<uint64_t> sample_bytes_{0};
    mutable std::atomic<int64_t> sample_us_{0};
    mutable std::atomic<double> bitrate_mbps_{0.0};
};

}  // namespace mcast
