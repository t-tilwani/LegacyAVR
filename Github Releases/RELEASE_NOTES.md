# LegacyAVR 1.5.0 - release notes and installation guide

LegacyAVR encodes your PC's surround sound to Dolby Digital, DTS or Dolby Digital Plus in real time, so an
older AV receiver, soundbar or TV connected by optical or HDMI ARC plays real 5.1 instead of stereo.

> **Status:** hobby project, tested by the author on Windows 11 with an AV receiver over HDMI ARC and optical.
> Releases are **not code-signed**, so Windows SmartScreen may warn you the first time (see step 3).

---

## What's new in 1.5.0

- **No subwoofer, no generated LFE.** With "Dedicated subwoofer" inactive, LegacyAVR no longer creates an LFE
  channel: the Dynamic 3.1 upmix is sent as real **3.0** (Dolby Digital / DD+), 5.1 as **5.0**, and 3.0 / 5.0 /
  7.0 sources stay without one. An LFE the source already has is **folded into the front speakers** (120 Hz
  low-pass, -3 dB each so its power is kept, safety limiter against clipping) - or discarded if you choose.
- **One LFE switch, two clear meanings:** with a subwoofer it controls LFE generation; without one it controls
  whether the source's LFE is folded into the fronts. Labels, Settings guide, status indicators and the live
  processing text say which, e.g. "Stereo → 3.0 upmix" (no longer "3.1" without a subwoofer),
  "5.1 → 5.0 (no LFE channel out) | LFE → front L/R".
- **Honest layouts.** Dolby Digital and DD+ now send real 3.0 / 3.1 for the 3.x upmix. DTS has no 3.x layout, so
  the dashboard says "in a 5.0 stream, surrounds silent". 7.0 / 7.1 cannot be sent as seven channels (the bus and
  encoders stop at 5.1): they are folded to 5.x and labelled as a fold-down; a 7.1 PCM device is shown as
  "5.1 in 7.1". PCM 2.0 folds the source LFE before the stereo downmix instead of dropping it.
- **Error banner:** the text can be selected and copied, and an **Open log folder** button opens the engine log.
- **Emergency reset** (Engine Settings): closes LegacyAVR, force-stops the engine processes and restarts -
  or just closes and stops.
- **Custom EQ indicator** in the live output card, and the EQ dialog's channel box now shows the channel name.
- **Quick save:** a Save button appears in the footer when the settings differ from the active profile
  (overwrites it, or asks for a name and creates a new profile for this output).
- New-install defaults: **Game Mode off**, **LFE generation while gaming on**. Upgrades keep your settings.
- The **Setup guide** is now the last tab. Many smaller fixes (see the change log).

## What's new in 1.4.0

- **Profiles** replace presets and device profiles. A profile stores every setting (output device, encoder,
  processing, speakers, trims, mutes, bass, LFE, custom EQ), can be imported/exported, and can be applied to
  another output. Your older presets and device profiles are converted automatically on first start.
- **Custom EQ** - an optional five-band parametric EQ for each speaker, opened from the EQ button on each
  mixer strip, with an "audition" (solo) mode and automatic protection against clipping.
- **Default output** button, so LegacyAVR always picks your AVR / TV output at start-up (Windows' own default
  device is not changed). A disconnected output is shown as "(not connected)" instead of a blank box.
- **Resume after silence** for LFE generation now takes 1-300 seconds (10 recommended).
- New **Upmixing & Bass** and **Engine Settings** tabs, a reset button on every slider, and a layout that
  stays tidy on large or maximised windows.

The full version history is in the repository's change log.

---

## Before you start - requirements

- Windows 11, 64-bit.
- **VB-Audio Virtual Cable (VB-CABLE)**, free / donationware from VB-Audio Software:
  <https://vb-audio.com/Cable/> (not included with LegacyAVR).
- An HDMI ARC or optical / coaxial output to an AVR, soundbar or TV that decodes Dolby Digital (or DTS).
  Dolby Digital Plus needs HDMI; optical cannot carry it.
- The **.NET 10 Desktop Runtime (x64)** and the **Microsoft Visual C++ Redistributable (x64)**. The installer
  checks for both and offers to install them: the Visual C++ Redistributable is included in Setup, the .NET
  runtime is downloaded from Microsoft. (Portable zip: install them yourself -
  <https://dotnet.microsoft.com/download/dotnet/10.0> and <https://aka.ms/vc14/vc_redist.x64.exe>.)

---

## 1. Download

From the **Assets** section of this release, download:

- `LegacyAVR_v1.5.0_Setup.exe` - the installer (recommended).
- `SHA256SUMS.txt` - checksums used to verify the download.
- Optional: `LegacyAVR_v1.5.0_Portable.zip` - the same program without an installer, for advanced users.

Only download LegacyAVR from this project's GitHub Releases page.

## 2. Verify the download (SHA256)

Checking the checksum confirms the file was not corrupted or altered. In **PowerShell**, in the folder where
you saved the files:

```powershell
Get-FileHash .\LegacyAVR_v1.5.0_Setup.exe -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

The `Hash` value printed by the first command must match the value next to the file name in `SHA256SUMS.txt`
exactly (upper/lower case does not matter). To compare automatically:

```powershell
$file     = 'LegacyAVR_v1.5.0_Setup.exe'
$expected = ((Get-Content .\SHA256SUMS.txt) -match [regex]::Escape($file) | Select-Object -First 1).Split(' ')[0]
$actual   = (Get-FileHash ".\$file" -Algorithm SHA256).Hash
if ($actual -ieq $expected) { 'OK - checksum matches' } else { 'MISMATCH - do not run this file' }
```

Command Prompt alternative: `certutil -hashfile LegacyAVR_v1.5.0_Setup.exe SHA256`

**If the checksums do not match, delete the file and do not run it.**

> For maintainers: `.\scripts\build.ps1 -Package` creates the installer, the zip and `SHA256SUMS.txt` in
> `Github Releases\`. Upload all three to the release.

## 3. Install

1. Run `LegacyAVR_v1.5.0_Setup.exe`. Releases are not code-signed, so if SmartScreen shows "Windows protected
   your PC", choose **More info -> Run anyway** only if you verified the checksum in step 2.
2. Follow the prompts. Setup installs to `C:\Program Files\LegacyAVR`, adds **LegacyAVR** to the Start menu
   and, if you tick it, the desktop.
3. If the Visual C++ Redistributable or the .NET 10 Desktop Runtime is missing, Setup lists it and offers
   **Install them now** (recommended). The Visual C++ Redistributable is included in Setup; the .NET runtime is
   downloaded from Microsoft and checked for Microsoft's digital signature before it runs.
4. *Advanced, optional:* **Run the audio engine as a Windows service** starts encoding at boot, before anyone
   signs in. Leave it unticked unless you need that.
5. The last page has a button to download **VB-CABLE**, and one to open the Sound control panel for step 4 below.

**Portable zip instead:** right-click the zip -> **Properties** -> tick **Unblock** (if shown) -> OK, extract it
to a folder you own (e.g. `%LOCALAPPDATA%\Programs\LegacyAVR`), keep all files together, and run
`LegacyAVR.UI.exe`.

Both contain `LegacyAVR.UI.exe` (dashboard), `AVREngine.exe` (audio engine, started automatically),
`AVREngineService.exe` (optional service), the FFmpeg DLLs, `scripts\install-service.ps1`, `README.txt`, and
`licenses\` (LegacyAVR's MIT licence, third-party notices, the FFmpeg LGPL licence and FFmpeg source
information).

## 4. Route the audio (one-time Windows setup)

Open the classic Sound control panel (`Win + R` -> `mmsys.cpl`, or the button in LegacyAVR's **Setup guide**
tab):

1. **Playback -> CABLE Input -> Configure -> 7.1 Surround**, tick every speaker, finish the wizard.
2. **CABLE Input -> Properties -> Advanced:** 24- or 32-bit, **48000 Hz**. Untick *Enable audio enhancements*;
   turn Spatial Sound off for this device.
3. Right-click **CABLE Input -> Set as Default Device**. Games and apps now play 7.1 into the cable.
4. **Your HDMI / optical device -> Properties -> Advanced:** tick both *Allow applications to take exclusive
   control* boxes.

```
 Games / apps  -->  CABLE Input (Windows default, 7.1)  -->  LegacyAVR  -->  HDMI / optical  -->  AVR
```

## 5. First run in LegacyAVR

1. **Output & Profiles** tab: Input = *CABLE Input*; Output = your HDMI / optical device, then press **Default**.
2. Format = **Dolby Digital 640 kbps** (works with almost every AVR), Mode = **WASAPI Exclusive**.
3. No subwoofer? In **Upmixing & Bass**, set *Dedicated subwoofer* to inactive and set the AVR's subwoofer
   to "None".
4. Your AVR's display should show **Dolby Digital** (or DTS).
5. Run the **Diagnostics** wizard to check every speaker, then **Save as new profile**.

Closing the window keeps LegacyAVR running in the system tray; use **Exit** in the tray menu to quit.
**Engine Settings** has *Start LegacyAVR with Windows*.

## Upgrading from an earlier version

Run the newer Setup. If LegacyAVR is running, Setup closes it first (you can cancel), and your settings in
`%APPDATA%\LegacyAVR\` and `%LOCALAPPDATA%\LegacyAVR\` are kept. If you used a zip or a self-built copy
before, exit it first (tray -> Exit) and switch *Start LegacyAVR with Windows* off and on again in the new
version so it points at the installed copy. Old presets are converted to profiles automatically (the original
files are left untouched).

## Uninstall

1. **Settings -> Apps -> Installed apps -> LegacyAVR -> Uninstall** (or *Uninstall LegacyAVR* in the Start
   menu). It stops LegacyAVR, removes the service if it was installed, and asks whether to delete your settings,
   profiles and logs as well.
2. Set your normal speakers / HDMI device back as the Windows default playback device. VB-CABLE has its own
   uninstaller.

Portable zip: tray -> **Exit**, untick *Start LegacyAVR with Windows*, delete the folder, and optionally
`%APPDATA%\LegacyAVR` and `%LOCALAPPDATA%\LegacyAVR`.

## Known limitations

- **Streaming services** (Netflix, Disney+, Prime Video, Crunchyroll) often send only stereo to Windows,
  especially in browsers, because of DRM. LegacyAVR cannot add channels that were never sent (the upmixer can
  create derived surround) and does not bypass DRM.
- About 70-85 ms of delay before the AVR is normal for live Dolby / DTS encoding.
- Dolby Digital Plus works only over HDMI to devices that list it; optical cannot carry it.
- The installer and programs are not code-signed (SmartScreen may warn); not affiliated with or certified by
  Dolby, DTS, VB-Audio or Microsoft.

## Getting help

Open an issue using the **Bug report** template. Logs are in `%LOCALAPPDATA%\LegacyAVR\AVREngine.log` -
please remove your Windows user name and any personal device names before attaching them.
