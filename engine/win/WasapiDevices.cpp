// <initguid.h> must come first in exactly one translation unit so that
// PKEY_Device_FriendlyName (DEFINE_PROPERTYKEY) gets a definition.
#include <initguid.h>

#include "WasapiDevices.h"

#include "../core/Types.h"

#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>

#pragma comment(lib, "ole32.lib")

namespace avr::win {

uint32_t speakerMaskFor(uint16_t channels) noexcept {
    switch (channels) {
        case 1: return SPK_FC;
        case 2: return kMaskStereo;
        case 6: return kMask51Back;    // KSAUDIO_SPEAKER_5POINT1 (what HDMI sinks report)
        case 8: return kMask71;        // KSAUDIO_SPEAKER_7POINT1_SURROUND
        default: return 0;
    }
}

PcmFormatInfo parseFormat(const WAVEFORMATEX* wf) noexcept {
    PcmFormatInfo f;
    if (!wf) return f;
    f.rate = wf->nSamplesPerSec;
    f.channels = wf->nChannels;
    f.bits = wf->wBitsPerSample;
    f.validBits = wf->wBitsPerSample;
    f.blockAlign = wf->nBlockAlign;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.mask = ext->dwChannelMask;
        f.validBits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : wf->wBitsPerSample;
        f.isFloat = IsEqualGUID(ext->SubFormat, kSubtypeFloat) != 0;
        f.isPcm = IsEqualGUID(ext->SubFormat, kSubtypePcm) != 0;
    } else {
        f.isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        f.isPcm = wf->wFormatTag == WAVE_FORMAT_PCM;
    }
    return f;
}

std::string describeFormat(const WAVEFORMATEX* wf) {
    const auto f = parseFormat(wf);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s%u %uch %u Hz mask 0x%X",
                  f.isFloat ? "float" : (f.isPcm ? "int" : "fmt"), unsigned(f.validBits),
                  unsigned(f.channels), unsigned(f.rate), unsigned(f.mask));
    return buf;
}

bool buildIecFormat(IecType type, int variant, IecWaveFormat& out) noexcept {
    out = {};
    const bool eac3 = type == IecType::EAC3;
    const GUID* sub = type == IecType::AC3 ? &kSubtypeIecAC3
                    : eac3                 ? &kSubtypeIecEAC3
                    : iecIsDts(type)       ? &kSubtypeIecDTS
                                           : nullptr;
    if (!sub) return false;
    const DWORD rate = iecTransportRate(type);   // 48 kHz, or 192 kHz for E-AC-3

    WAVEFORMATEX& wf = out.FormatExt.Format;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;

    switch (variant) {
        case 0:   // WAVEFORMATEXTENSIBLE_IEC61937 - preferred by modern HDMI drivers
            wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wf.cbSize = sizeof(IecWaveFormat) - sizeof(WAVEFORMATEX);
            out.FormatExt.Samples.wValidBitsPerSample = 16;
            out.FormatExt.dwChannelMask = kMaskStereo;
            out.FormatExt.SubFormat = *sub;
            out.dwEncodedSamplesPerSec = 48000;
            out.dwEncodedChannelCount = 6;
            out.dwAverageBytesPerSec = 0;
            return true;
        case 1:   // plain WAVEFORMATEXTENSIBLE with IEC 61937 sub-format
            wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wf.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            out.FormatExt.Samples.wValidBitsPerSample = 16;
            out.FormatExt.dwChannelMask = kMaskStereo;
            out.FormatExt.SubFormat = *sub;
            return true;
        case 2:   // legacy WAVE_FORMAT_DOLBY_AC3_SPDIF (S/PDIF drivers, AC-3 only)
            if (type != IecType::AC3) return false;
            wf.wFormatTag = kWaveFormatDolbyAc3Spdif;
            wf.cbSize = 0;
            return true;
        default:
            return false;
    }
}

WAVEFORMATEXTENSIBLE buildPcmFormat(uint32_t rate, uint16_t channels, uint16_t bits, bool isFloat) noexcept {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = channels;
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = bits;
    f.Format.nBlockAlign = WORD(channels * bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = (bits == 32 && !isFloat) ? 24 : bits;
    f.dwChannelMask = speakerMaskFor(channels);
    f.SubFormat = isFloat ? kSubtypeFloat : kSubtypePcm;
    return f;
}

std::string deviceName(IMMDevice* dev) {
    if (!dev) return {};
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
        name = toUtf8(pv.pwszVal);
    PropVariantClear(&pv);
    return name;
}

std::wstring deviceId(IMMDevice* dev) {
    if (!dev) return {};
    LPWSTR id = nullptr;
    std::wstring r;
    if (SUCCEEDED(dev->GetId(&id)) && id) r = id;
    CoTaskMemFree(id);
    return r;
}

bool isRenderEndpoint(IMMDevice* dev) {
    ComPtr<IMMEndpoint> ep;
    EDataFlow flow = eRender;
    if (dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&ep)))) ep->GetDataFlow(&flow);
    return flow == eRender;
}

ComPtr<IMMDevice> openDevice(const std::wstring& id, EDataFlow flow, std::string& error) {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) { error = "MMDeviceEnumerator: " + hresultText(hr); return nullptr; }
    ComPtr<IMMDevice> dev;
    if (id.empty() || id == L"default") hr = en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
    else hr = en->GetDevice(id.c_str(), &dev);
    if (FAILED(hr)) { error = "open device: " + hresultText(hr); return nullptr; }
    DWORD state = 0;
    if (SUCCEEDED(dev->GetState(&state)) && state != DEVICE_STATE_ACTIVE) {
        error = "device is not active (disabled or unplugged)";
        return nullptr;
    }
    return dev;
}

uint32_t probeIecCaps(IMMDevice* dev) {
    uint32_t caps = 0;
    bool definitive = false;   // v1.2.0: at least one answer that was not "device in use"
    const IecType types[] = {IecType::AC3, IecType::DTS1, IecType::EAC3};
    for (int t = 0; t < 3; ++t) {
        for (int variant = 0; variant < 3; ++variant) {
            IecWaveFormat fmt;
            if (!buildIecFormat(types[t], variant, fmt)) continue;
            ComPtr<IAudioClient> client;
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
                return caps | (definitive ? kIecCapsKnown : 0u);
            const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                         reinterpret_cast<const WAVEFORMATEX*>(&fmt), nullptr);
            if (hr != AUDCLNT_E_DEVICE_IN_USE) definitive = true;
            if (hr == S_OK) { caps |= 1u << t; break; }
        }
    }
    return caps | (definitive ? kIecCapsKnown : 0u);
}

uint32_t probeIecCaps(const std::wstring& deviceId) {
    std::string err;
    auto dev = openDevice(deviceId, eRender, err);
    return dev ? probeIecCaps(dev.Get()) : 0u;
}

std::vector<DeviceInfo> enumerateDevices(bool probeIec, std::string& error) {
    std::vector<DeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) { error = "MMDeviceEnumerator: " + hresultText(hr); return out; }

    std::wstring defRender, defCapture;
    {
        ComPtr<IMMDevice> d;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) defRender = deviceId(d.Get());
        d.Reset();
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &d))) defCapture = deviceId(d.Get());
    }

    ComPtr<IMMDeviceCollection> coll;
    hr = en->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr)) { error = "EnumAudioEndpoints: " + hresultText(hr); return out; }
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        DeviceInfo info;
        info.id = deviceId(dev.Get());
        info.name = deviceName(dev.Get());
        info.render = isRenderEndpoint(dev.Get());
        info.isDefault = info.id == (info.render ? defRender : defCapture);
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
                info.channels = mix->nChannels;
                info.rate = mix->nSamplesPerSec;
                CoTaskMemFree(mix);
            }
        }
        if (probeIec && info.render) info.iecCaps = probeIecCaps(dev.Get());
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace avr::win
