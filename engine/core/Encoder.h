// LegacyAVR - FFmpeg libavcodec encoder wrapper (AC-3 / E-AC-3 / DTS core).
//
// Real-time notes
//  * Every AVFrame / AVPacket / sample buffer is allocated in open().
//  * Packet payloads come from an AVBufferPool through a custom get_encode_buffer
//    callback (the ac3/eac3/dca encoders advertise AV_CODEC_CAP_DR1), so steady
//    state never allocates audio-sized buffers. libavcodec still creates a small
//    AVBufferRef descriptor per packet; this is why encoding runs on the DSP thread,
//    decoupled by a ring from the device-deadline render thread (ARCHITECTURE.md §5).
//  * thread_count = 1: frame threading would add whole frames of latency.
//  * SIMD: libavcodec dispatches its x86 kernels (SSE/AVX/AVX2 MDCT, float->fixed,
//    exponent/bit-allocation helpers) at runtime via av_get_cpu_flags().
#pragma once

#include "Iec61937.h"
#include "Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferPool;

namespace avr {

class AudioEncoder {
public:
    using PacketFn = void (*)(void* user, const uint8_t* data, size_t bytes);

    AudioEncoder() = default;
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    // channels: 2 (stereo), 5 (5.0, no LFE - v1.2.0) or 6 (5.1). Not real-time.
    bool open(Codec codec, uint32_t bitrate, int channels, std::string& error);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return ctx_ != nullptr; }

    // Encodes exactly kBlockFrames (1536) frames of planar float audio
    // (`channels()` planes). DTS is fed as three 512-sample codec frames.
    // Calls fn once per produced packet.
    bool encodeBlock(const float* const* planes, PacketFn fn, void* user) noexcept;

    [[nodiscard]] Codec codec() const noexcept { return codec_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] uint32_t bitrate() const noexcept { return bitrate_; }
    [[nodiscard]] size_t codecFrameSamples() const noexcept { return frameSamples_; }
    [[nodiscard]] IecType iecType() const noexcept;
    [[nodiscard]] int delaySamples() const noexcept { return delay_; }   // AC-3: 256 (MDCT overlap)
    [[nodiscard]] uint64_t packetsOut() const noexcept { return packets_; }

    // Maps libavcodec log output to a callback (installed once per process).
    using LogFn = void (*)(int level, const char* message);
    static void installLogHandler(LogFn fn) noexcept;

private:
    static int getEncodeBuffer(AVCodecContext* ctx, AVPacket* pkt, int flags);

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVBufferPool* pool_ = nullptr;
    size_t poolSize_ = 0;
    Codec codec_ = Codec::AC3;
    int channels_ = 0;
    std::array<uint8_t, 6> planeMap_{0, 1, 2, 3, 4, 5};   // encoder channel -> bus plane
    uint32_t bitrate_ = 0;
    size_t frameSamples_ = 0;
    int delay_ = 0;
    int64_t pts_ = 0;
    uint64_t packets_ = 0;
};

// Snaps a requested bitrate to one the codec / transport actually supports.
uint32_t normaliseBitrate(Codec codec, uint32_t requested, int channels) noexcept;

} // namespace avr
