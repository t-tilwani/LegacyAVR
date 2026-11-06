#include "Resampler.h"

#include "Types.h"

#include <cmath>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace avr {

namespace {
constexpr int64_t kCompensationDistance = 1'000'000;   // delta is expressed directly in ppm
}

Resampler::~Resampler() { close(); }

void Resampler::close() noexcept {
    if (swr_) swr_free(&swr_);
    lastDelta_ = 0;
}

bool Resampler::open(uint32_t inRate, int channels, size_t maxInFrames, std::string& error) {
    close();
    inRate_ = inRate;
    channels_ = channels;

    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    int ret = swr_alloc_set_opts2(&swr_, &layout, AV_SAMPLE_FMT_FLTP, int(kBusRate),
                                  &layout, AV_SAMPLE_FMT_FLTP, int(inRate), 0, nullptr);
    av_channel_layout_uninit(&layout);
    if (ret < 0 || !swr_) { error = "swr_alloc_set_opts2 failed"; return false; }

    av_opt_set_int(swr_, "flags", SWR_FLAG_RESAMPLE, 0);   // keep resampler engaged for compensation
    av_opt_set_int(swr_, "filter_size", 32, 0);
    av_opt_set_int(swr_, "phase_shift", 10, 0);
    av_opt_set_int(swr_, "linear_interp", 1, 0);
    av_opt_set_double(swr_, "cutoff", 0.97, 0);

    if ((ret = swr_init(swr_)) < 0) { error = "swr_init failed"; close(); return false; }

    // Warm-up: run the largest chunk once so swr sizes its internal buffers now,
    // not on the first real-time call. The output (silence) is discarded.
    const int maxIn = int(maxInFrames);
    const int maxOut = maxOutput(maxIn);
    std::vector<std::vector<float>> zin(size_t(channels), std::vector<float>(size_t(maxIn), 0.0f));
    std::vector<std::vector<float>> zout(size_t(channels), std::vector<float>(size_t(maxOut), 0.0f));
    std::vector<const uint8_t*> pin(static_cast<size_t>(channels));
    std::vector<uint8_t*> pout(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        pin[size_t(c)] = reinterpret_cast<const uint8_t*>(zin[size_t(c)].data());
        pout[size_t(c)] = reinterpret_cast<uint8_t*>(zout[size_t(c)].data());
    }
    for (int i = 0; i < 2; ++i)
        swr_convert(swr_, pout.data(), maxOut, pin.data(), maxIn);
    swr_set_compensation(swr_, 0, int(kCompensationDistance));
    return true;
}

int Resampler::maxOutput(int inFrames) const noexcept {
    // Output for inFrames plus anything buffered, plus +-0.1% compensation headroom.
    const double ratio = double(kBusRate) / double(inRate_ ? inRate_ : kBusRate);
    return int(std::ceil(inFrames * ratio * 1.002)) + 64;
}

int Resampler::process(const float* const* in, int inFrames, float* const* out, int outCapacity) noexcept {
    if (!swr_) return -1;
    return swr_convert(swr_, reinterpret_cast<uint8_t* const*>(out), outCapacity,
                       reinterpret_cast<const uint8_t* const*>(in), inFrames);
}

void Resampler::setCompensationPpm(double ppm) noexcept {
    if (!swr_) return;
    const long delta = std::lround(ppm);
    if (delta == lastDelta_) return;
    lastDelta_ = delta;
    swr_set_compensation(swr_, int(delta), int(kCompensationDistance));
}

double Resampler::delayMs() const noexcept {
    return swr_ ? double(swr_get_delay(swr_, 1000)) : 0.0;
}

} // namespace avr
