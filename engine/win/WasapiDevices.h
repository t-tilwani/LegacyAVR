// LegacyAVR - endpoint enumeration, format helpers and IEC 61937 wave formats.
#pragma once

#include "WinUtil.h"

#include "../core/Iec61937.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstdint>
#include <string>
#include <vector>

namespace avr::win {

// Sub-format GUIDs (spelled out so we do not depend on ksmedia.h GUID linkage).
inline constexpr GUID kSubtypePcm   = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kSubtypeIecAC3  = {0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kSubtypeIecDTS  = {0x00000008, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kSubtypeIecEAC3 = {0x0000000A, 0x0CEA, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr WORD kWaveFormatDolbyAc3Spdif = 0x0092;   // WAVE_FORMAT_DOLBY_AC3_SPDIF

#pragma pack(push, 1)
// Layout-identical to WAVEFORMATEXTENSIBLE_IEC61937 (mmreg.h / Windows 7+).
struct IecWaveFormat {
    WAVEFORMATEXTENSIBLE FormatExt;
    DWORD dwEncodedSamplesPerSec;
    DWORD dwEncodedChannelCount;
    DWORD dwAverageBytesPerSec;
};
#pragma pack(pop)
static_assert(sizeof(IecWaveFormat) == sizeof(WAVEFORMATEXTENSIBLE) + 12);

struct PcmFormatInfo {
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;          // container bits
    uint16_t validBits = 0;
    uint16_t blockAlign = 0;
    uint32_t mask = 0;
    bool isFloat = false;
    bool isPcm = false;         // integer PCM
};

PcmFormatInfo parseFormat(const WAVEFORMATEX* wf) noexcept;
std::string describeFormat(const WAVEFORMATEX* wf);

// Build an exclusive-mode IEC 61937 format. variant 0: WAVEFORMATEXTENSIBLE_IEC61937,
// 1: plain WAVEFORMATEXTENSIBLE, 2: WAVE_FORMAT_DOLBY_AC3_SPDIF (AC-3 only).
// Returns false when the variant does not exist for the type.
bool buildIecFormat(IecType type, int variant, IecWaveFormat& out) noexcept;

// PCM WAVEFORMATEXTENSIBLE (float32 or int16/int24-in-32).
WAVEFORMATEXTENSIBLE buildPcmFormat(uint32_t rate, uint16_t channels, uint16_t bits, bool isFloat) noexcept;
uint32_t speakerMaskFor(uint16_t channels) noexcept;

struct DeviceInfo {
    std::wstring id;
    std::string name;
    bool render = true;
    bool isDefault = false;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint32_t iecCaps = 0;    // bit0 AC-3, bit1 DTS, bit2 E-AC-3 (exclusive mode probe), bit7 = probe answered
};

std::vector<DeviceInfo> enumerateDevices(bool probeIec, std::string& error);

// id empty or "default" => default endpoint for `flow` (eConsole role).
ComPtr<IMMDevice> openDevice(const std::wstring& id, EDataFlow flow, std::string& error);
std::string deviceName(IMMDevice* dev);
std::wstring deviceId(IMMDevice* dev);
bool isRenderEndpoint(IMMDevice* dev);
// Bit 7 (kIecCapsKnown) is set when the device actually answered; without it the device
// was busy (e.g. held in exclusive mode) and bits 0-2 mean nothing.
inline constexpr uint32_t kIecCapsKnown = 0x80;
uint32_t probeIecCaps(IMMDevice* dev);
uint32_t probeIecCaps(const std::wstring& deviceId);

} // namespace avr::win
