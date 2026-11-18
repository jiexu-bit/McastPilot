#Requires -Version 5.1
<#
  Configure + build with the portable UCRT64 toolchain.
  Nothing is written outside the build directory.
#>

[CmdletBinding()]
param(
    [string]$Toolchain = "F:\091-Opencode\toolchain\msys64",
    [string]$BuildDir  = "build",
    [switch]$DebugBuild,
    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$ucrt = Join-Path $Toolchain "ucrt64"
$bin  = Join-Path $ucrt "bin"

if (-not (Test-Path (Join-Path $bin "g++.exe"))) {
    throw "Toolchain not found at $Toolchain. Run scripts\setup-toolchain.ps1 first."
}

# Process-scoped only: never touches the system PATH.
$env:PATH = "$bin;" + $env:PATH
$env:MSYSTEM = "UCRT64"
$env:PKG_CONFIG_PATH = (Join-Path $ucrt "lib\pkgconfig")
$env:PKG_CONFIG_LIBDIR = (Join-Path $ucrt "lib\pkgconfig")
if (-not $env:CMAKE_PREFIX_PATH) {
    $env:CMAKE_PREFIX_PATH = $ucrt
} elseif ($env:CMAKE_PREFIX_PATH -notlike "*$ucrt*") {
    $env:CMAKE_PREFIX_PATH = "$ucrt;$env:CMAKE_PREFIX_PATH"
}

$cfg = if ($DebugBuild) { "Debug" } else { "Release" }

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host "==> configure ($cfg)" -ForegroundColor Cyan
& cmake -S . -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$cfg"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "==> build" -ForegroundColor Cyan
& cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $root "$BuildDir\bin\mcastctl.exe"
Write-Host ""
Write-Host "OK -> $exe" -ForegroundColor Green

if ($Run) {
    & $exe nics
}
