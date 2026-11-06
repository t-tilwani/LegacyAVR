<#
.SYNOPSIS
  Builds LegacyAVR with MSBuild (engine, service, tests and - if the .NET 10 SDK is present - the WPF UI).
  Works with the full Visual Studio 2026 IDE *or* the "Build Tools for Visual Studio 2026".
.EXAMPLE
  .\scripts\build.ps1                       # Release|x64
  .\scripts\build.ps1 -RunTests
  .\scripts\build.ps1 -NativeOnly           # C++ only (no .NET SDK needed)
  .\scripts\build.ps1 -Latency              # measure pipeline latency per margin setting
#>
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [switch] $RunTests,
    [switch] $Bench,
    [switch] $Latency,
    [switch] $NativeOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

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
if (-not $NativeOnly -and -not $hasNet10) {
    Write-Warning (".NET 10 SDK not found - building the C++ engine and tests only (the WPF UI is skipped).`n" +
                   "To build the UI: add the '.NET desktop build tools' workload in Visual Studio Installer, or install the .NET 10 SDK, then re-run.")
    $NativeOnly = $true
}

# ---------------------------------------------------------------- build
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

$bin = Join-Path $root "bin\$Configuration"
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
