// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef MINI_AUDIO_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace mini {
struct AudioDecoder::Impl {
    unsigned ct = 0;
#ifdef MINI_AUDIO_FFMPEG
    AVCodecContext* context = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    ~Impl() { avcodec_free_context(&context); av_packet_free(&packet); av_frame_free(&frame); }
#endif
};
AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;
#ifdef MINI_AUDIO_FFMPEG
namespace {
AVCodecID codec_id(unsigned ct) { return ct == 2 ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC; }
void check(int result, const char* action) {
    if (result >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result, message, sizeof(message));
    throw std::runtime_error(std::string(action) + ": " + message);
}
template<typename T> T sample(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(v)); return v; }
int16_t to_s16(const uint8_t* p, AVSampleFormat format) {
    switch (format) {
    case AV_SAMPLE_FMT_S16: return sample<int16_t>(p);
    case AV_SAMPLE_FMT_S32: return int16_t(sample<int32_t>(p) >> 16);
    case AV_SAMPLE_FMT_U8: return int16_t((int(*p) - 128) * 256);
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_DBL: {
        const double value = format == AV_SAMPLE_FMT_FLT ? sample<float>(p) : sample<double>(p);
        if (!std::isfinite(value)) return 0;
        return int16_t(std::lround(std::clamp(value, -1.0, 32767.0 / 32768.0) * 32768.0));
    }
    default: throw std::runtime_error("unsupported decoded audio sample format");
    }
}
}
#endif
bool AudioDecoder::available(unsigned ct) {
#ifdef MINI_AUDIO_FFMPEG
    return (ct == 2 || ct == 4 || ct == 8) && avcodec_find_decoder(codec_id(ct));
#else
    (void)ct; return false;
#endif
}
void AudioDecoder::configure(unsigned ct, unsigned spf) {
    impl_->ct = 0;
#ifdef MINI_AUDIO_FFMPEG
    avcodec_free_context(&impl_->context);
    if (!available(ct)) throw std::runtime_error("FFmpeg is missing the requested AAC/ALAC decoder");
    if (!impl_->packet || !impl_->frame) throw std::bad_alloc();
    auto* ctx = avcodec_alloc_context3(avcodec_find_decoder(codec_id(ct)));
    if (!ctx) throw std::bad_alloc();
    impl_->context = ctx;
    ctx->sample_rate = 44100;
    av_channel_layout_default(&ctx->ch_layout, 2);
    ctx->thread_count = 1;
    std::vector<uint8_t> extra;
    if (ct == 2) {
        if (!spf) spf = 352;
        // ALAC atom + ALACSpecificConfig, as used by upstream UxPlay.
        extra = {0,0,0,36, 'a','l','a','c', 0,0,0,0, 0,0,0,0,
                 0,16,40,10, 14,2,0,255, 0,0,0,0, 0,0,0,0, 0,0,172,68};
        for (int i = 0; i < 4; ++i) extra[12 + i] = uint8_t(spf >> (24 - 8 * i));
    } else if (ct == 8) {
        if (!spf) spf = 480;
        if (spf != 480 && spf != 512) throw std::runtime_error("AAC-ELD requires 480 or 512 samples/frame");
        // AudioSpecificConfig: escaped AOT 39, 44100 Hz, stereo, ELD frameLengthFlag.
        extra = {0xf8, 0xe8, uint8_t(spf == 480 ? 0x50 : 0x40), 0x00};
    } else {
        if (spf && spf != 1024) throw std::runtime_error("AAC-LC requires 1024 samples/frame");
        extra = {0x12, 0x10};
    }
    ctx->extradata = static_cast<uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!ctx->extradata) throw std::bad_alloc();
    ctx->extradata_size = int(extra.size());
    std::memcpy(ctx->extradata, extra.data(), extra.size());
    check(avcodec_open2(ctx, ctx->codec, nullptr), "open audio decoder");
    impl_->ct = ct;
#else
    (void)ct; (void)spf;
    throw std::runtime_error("audio support was disabled at build time");
#endif
}
void AudioDecoder::flush() {
#ifdef MINI_AUDIO_FFMPEG
    if (impl_->context) avcodec_flush_buffers(impl_->context);
#endif
}
std::vector<int16_t> AudioDecoder::decode(const uint8_t* data, size_t size) {
    if (!impl_->ct) throw std::runtime_error("audio decoder is not configured");
    if (!size) return {};
    if (!data || size > size_t(std::numeric_limits<int>::max())) throw std::invalid_argument("invalid audio packet");
    std::vector<int16_t> output;
#ifdef MINI_AUDIO_FFMPEG
    auto* packet = impl_->packet;
    av_packet_unref(packet);
    check(av_new_packet(packet, int(size)), "allocate audio packet");
    std::memcpy(packet->data, data, size); // FFmpeg requires zero-padded, owned input.
    const int sent = avcodec_send_packet(impl_->context, packet);
    av_packet_unref(packet);
    check(sent, "submit audio packet");
    for (;;) {
        auto* frame = impl_->frame;
        const int result = avcodec_receive_frame(impl_->context, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        check(result, "decode audio");
        if (frame->sample_rate != 44100 || frame->ch_layout.nb_channels != 2)
            throw std::runtime_error("AirPlay audio must be 44100 Hz stereo");
        const auto format = AVSampleFormat(frame->format);
        const bool planar = av_sample_fmt_is_planar(format);
        const auto packed = av_get_packed_sample_fmt(format);
        const int bytes = av_get_bytes_per_sample(format);
        if (!bytes) throw std::runtime_error("invalid decoded sample format");
        const size_t offset = output.size();
        output.resize(offset + size_t(frame->nb_samples) * 2);
        for (int i = 0; i < frame->nb_samples; ++i) for (int ch = 0; ch < 2; ++ch) {
            const auto* p = frame->extended_data[planar ? ch : 0] + size_t(planar ? i : i * 2 + ch) * bytes;
            output[offset + size_t(i) * 2 + ch] = to_s16(p, packed);
        }
        av_frame_unref(frame);
    }
#endif
    return output;
}
}
