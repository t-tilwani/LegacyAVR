// LegacyAVR - shared-memory telemetry publisher (seqlock writer side).
#pragma once

#include "WinUtil.h"

#include "../ipc/TelemetryLayout.h"

#include <string>

namespace avr::win {

class TelemetryWriter {
public:
    ~TelemetryWriter() { close(); }
    bool open(bool serviceMode, std::string& error);
    void close() noexcept;

    // Seqlock publish: the callback fills a local copy, which is then copied into
    // the mapping between two sequence increments.
    template <class Fill>
    void publish(Fill&& fill) noexcept {
        if (!view_) return;
        ipc::TelemetryBlock local = shadow_;
        fill(local);
        local.magic = ipc::kTelemetryMagic;
        local.version = ipc::kTelemetryVersion;
        local.updateCounter = ++counter_;
        local.qpcTimestamp = qpcNow();
        write(local);
        shadow_ = local;
    }

private:
    void write(const ipc::TelemetryBlock& b) noexcept;

    Handle mapping_;
    ipc::TelemetryBlock* view_ = nullptr;
    ipc::TelemetryBlock shadow_{};
    uint64_t counter_ = 0;
    uint32_t seq_ = 0;
};

} // namespace avr::win
