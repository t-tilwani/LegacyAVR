<#
.SYNOPSIS
  Installs (or removes) AVREngineService.exe as the "LegacyAVREngine" Windows service.
  Must be run elevated. Stops a running per-user AVREngine.exe first (only one engine may run).
#>
param(
    [switch] $Uninstall,
    [string] $BinDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bin\Release')
)
$ErrorActionPreference = 'Stop'

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
