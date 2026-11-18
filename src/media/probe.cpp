#include "media/probe.h"

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace mcast {
namespace {

void EnsureNetworkInit() {
    static std::once_flag once;
    std::call_once(once, [] { avformat_network_init(); });
}

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

const char* StreamTypeName(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return "video";
        case AVMEDIA_TYPE_AUDIO: return "audio";
        default: return "other";
    }
}

}  // namespace

bool ProbeInput(const std::string& input, ProbeResult& result, std::string& err) {
    result = ProbeResult{};
    if (input.empty()) {
        err = "input is empty";
        return false;
    }

    EnsureNetworkInit();

    AVFormatContext* ic = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rw_timeout", "10000000", 0);
    int ret = avformat_open_input(&ic, input.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
        err = "cannot open input '" + input + "': " + AvError(ret);
        return false;
    }

    ret = avformat_find_stream_info(ic, nullptr);
    if (ret < 0) {
        err = "cannot read stream info: " + AvError(ret);
        avformat_close_input(&ic);
        return false;
    }

    result.format = ic->iformat && ic->iformat->name ? ic->iformat->name : "";
    result.duration_sec =
        ic->duration > 0 ? static_cast<double>(ic->duration) / AV_TIME_BASE : 0.0;
    result.bitrate = ic->bit_rate;

    int64_t stream_bitrate_sum = 0;
    for (unsigned i = 0; i < ic->nb_streams; ++i) {
        const AVStream* st = ic->streams[i];
        ProbeStream item;
        item.index = static_cast<int>(i);
        item.type = StreamTypeName(st->codecpar->codec_type);
        item.codec = avcodec_get_name(st->codecpar->codec_id);
        item.bitrate = st->codecpar->bit_rate;
        stream_bitrate_sum += item.bitrate;

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            item.width = st->codecpar->width;
            item.height = st->codecpar->height;
            item.fps = av_q2d(st->avg_frame_rate);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            item.sample_rate = st->codecpar->sample_rate;
            item.channels = st->codecpar->ch_layout.nb_channels;
        }
        result.streams.push_back(std::move(item));
    }
    if (result.bitrate <= 0) result.bitrate = stream_bitrate_sum;

    avformat_close_input(&ic);
    return true;
}

}  // namespace mcast
