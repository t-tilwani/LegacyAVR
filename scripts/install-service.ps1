<#
.SYNOPSIS
  Installs (or removes) AVREngineService.exe as the "LegacyAVREngine" Windows service.
  Must be run elevated. Stops a running per-user AVREngine.exe first (only one engine may run).
  The installer offers the same thing as an optional task; this script is for the portable zip and
  for source builds.
#>
param(
    [switch] $Uninstall,
    [string] $BinDir
)
$ErrorActionPreference = 'Stop'

if (-not $BinDir) {
    # Installed / portable layout: <LegacyAVR>\scripts\install-service.ps1 next to <LegacyAVR>\AVREngineService.exe.
    # Source tree layout:          <repo>\scripts\install-service.ps1 and <repo>\bin\Release\.
    $parent = Split-Path -Parent $PSScriptRoot
    $BinDir = if (Test-Path (Join-Path $parent 'AVREngineService.exe')) { $parent } else { Join-Path $parent 'bin\Release' }
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated (Administrator) PowerShell."
}

$svcExe = Join-Path $BinDir 'AVREngineService.exe'
if (-not (Test-Path $svcExe)) { throw "Not found: $svcExe (build Release|x64 first)" }

if ($Uninstall) {
    & $svcExe --uninstall
    exit $LASTEXITCODE
}

Get-Process -Name 'AVREngine' -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "Stopping per-user AVREngine.exe (PID $($_.Id))..."
    $_ | Stop-Process -Force
}
& $svcExe --install
exit $LASTEXITCODE
