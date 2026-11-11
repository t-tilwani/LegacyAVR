#include "Chime.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace avr {

void ActivationChime::prepare(double sampleRate) {
    const size_t n = size_t(kLengthSec * sampleRate);
    table_.assign(n, 0.0f);
    pos_ = 0;
    active_ = false;
    // Two soft bell tones a fifth apart, the second 110 ms after the first. Each tone has a
    // 4 ms attack, a 220 ms decay and a quieter octave partial that dies away faster, which
    // gives a rounded, unobtrusive "ding-ding" rather than a beep.
    struct Note { double hz, startSec, weight; };
    static constexpr Note notes[] = {{880.0, 0.0, 0.55}, {1318.51, 0.11, 0.45}};
    constexpr double twoPi = 2.0 * std::numbers::pi;
    double peak = 0.0;
    std::vector<double> acc(n, 0.0);
    for (const Note& note : notes) {
        const size_t start = size_t(note.startSec * sampleRate);
        for (size_t i = start; i < n; ++i) {
            const double t = double(i - start) / sampleRate;
            const double env = (1.0 - std::exp(-t / 0.004)) * std::exp(-t / 0.22);
            const double tone = std::sin(twoPi * note.hz * t) + 0.25 * std::exp(-t / 0.08) * std::sin(twoPi * 2.0 * note.hz * t);
            acc[i] += note.weight * env * tone;
        }
    }
    for (double v : acc) peak = std::max(peak, std::fabs(v));
    const size_t fade = size_t(0.05 * sampleRate);   // last 50 ms fade to exact silence
    for (size_t i = 0; i < n; ++i) {
        double v = peak > 0.0 ? acc[i] / peak * double(kPeak) : 0.0;
        if (i + fade >= n) v *= double(n - 1 - i) / double(fade);
        table_[i] = float(v);
    }
}

void ActivationChime::mix(float* left, float* right, size_t frames) noexcept {
    if (!active_) return;
    const size_t n = std::min(frames, table_.size() - pos_);
    for (size_t i = 0; i < n; ++i) {
        const float v = table_[pos_ + i];
        left[i] += v;
        right[i] += v;
    }
    pos_ += n;
    if (pos_ >= table_.size()) stop();
}

} // namespace avr
