#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct AVFormatContext;
struct AVStream;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace mcast {

struct TranscodeSettings {
    int video_bitrate = 4000000;
    int audio_bitrate = 128000;
    std::string preset = "veryfast";
    int gop = 0;
};

// Decodes input audio/video and re-encodes to H.264 + AAC for the mpegts
// output context. Packets passed to Process() must already have normalized,
// monotonically increasing timestamps.
class Transcoder {
public:
    using PacketSink = std::function<bool(int stream_index, AVPacket* packet)>;

    Transcoder();
    ~Transcoder();

    Transcoder(const Transcoder&) = delete;
    Transcoder& operator=(const Transcoder&) = delete;

    bool Configure(AVFormatContext* input, const std::vector<int>& input_streams,
                   AVFormatContext* output, const TranscodeSettings& settings,
                   std::string& err);
    bool Process(const AVPacket* packet, const PacketSink& sink, std::string& err);
    bool Flush(const PacketSink& sink, std::string& err);
    void Close();

    size_t OutputStreamCount() const { return states_.size(); }

private:
    struct State {
        int input_index = 0;
        int output_index = 0;
        AVStream* input_stream = nullptr;
        AVStream* output_stream = nullptr;
        AVCodecContext* decoder = nullptr;
        AVCodecContext* encoder = nullptr;
        SwsContext* sws = nullptr;
        SwrContext* swr = nullptr;
        AVFrame* decoded = nullptr;
        AVFrame* converted = nullptr;
        AVPacket* packet = nullptr;
        int64_t next_pts = 0;
        int64_t last_pts = 0;
        bool audio = false;
    };

    bool InitVideo(State& state, std::string& err);
    bool InitAudio(State& state, std::string& err);
    bool DrainDecoder(State& state, const PacketSink& sink, std::string& err);
    bool EncodeFrame(State& state, AVFrame* frame, const PacketSink& sink,
                     std::string& err);
    bool SendFrame(State& state, AVFrame* frame, const PacketSink& sink,
                   std::string& err);
    bool DrainEncoder(State& state, const PacketSink& sink, std::string& err);

    std::vector<State*> states_;
    std::unordered_map<int, State*> by_input_;
    AVFormatContext* output_ = nullptr;
    TranscodeSettings settings_;
};

}  // namespace mcast
