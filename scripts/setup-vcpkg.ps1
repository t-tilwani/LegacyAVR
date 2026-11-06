<#
.SYNOPSIS
  One-time vcpkg setup for LegacyAVR (run from a Developer PowerShell for VS 2026).

  * Uses the vcpkg bundled with Visual Studio (VCPKG_ROOT is set in the Developer shell),
    or a standalone clone if VCPKG_ROOT points to one.
  * Adds a "builtin-baseline" to vcpkg.json so manifest mode resolves exact port versions.
  * Enables MSBuild integration so LegacyAVR.sln picks up FFmpeg automatically.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT is not set. Open 'Developer PowerShell for VS 2026' (with the vcpkg component installed) and re-run."
}
$vcpkg = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
if (-not (Test-Path $vcpkg)) { throw "vcpkg.exe not found at $vcpkg" }

$manifest = Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
if (-not $manifest.'builtin-baseline') {
    Write-Host "Pinning vcpkg baseline..." -ForegroundColor Cyan
    & $vcpkg x-update-baseline --add-initial-baseline
    if ($LASTEXITCODE -ne 0) { throw "vcpkg x-update-baseline failed" }
}

Write-Host "Enabling vcpkg MSBuild integration (user-wide)..." -ForegroundColor Cyan
& $vcpkg integrate install
if ($LASTEXITCODE -ne 0) { throw "vcpkg integrate install failed" }

Write-Host "Done. FFmpeg (avcodec + swresample) will be built on the first solution build." -ForegroundColor Green
