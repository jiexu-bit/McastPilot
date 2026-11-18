#Requires -Version 5.1
<#
  Run mcastd with the portable toolchain DLLs on PATH.

  Examples:
    powershell -File scripts\mcastd.ps1
    powershell -File scripts\mcastd.ps1 --listen 0.0.0.0 --port 8787 --media-root D:\Videos

  The toolchain location can be overridden with the MCAST_TOOLCHAIN env var.
#>

$toolchain = $env:MCAST_TOOLCHAIN
if (-not $toolchain) { $toolchain = "F:\091-Opencode\toolchain\msys64" }

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\mcastd.exe"
if (-not (Test-Path $exe)) {
    throw "mcastd.exe not found. Run scripts\build.ps1 first."
}

$env:PATH = (Join-Path $toolchain "ucrt64\bin") + ";" + $env:PATH
& $exe @args
exit $LASTEXITCODE
