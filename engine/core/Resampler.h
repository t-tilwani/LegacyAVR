// LegacyAVR - libswresample wrapper: N-channel planar float @ any rate -> 48 kHz,
// with soft clock-drift compensation (swr_set_compensation).
//
// The resampler is ALWAYS active (SWR_FLAG_RESAMPLE), even for 48 kHz input, so
// drift compensation can be changed per block without re-initialising swr
// (re-init would allocate on the DSP thread).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SwrContext;

namespace avr {

class Resampler {
public:
    Resampler() = default;
    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    // Not real-time. maxInFrames bounds every later process() call.
    bool open(uint32_t inRate, int channels, size_t maxInFrames, std::string& error);
    void close() noexcept;

    // Real-time. Returns frames written to out (<= outCapacity) or -1 on error.
    int process(const float* const* in, int inFrames, float* const* out, int outCapacity) noexcept;

    // Positive ppm => produce MORE output samples (output buffer is running low).
    void setCompensationPpm(double ppm) noexcept;

    [[nodiscard]] int maxOutput(int inFrames) const noexcept;
    [[nodiscard]] uint32_t inRate() const noexcept { return inRate_; }
    [[nodiscard]] double delayMs() const noexcept;

private:
    SwrContext* swr_ = nullptr;
    uint32_t inRate_ = 0;
    int channels_ = 0;
    long lastDelta_ = 0;
};

} // namespace avr
