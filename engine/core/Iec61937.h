// LegacyAVR - IEC 61937 data-burst packing (encoder output) and detection of
// pre-encoded AC-3 / DTS / E-AC-3 arriving as "PCM" on the capture endpoint.
//
// Burst layout (16-bit little-endian words, as carried in IEC 60958 sub-frames):
//   Pa = 0xF872  Pb = 0x4E1F  Pc = data type (+ bsmod)  Pd = payload length
//   payload (codec stream is big-endian 16-bit words => byte-swapped in LE)
//   zero stuffing up to the repetition period.
//
//   type    Pc     period (IEC 60958 frames)   burst bytes   Pd unit
//   AC-3    0x01   1536                        6144          bits
//   DTS-I   0x0B    512                        2048          bits
//   DTS-II  0x0C   1024                        4096          bits
//   DTS-III 0x0D   2048                        8192          bits
//   E-AC-3  0x15   6144 (4x rate = 192 kHz)    24576         bytes
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace avr {

inline constexpr uint16_t kIecPa = 0xF872;
inline constexpr uint16_t kIecPb = 0x4E1F;
inline constexpr size_t kIecHeaderBytes = 8;
inline constexpr size_t kIecMaxBurstBytes = 24576;

enum class IecType : uint16_t {
    None = 0x00, AC3 = 0x01, Pause = 0x03, DTS1 = 0x0B, DTS2 = 0x0C, DTS3 = 0x0D, EAC3 = 0x15,
};

[[nodiscard]] size_t iecBurstBytes(IecType t) noexcept;
// Repetition period in IEC 60958 frames (stereo 16-bit sample pairs) at the transport rate.
[[nodiscard]] inline size_t iecBurstFrames(IecType t) noexcept { return iecBurstBytes(t) / 4; }
// Transport sample rate for a 48 kHz source (E-AC-3 needs 4x).
[[nodiscard]] inline uint32_t iecTransportRate(IecType t) noexcept { return t == IecType::EAC3 ? 192000u : 48000u; }
[[nodiscard]] const char* iecTypeName(IecType t) noexcept;
[[nodiscard]] bool iecIsDts(IecType t) noexcept;

// Packs one codec frame into one IEC 61937 burst.
// Returns bytes written (always the full repetition period) or 0 if it cannot fit.
// DTS frames that exactly fill the period are sent "raw" (no preamble), the same
// behaviour as FFmpeg's spdif muxer and hardware DTS Interactive encoders.
size_t iecPackBurst(IecType type, const uint8_t* frame, size_t frameBytes,
                    uint8_t* out, size_t outCapacity, uint8_t bsmod = 0) noexcept;

class Iec61937Detector {
public:
    void reset() noexcept;

    // Feeds 16-bit words (interleaved stereo int16 samples). Returns bursts completed.
    size_t feed(const int16_t* words, size_t count) noexcept;

    // True once >= 2 correctly spaced bursts were seen and the last one is recent.
    [[nodiscard]] bool locked() const noexcept;
    [[nodiscard]] IecType type() const noexcept { return lockedType_; }
    [[nodiscard]] uint64_t burstCount() const noexcept { return bursts_; }

    // Last extracted codec frame (big-endian codec byte order restored).
    [[nodiscard]] bool frameReady() const noexcept { return frameReady_; }
    [[nodiscard]] const uint8_t* frameData() const noexcept { return frame_.data(); }
    [[nodiscard]] size_t frameBytes() const noexcept { return frameLen_; }
    void consumeFrame() noexcept { frameReady_ = false; }

private:
    enum class St : uint8_t { Search, Pc, Pd, Payload };
    St st_ = St::Search;
    uint16_t prev_ = 0;
    uint16_t pc_ = 0;
    size_t want_ = 0, got_ = 0;
    uint64_t wordsSinceSync_ = ~0ull >> 2;
    uint64_t bursts_ = 0;
    uint32_t consecutive_ = 0;
    IecType curType_ = IecType::None;
    IecType lockedType_ = IecType::None;
    std::array<uint8_t, kIecMaxBurstBytes> frame_{};
    size_t frameLen_ = 0;
    bool frameReady_ = false;
};

} // namespace avr
