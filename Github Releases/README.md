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

## A personal note

I built LegacyAVR for my own AVR setup and decided to share it in case it might be useful to someone else. Older
AVRs can be a great way to enjoy high-quality audio on a budget, so it's a shame that Dolby Digital Live and DTS
Connect support has largely disappeared from PCs. I hope this project helps people keep getting more out of the
systems they already own.

I'm not a professional programmer; my background is more as a tinkerer, with only knowledge of web development.
I built this with help from Claude Code, bringing my audio knowledge to the process and testing it with my own
setup. The stereo upmixing has worked nicely for my music so far, but I'm continuing to improve stability.
Feedback and suggestions are very welcome.

I was inspired by strepto42's [virtual-ac3-encoder](https://github.com/strepto42/virtual-ac3-encoder) - thank
you for the inspiration. This isn't meant to compete with it; it's simply a different take. I use my AVR over
HDMI ARC and wanted to explore features including Dolby Digital Plus compatibility.

I've tried to include helpful diagnostics and plenty of options to let people tailor the audio to their setup.
There's still room to polish the look and feel, but for now I'm focused on functionality and features. I'm
sharing it because I thought it might help someone else, too.

---

## Contents

1. [Features](#features)
2. [How it works](#how-it-works)
3. [Requirements](#requirements)
4. [Installation](#installation)
5. [Windows audio setup (VB-Audio Cable)](#windows-audio-setup-vb-audio-cable)
6. [Building from source](#building-from-source)
7. [Using LegacyAVR](#using-legacyavr)
8. [Streaming services and DRM](#streaming-services-and-drm-netflix-disney-prime-video-crunchyroll)
9. [Troubleshooting](#troubleshooting)
10. [Project layout](#project-layout)
11. [Licence, third-party software and trademarks](#licence-third-party-software-and-trademarks)

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
- LFE generation for 5.0 / 7.0 sources (with a subwoofer), with a safeguard that stops it once a source uses its
  own LFE.
- Setups without a subwoofer: no LFE channel is generated or sent (3.0 / 5.0), and a source's own LFE is folded
  into the front speakers (low-passed, -3 dB each, limiter-protected) - or discarded, if you prefer.
  See [Channel layouts](#channel-layouts-and-the-subwoofer-switch).
- Optional custom five-band parametric EQ per speaker, with click-free changes, automatic headroom against
  clipping and an "audition" mode.
- Night Mode compression, safety limiter, Game Mode with full-screen game detection.

**Dashboard (WPF)**
- Live input/output meters, processing and LFE status, effective-output and Custom EQ indicators.
- Profiles that store every setting, with import/export, portable apply to another output and a one-click
  footer save when settings change.
- Selectable error messages with a one-click "Open log folder", and an emergency reset that force-stops the
  engine and restarts the dashboard.
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
- The [.NET 10 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/10.0) (x64) and the
  [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist) (x64).
  **The installer checks for both and installs them for you if needed** (the Visual C++ Redistributable is
  included in Setup; the .NET runtime is downloaded from Microsoft). With the portable zip you install them
  yourself.
- To build: Visual Studio 2026 (or Build Tools 2026) with *Desktop development with C++* (MSVC, Windows 11 SDK,
  C++ CMake tools, vcpkg) and the .NET 10 SDK. To create the installer as well:
  [Inno Setup](https://jrsoftware.org/isdl.php) 6.3 or later.

---

## Installation

### 📦 Quick installation (recommended)

1. Download **`LegacyAVR_v<version>_Setup.exe`** from the
   [latest GitHub release](https://github.com/T-Tilwani/LegacyAVR/releases/latest).
2. Run it and follow the on-screen prompts. LegacyAVR is installed to `C:\Program Files\LegacyAVR` with a
   Start menu entry (and, if you tick it, a desktop shortcut).
   - If the Visual C++ Redistributable or the .NET 10 Desktop Runtime is missing, Setup offers to install it
     (the Visual C++ one is included; .NET is downloaded from Microsoft).
   - The installer is not code-signed, so Windows SmartScreen may say *"Windows protected your PC"*. Choose
     **More info -> Run anyway** only for a file downloaded from this project's Releases page (you can check its
     SHA256 checksum, see the release notes).
3. Download and install [VB-CABLE Virtual Audio Device](https://vb-audio.com/Cable/) (Setup's last page has a
   button for it). Restart your PC if VB-CABLE's installer asks you to.
4. Open the Sound control panel (`Win + R` -> type `mmsys.cpl` -> Enter): configure **CABLE Input** as
   **7.1 Surround** (or 5.1) and set it as the **default playback device**. The full checklist is in
   [Windows audio setup](#windows-audio-setup-vb-audio-cable).
5. Start **LegacyAVR** from the Start menu and select your HDMI ARC or optical (S/PDIF) output device.

*Prefer not to use an installer? Each release also has `LegacyAVR_v<version>_Portable.zip`: extract it anywhere
and run `LegacyAVR.UI.exe`. Install the .NET 10 Desktop Runtime and the Visual C++ Redistributable yourself
(links above).*

**Updating:** run the newer Setup; it closes LegacyAVR if it is running and keeps your settings and profiles.
**Uninstalling:** *Settings -> Apps -> Installed apps -> LegacyAVR -> Uninstall* (it asks whether to keep your
settings).

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
.\scripts\build.ps1 -Package                  # clean build + tests, then the installer and the portable zip
.\bin\Release\LegacyAVR.Tests.exe --bench     # real-time CPU benchmark
.\bin\Release\LegacyAVR.Tests.exe --latency   # latency per safety-margin setting
```

`-Package` needs [Inno Setup](https://jrsoftware.org/isdl.php) 6.3 or later (`winget install JRSoftware.InnoSetup`).
It cleans `bin\Release`, builds and tests, checks that FFmpeg is the LGPL build, stages the release files in
`out\package\LegacyAVR\` (programs, FFmpeg DLLs, licences, FFmpeg source information), compiles
`scripts\installer.iss` (embedding Visual Studio's own `vc_redist.x64.exe`), and writes to `Github Releases\`:

| File | What it is |
|---|---|
| `LegacyAVR_v<version>_Setup.exe` | Installer: Program Files, Start menu, dependency checks, optional service, uninstaller |
| `LegacyAVR_v<version>_Portable.zip` | The same files without an installer |
| `SHA256SUMS.txt` | Checksums to publish with the release |

The version comes from `<Version>` in `ui/LegacyAVR.UI/LegacyAVR.UI.csproj`. These files are git-ignored:
attach them to a GitHub release instead of committing them.

You can also open `LegacyAVR.sln` in Visual Studio and build **Release | x64**. The first build compiles
FFmpeg (`avcodec` and `swresample` only, LGPL configuration, shared DLLs) through vcpkg manifest mode; it takes
a few minutes once and is cached afterwards.

CMake alternative (engine and native tests only):

```powershell
cmake --preset vs2026-x64
cmake --build --preset release
ctest --preset release
```

Optional Windows service: the installer offers it as an *Advanced* task. Otherwise, from an elevated
PowerShell (works in a source tree, an installed copy or the portable folder):

```powershell
.\scripts\install-service.ps1             # installs and starts AVREngineService
.\scripts\install-service.ps1 -Uninstall
```

Run either the service **or** `AVREngine.exe`, not both; a global mutex enforces this.

---

## Using LegacyAVR

- Start **LegacyAVR** from the Start menu (or `LegacyAVR.UI.exe`); it starts the engine automatically. Closing the window sends it to the tray and
  encoding continues; use **Exit** in the tray menu to quit.
- **Output & Profiles:** devices, format, bitrate, output mode, safety margin, and profiles (save, apply,
  import, export). When the settings differ from the active profile a **Save** button appears in the footer:
  it overwrites the active profile, or asks for a name and creates a new one for the selected output.
- **Upmixing & Bass:** stereo handling, upmix sliders, bass / subwoofer, custom EQ, the LFE switch, Game Mode,
  Night Mode.
- **Mixer:** per-speaker trims, mutes and the EQ button for each speaker.
- **Diagnostics**, **Settings guide** (every setting explained, with its control), **Engine Settings** (start
  with Windows, engine controls, logs, emergency reset) and the **Setup guide** (last tab).

Defaults for a new installation: Game Mode off, *LFE generation while gaming* on (it only acts while Game Mode is
active), dedicated subwoofer active. An existing configuration keeps its own settings when you upgrade.

Logs: `%LOCALAPPDATA%\LegacyAVR\AVREngine.log` (service: `%ProgramData%\LegacyAVR\`); dashboard errors go to
`LegacyAVR.UI.log` in the same folder. The red error banner's **Open log folder** button opens it, and its text
can be selected and copied. Settings and profiles: `%APPDATA%\LegacyAVR\`.

If the engine or the dashboard stops responding, **Engine Settings > Emergency reset** closes LegacyAVR,
force-stops every engine process (`AVREngine`) and starts LegacyAVR again; the second button does the same
without restarting. (An engine running as a Windows service needs administrator rights to stop - you are told if
it keeps running.)

### Channel layouts and the subwoofer switch

LegacyAVR processes audio on an internal 5.1 bus. What actually leaves depends on the format:

| Format | Layouts it can send | Without a subwoofer |
|---|---|---|
| Dolby Digital (AC-3) | 2.0, 3.0, 3.1, 5.0, 5.1 | 3.1 upmix → **3.0**, 5.1 → **5.0** |
| Dolby Digital Plus (E-AC-3) | 2.0, 3.0, 3.1, 5.0, 5.1 | 3.1 upmix → **3.0**, 5.1 → **5.0** |
| DTS (FFmpeg encoder) | 2.0, 5.0, 5.1 - no 3.x layout | 5.1 → **5.0**; a 3.0 upmix travels in a 5.0 stream with silent surrounds (the live indicator says so) |
| PCM | the device's format (2.0, 5.1 or 7.1) | the LFE slot of a 5.1 / 7.1 format stays silent |

- **"Dedicated subwoofer" inactive:** LegacyAVR never generates an LFE channel. 3.0, 5.0 and 7.0 sources stay
  without one; the Dynamic upmixes become 3.0 / 5.0. An LFE that the *source* already has (5.1 / 7.1 films and
  games) is **not thrown away**: the LFE switch folds it into the front left and right speakers (120 Hz low-pass,
  -3 dB per speaker so its power is kept, safety limiter against clipping). Set the switch to *Off* to discard
  it instead - the LFE indicator then says "discarded". The LFE trim on the Mixer sets how loud the folded bass is.
- **"Dedicated subwoofer" active:** the same switch controls LFE generation (Auto / Always / Off) for sources
  without an LFE.
- **7.0 / 7.1:** cannot be sent as seven discrete channels. The internal bus and these encoders (AC-3, E-AC-3
  and DTS in FFmpeg) stop at 5.1, so 7.x sources are folded to 5.x (side and back speakers combined at -3 dB)
  and shown as "7.1 → 5.0 fold-down" (or 5.1 with a subwoofer). A 7.1 PCM device receives 5.x with a silent
  back pair, and the dashboard labels it that way.
- **PCM 2.0** has no LFE channel either: the source's LFE is folded into L/R before the stereo downmix, and
  nothing is generated.

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
| LegacyAVR does not start after installing | The .NET 10 Desktop Runtime or Visual C++ Redistributable (x64) is missing: run Setup again and let it install them, or use the links in [Requirements](#requirements). |
| Pre-encoded audio not passed through | The cable must be 48 kHz, Windows volume 100 %, no enhancements, so the bits arrive untouched. |
| Engine or dashboard not responding | *Engine Settings > Emergency reset*. Then check the log (**Open log folder** in the red banner or in Engine Settings). |
| No bass without a subwoofer | Is the LFE switch on *Fold the source's LFE into front L/R*? Only sources with their own LFE have any to fold; without a subwoofer nothing is generated. |

The in-app **Settings guide** covers every setting and readout in more detail.

---

## Project layout

```
engine/            C++23 engine: core/ (DSP, encoder, IEC 61937), win/ (WASAPI, host, IPC), ipc/, app/
ui/                WPF dashboard (LegacyAVR.UI) and its xUnit tests (LegacyAVR.UI.Tests)
tests/             native unit / integration tests and benchmarks
scripts/           build / packaging, vcpkg setup and service scripts (PowerShell) and installer.iss (Inno Setup)
build/             shared MSBuild properties
docs/              ARCHITECTURE.md, images/
Github Releases/   release documentation (README, LICENSE, notices, release notes); -Package writes the
                   installer, zip and checksums here (git-ignored)
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
