#include "Encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

namespace avr {

namespace {
constexpr size_t kMaxPacketBytes = 32768;   // > largest DTS (16 KiB) / E-AC-3 frame

std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

AudioEncoder::LogFn g_logFn = nullptr;

void avLogTrampoline(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > av_log_get_level() || !g_logFn) return;
    char line[1024];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    g_logFn(level, line);
}
} // namespace

void AudioEncoder::installLogHandler(LogFn fn) noexcept {
    g_logFn = fn;
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(fn ? &avLogTrampoline : &av_log_default_callback);
}

uint32_t normaliseBitrate(Codec codec, uint32_t requested, int channels) noexcept {
    switch (codec) {
        case Codec::AC3: {
            static constexpr std::array<uint32_t, 19> rates{
                32000, 40000, 48000, 56000, 64000, 80000, 96000, 112000, 128000, 160000,
                192000, 224000, 256000, 320000, 384000, 448000, 512000, 576000, 640000};
            const uint32_t floor = channels >= 5 ? 384000u : 96000u;   // sensible quality floor
            uint32_t best = rates.back();
            for (uint32_t r : rates) if (r <= requested) best = r;
            return std::max(best, floor);
        }
        case Codec::EAC3:
            // 11-bit frmsiz => max 4096 bytes per 1536-sample frame => 1.024 Mbps @ 48 kHz.
            return std::clamp<uint32_t>(requested, 192000u, 1024000u);
        case Codec::DTS: {
            // DTS Interactive / DTS Connect rates. 1509.75k fits a type-I burst with
            // preamble (2016-byte frames); 754.5k is the "half rate" mode.
            if (requested == 0) return 1509750;
            return requested >= 1132125 ? 1509750u : 754500u;
        }
        case Codec::PCM: return 0;
    }
    return requested;
}

AudioEncoder::~AudioEncoder() { close(); }

void AudioEncoder::close() noexcept {
    if (ctx_) avcodec_free_context(&ctx_);
    if (frame_) av_frame_free(&frame_);
    if (pkt_) av_packet_free(&pkt_);
    if (pool_) av_buffer_pool_uninit(&pool_);
    pts_ = 0;
    packets_ = 0;
}

IecType AudioEncoder::iecType() const noexcept {
    switch (codec_) {
        case Codec::AC3: return IecType::AC3;
        case Codec::EAC3: return IecType::EAC3;
        case Codec::DTS:
            return frameSamples_ == 512 ? IecType::DTS1 : frameSamples_ == 1024 ? IecType::DTS2 : IecType::DTS3;
        default: return IecType::None;
    }
}

int AudioEncoder::getEncodeBuffer(AVCodecContext* ctx, AVPacket* pkt, int flags) {
    auto* self = static_cast<AudioEncoder*>(ctx->opaque);
    const size_t need = size_t(pkt->size) + AV_INPUT_BUFFER_PADDING_SIZE;
    if (!self || !self->pool_ || need > self->poolSize_)
        return avcodec_default_get_encode_buffer(ctx, pkt, flags);
    AVBufferRef* buf = av_buffer_pool_get(self->pool_);
    if (!buf) return AVERROR(ENOMEM);
    std::memset(buf->data + pkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt->buf = buf;
    pkt->data = buf->data;
    return 0;
}

bool AudioEncoder::open(Codec codec, uint32_t bitrate, int channels, std::string& error) {
    close();
    if (codec == Codec::PCM) { error = "PCM does not use an encoder"; return false; }
    if (channels != 2 && channels != 5 && channels != 6) { error = "encoder supports 2, 5 or 6 channels"; return false; }

    const char* name = codec == Codec::AC3 ? "ac3" : codec == Codec::EAC3 ? "eac3" : "dca";
    const AVCodec* c = avcodec_find_encoder_by_name(name);
    if (!c) { error = std::string("FFmpeg build has no '") + name + "' encoder"; return false; }

    ctx_ = avcodec_alloc_context3(c);
    if (!ctx_) { error = "avcodec_alloc_context3 failed"; return false; }

    codec_ = codec;
    channels_ = channels;
    bitrate_ = normaliseBitrate(codec, bitrate, channels);
    // Bus plane feeding each encoder channel (FFmpeg native order). 5.0 (v1.2.0, used when
    // there is no subwoofer) skips the LFE plane: FL FR FC SL SR.
    static constexpr std::array<uint8_t, 6> kMap51{FL, FR, FC, LFE, SL, SR};
    static constexpr std::array<uint8_t, 6> kMap50{FL, FR, FC, SL, SR, 0};
    planeMap_ = channels == 5 ? kMap50 : kMap51;

    ctx_->sample_rate = int(kBusRate);
    av_channel_layout_uninit(&ctx_->ch_layout);
    av_channel_layout_from_mask(&ctx_->ch_layout, channels == 6   ? AV_CH_LAYOUT_5POINT1
                                                  : channels == 5 ? AV_CH_LAYOUT_5POINT0
                                                                  : AV_CH_LAYOUT_STEREO);
    ctx_->sample_fmt = codec == Codec::DTS ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_FLTP;
    ctx_->bit_rate = bitrate_;
    ctx_->thread_count = 1;
    ctx_->opaque = this;
    ctx_->get_encode_buffer = &AudioEncoder::getEncodeBuffer;
    if (codec == Codec::DTS) ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (codec == Codec::AC3 || codec == Codec::EAC3) {
        // -31 dialnorm == "no attenuation": the AVR plays at the level we send.
        av_opt_set_int(ctx_->priv_data, "dialnorm", -31, 0);
        av_opt_set_int(ctx_->priv_data, "dsur_mode", 0, 0);
    }

    int ret = avcodec_open2(ctx_, c, nullptr);
    if (ret < 0) { error = std::string("avcodec_open2(") + name + "): " + avErr(ret); close(); return false; }

    frameSamples_ = size_t(ctx_->frame_size);
    if (frameSamples_ == 0 || kBlockFrames % frameSamples_ != 0) {
        error = "unexpected encoder frame size " + std::to_string(frameSamples_);
        close();
        return false;
    }
    delay_ = ctx_->initial_padding;

    poolSize_ = kMaxPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE;
    pool_ = av_buffer_pool_init(poolSize_, nullptr);
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!pool_ || !frame_ || !pkt_) { error = "out of memory"; close(); return false; }

    frame_->nb_samples = int(frameSamples_);
    frame_->format = ctx_->sample_fmt;
    frame_->sample_rate = ctx_->sample_rate;
    av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout);
    ret = av_frame_get_buffer(frame_, 0);
    if (ret < 0) { error = "av_frame_get_buffer: " + avErr(ret); close(); return false; }

    // Warm the packet pool so the steady state recycles instead of allocating.
    std::array<AVBufferRef*, 4> warm{};
    for (auto& b : warm) b = av_buffer_pool_get(pool_);
    for (auto& b : warm) av_buffer_unref(&b);
    return true;
}

bool AudioEncoder::encodeBlock(const float* const* planes, PacketFn fn, void* user) noexcept {
    if (!ctx_) return false;
    const size_t sub = kBlockFrames / frameSamples_;
    for (size_t k = 0; k < sub; ++k) {
        if (av_frame_make_writable(frame_) < 0) return false;
        const size_t off = k * frameSamples_;
        if (codec_ == Codec::DTS) {
            auto* dst = reinterpret_cast<int32_t*>(frame_->data[0]);
            for (size_t i = 0; i < frameSamples_; ++i) {
                for (int c = 0; c < channels_; ++c) {
                    const double v = std::clamp(double(planes[planeMap_[size_t(c)]][off + i]), -1.0, 0.99999999);
                    dst[i * size_t(channels_) + size_t(c)] = static_cast<int32_t>(v * 2147483648.0);
                }
            }
        } else {
            for (int c = 0; c < channels_; ++c)
                std::memcpy(frame_->extended_data[c], planes[planeMap_[size_t(c)]] + off, frameSamples_ * sizeof(float));
        }
        frame_->pts = pts_;
        pts_ += int64_t(frameSamples_);

        int ret = avcodec_send_frame(ctx_, frame_);
        if (ret < 0) return false;
        while ((ret = avcodec_receive_packet(ctx_, pkt_)) == 0) {
            fn(user, pkt_->data, size_t(pkt_->size));
            ++packets_;
            av_packet_unref(pkt_);
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return false;
    }
    return true;
}

} // namespace avr
