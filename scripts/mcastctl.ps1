#Requires -Version 5.1
<#
  Run mcastctl with the portable toolchain DLLs on PATH.

  Examples:
    powershell -File scripts\mcastctl.ps1 nics
    powershell -File scripts\mcastctl.ps1 send --input demo.ts --group 239.1.1.1 --port 5000 --nic 1

  The toolchain location can be overridden with the MCAST_TOOLCHAIN env var.
#>

$toolchain = $env:MCAST_TOOLCHAIN
if (-not $toolchain) { $toolchain = "F:\091-Opencode\toolchain\msys64" }

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\mcastctl.exe"
if (-not (Test-Path $exe)) {
    throw "mcastctl.exe not found. Run scripts\build.ps1 first."
}

$env:PATH = (Join-Path $toolchain "ucrt64\bin") + ";" + $env:PATH
& $exe @args
exit $LASTEXITCODE
