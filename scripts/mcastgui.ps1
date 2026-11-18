#Requires -Version 5.1
<#
  Run mcastgui with the portable Qt runtime on PATH.
  Normally windeployqt copies the Qt DLLs next to the exe, so double-click works;
  this wrapper is a fallback for development.
#>

$toolchain = $env:MCAST_TOOLCHAIN
if (-not $toolchain) { $toolchain = "F:\091-Opencode\toolchain\msys64" }

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\mcastgui.exe"
if (-not (Test-Path $exe)) {
    throw "mcastgui.exe not found. Run scripts\build.ps1 first."
}

$env:PATH = (Join-Path $toolchain "ucrt64\bin") + ";" + $env:PATH
& $exe @args
