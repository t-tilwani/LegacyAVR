#include "SpeechClips.h"

#include "WinUtil.h"

#include <mmreg.h>
#include <sapi.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace avr::win {

namespace {
// SPDFID_WaveFormatEx = {C31ADBAE-527F-4ff5-A230-F62BB61FF70C} (sapi.h declares it extern;
// spelled out here so no sapiguid/sapi.lib dependency is needed).
constexpr GUID kSpdfidWaveFormatEx = {0xC31ADBAE, 0x527F, 0x4FF5, {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};

std::vector<float> speak(ISpVoice* voice, const wchar_t* text, std::string& error) {
    std::vector<float> out;
    ComPtr<IStream> mem;
    mem.Attach(SHCreateMemStream(nullptr, 0));
    if (!mem) { error = "SHCreateMemStream failed"; return out; }

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = 96000;

    ComPtr<ISpStream> sp;
    HRESULT hr = CoCreateInstance(__uuidof(SpStream), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sp));
    if (FAILED(hr)) { error = "SpStream: " + hresultText(hr); return out; }
    hr = sp->SetBaseStream(mem.Get(), kSpdfidWaveFormatEx, &wf);
    if (FAILED(hr)) { error = "SetBaseStream: " + hresultText(hr); return out; }
    hr = voice->SetOutput(sp.Get(), FALSE);
    if (FAILED(hr)) { error = "SetOutput: " + hresultText(hr); return out; }
    hr = voice->Speak(text, SPF_DEFAULT | SPF_IS_NOT_XML, nullptr);
    voice->SetOutput(nullptr, FALSE);
    if (FAILED(hr)) { error = "Speak: " + hresultText(hr); return out; }

    STATSTG st{};
    mem->Stat(&st, STATFLAG_NONAME);
    const ULONG bytes = ULONG(st.cbSize.QuadPart);
    std::vector<int16_t> pcm(bytes / 2);
    LARGE_INTEGER zero{};
    mem->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    mem->Read(pcm.data(), bytes, &read);
    out.resize(read / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = float(pcm[i]) / 32768.0f;
    return out;
}
} // namespace

std::array<std::vector<float>, 6> synthesiseChannelNames(std::string& error) {
    std::array<std::vector<float>, 6> clips;
    ComPtr<ISpVoice> voice;
    HRESULT hr = CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice));
    if (FAILED(hr)) { error = "SAPI voice unavailable: " + hresultText(hr); return clips; }
    voice->SetRate(0);
    static constexpr const wchar_t* names[6] = {
        L"Front left", L"Front right", L"Centre", L"Subwoofer", L"Surround left", L"Surround right"};
    for (size_t i = 0; i < 6; ++i) {
        clips[i] = speak(voice.Get(), names[i], error);
        if (clips[i].empty()) break;
    }
    return clips;
}

} // namespace avr::win
