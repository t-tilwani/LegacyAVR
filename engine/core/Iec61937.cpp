#include "Iec61937.h"

#include <algorithm>
#include <cstring>

namespace avr {

size_t iecBurstBytes(IecType t) noexcept {
    switch (t) {
        case IecType::AC3:  return 6144;
        case IecType::DTS1: return 2048;
        case IecType::DTS2: return 4096;
        case IecType::DTS3: return 8192;
        case IecType::EAC3: return 24576;
        default: return 0;
    }
}

const char* iecTypeName(IecType t) noexcept {
    switch (t) {
        case IecType::AC3:   return "AC-3";
        case IecType::DTS1:  return "DTS (type I)";
        case IecType::DTS2:  return "DTS (type II)";
        case IecType::DTS3:  return "DTS (type III)";
        case IecType::EAC3:  return "E-AC-3";
        case IecType::Pause: return "Pause";
        default:             return "None";
    }
}

bool iecIsDts(IecType t) noexcept {
    return t == IecType::DTS1 || t == IecType::DTS2 || t == IecType::DTS3;
}

namespace {
inline void putLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}
// Codec stream is big-endian 16-bit words; emit as little-endian words.
inline void putSwapped(uint8_t* dst, const uint8_t* src, size_t bytes) noexcept {
    const size_t pairs = bytes / 2;
    for (size_t i = 0; i < pairs; ++i) {
        dst[2 * i]     = src[2 * i + 1];
        dst[2 * i + 1] = src[2 * i];
    }
    if (bytes & 1) {   // lone final byte is MSB-aligned in its word
        dst[2 * pairs]     = 0;
        dst[2 * pairs + 1] = src[bytes - 1];
    }
}
} // namespace

size_t iecPackBurst(IecType type, const uint8_t* frame, size_t frameBytes,
                    uint8_t* out, size_t cap, uint8_t bsmod) noexcept {
    const size_t period = iecBurstBytes(type);
    if (period == 0 || cap < period) return 0;
    const size_t padded = (frameBytes + 1) & ~size_t(1);

    if (iecIsDts(type) && padded > period - kIecHeaderBytes) {
        if (padded > period) return 0;
        putSwapped(out, frame, frameBytes);           // raw DTS, receiver syncs on 0x7FFE8001
        std::memset(out + padded, 0, period - padded);
        return period;
    }
    if (padded + kIecHeaderBytes > period) return 0;

    uint16_t pc = static_cast<uint16_t>(type);
    if (type == IecType::AC3) pc |= static_cast<uint16_t>((bsmod & 0x7) << 8);
    const uint16_t pd = (type == IecType::EAC3) ? static_cast<uint16_t>(frameBytes)
                                                 : static_cast<uint16_t>(padded * 8);
    putLe16(out + 0, kIecPa);
    putLe16(out + 2, kIecPb);
    putLe16(out + 4, pc);
    putLe16(out + 6, pd);
    putSwapped(out + kIecHeaderBytes, frame, frameBytes);
    std::memset(out + kIecHeaderBytes + padded, 0, period - kIecHeaderBytes - padded);
    return period;
}

// ---------------------------------------------------------------------------
void Iec61937Detector::reset() noexcept {
    st_ = St::Search;
    prev_ = 0; pc_ = 0; want_ = got_ = 0;
    wordsSinceSync_ = ~0ull >> 2;
    bursts_ = 0; consecutive_ = 0;
    curType_ = lockedType_ = IecType::None;
    frameLen_ = 0; frameReady_ = false;
}

bool Iec61937Detector::locked() const noexcept {
    if (consecutive_ < 2 || lockedType_ == IecType::None) return false;
    const size_t periodWords = iecBurstBytes(lockedType_) / 2;
    return wordsSinceSync_ < periodWords * 4;
}

size_t Iec61937Detector::feed(const int16_t* words, size_t count) noexcept {
    size_t completed = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t w = static_cast<uint16_t>(words[i]);
        ++wordsSinceSync_;
        switch (st_) {
            case St::Search:
                if (prev_ == kIecPa && w == kIecPb) st_ = St::Pc;
                break;
            case St::Pc:
                pc_ = w;
                st_ = St::Pd;
                break;
            case St::Pd: {
                const auto t = static_cast<IecType>(pc_ & 0x1F);
                if (t == IecType::Pause) {        // pause bursts keep lock but carry no frame
                    wordsSinceSync_ = 0;
                    st_ = St::Search;
                    break;
                }
                const size_t period = iecBurstBytes(t);
                const size_t bytes = (t == IecType::EAC3) ? size_t(w) : (size_t(w) + 7) / 8;
                if (period == 0 || bytes == 0 || bytes + kIecHeaderBytes > period) {
                    st_ = St::Search;             // not a valid burst header: false sync
                    consecutive_ = 0;
                    break;
                }
                // A burst must start exactly one repetition period after the previous one
                // (+/- a few words of tolerance). Pd -> Pd distance == period words.
                const size_t periodWords = period / 2;
                if (curType_ == t && wordsSinceSync_ + 4 >= periodWords && wordsSinceSync_ <= periodWords + 4)
                    ++consecutive_;
                else
                    consecutive_ = 1;
                curType_ = t;
                wordsSinceSync_ = 0;
                want_ = bytes; got_ = 0;
                frameReady_ = false;
                st_ = St::Payload;
                break;
            }
            case St::Payload:
                if (got_ + 1 < want_) {
                    frame_[got_]     = static_cast<uint8_t>(w >> 8);
                    frame_[got_ + 1] = static_cast<uint8_t>(w & 0xFF);
                    got_ += 2;
                } else {
                    frame_[got_++] = static_cast<uint8_t>(w >> 8);
                }
                if (got_ >= want_) {
                    frameLen_ = want_;
                    frameReady_ = true;
                    ++bursts_;
                    ++completed;
                    if (consecutive_ >= 2) lockedType_ = curType_;
                    st_ = St::Search;
                }
                break;
        }
        prev_ = w;
    }
    return completed;
}

} // namespace avr
