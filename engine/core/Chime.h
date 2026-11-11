// LegacyAVR - activation chime (v1.6.0).
//
// A short, gentle, original two-note bell (A5 then E6, soft attack, exponential decay,
// about 0.9 s, peak about -24 dBFS) mixed into the front left/right speakers of the
// output. It is synthesised once in prepare() (not real-time) and played from a table, so
// mixing it is allocation-free. It is added after the processing and before the safety
// limiter, so it is never sent anywhere else: it reaches only the configured output, never
// the capture input (the host refuses an input that is the output endpoint).
#pragma once

#include <cstddef>
#include <vector>

namespace avr {

class ActivationChime {
public:
    static constexpr float kPeak = 0.063f;        // ~ -24 dBFS per front speaker
    static constexpr double kLengthSec = 0.9;

    // Builds the waveform at `sampleRate` (allocates - call from configure()).
    void prepare(double sampleRate);
    void trigger() noexcept { pos_ = 0; active_ = !table_.empty(); }
    void stop() noexcept { active_ = false; pos_ = 0; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] size_t length() const noexcept { return table_.size(); }
    [[nodiscard]] float sample(size_t i) const noexcept { return i < table_.size() ? table_[i] : 0.0f; }

    // Adds the next `frames` samples into bus[left] and bus[right]. Real-time safe.
    void mix(float* left, float* right, size_t frames) noexcept;

private:
    std::vector<float> table_;
    size_t pos_ = 0;
    bool active_ = false;
};

} // namespace avr
