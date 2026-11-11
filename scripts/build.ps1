<#
.SYNOPSIS
  Builds LegacyAVR with MSBuild (engine, service, tests and - if the .NET 10 SDK is present - the WPF UI).
  Works with the full Visual Studio 2026 IDE *or* the "Build Tools for Visual Studio 2026".
.EXAMPLE
  .\scripts\build.ps1                       # Release|x64
  .\scripts\build.ps1 -RunTests
  .\scripts\build.ps1 -NativeOnly           # C++ only (no .NET SDK needed)
  .\scripts\build.ps1 -Latency              # measure pipeline latency per margin setting
  .\scripts\build.ps1 -Package              # clean Release build + tests, then the installer and the zip
  .\scripts\build.ps1 -Package -SkipTests   # same, without running the tests (not recommended)

  -Package needs Inno Setup 6.3 or later (Inno Setup 7 is fine): https://jrsoftware.org/isdl.php
  or:  winget install JRSoftware.InnoSetup
  It writes to "Github Releases\":
    LegacyAVR_v<version>_Setup.exe     installer (recommended for users)
    LegacyAVR_v<version>_Portable.zip  the same files without an installer
    SHA256SUMS.txt                     checksums to publish with the release
#>
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [switch] $RunTests,
    [switch] $Bench,
    [switch] $Latency,
    [switch] $NativeOnly,
    [switch] $Package,
    [switch] $SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if ($Package) {
    if ($Configuration -ne 'Release') { throw '-Package always builds Release. Remove -Configuration Debug.' }
    if ($NativeOnly) { throw '-Package needs the dashboard (.NET) build. Remove -NativeOnly.' }
    # Never ship an untested build unless explicitly asked to.
    if (-not $SkipTests) { $RunTests = $true }
}

# ---------------------------------------------------------------- locate Inno Setup (only for -Package)
# The version is NOT read here: ISCC.exe's own file version is unreliable. ISCC prints its real version
# ("Compiler engine version") and scripts\installer.iss stops with a clear message if it is older than 6.3.
function Find-ISCC {
    $dirs = New-Object System.Collections.Generic.List[string]
    # 1) Inno Setup's own uninstall entries (per-machine and per-user / winget installs), newest major first.
    $uninstallKeys = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup*_is1',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup*_is1',
                     'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup*_is1'
    $entries = @(foreach ($key in $uninstallKeys) { Get-ItemProperty -Path $key -ErrorAction SilentlyContinue })
    foreach ($e in ($entries | Sort-Object PSChildName -Descending)) {
        if ($e.InstallLocation) { $dirs.Add($e.InstallLocation) }
    }
    # 2) The usual install folders ("Inno Setup 7" sorts before "Inno Setup 6").
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
        if ($base -and (Test-Path $base)) {
            Get-ChildItem -Path $base -Directory -Filter 'Inno Setup*' -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | ForEach-Object { $dirs.Add($_.FullName) }
        }
    }
    foreach ($d in $dirs) {
        $exe = Join-Path $d 'ISCC.exe'
        if (Test-Path $exe) { return (Get-Item -LiteralPath $exe).FullName }
    }
    # 3) Anything on PATH.
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$iscc = $null
if ($Package) {
    # Checked before the (long) build so a missing tool is reported straight away.
    $iscc = Find-ISCC
    if (-not $iscc) {
        throw ("Inno Setup was not found. Install Inno Setup 6.3 or later from https://jrsoftware.org/isdl.php " +
               "(or run: winget install JRSoftware.InnoSetup), then re-run with -Package.")
    }
    Write-Host "Inno Setup: $iscc" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- locate MSBuild
function Find-MSBuild {
    # 1) Developer PowerShell already has MSBuild on PATH.
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # 2) vswhere. '-products *' is required, otherwise Build Tools installs are ignored.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        foreach ($pattern in 'MSBuild\**\Bin\amd64\MSBuild.exe', 'MSBuild\**\Bin\MSBuild.exe') {
            $found = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find $pattern |
                     Select-Object -First 1
            if ($found) { return $found }
        }
    }
    return $null
}

$msbuild = Find-MSBuild
if (-not $msbuild) {
    throw "MSBuild not found. Install 'Build Tools for Visual Studio 2026' (or the IDE) with the 'Desktop development with C++' workload, then run this from 'Developer PowerShell for VS 2026'."
}
Write-Host "MSBuild: $msbuild" -ForegroundColor DarkGray

# ---------------------------------------------------------------- .NET SDK check (UI)
$hasNet10 = $false
if (Get-Command dotnet -ErrorAction SilentlyContinue) {
    $hasNet10 = [bool](dotnet --list-sdks 2>$null | Where-Object { $_ -match '^10\.' })
}
if ($Package -and -not $hasNet10) {
    throw "-Package needs the .NET 10 SDK to build the dashboard. Install it (or the '.NET desktop build tools' workload) and re-run."
}
if (-not $NativeOnly -and -not $hasNet10) {
    Write-Warning (".NET 10 SDK not found - building the C++ engine and tests only (the WPF UI is skipped).`n" +
                   "To build the UI: add the '.NET desktop build tools' workload in Visual Studio Installer, or install the .NET 10 SDK, then re-run.")
    $NativeOnly = $true
}

# ---------------------------------------------------------------- build
$bin = Join-Path $root "bin\$Configuration"
if ($Package -and (Test-Path $bin)) {
    # A package must contain only what this build produced (no stale DLLs from older builds).
    Write-Host "Cleaning $bin for a fresh package build..." -ForegroundColor Cyan
    try { Remove-Item $bin -Recurse -Force }
    catch { throw "Could not clean $bin - is LegacyAVR running from it? Exit it (tray -> Exit) and retry. ($($_.Exception.Message))" }
}

$common = @('/m', '/nologo', '/v:minimal', "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:SolutionDir=$root\")

if ($NativeOnly) {
    $projects = @('engine\AVREngine.vcxproj', 'engine\AVREngineService.vcxproj', 'tests\LegacyAVR.Tests.vcxproj')
    foreach ($p in $projects) {
        Write-Host "Building $p ($Configuration|x64)..." -ForegroundColor Cyan
        & $msbuild (Join-Path $root $p) @common
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $p" }
    }
} else {
    Write-Host "Restoring + building LegacyAVR.sln ($Configuration|x64)..." -ForegroundColor Cyan
    & $msbuild (Join-Path $root 'LegacyAVR.sln') /restore @common
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}

Write-Host "Output: $bin" -ForegroundColor Green

# ---------------------------------------------------------------- tests
if ($RunTests) {
    $report = Join-Path $bin 'native-test-failures.txt'
    & (Join-Path $bin 'LegacyAVR.Tests.exe') --report $report
    if ($LASTEXITCODE -ne 0) {
        # v1.2.0: name the failing tests instead of only counting them.
        $names = if (Test-Path $report) { (Get-Content $report) -join "`n  " } else { '(see [ FAIL ] lines above)' }
        throw ("Native tests failed ($LASTEXITCODE failures):`n  " + $names)
    }
    if (-not $NativeOnly) {
        dotnet test (Join-Path $root 'ui\LegacyAVR.UI.Tests\LegacyAVR.UI.Tests.csproj') -c $Configuration -p:Platform=x64
        if ($LASTEXITCODE -ne 0) { throw "UI tests failed" }
    }
}
if ($Bench) {
    & (Join-Path $bin 'LegacyAVR.Tests.exe') --bench
}
if ($Latency) {
    & (Join-Path $bin 'LegacyAVR.Tests.exe') --latency
}

# ================================================================ packaging (-Package)
if (-not $Package) { return }

$releaseDir = Join-Path $root 'Github Releases'
$stage      = Join-Path $root 'out\package\LegacyAVR'

# ---------------------------------------------------------------- version (single source: the dashboard project)
$csproj  = Join-Path $root 'ui\LegacyAVR.UI\LegacyAVR.UI.csproj'
$version = (Select-Xml -Path $csproj -XPath '//Version' | Select-Object -First 1).Node.InnerText
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Could not read <Version> (x.y.z) from $csproj (found '$version')." }
$engineVi = (Get-Item (Join-Path $bin 'AVREngine.exe')).VersionInfo
$engineVersion = '{0}.{1}.{2}' -f $engineVi.FileMajorPart, $engineVi.FileMinorPart, $engineVi.FileBuildPart
if ($engineVersion -ne $version) {
    Write-Warning "Version mismatch: dashboard $version, AVREngine.exe $engineVersion (engine\app\AVREngine\AVREngine.rc). Packaging as $version."
}
Write-Host "Packaging LegacyAVR $version" -ForegroundColor Cyan

# ---------------------------------------------------------------- FFmpeg licence check (must be the LGPL build)
$vcpkgDir  = Join-Path $root 'vcpkg_installed'
$statusFile = Join-Path $vcpkgDir 'vcpkg\status'
$ffShare   = Join-Path $vcpkgDir 'x64-windows\share\ffmpeg'
if (-not (Test-Path $statusFile)) { throw "vcpkg status file not found: $statusFile. Run .\scripts\setup-vcpkg.ps1 and build once." }
$ffmpegVersion = $null
foreach ($para in ((Get-Content $statusFile -Raw) -split "(?:\r?\n){2,}")) {
    if ($para -notmatch '(?m)^Package: ffmpeg\s*$' -or $para -notmatch '(?m)^Status: install ok installed') { continue }
    if ($para -match '(?m)^Feature: (gpl|version3|nonfree)\s*$') {
        throw "FFmpeg was built with the '$($Matches[1])' feature, which changes its licence. Release packages must use the LGPL build (see vcpkg.json and THIRD_PARTY_NOTICES.md)."
    }
    if ($para -match '(?m)^Version: (\S+)') {
        $ffmpegVersion = $Matches[1]
        if ($para -match '(?m)^Port-Version: (\d+)') { $ffmpegPortVersion = $Matches[1] } else { $ffmpegPortVersion = '0' }
    }
}
if (-not $ffmpegVersion) { throw "FFmpeg is not listed as installed in $statusFile." }
$ffCopyright = Join-Path $ffShare 'copyright'
if (-not (Test-Path $ffCopyright)) { throw "FFmpeg licence file not found: $ffCopyright" }
Write-Host "FFmpeg $ffmpegVersion (vcpkg port-version $ffmpegPortVersion): LGPL configuration confirmed" -ForegroundColor DarkGray

# ---------------------------------------------------------------- stage the release files
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$null = New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'licenses'), (Join-Path $stage 'scripts')

$appFiles = 'LegacyAVR.UI.exe', 'LegacyAVR.UI.dll', 'LegacyAVR.UI.runtimeconfig.json', 'LegacyAVR.UI.deps.json',
            'AVREngine.exe', 'AVREngineService.exe'
foreach ($f in $appFiles) {
    $p = Join-Path $bin $f
    if (-not (Test-Path $p)) { throw "Missing build output: $p" }
    Copy-Item $p $stage
}
$ffmpegDlls = @()
foreach ($lib in 'avcodec', 'avutil', 'swresample') {
    $dll = @(Get-ChildItem -Path $bin -Filter "$lib-*.dll")
    if ($dll.Count -ne 1) { throw "Expected exactly one $lib-*.dll in $bin, found $($dll.Count)." }
    Copy-Item $dll[0].FullName $stage
    $ffmpegDlls += $dll[0].Name
}
# Any other DLL in the output is an unknown run-time dependency: stop rather than ship a package that may not start.
$known = @('LegacyAVR.UI.dll') + $ffmpegDlls
$unknown = @(Get-ChildItem -Path $bin -Filter '*.dll' | Where-Object { $known -notcontains $_.Name })
if ($unknown.Count) {
    throw ("Unexpected DLLs in ${bin}: " + (($unknown | ForEach-Object Name) -join ', ') +
           ". If they are needed at run time, add them to `$appFiles in scripts\build.ps1.")
}

Copy-Item (Join-Path $root 'scripts\install-service.ps1') (Join-Path $stage 'scripts')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'licenses\LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $stage 'licenses\THIRD_PARTY_NOTICES.md')
Copy-Item $ffCopyright (Join-Path $stage 'licenses\FFmpeg-LICENSE.txt')
$spdx = Join-Path $ffShare 'vcpkg.spdx.json'
if (Test-Path $spdx) { Copy-Item $spdx (Join-Path $stage 'licenses\FFmpeg-vcpkg.spdx.json') }

$baseline = (Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
$ffSource = @"
FFmpeg in LegacyAVR $version
============================

LegacyAVR uses the FFmpeg libraries libavcodec, libavutil and libswresample, version $ffmpegVersion,
under the GNU Lesser General Public License v2.1 or later (full text: FFmpeg-LICENSE.txt).
They are the separate files $($ffmpegDlls -join ', ') and can be replaced with a
compatible build of your own.

Corresponding source code
  FFmpeg $ffmpegVersion source:   https://ffmpeg.org/releases/ffmpeg-$ffmpegVersion.tar.xz
  FFmpeg project / legal:  https://ffmpeg.org  /  https://ffmpeg.org/legal.html
  Build recipe (vcpkg port "ffmpeg", port-version $ffmpegPortVersion, features avcodec + swresample only,
  triplet x64-windows = shared DLLs), including vcpkg's build patches:
                           https://github.com/microsoft/vcpkg/tree/$baseline/ports/ffmpeg
  LegacyAVR source:        https://github.com/T-Tilwani/LegacyAVR

FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project.
"@
Set-Content -Path (Join-Path $stage 'licenses\FFmpeg-SOURCE.txt') -Value $ffSource -Encoding UTF8

$readme = @"
LegacyAVR $version
==================

Real-time Dolby Digital / DTS / Dolby Digital Plus encoding for Windows 11, so an older AV receiver,
soundbar or TV on optical or HDMI ARC plays real 5.1 surround.

Start:      LegacyAVR.UI.exe (the dashboard starts the audio engine itself)
Needs:      .NET 10 Desktop Runtime (x64)          https://dotnet.microsoft.com/download/dotnet/10.0
            Microsoft Visual C++ Redistributable   https://aka.ms/vc14/vc_redist.x64.exe
            VB-CABLE virtual audio device          https://vb-audio.com/Cable/
            (The installer, LegacyAVR_v${version}_Setup.exe, installs the two Microsoft parts when needed.)
Settings:   %APPDATA%\LegacyAVR and %LOCALAPPDATA%\LegacyAVR (log: %LOCALAPPDATA%\LegacyAVR\AVREngine.log)
Service:    optional - elevated PowerShell: .\scripts\install-service.ps1   (remove: -Uninstall)
Licences:   licenses\ (LegacyAVR: MIT; FFmpeg: LGPL v2.1+, see FFmpeg-SOURCE.txt)
Help:       the Setup Guide and Settings Guide tabs in the dashboard, and
            https://github.com/T-Tilwani/LegacyAVR
"@
Set-Content -Path (Join-Path $stage 'README.txt') -Value $readme -Encoding UTF8

# ---------------------------------------------------------------- dependency information for the installer
# Microsoft's Visual C++ Redistributable from the Visual Studio that built the engine. Setup embeds it and
# runs it only when the PC's runtime is missing or older, so the runtime always matches the build.
function Find-VCRedist {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
          Select-Object -First 1
    if (-not $vs) { return $null }
    $candidates = @()
    $defaultTxt = Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt'
    if (Test-Path $defaultTxt) {
        $v = (Get-Content $defaultTxt -TotalCount 1).Trim()
        $candidates += Join-Path $vs "VC\Redist\MSVC\$v\vc_redist.x64.exe"
    }
    $candidates += @(Get-ChildItem -Path (Join-Path $vs 'VC\Redist\MSVC') -Filter 'vc_redist.x64.exe' -Recurse -ErrorAction SilentlyContinue |
                     Sort-Object { $_.VersionInfo.FileMajorPart }, { $_.VersionInfo.FileMinorPart }, { $_.VersionInfo.FileBuildPart } -Descending |
                     ForEach-Object FullName)
    foreach ($c in $candidates) {
        if (-not (Test-Path $c)) { continue }
        $vi = (Get-Item -LiteralPath $c).VersionInfo
        if ($vi.FileMajorPart -ne 14) { continue }
        return [pscustomobject]@{ Path = $c; Version = ('{0}.{1}.{2}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart) }
    }
    return $null
}

function Get-DotNetDesktopRuntimeUrl {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    foreach ($metaUrl in 'https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json',
                         'https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/10.0/releases.json') {
        try {
            $meta = Invoke-RestMethod -Uri $metaUrl -TimeoutSec 30
            foreach ($rel in $meta.releases) {
                $file = $rel.windowsdesktop.files | Where-Object { $_.name -eq 'windowsdesktop-runtime-win-x64.exe' } |
                        Select-Object -First 1
                if ($file -and $file.url -match '^https://[^/]+\.(microsoft\.com|windows\.net)/') {
                    Write-Host ".NET Desktop Runtime for the installer: $($rel.windowsdesktop.version)" -ForegroundColor DarkGray
                    return $file.url
                }
            }
        } catch {
            Write-Warning "Could not read $metaUrl ($($_.Exception.Message))"
        }
    }
    return ''
}

$redistDir = Join-Path $root 'out\package\redist'
if (Test-Path $redistDir) { Remove-Item $redistDir -Recurse -Force }
$vcRedist = Find-VCRedist
$vcRedistStaged = ''
if ($vcRedist) {
    $null = New-Item -ItemType Directory -Force -Path $redistDir
    $vcRedistStaged = Join-Path $redistDir 'vc_redist.x64.exe'
    Copy-Item -LiteralPath $vcRedist.Path $vcRedistStaged
    $vcMin = $vcRedist.Version
    Write-Host "Visual C++ Redistributable embedded in the installer: $vcMin" -ForegroundColor DarkGray
} else {
    $vcMin = '14.0.0'
    Write-Warning ("vc_redist.x64.exe was not found in the Visual Studio installation; the installer will download " +
                   "the latest one from Microsoft instead and accept any Visual C++ 14.x runtime.")
}
$dotnetUrl = Get-DotNetDesktopRuntimeUrl
if (-not $dotnetUrl) {
    Write-Warning 'Could not resolve the .NET 10 Desktop Runtime download; the installer will open the download page instead of installing it.'
}

# ---------------------------------------------------------------- installer (Inno Setup)
$null = New-Item -ItemType Directory -Force -Path $releaseDir
$setupExe = Join-Path $releaseDir "LegacyAVR_v${version}_Setup.exe"
$zip      = Join-Path $releaseDir "LegacyAVR_v${version}_Portable.zip"
foreach ($old in $setupExe, $zip) { if (Test-Path $old) { Remove-Item $old -Force } }

Write-Host "Compiling the installer..." -ForegroundColor Cyan
& $iscc "/DAppVersion=$version" "/DStageDir=$stage" "/DVCRedistMin=$vcMin" "/DVCRedistExe=$vcRedistStaged" `
    "/DDotNetRuntimeUrl=$dotnetUrl" "/O$releaseDir" (Join-Path $PSScriptRoot 'installer.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed (exit code $LASTEXITCODE)." }
if (-not (Test-Path $setupExe)) { throw "Installer not found after compilation: $setupExe" }

# ---------------------------------------------------------------- portable zip (same files, no installer)
Write-Host "Creating the portable zip..." -ForegroundColor Cyan
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

# ---------------------------------------------------------------- checksums
$sums = Join-Path $releaseDir 'SHA256SUMS.txt'
Get-FileHash -Algorithm SHA256 -Path $setupExe, $zip |
    ForEach-Object { "$($_.Hash.ToLower())  $(Split-Path $_.Path -Leaf)" } |
    Set-Content -Path $sums -Encoding ASCII

Write-Host ''
Write-Host "Release files in ${releaseDir}:" -ForegroundColor Green
Get-Item $setupExe, $zip, $sums | ForEach-Object { '  {0,-40} {1,8:N1} MB' -f $_.Name, ($_.Length / 1MB) } | Write-Host
Write-Host 'Attach all three to the GitHub release. They are git-ignored on purpose (do not commit them).' -ForegroundColor DarkGray
