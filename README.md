# LegacyAVR

![Windows 11](https://img.shields.io/badge/Windows-11-0078D4?logo=windows11&logoColor=white)
![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)
![.NET 10](https://img.shields.io/badge/.NET-10-512BD4?logo=dotnet&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)

**Real-time Dolby Digital (AC-3), DTS and Dolby Digital Plus (E-AC-3) encoding for Windows**, so PC games, films
and apps play genuine 5.1 surround on older AV receivers, soundbars and TVs that only accept Dolby/DTS
bitstreams over optical S/PDIF or plain HDMI ARC.

Modern PCs output multichannel audio as PCM. Many older AVRs and ARC links can only receive surround as a
compressed Dolby or DTS bitstream, so they fall back to stereo. Hardware "live" encoding has largely
disappeared from sound cards; LegacyAVR does the same job in software.

> LegacyAVR is an independent hobby project by [T-Tilwani](https://github.com/T-Tilwani), developed with the
> assistance of [Claude Code](https://claude.com/claude-code). It is not affiliated with, endorsed by or certified
> by Dolby Laboratories, DTS / Xperi, VB-Audio Software, Microsoft or any streaming service.
> See [Trademarks](#trademarks).

<!-- Screenshot placeholder - add the image as docs/images/main-window.png, then remove the comment markers:
![LegacyAVR dashboard](docs/images/main-window.png)
-->
> *Screenshot coming soon: `docs/images/main-window.png` (dashboard), `docs/images/eq-dialog.png` (custom EQ).*

---

## Contents

1. [Features](#features)
2. [How it works](#how-it-works)
3. [Requirements](#requirements)
4. [Windows audio setup (VB-Audio Cable)](#windows-audio-setup-vb-audio-cable)
5. [Building from source](#building-from-source)
6. [Using LegacyAVR](#using-legacyavr)
7. [Streaming services and DRM](#streaming-services-and-drm-netflix-disney-prime-video-crunchyroll)
8. [Troubleshooting](#troubleshooting)
9. [Project layout](#project-layout)
10. [Licence, third-party software and trademarks](#licence-third-party-software-and-trademarks)

---

## Features

**Encoding and output**
- Live encoding to Dolby Digital (AC-3, up to 640 kbps), DTS (754.5 / 1509.75 kbps) or Dolby Digital Plus
  (E-AC-3, up to 1024 kbps, HDMI only), packed as IEC 61937 and sent with WASAPI exclusive mode.
- Bit-exact passthrough of audio that is already Dolby/DTS encoded.
- "Lossless PCM 2.0" mode for music, which switches to the bitstream automatically when surround starts.
- Clock-drift control between the capture and output devices; per-stage latency readout (about 70-85 ms
  before the AVR with the automatic safety margin).

**Processing**
- Stereo upmix (Dynamic 3.1 / 5.1, or 3.0 / 5.0 without a subwoofer). Real multichannel audio is never upmixed.
- LFE generation for 5.0 / 7.0 sources, with a safeguard that stops it once a source uses its own LFE.
- Bass management for setups without a subwoofer (LFE folded into the front speakers).
- Optional custom five-band parametric EQ per speaker, with click-free changes, automatic headroom against
  clipping and an "audition" mode.
- Night Mode compression, safety limiter, Game Mode with full-screen game detection.

**Dashboard (WPF)**
- Live input/output meters, processing and LFE status, effective-output indicator.
- Profiles that store every setting, with import/export and portable apply to another output.
- Diagnostics wizard (channel identification, levels, centre bleed, LFE, polarity).
- Searchable Settings guide with the real controls built in; system-tray operation.

---

## How it works

```
 Games / apps / players
          |  7.1 float PCM (Windows default playback device)
          v
 +---------------------------+
 |  VB-Audio Virtual Cable   |   "CABLE Input", configured as 7.1 / 48 kHz
 +---------------------------+
          |  WASAPI loopback capture
          v
 +--------------------------------------------------------------------------+
 |  AVREngine.exe  (C++23, or AVREngineService.exe as a Windows service)     |
 |                                                                          |
 |  capture --> lock-free ring --> DSP thread (MMCSS "Pro Audio")           |
 |                                  | detect PCM / IEC 61937 bitstream      |
 |                                  | route 7.1 -> 5.1, resample, drift trim|
 |                                  | upmix | LFE gen | bass | EQ | gains   |
 |                                  | Night Mode | limiter | meters         |
 |                                  v                                       |
 |                    FFmpeg encode (AC-3 / DTS / E-AC-3)                   |
 |                                  | IEC 61937 burst packing               |
 |                                  v                                       |
 |                  lock-free ring --> WASAPI exclusive render               |
 +--------------------------------------------------------------------------+
          |  bitstream                    ^                 |
          v                               | named pipe      | shared memory
 HDMI ARC / optical S/PDIF                | (commands)      | (telemetry, 30 Hz)
          |                               |                 v
          v                     +--------------------------------------+
 AVR / soundbar / TV            |  LegacyAVR.UI.exe  (C#, WPF, .NET 10) |
 (Dolby / DTS decoder)          |  dashboard, profiles, EQ, tray icon   |
                                +--------------------------------------+
```

<!-- Diagram placeholder - an image version can go in docs/images/architecture.png:
![Architecture](docs/images/architecture.png)
-->

The full design (thread model, real-time memory policy, latency budget, IPC layout, input-format rules) is in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Requirements

- Windows 11 (64-bit).
- [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (VB-CABLE) or Hi-Fi Cable - free / donationware from
  VB-Audio Software, installed separately.
- An HDMI (ARC) or optical / coaxial S/PDIF output connected to an AVR, soundbar or TV whose *Supported
  Formats* list includes Dolby Digital (and DTS / Dolby Digital Plus if you want those).
- To run a release: the [.NET 10 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/10.0) (x64)
  and the [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist) (x64).
- To build: Visual Studio 2026 (or Build Tools 2026) with *Desktop development with C++* (MSVC, Windows 11 SDK,
  C++ CMake tools, vcpkg) and the .NET 10 SDK.

---

## Windows audio setup (VB-Audio Cable)

One-time setup. All of these are in the classic Sound control panel (`mmsys.cpl`); the dashboard's Setup
guide tab has buttons that open it.

1. Install **VB-CABLE** from [vb-audio.com/Cable](https://vb-audio.com/Cable/) and reboot if asked.
2. **Playback -> CABLE Input -> Configure -> 7.1 Surround**, ticking every speaker in the wizard.
3. **CABLE Input -> Properties -> Advanced:** 24- or 32-bit, **48000 Hz**. Untick *Enable audio enhancements*
   and turn Spatial Sound off on this device.
4. Set **CABLE Input as the Default Device**. Games and apps now output 7.1 into the cable.
5. **Your HDMI / S/PDIF device -> Properties -> Advanced:** tick both *Allow applications to take exclusive
   control* boxes. Its *Supported Formats* tab should list Dolby Digital (and DTS).
6. In LegacyAVR (**Output & Profiles** tab): Input = *CABLE Input*, Output = your HDMI / S/PDIF device (press
   **Default** so it is chosen at every start), Format = *Dolby Digital 640 kbps*, Mode = *WASAPI Exclusive*.
7. No subwoofer? Switch *Dedicated subwoofer* to inactive and set the AVR's subwoofer to "None".
8. The AVR should now show Dolby Digital (or DTS). Run the **Diagnostics** wizard to check every speaker.

Discord, Spotify and system sounds keep working: they play into the cable, which LegacyAVR mixes and encodes.
Exclusive mode only locks the HDMI / optical device.

---

## Building from source

Builds are native Windows builds with MSVC and the .NET SDK. From a **Developer PowerShell for VS 2026** in
the repository root:

```powershell
.\scripts\setup-vcpkg.ps1                     # one time: pins the vcpkg baseline, enables MSBuild integration
.\scripts\build.ps1                           # Release|x64: engine, service, tests, UI -> .\bin\Release\
.\scripts\build.ps1 -RunTests                 # build, then run the native and .NET test suites
.\bin\Release\LegacyAVR.Tests.exe --bench     # real-time CPU benchmark
.\bin\Release\LegacyAVR.Tests.exe --latency   # latency per safety-margin setting
```

You can also open `LegacyAVR.sln` in Visual Studio and build **Release | x64**. The first build compiles
FFmpeg (`avcodec` and `swresample` only, LGPL configuration, shared DLLs) through vcpkg manifest mode; it takes
a few minutes once and is cached afterwards.

CMake alternative (engine and native tests only):

```powershell
cmake --preset vs2026-x64
cmake --build --preset release
ctest --preset release
```

Optional Windows service (elevated PowerShell):

```powershell
.\scripts\install-service.ps1             # installs and starts AVREngineService
.\scripts\install-service.ps1 -Uninstall
```

Run either the service **or** `AVREngine.exe`, not both; a global mutex enforces this.

---

## Using LegacyAVR

- Start `LegacyAVR.UI.exe`; it starts the engine automatically. Closing the window sends it to the tray and
  encoding continues; use **Exit** in the tray menu to quit.
- **Output & Profiles:** devices, format, bitrate, output mode, safety margin, and profiles (save, apply,
  import, export).
- **Upmixing & Bass:** stereo handling, upmix sliders, bass / subwoofer, custom EQ, LFE generation, Game Mode,
  Night Mode.
- **Mixer:** per-speaker trims, mutes and the EQ button for each speaker.
- **Diagnostics**, **Settings guide** (every setting explained, with its control), **Setup guide**, and
  **Engine Settings** (start with Windows, engine controls, logs).

Logs: `%LOCALAPPDATA%\LegacyAVR\AVREngine.log` (service: `%ProgramData%\LegacyAVR\`). Settings and profiles:
`%APPDATA%\LegacyAVR\`.

---

## Streaming services and DRM (Netflix, Disney+, Prime Video, Crunchyroll)

> **Important:** streaming services often deliver **stereo only** on Windows - especially in web browsers -
> because of copy protection (DRM) and browser limits. What you get depends on the service, the browser or app,
> your hardware and your subscription. If LegacyAVR's input meters show only FL and FR while a film plays, the
> service is sending stereo and there are no surround channels to pass on.
>
> - LegacyAVR **does not and cannot bypass DRM**. It only processes the audio Windows plays into the virtual
>   cable, after the service's own player has decoded it.
> - It cannot tell which website or app is playing (the cable carries mixed audio, not the app or URL), so it
>   does not change settings per service.
> - For stereo sources, the Dynamic 5.1 upmix creates derived surround channels. It sounds fuller, but it cannot
>   recover surround tracks the service did not send.
> - Some third-party browser add-ons claim to request 5.1 audio from particular services. They are not part of
>   LegacyAVR, not endorsed by it, and may stop working at any time; check their permissions before installing.

---

## Troubleshooting

| Symptom | What to check |
|---|---|
| AVR shows PCM / stereo | *Effective output* in the Output card: a fallback means the device refused the format. Use WASAPI Exclusive and a format the device lists. |
| `0x88890008` / format rejected | The output (or the TV/AVR behind ARC) does not accept that bitstream. Dolby Digital 640 kbps works almost everywhere. Optical cannot carry Dolby Digital Plus. |
| No sound at all | Is the audio pipeline stopped (red bar)? Is CABLE Input the Windows default device? Is another app holding the HDMI device in exclusive mode? |
| Short silence when changing settings | Normal: format, bitrate, mode, margin, device and subwoofer changes re-open the output and the AVR re-syncs for about a second. |
| Pre-encoded audio not passed through | The cable must be 48 kHz, Windows volume 100 %, no enhancements, so the bits arrive untouched. |

The in-app **Settings guide** covers every setting and readout in more detail.

---

## Project layout

```
engine/            C++23 engine: core/ (DSP, encoder, IEC 61937), win/ (WASAPI, host, IPC), ipc/, app/
ui/                WPF dashboard (LegacyAVR.UI) and its xUnit tests (LegacyAVR.UI.Tests)
tests/             native unit / integration tests and benchmarks
scripts/           build, vcpkg setup and service install scripts (PowerShell)
build/             shared MSBuild properties
docs/              ARCHITECTURE.md, images/
Github Releases/   release documentation (README, LICENSE, notices, release notes)
```

Contributions are welcome - see the issue and pull-request templates in `.github/`.

---

## Licence, third-party software and trademarks

LegacyAVR's own source code is released under the [MIT License](LICENSE).

It uses third-party software under its own licences - most importantly **FFmpeg** (`libavcodec`,
`libswresample`), which is LGPL v2.1 or later and is linked **dynamically** as separate DLLs. Full details,
including what a binary release must contain, are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
VB-Audio Virtual Cable is a separate product by VB-Audio Software and is **not** included.

### Trademarks

Dolby, Dolby Digital, Dolby Digital Plus and Dolby Digital Live are trademarks of Dolby Laboratories. DTS is a
trademark of DTS, Inc. / Xperi. VB-CABLE and VB-Audio are trademarks of VB-Audio Software. Windows is a
trademark of Microsoft Corporation. Netflix, Disney+, Prime Video and Crunchyroll are trademarks of their
respective owners. These names are used only to describe compatibility. LegacyAVR is not a Dolby- or
DTS-licensed or certified product.

### Disclaimer

LegacyAVR is provided "as is", without warranty of any kind (see [LICENSE](LICENSE)). You are responsible for
making sure your use of audio codecs complies with the laws and licence terms that apply where you live. Start
with a low AVR volume when testing new settings.
