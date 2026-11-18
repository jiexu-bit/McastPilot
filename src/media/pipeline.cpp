#include "media/pipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

#include "common/log.h"

namespace mcast {
namespace {

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// Returns the offset of the first plausible MPEG-TS sync pattern
// (0x47 at +0, +188, +376), or n when the buffer is not recoverable.
size_t FindTsSyncOffset(const uint8_t* data, size_t n) {
    for (size_t k = 1; k + 188 * 2 < n; ++k) {
        if (data[k] == 0x47 && data[k + 188] == 0x47 && data[k + 376] == 0x47) {
            return k;
        }
    }
    return n;
}

const char* MediaTypeName(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return "video";
        case AVMEDIA_TYPE_AUDIO: return "audio";
        default: return "data";
    }
}

// The mpegts muxer logs "dts < pcr, TS is invalid" for B-frame streams copied
// as-is. The output still decodes cleanly, but the message would flood the log
// at ~40 lines/s, so filter exactly that line and keep everything else.
void FfmpegLogCallback(void* ptr, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level()) return;
    char line[1024];
    std::vsnprintf(line, sizeof(line), fmt, args);
    if (std::strstr(line, "dts < pcr, TS is invalid") != nullptr) return;
    if (level <= AV_LOG_WARNING) {
        std::fputs(line, stderr);
        std::fflush(stderr);
    }
}

void InstallFfmpegLogFilter() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(&FfmpegLogCallback);
    });
}

}  // namespace

Pipeline::Pipeline() = default;

Pipeline::~Pipeline() { Stop(); }

bool Pipeline::Start(const PipelineConfig& config, std::string& err) {
    if (running_.load(std::memory_order_relaxed)) {
        err = "pipeline is already running";
        return false;
    }
    if (config.input.empty()) {
        err = "input is empty";
        return false;
    }
    if (config.mcast.group.empty() || config.mcast.port == 0) {
        err = "multicast group/port is empty";
        return false;
    }
    if (config.pkt_size < 188 || config.pkt_size > 65507 || config.pkt_size % 188 != 0) {
        err = "packet size must be a multiple of 188 within [188, 65507]";
        return false;
    }

    config_ = config;
    if (config_.buffer_bytes < 64 * 1024) config_.buffer_bytes = 64 * 1024;
    if (config_.speed <= 0.0) config_.speed = 1.0;

    ring_ = std::make_unique<ByteRing>(config_.buffer_bytes, 188);
    if (!sender_.Open(config_.mcast, err)) {
        ring_.reset();
        return false;
    }

    frames_.store(0);
    muxed_bytes_.store(0);    rate_bps_.store(0);
    sample_bytes_.store(0);
    sample_us_.store(0);
    bitrate_mbps_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_.clear();
    }
    started_us_ = av_gettime_relative();
    running_.store(true, std::memory_order_relaxed);

    sender_thread_ = std::thread(&Pipeline::SenderLoop, this);
    reader_thread_ = std::thread(&Pipeline::ReaderLoop, this);
    return true;
}

void Pipeline::Stop() {
    running_.store(false, std::memory_order_relaxed);
    if (ring_) ring_->Close();
    if (reader_thread_.joinable()) reader_thread_.join();
    if (sender_thread_.joinable()) sender_thread_.join();
    sender_.Close();
    ring_.reset();
}

void Pipeline::SleepInterruptible(int64_t microseconds) {
    const int64_t deadline = av_gettime_relative() + microseconds;
    while (running_.load(std::memory_order_relaxed)) {
        const int64_t left = deadline - av_gettime_relative();
        if (left <= 0) break;
        std::this_thread::sleep_for(
            std::chrono::microseconds(std::min<int64_t>(left, 20000)));
    }
}

int Pipeline::WriteCallback(void* opaque, const uint8_t* buf, int size) {
    auto* self = static_cast<Pipeline*>(opaque);
    if (!self->ring_ || size <= 0) return 0;
    const size_t written = self->ring_->Write(buf, static_cast<size_t>(size));
    self->muxed_bytes_.fetch_add(written, std::memory_order_relaxed);
    // Always report success: loss is accounted for by the ring, and returning
    // an error here would abort the muxer instead of degrading gracefully.
    return size;
}

int Pipeline::InterruptCallback(void* opaque) {
    auto* self = static_cast<Pipeline*>(opaque);
    return self->running_.load(std::memory_order_relaxed) ? 0 : 1;
}

void Pipeline::SetError(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = message;
}

void Pipeline::ReaderLoop() {
    InstallFfmpegLogFilter();

    AVFormatContext* ic = nullptr;
    AVFormatContext* oc = nullptr;
    AVIOContext* avio = nullptr;
    AVDictionary* opts = nullptr;
    AVPacket* pkt = av_packet_alloc();
    std::vector<int> stream_map;
    std::string error;
    bool header_written = false;

    int64_t out_shift = 0;      // aligns the earliest PTS/DTS to zero
    int64_t loop_offset = 0;    // grows on every loop, keeps the timeline monotonic
    int64_t last_norm = 0;      // highest normalized DTS written so far
    int64_t wall_start = av_gettime_relative();
    int64_t muxrate = 0;
    int64_t last_write_error_log = 0;

    do {
        // ---------------------------------------------------------- input ---
        ic = avformat_alloc_context();
        if (!ic) {
            error = "avformat_alloc_context failed";
            break;
        }
        ic->interrupt_callback.callback = &Pipeline::InterruptCallback;
        ic->interrupt_callback.opaque = this;

        av_dict_set(&opts, "rw_timeout", "10000000", 0);  // 10 s for network I/O
        int ret = avformat_open_input(&ic, config_.input.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            error = "cannot open input '" + config_.input + "': " + AvError(ret);
            break;
        }

        ret = avformat_find_stream_info(ic, nullptr);
        if (ret < 0) {
            error = "cannot read stream info: " + AvError(ret);
            break;
        }

        LOG_INFO("input: %s", config_.input.c_str());
        for (unsigned i = 0; i < ic->nb_streams; ++i) {
            AVStream* st = ic->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                continue;
            }
            stream_map.push_back(static_cast<int>(i));
            const char* codec = avcodec_get_name(st->codecpar->codec_id);
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                LOG_INFO("  #%u %s: %s %dx%d @ %.3f fps, %lld bps", i,
                         MediaTypeName(st->codecpar->codec_type), codec,
                         st->codecpar->width, st->codecpar->height,
                         av_q2d(st->avg_frame_rate),
                         static_cast<long long>(st->codecpar->bit_rate));
            } else {
                LOG_INFO("  #%u %s: %s %d Hz, %d ch, %lld bps", i,
                         MediaTypeName(st->codecpar->codec_type), codec,
                         st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels,
                         static_cast<long long>(st->codecpar->bit_rate));
            }
        }
        if (stream_map.empty()) {
            error = "input contains no audio/video stream";
            break;
        }

        // --------------------------------------------------------- output ---
        ret = avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr);
        if (ret < 0 || !oc) {
            error = "mpegts muxer is unavailable: " + AvError(ret);
            break;
        }

        bool setup_ok = true;
        for (size_t oi = 0; oi < stream_map.size(); ++oi) {
            AVStream* in = ic->streams[stream_map[oi]];
            AVStream* out = avformat_new_stream(oc, nullptr);
            if (!out) {
                setup_ok = false;
                error = "avformat_new_stream failed";
                break;
            }
            if (avcodec_parameters_copy(out->codecpar, in->codecpar) < 0) {
                setup_ok = false;
                error = "avcodec_parameters_copy failed";
                break;
            }
            out->codecpar->codec_tag = 0;
            out->time_base = in->time_base;
            out->avg_frame_rate = in->avg_frame_rate;
            out->sample_aspect_ratio = in->sample_aspect_ratio;
        }
        if (!setup_ok) break;

        // B-frame reordering means the first DTS is larger than the first PTS.
        // Align on the earliest PTS/DTS so the muxer never sees negative
        // timestamps ("dts < pcr, TS is invalid").
        int64_t start_us = ic->start_time;
        const bool seekable = ic->pb && (ic->pb->seekable & AVIO_SEEKABLE_NORMAL);
        if (seekable) {
            int64_t min_us = AV_NOPTS_VALUE;
            AVPacket* peek = av_packet_alloc();
            int seen = 0;
            const int want = stream_map.size() >= 31
                                 ? -1
                                 : static_cast<int>((1u << stream_map.size()) - 1u);
            for (int i = 0; i < 600 && peek && seen != want; ++i) {
                if (av_read_frame(ic, peek) < 0) break;
                int out_idx = -1;
                for (size_t k = 0; k < stream_map.size(); ++k) {
                    if (stream_map[k] == peek->stream_index) {
                        out_idx = static_cast<int>(k);
                        break;
                    }
                }
                if (out_idx >= 0) {
                    seen |= 1 << out_idx;
                    const AVRational tb = ic->streams[peek->stream_index]->time_base;
                    for (int64_t ts : {peek->dts, peek->pts}) {
                        if (ts == AV_NOPTS_VALUE) continue;
                        const int64_t us = av_rescale_q(ts, tb, AV_TIME_BASE_Q);
                        if (min_us == AV_NOPTS_VALUE || us < min_us) min_us = us;
                    }
                }
                av_packet_unref(peek);
            }
            if (peek) av_packet_free(&peek);
            if (min_us != AV_NOPTS_VALUE) start_us = min_us;
            if (av_seek_frame(ic, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                LOG_WARN("cannot rewind input after timestamp probing");
            }
            avformat_flush(ic);
        }

        muxrate = config_.muxrate;
        if (muxrate <= 0) {
            int64_t guess = ic->bit_rate;
            if (guess <= 0) {
                for (int index : stream_map) guess += ic->streams[index]->codecpar->bit_rate;
            }
            if (guess <= 0) guess = 8000000;
            muxrate = static_cast<int64_t>(static_cast<double>(guess) * 1.15);
            if (muxrate < 1000000) muxrate = 1000000;
        }
        rate_bps_.store(muxrate, std::memory_order_relaxed);

        LOG_INFO("output: mpegts CBR %.3f Mbps -> %s:%u, pkt=%d B, ring=%llu KB",
                 static_cast<double>(muxrate) / 1e6, config_.mcast.group.c_str(),
                 static_cast<unsigned>(config_.mcast.port), config_.pkt_size,
                 static_cast<unsigned long long>(config_.buffer_bytes / 1024));

        constexpr int kIoBufferSize = 32768;
        auto* io_buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
        if (!io_buffer) {
            error = "av_malloc failed";
            break;
        }
        avio = avio_alloc_context(io_buffer, kIoBufferSize, 1, this, nullptr,
                                  &Pipeline::WriteCallback, nullptr);
        if (!avio) {
            av_free(io_buffer);
            error = "avio_alloc_context failed";
            break;
        }
        avio->seekable = 0;
        oc->pb = avio;
        oc->flags |= AVFMT_FLAG_FLUSH_PACKETS;

        av_dict_set_int(&opts, "muxrate", muxrate, 0);
        av_dict_set(&opts, "pcr_period", "20", 0);
        av_dict_set(&opts, "pat_period", "0.2", 0);
        ret = avformat_write_header(oc, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            error = "avformat_write_header failed: " + AvError(ret);
            break;
        }
        header_written = true;

        if (start_us != AV_NOPTS_VALUE && oc->nb_streams > 0) {
            out_shift = -av_rescale_q(start_us, AV_TIME_BASE_Q, oc->streams[0]->time_base);
            LOG_INFO("timestamp alignment: %lld ticks (start %.3f s)",
                     static_cast<long long>(out_shift),
                     static_cast<double>(start_us) / AV_TIME_BASE);
        }

        // ------------------------------------------------------- read loop ---
        while (running_.load(std::memory_order_relaxed)) {
            ret = av_read_frame(ic, pkt);
            if (ret < 0) {
                if (!running_.load(std::memory_order_relaxed)) break;

                if (config_.loop) {
                    if (av_seek_frame(ic, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                        error = "loop seek failed: " + AvError(ret);
                        break;
                    }
                    avformat_flush(ic);
                    loop_offset = last_norm + 1;
                    wall_start = av_gettime_relative();
                    av_packet_unref(pkt);
                    continue;
                }

                if (ret == AVERROR_EOF) {
                    LOG_INFO("end of input");
                } else {
                    error = "read frame failed: " + AvError(ret);
                }
                break;
            }

            int out_index = -1;
            for (size_t k = 0; k < stream_map.size(); ++k) {
                if (stream_map[k] == pkt->stream_index) {
                    out_index = static_cast<int>(k);
                    break;
                }
            }
            if (out_index < 0) {
                av_packet_unref(pkt);
                continue;
            }

            AVStream* ist = ic->streams[pkt->stream_index];
            AVStream* ost = oc->streams[out_index];

            av_packet_rescale_ts(pkt, ist->time_base, ost->time_base);
            const int64_t shift = out_shift + loop_offset;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += shift;
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += shift;
            pkt->stream_index = out_index;
            pkt->pos = -1;

            const int64_t norm_dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            if (norm_dts != AV_NOPTS_VALUE) {
                last_norm = std::max(last_norm, norm_dts);

                if (config_.realtime) {
                    const int64_t relative = norm_dts - loop_offset;
                    const double seconds =
                        static_cast<double>(relative) * av_q2d(ost->time_base) / config_.speed;
                    const int64_t target_us = wall_start + static_cast<int64_t>(seconds * 1e6);
                    const int64_t now_us = av_gettime_relative();
                    if (target_us > now_us) {
                        SleepInterruptible(target_us - now_us);
                    } else if (now_us - target_us > 10000000) {
                        // Timestamps jumped; resync instead of bursting.
                        wall_start = now_us - static_cast<int64_t>(seconds * 1e6);
                    }
                }
            }

            ret = av_interleaved_write_frame(oc, pkt);
            if (ret < 0) {
                const int64_t now = av_gettime_relative();
                if (now - last_write_error_log > 2000000) {
                    LOG_WARN("write frame failed: %s", AvError(ret).c_str());
                    last_write_error_log = now;
                }
            }
            frames_.fetch_add(1, std::memory_order_relaxed);
            av_packet_unref(pkt);

            if (config_.duration_ms > 0 &&
                (av_gettime_relative() - started_us_) / 1000 >= config_.duration_ms) {
                LOG_INFO("duration reached, stopping");
                break;
            }
        }

        LOG_INFO("stream loop finished");
    } while (false);

    if (header_written && oc) {
        av_write_trailer(oc);
    }
    if (pkt) av_packet_free(&pkt);
    if (oc) avformat_free_context(oc);
    if (avio) avio_context_free(&avio);
    if (ic) avformat_close_input(&ic);

    if (!error.empty()) {
        SetError(error);
        LOG_ERROR("%s", error.c_str());
    }
    running_.store(false, std::memory_order_relaxed);
    if (ring_) ring_->Close();
}

void Pipeline::SenderLoop() {
    if (!ring_) return;

    const int pkt_size = std::clamp(config_.pkt_size, 188, 65507);
    std::vector<uint8_t> buffer(static_cast<size_t>(pkt_size));

    // Credit is refilled from the *measured* elapsed time. Windows' default
    // 15.6 ms timer resolution makes per-packet sleeps wildly inaccurate, so a
    // token bucket that assumes the requested sleep duration would run at a
    // fraction of the configured bitrate.
    double budget = 0.0;
    auto last = std::chrono::steady_clock::now();
    bool logged_rate = false;
    uint64_t unsync = 0;

    while (running_.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - last).count();
        last = now;

        const double bytes_per_second =
            std::max<double>(static_cast<double>(rate_bps_.load(std::memory_order_relaxed)) / 8.0,
                             1000.0);
        if (!logged_rate && bytes_per_second > 100000.0) {
            LOG_INFO("sender pacing at %.3f Mbps (%d B/pkt)", bytes_per_second * 8.0 / 1e6,
                     pkt_size);
            logged_rate = true;
        }

        const double burst_bytes = bytes_per_second * 0.005;  // 5 ms of slack
        budget = std::min(burst_bytes, budget + elapsed * bytes_per_second);

        int sent = 0;
        while (budget >= static_cast<double>(pkt_size) && sent < 256) {
            const size_t n = ring_->Read(buffer.data(), buffer.size(), 0);
            if (n == 0) break;

            size_t offset = 0;
            if (buffer[0] != 0x47) {
                offset = FindTsSyncOffset(buffer.data(), n);
                if (offset >= n) {
                    ++unsync;
                    budget -= static_cast<double>(n);
                    break;  // garbage chunk, drop it
                }
            }

            const size_t len = n - offset;
            budget -= static_cast<double>(len);
            sender_.Send(buffer.data() + offset, len);
            ++sent;
        }

        if (sent == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (unsync > 0) {
        LOG_WARN("sender skipped %llu non-TS chunks", static_cast<unsigned long long>(unsync));
    }
}

PipelineStats Pipeline::Snapshot() const {
    PipelineStats stats;
    stats.running = running_.load(std::memory_order_relaxed);

    const net::SenderStats sender = sender_.Stats();
    stats.muxed_bytes = muxed_bytes_.load(std::memory_order_relaxed);
    stats.muxrate_bps = rate_bps_.load(std::memory_order_relaxed);
    stats.output_bytes = sender.bytes;
    stats.packets_sent = sender.packets;
    stats.send_errors = sender.errors;
    stats.packets_dropped = sender.drops;
    stats.buffer_drop_bytes = ring_ ? ring_->DroppedBytes() : 0;
    stats.frames = frames_.load(std::memory_order_relaxed);

    if (ring_) {
        stats.buffer_bytes = ring_->Size();
        stats.buffer_capacity = ring_->Capacity();
    }

    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        stats.last_error = last_error_;
    }

    const int64_t now = av_gettime_relative();
    stats.elapsed_sec = started_us_ > 0 ? static_cast<double>(now - started_us_) / 1e6 : 0.0;

    const int64_t previous_us = sample_us_.exchange(now);
    const uint64_t previous_bytes = sample_bytes_.exchange(sender.bytes);
    if (previous_us > 0 && now > previous_us && sender.bytes >= previous_bytes) {
        const double seconds = static_cast<double>(now - previous_us) / 1e6;
        const double mbps =
            static_cast<double>(sender.bytes - previous_bytes) * 8.0 / seconds / 1e6;
        bitrate_mbps_.store(mbps, std::memory_order_relaxed);
    }
    stats.bitrate_mbps = bitrate_mbps_.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace mcast
