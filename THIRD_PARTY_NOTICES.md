# Third-party notices

LegacyAVR's own source code is licensed under the MIT License (see [LICENSE](LICENSE)). It is built with, and
at run time uses, the third-party components below, each under its own licence. This file explains what each
component is, how LegacyAVR uses it, and what a **binary release** of LegacyAVR must do to respect its licence.

> This document describes the project's configuration and the licence terms as the maintainer understands
> them. It is not legal advice.

| Component | Licence | How it is used | Shipped in a binary release? |
|---|---|---|---|
| FFmpeg `libavcodec`, `libavutil`, `libswresample` | GNU LGPL v2.1 or later | Audio encoding and resampling, **dynamically linked** DLLs | Yes - as separate, replaceable DLLs |
| Microsoft Windows SDK / OS components (WASAPI, MMDevice, AVRT, SAPI, Shell, COM) | Windows SDK licence terms | Import libraries at build time; system DLLs at run time | No - part of Windows |
| Microsoft Visual C++ runtime | Visual Studio licence ("Distributable Code") | C++ runtime of the engine | Not bundled by default; users install the official redistributable |
| .NET 10 (incl. WPF, Windows Forms) | MIT | Dashboard runtime | No - framework-dependent; users install the .NET 10 Desktop Runtime |
| vcpkg | MIT | Build tool: builds FFmpeg | No |
| CMake | BSD 3-Clause | Optional build tool | No |
| xUnit, xunit.runner.visualstudio | Apache 2.0 | .NET unit tests only | No |
| Microsoft.NET.Test.Sdk | MIT | .NET unit tests only | No |
| VB-Audio Virtual Cable (VB-CABLE) | VB-Audio Software licence (donationware) | Separate product the user installs | **No - never bundled** |

---

## 1. FFmpeg - GNU LGPL v2.1 or later

**What is used.** Only the FFmpeg libraries `libavcodec` (the AC-3, E-AC-3 and DTS ("dca") encoders, all
native FFmpeg code) and `libswresample`, plus `libavutil` which both depend on. FFmpeg is © the FFmpeg
developers - <https://ffmpeg.org>.

**Why the LGPL (not the GPL) applies - configuration evidence in this repository:**

- `vcpkg.json` requests `ffmpeg` with `"default-features": false` and only the features `avcodec` and
  `swresample`. The vcpkg features that would switch FFmpeg to the GPL or to a non-redistributable
  configuration (`gpl`, `version3`, `nonfree`, and GPL-only external libraries such as `x264` / `x265`) are
  **not** enabled.
- The build uses the vcpkg triplet `x64-windows`, which builds FFmpeg as **shared libraries** (DLLs).
  LegacyAVR links against their import libraries only; no FFmpeg code is compiled into LegacyAVR's own
  executables. A user can replace the FFmpeg DLLs with a compatible build of their own.
- LegacyAVR does not modify FFmpeg's source code. (vcpkg applies its own build patches from the FFmpeg port at
  the pinned `builtin-baseline`; those are part of the corresponding source below.)

**How to confirm a build is LGPL** (do this before every binary release):

1. After building, open `vcpkg_installed\x64-windows\share\ffmpeg\` and check the port's licence/copyright
   file and build configuration: it must not contain `--enable-gpl`, `--enable-version3` or `--enable-nonfree`.
2. Optionally, a small check program can print `avcodec_license()`, which must return
   `LGPL version 2.1 or later`.
3. Note the exact FFmpeg version (`vcpkg list ffmpeg`) for the release notes.

**What every binary release must include to comply with the LGPL v2.1:**

- [ ] The FFmpeg DLLs as separate files (`avcodec-*.dll`, `avutil-*.dll`, `swresample-*.dll`) - never
      statically linked or merged into LegacyAVR's executables.
- [ ] The full text of the GNU LGPL v2.1 (e.g. `licenses/FFmpeg-LGPL-2.1.txt`) and this notices file.
- [ ] A prominent notice that the release uses FFmpeg under the LGPL, with a link to <https://ffmpeg.org>.
- [ ] The **corresponding source** of the exact FFmpeg build: the FFmpeg version number plus a link to (or a
      copy of) its source tarball, and the vcpkg port files/patches at the commit given in `vcpkg.json`
      (`builtin-baseline`), so anyone can rebuild the same libraries. Keep that source available for as long
      as the binaries are offered.
- [ ] No terms that forbid users from modifying the FFmpeg libraries or reverse-engineering LegacyAVR for the
      purpose of debugging such modifications (the MIT License does not forbid this).

LGPL v2.1 text: <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html> ·
FFmpeg legal page: <https://ffmpeg.org/legal.html>

---

## 2. Microsoft components

**Windows SDK and operating-system components.** The engine uses Windows APIs (WASAPI audio, MMDevice, AVRT
thread scheduling, Speech API for the channel-ID voice, Shell, COM, Windows services). These are provided by
Windows itself; LegacyAVR only links against the import libraries from the Windows SDK at build time.
**Nothing from the Windows SDK is redistributed**, so no redistribution rights are needed.

**Microsoft Visual C++ runtime.** The engine needs the Visual C++ runtime DLLs (for example `vcruntime140.dll`,
`msvcp140.dll`). The Visual Studio licence permits redistributing the files listed in its "Distributable Code"
/ `redist.txt`, unmodified, together with your application. LegacyAVR releases **do not bundle** them by
default; users install the official **Microsoft Visual C++ Redistributable (x64)**:
<https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist>. If a future release does bundle them,
check the redistribution terms of the Visual Studio edition used to build it.

**.NET 10, WPF and Windows Forms** are open source under the MIT License (<https://github.com/dotnet>).
The dashboard is framework-dependent: users install the **.NET 10 Desktop Runtime (x64)**, so the runtime is
not redistributed.

---

## 3. Build and test tools (not redistributed)

- **vcpkg** - MIT License, © Microsoft Corporation. <https://github.com/microsoft/vcpkg>
- **CMake** - BSD 3-Clause, © Kitware, Inc. and contributors. <https://cmake.org>
- **xUnit.net** and **xunit.runner.visualstudio** - Apache License 2.0, © .NET Foundation and contributors.
  <https://xunit.net>
- **Microsoft.NET.Test.Sdk** - MIT License, © Microsoft Corporation.

These are used only to build and test LegacyAVR and are not part of a release package.

---

## 4. VB-Audio Virtual Cable - credit

LegacyAVR is designed to capture audio from **VB-Audio Virtual Cable (VB-CABLE)** or **Hi-Fi Cable**, made by
**VB-Audio Software** (Vincent Burel) - <https://vb-audio.com/Cable/>. Many thanks for making it available.

VB-CABLE is a separate product with its own licence (donationware). It is **not included** in this repository
or in LegacyAVR releases, and LegacyAVR is not affiliated with or endorsed by VB-Audio Software. Download it
only from the official website and follow its licence terms.

---

## 5. Codecs, patents and trademarks

- LegacyAVR uses FFmpeg's own open-source encoders. It contains no code licensed from Dolby or DTS and is not
  a Dolby- or DTS-certified product.
- The core AC-3 (Dolby Digital) patents are widely reported to have expired. The patent status of other
  formats (such as E-AC-3 and DTS) can differ by country and has not been verified by this project. Users and
  redistributors are responsible for complying with the laws that apply to them.
- Dolby, Dolby Digital, Dolby Digital Plus and Dolby Digital Live are trademarks of Dolby Laboratories. DTS is
  a trademark of DTS, Inc. / Xperi. Windows is a trademark of Microsoft Corporation. VB-CABLE and VB-Audio are
  trademarks of VB-Audio Software. Netflix, Disney+, Prime Video and Crunchyroll are trademarks of their
  respective owners. They are mentioned only to describe compatibility.

---

## 6. Third-party browser add-ons (mentioned, not included)

The in-app Setup guide mentions an optional third-party Microsoft Edge add-on that requests 5.1 audio from one
streaming service. It is made by an independent developer, is not part of LegacyAVR, is not distributed with
it and is not endorsed by this project.
