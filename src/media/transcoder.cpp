#include "media/transcoder.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace mcast {
namespace {

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

int SampleRateOr(AVCodecContext* decoder, int fallback) {
    return decoder && decoder->sample_rate > 0 ? decoder->sample_rate : fallback;
}

AVChannelLayout ChannelLayoutOr(const AVChannelLayout& layout, int channels) {
    AVChannelLayout result{};
    if (layout.nb_channels > 0) {
        av_channel_layout_copy(&result, &layout);
        return result;
    }
    av_channel_layout_default(&result, channels > 0 ? channels : 2);
    return result;
}

}  // namespace

Transcoder::Transcoder() = default;

Transcoder::~Transcoder() { Close(); }

bool Transcoder::Configure(AVFormatContext* input, const std::vector<int>& input_streams,
                           AVFormatContext* output, const TranscodeSettings& settings,
                           std::string& err) {
    Close();
    output_ = output;
    settings_ = settings;

    for (int input_index : input_streams) {
        if (input_index < 0 || input_index >= static_cast<int>(input->nb_streams)) continue;
        AVStream* stream = input->streams[input_index];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        const AVCodec* decoder_codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder_codec) {
            err = "no decoder for input stream #" + std::to_string(input_index) + " (" +
                  avcodec_get_name(stream->codecpar->codec_id) + ")";
            Close();
            return false;
        }

        auto* state = new State();
        state->input_index = input_index;
        state->input_stream = stream;
        state->output_index = static_cast<int>(states_.size());
        states_.push_back(state);
        by_input_[input_index] = state;

        state->decoder = avcodec_alloc_context3(decoder_codec);
        if (!state->decoder) {
            err = "cannot allocate decoder";
            Close();
            return false;
        }
        int ret = avcodec_parameters_to_context(state->decoder, stream->codecpar);
        if (ret < 0) {
            err = "cannot configure decoder: " + AvError(ret);
            Close();
            return false;
        }
        state->decoder->pkt_timebase = stream->time_base;
        ret = avcodec_open2(state->decoder, decoder_codec, nullptr);
        if (ret < 0) {
            err = "cannot open decoder: " + AvError(ret);
            Close();
            return false;
        }
        state->decoded = av_frame_alloc();
        state->packet = av_packet_alloc();
        if (!state->decoded || !state->packet) {
            err = "cannot allocate decoder frames";
            Close();
            return false;
        }

        bool ok = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
                      ? InitVideo(*state, err)
                      : InitAudio(*state, err);
        if (!ok) {
            Close();
            return false;
        }
    }

    if (states_.empty()) {
        err = "no audio/video streams to transcode";
        Close();
        return false;
    }
    return true;
}

bool Transcoder::InitVideo(State& state, std::string& err) {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        err = "H.264 encoder (libx264) is unavailable";
        return false;
    }

    AVRational fps = state.input_stream->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) fps = state.decoder->framerate;
    if (fps.num <= 0 || fps.den <= 0) fps = AVRational{25, 1};

    state.encoder = avcodec_alloc_context3(codec);
    if (!state.encoder) {
        err = "cannot allocate H.264 encoder";
        return false;
    }
    AVCodecContext* enc = state.encoder;
    enc->width = state.decoder->width;
    enc->height = state.decoder->height;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = AVRational{1, 90000};
    enc->framerate = fps;
    enc->bit_rate = settings_.video_bitrate;
    const int fps_integer = std::max(1, static_cast<int>(av_q2d(fps) + 0.5));
    enc->gop_size = settings_.gop > 0 ? settings_.gop : fps_integer * 2;
    enc->max_b_frames = 0;
    enc->thread_count = 0;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(enc->priv_data, "preset", settings_.preset.c_str(), 0);
    av_opt_set(enc->priv_data, "tune", "zerolatency", 0);

    int ret = avcodec_open2(enc, codec, nullptr);
    if (ret < 0) {
        err = "cannot open H.264 encoder: " + AvError(ret);
        return false;
    }

    AVStream* output = avformat_new_stream(output_, nullptr);
    if (!output) {
        err = "cannot create output video stream";
        return false;
    }
    output->time_base = enc->time_base;
    output->avg_frame_rate = fps;
    ret = avcodec_parameters_from_context(output->codecpar, enc);
    if (ret < 0) {
        err = "cannot copy H.264 parameters: " + AvError(ret);
        return false;
    }
    output->codecpar->codec_tag = 0;
    state.output_stream = output;

    state.sws = sws_getContext(state.decoder->width, state.decoder->height,
                               state.decoder->pix_fmt, enc->width, enc->height,
                               enc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!state.sws) {
        err = "cannot create video scaler";
        return false;
    }

    state.converted = av_frame_alloc();
    if (!state.converted) {
        err = "cannot allocate video frame";
        return false;
    }
    state.converted->format = enc->pix_fmt;
    state.converted->width = enc->width;
    state.converted->height = enc->height;
    ret = av_frame_get_buffer(state.converted, 32);
    if (ret < 0) {
        err = "cannot allocate video frame buffer: " + AvError(ret);
        return false;
    }
    state.audio = false;
    return true;
}

bool Transcoder::InitAudio(State& state, std::string& err) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        err = "AAC encoder is unavailable";
        return false;
    }

    const int sample_rate = SampleRateOr(state.decoder, 48000);
    const int input_rate = SampleRateOr(state.decoder, sample_rate);
    AVChannelLayout layout = ChannelLayoutOr(state.decoder->ch_layout, 2);

    state.encoder = avcodec_alloc_context3(codec);
    if (!state.encoder) {
        av_channel_layout_uninit(&layout);
        err = "cannot allocate AAC encoder";
        return false;
    }
    AVCodecContext* enc = state.encoder;
    enc->sample_rate = sample_rate;
    av_channel_layout_copy(&enc->ch_layout, &layout);
    av_channel_layout_uninit(&layout);
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc->bit_rate = settings_.audio_bitrate;
    enc->time_base = AVRational{1, sample_rate};
    enc->thread_count = 0;

    int ret = avcodec_open2(enc, codec, nullptr);
    if (ret < 0) {
        err = "cannot open AAC encoder: " + AvError(ret);
        return false;
    }

    AVStream* output = avformat_new_stream(output_, nullptr);
    if (!output) {
        err = "cannot create output audio stream";
        return false;
    }
    output->time_base = enc->time_base;
    ret = avcodec_parameters_from_context(output->codecpar, enc);
    if (ret < 0) {
        err = "cannot copy AAC parameters: " + AvError(ret);
        return false;
    }
    output->codecpar->codec_tag = 0;
    state.output_stream = output;

    AVChannelLayout input_layout = ChannelLayoutOr(state.decoder->ch_layout, 2);
    ret = swr_alloc_set_opts2(&state.swr, &enc->ch_layout, enc->sample_fmt,
                              enc->sample_rate, &input_layout,
                              state.decoder->sample_fmt, input_rate, 0, nullptr);
    av_channel_layout_uninit(&input_layout);
    if (ret < 0 || !state.swr) {
        err = "cannot create audio resampler: " + AvError(ret);
        return false;
    }
    ret = swr_init(state.swr);
    if (ret < 0) {
        err = "cannot initialize audio resampler: " + AvError(ret);
        return false;
    }
    state.audio = true;
    return true;
}

bool Transcoder::Process(const AVPacket* packet, const PacketSink& sink, std::string& err) {
    auto it = by_input_.find(packet->stream_index);
    if (it == by_input_.end()) return true;  // stream intentionally dropped
    State& state = *it->second;

    int ret = avcodec_send_packet(state.decoder, packet);
    if (ret == AVERROR(EAGAIN)) {
        if (!DrainDecoder(state, sink, err)) return false;
        ret = avcodec_send_packet(state.decoder, packet);
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        err = "decode failed: " + AvError(ret);
        return false;
    }
    return DrainDecoder(state, sink, err);
}

bool Transcoder::DrainDecoder(State& state, const PacketSink& sink, std::string& err) {
    while (true) {
        int ret = avcodec_receive_frame(state.decoder, state.decoded);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            err = "decoder receive failed: " + AvError(ret);
            return false;
        }
        if (!EncodeFrame(state, state.decoded, sink, err)) return false;
    }
}

bool Transcoder::EncodeFrame(State& state, AVFrame* frame, const PacketSink& sink,
                             std::string& err) {
    if (!state.audio) {
        const int64_t timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                      ? frame->best_effort_timestamp
                                      : frame->pts;
        int64_t out_pts = state.next_pts;
        if (timestamp != AV_NOPTS_VALUE) {
            out_pts = av_rescale_q(timestamp, state.decoder->pkt_timebase,
                                   state.encoder->time_base);
        }
        if (out_pts <= state.last_pts) out_pts = state.last_pts + 1;
        state.last_pts = out_pts;
        state.next_pts = out_pts + 1;

        if (av_frame_make_writable(state.converted) < 0) {
            err = "cannot make video frame writable";
            return false;
        }
        sws_scale(state.sws, frame->data, frame->linesize, 0, state.decoder->height,
                  state.converted->data, state.converted->linesize);
        state.converted->pts = out_pts;
        state.converted->duration = 1;
        return SendFrame(state, state.converted, sink, err);
    }

    const int max_samples = std::max(1, swr_get_out_samples(state.swr, frame->nb_samples));
    AVFrame* converted = av_frame_alloc();
    if (!converted) {
        err = "cannot allocate audio frame";
        return false;
    }
    converted->format = state.encoder->sample_fmt;
    converted->sample_rate = state.encoder->sample_rate;
    av_channel_layout_copy(&converted->ch_layout, &state.encoder->ch_layout);
    converted->nb_samples = max_samples;
    int ret = av_frame_get_buffer(converted, 0);
    if (ret < 0) {
        av_frame_free(&converted);
        err = "cannot allocate audio frame buffer: " + AvError(ret);
        return false;
    }

    const int samples = swr_convert(state.swr, converted->data, max_samples,
                                    (const uint8_t**)frame->extended_data,
                                    frame->nb_samples);
    if (samples < 0) {
        av_frame_free(&converted);
        err = "audio resample failed: " + AvError(samples);
        return false;
    }
    converted->nb_samples = samples;
    converted->duration = samples;

    const int64_t timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                  ? frame->best_effort_timestamp
                                  : frame->pts;
    int64_t out_pts = state.next_pts;
    if (timestamp != AV_NOPTS_VALUE) {
        out_pts = av_rescale_q(timestamp, state.decoder->pkt_timebase,
                               state.encoder->time_base);
    }
    if (out_pts < state.next_pts) out_pts = state.next_pts;
    converted->pts = out_pts;
    state.next_pts = out_pts + samples;

    const bool ok = SendFrame(state, converted, sink, err);
    av_frame_free(&converted);
    return ok;
}

bool Transcoder::SendFrame(State& state, AVFrame* frame, const PacketSink& sink,
                           std::string& err) {
    int ret = avcodec_send_frame(state.encoder, frame);
    if (ret == AVERROR(EAGAIN)) {
        if (!DrainEncoder(state, sink, err)) return false;
        ret = avcodec_send_frame(state.encoder, frame);
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        err = "encode failed: " + AvError(ret);
        return false;
    }
    return DrainEncoder(state, sink, err);
}

bool Transcoder::DrainEncoder(State& state, const PacketSink& sink, std::string& err) {
    while (true) {
        int ret = avcodec_receive_packet(state.encoder, state.packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            err = "encoder receive failed: " + AvError(ret);
            return false;
        }
        state.packet->stream_index = state.output_index;
        av_packet_rescale_ts(state.packet, state.encoder->time_base,
                             state.output_stream->time_base);
        if (!sink(state.output_index, state.packet)) {
            av_packet_unref(state.packet);
            err = "muxer write failed";
            return false;
        }
        av_packet_unref(state.packet);
    }
}

bool Transcoder::Flush(const PacketSink& sink, std::string& err) {
    for (State* state : states_) {
        int ret = avcodec_send_packet(state->decoder, nullptr);
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            err = "decoder flush failed: " + AvError(ret);
            return false;
        }
        if (!DrainDecoder(*state, sink, err)) return false;

        ret = avcodec_send_frame(state->encoder, nullptr);
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            err = "encoder flush failed: " + AvError(ret);
            return false;
        }
        if (!DrainEncoder(*state, sink, err)) return false;
    }
    return true;
}

void Transcoder::Close() {
    for (State* state : states_) {
        if (state->decoder) avcodec_free_context(&state->decoder);
        if (state->encoder) avcodec_free_context(&state->encoder);
        if (state->sws) sws_freeContext(state->sws);
        if (state->swr) swr_free(&state->swr);
        if (state->decoded) av_frame_free(&state->decoded);
        if (state->converted) av_frame_free(&state->converted);
        if (state->packet) av_packet_free(&state->packet);
        delete state;
    }
    states_.clear();
    by_input_.clear();
    output_ = nullptr;
}

}  // namespace mcast
