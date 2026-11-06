# LegacyAVR 1.4.0 - release notes and installation guide

LegacyAVR encodes your PC's surround sound to Dolby Digital, DTS or Dolby Digital Plus in real time, so an
older AV receiver, soundbar or TV connected by optical or HDMI ARC plays real 5.1 instead of stereo.

> **Status:** hobby project, tested by the author on Windows 11 with an AV receiver over HDMI ARC and optical.
> Releases are **not code-signed**, so Windows SmartScreen may warn you the first time (see step 3).

---

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
- **.NET 10 Desktop Runtime (x64):** <https://dotnet.microsoft.com/download/dotnet/10.0>
- **Microsoft Visual C++ Redistributable (x64):** <https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist>
- **VB-Audio Virtual Cable (VB-CABLE)**, free / donationware from VB-Audio Software:
  <https://vb-audio.com/Cable/> (not included with LegacyAVR).
- An HDMI ARC or optical / coaxial output to an AVR, soundbar or TV that decodes Dolby Digital (or DTS).
  Dolby Digital Plus needs HDMI; optical cannot carry it.

---

## 1. Download

From the **Assets** section of this release, download:

- `LegacyAVR-1.4.0-win-x64.zip` - the program.
- `SHA256SUMS.txt` - checksums used to verify the download.

Only download LegacyAVR from this project's GitHub Releases page.

## 2. Verify the download (SHA256)

Checking the checksum confirms the file was not corrupted or altered. In **PowerShell**, in the folder where
you saved the files:

```powershell
Get-FileHash .\LegacyAVR-1.4.0-win-x64.zip -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

The `Hash` value printed by the first command must match the value next to the file name in `SHA256SUMS.txt`
exactly (upper/lower case does not matter). To compare automatically:

```powershell
$expected = (Select-String -Path .\SHA256SUMS.txt -Pattern 'LegacyAVR-1.4.0-win-x64.zip').Line.Split(' ')[0]
$actual   = (Get-FileHash .\LegacyAVR-1.4.0-win-x64.zip -Algorithm SHA256).Hash
if ($actual -ieq $expected) { 'OK - checksum matches' } else { 'MISMATCH - do not run this file' }
```

Command Prompt alternative: `certutil -hashfile LegacyAVR-1.4.0-win-x64.zip SHA256`

**If the checksums do not match, delete the file and do not run it.**

> For maintainers: create `SHA256SUMS.txt` when publishing a release with
> `Get-FileHash *.zip -Algorithm SHA256 | ForEach-Object { "$($_.Hash.ToLower())  $(Split-Path $_.Path -Leaf)" } | Set-Content SHA256SUMS.txt`

## 3. Install

1. Right-click the zip -> **Properties** -> tick **Unblock** (if shown) -> OK.
2. Extract it to a folder you own, for example `%LOCALAPPDATA%\Programs\LegacyAVR`. Keep all files together:
   the program needs the FFmpeg DLLs next to it.
3. Run `LegacyAVR.UI.exe`. If SmartScreen shows "Windows protected your PC", choose **More info -> Run
   anyway** only if you verified the checksum in step 2.

The package contains `LegacyAVR.UI.exe` (dashboard), `AVREngine.exe` (audio engine, started automatically),
`AVREngineService.exe` (optional Windows service), the FFmpeg DLLs, `scripts\install-service.ps1`, and the
licence files (`LICENSE`, `THIRD_PARTY_NOTICES.md`, the GNU LGPL v2.1 text for FFmpeg).

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

Exit the old version completely first (tray -> Exit): the 1.4 dashboard and engine cannot talk to older
versions. Your settings in `%APPDATA%\LegacyAVR\` and `%LOCALAPPDATA%\LegacyAVR\` are kept, and old presets
are converted to profiles automatically (the original files are left untouched).

## Uninstall

1. Tray icon -> **Exit**. If you installed the service: elevated PowerShell ->
   `.\scripts\install-service.ps1 -Uninstall`.
2. Untick *Start LegacyAVR with Windows* (or remove it from Task Manager -> Startup apps), then delete the
   program folder.
3. Optional: delete `%APPDATA%\LegacyAVR` and `%LOCALAPPDATA%\LegacyAVR` (settings, profiles, logs).
4. Set your normal speakers / HDMI device back as the Windows default playback device.

## Known limitations

- **Streaming services** (Netflix, Disney+, Prime Video, Crunchyroll) often send only stereo to Windows,
  especially in browsers, because of DRM. LegacyAVR cannot add channels that were never sent (the upmixer can
  create derived surround) and does not bypass DRM.
- About 70-85 ms of delay before the AVR is normal for live Dolby / DTS encoding.
- Dolby Digital Plus works only over HDMI to devices that list it; optical cannot carry it.
- Not code-signed; not affiliated with or certified by Dolby, DTS, VB-Audio or Microsoft.

## Getting help

Open an issue using the **Bug report** template. Logs are in `%LOCALAPPDATA%\LegacyAVR\AVREngine.log` -
please remove your Windows user name and any personal device names before attaching them.
