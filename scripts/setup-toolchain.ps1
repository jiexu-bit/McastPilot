#Requires -Version 5.1
<#
  Portable MSYS2 toolchain setup (no admin, no registry, no system PATH changes).

  Installs:
    - MSYS2 (self-contained) at -Root
    - UCRT64 toolchain: gcc, cmake, ninja, gdb, pkgconf, ffmpeg, qt6-base

  Uninstall:
    scripts\clean-toolchain.ps1   (or just delete the toolchain folder)

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts\setup-toolchain.ps1
#>

[CmdletBinding()]
param(
    [string]$Root = "F:\091-Opencode\toolchain\msys64",
    [string]$DownloadDir = "F:\091-Opencode\toolchain\_download",
    [switch]$SkipPackages
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "    $msg" -ForegroundColor Yellow }

Write-Step "Portable MSYS2 toolchain"
Write-Host "    root     : $Root"
Write-Host "    download : $DownloadDir"

# ---------------------------------------------------------------- download ---
$installer = Join-Path $DownloadDir "msys2-installer.exe"
if (-not (Test-Path $installer)) {
    New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
    $urls = @(
        "https://mirrors.tuna.tsinghua.edu.cn/msys2/distrib/msys2-x86_64-latest.exe",
        "https://mirrors.ustc.edu.cn/msys2/distrib/msys2-x86_64-latest.exe",
        "https://repo.msys2.org/distrib/msys2-x86_64-latest.exe"
    )
    $downloaded = $false
    foreach ($u in $urls) {
        try {
            Write-Step "Downloading MSYS2 installer"
            Write-Host "    $u"
            & curl.exe -L --fail --retry 3 --connect-timeout 20 -o $installer $u
            if ($LASTEXITCODE -eq 0 -and (Test-Path $installer) -and (Get-Item $installer).Length -gt 40MB) {
                $downloaded = $true
                break
            }
        } catch {
            Write-Warn "mirror failed: $($_.Exception.Message)"
        }
    }
    if (-not $downloaded) { throw "Failed to download MSYS2 installer from all mirrors." }
}
Write-Ok ("installer: {0:N1} MB" -f ((Get-Item $installer).Length / 1MB))

# ---------------------------------------------------------------- install ----
$bash = Join-Path $Root "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    Write-Step "Installing MSYS2 (silent) -> $Root"
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    $p = Start-Process -FilePath $installer -Wait -PassThru -NoNewWindow -ArgumentList @(
        "in", "--confirm-command", "--accept-licenses", "--accept-messages",
        "--root", ($Root -replace '\\', '/')
    )
    if ($p.ExitCode -ne 0) { throw "MSYS2 installer failed with exit code $($p.ExitCode)" }
} else {
    Write-Ok "MSYS2 already present, skipping installer."
}

if (-not (Test-Path $bash)) { throw "bash.exe not found after install: $bash" }

# ---------------------------------------------------------------- mirrors ----
Write-Step "Configuring pacman mirrors (TUNA / USTC)"
$mirrorDir = Join-Path $Root "etc\pacman.d"
$msysMirror = @(
    "Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/msys/`$arch",
    "Server = https://mirrors.ustc.edu.cn/msys2/msys/`$arch",
    "Server = https://repo.msys2.org/msys/`$arch"
) -join "`n"
$mingwMirror = @(
    "Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/`$repo",
    "Server = https://mirrors.ustc.edu.cn/msys2/mingw/`$repo",
    "Server = https://repo.msys2.org/mingw/`$repo"
) -join "`n"

# UTF8 without BOM
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $mirrorDir "mirrorlist.msys"),    $msysMirror  + "`n", $utf8)
[System.IO.File]::WriteAllText((Join-Path $mirrorDir "mirrorlist.mingw"),   $mingwMirror + "`n", $utf8)
[System.IO.File]::WriteAllText((Join-Path $mirrorDir "mirrorlist.ucrt64"),  $mingwMirror + "`n", $utf8)
Write-Ok "mirrorlist.msys / mirrorlist.mingw / mirrorlist.ucrt64"

# ---------------------------------------------------------------- pacman -----
function Invoke-Bash([string]$cmd, [switch]$AllowFail) {
    $env:MSYSTEM = "UCRT64"
    $env:CHERE_INVOKING = "1"
    & $bash -lc $cmd
    if ($LASTEXITCODE -ne 0 -and -not $AllowFail) {
        throw "bash command failed ($LASTEXITCODE): $cmd"
    }
}

Write-Step "pacman: core update (pass 1/2)"
Invoke-Bash "pacman -Syu --noconfirm --disable-download-timeout" -AllowFail

Write-Step "pacman: core update (pass 2/2)"
Invoke-Bash "pacman -Syu --noconfirm --disable-download-timeout" -AllowFail

if (-not $SkipPackages) {
    Write-Step "pacman: installing UCRT64 toolchain (this takes a while)"
    $pkgs = @(
        "mingw-w64-ucrt-x86_64-gcc",
        "mingw-w64-ucrt-x86_64-gcc-libs",
        "mingw-w64-ucrt-x86_64-cmake",
        "mingw-w64-ucrt-x86_64-ninja",
        "mingw-w64-ucrt-x86_64-gdb",
        "mingw-w64-ucrt-x86_64-pkgconf",
        "mingw-w64-ucrt-x86_64-ffmpeg",
        "mingw-w64-ucrt-x86_64-qt6-base"
    ) -join " "
    Invoke-Bash "pacman -S --needed --noconfirm --disable-download-timeout $pkgs"
}

# ---------------------------------------------------------------- verify -----
Write-Step "Verifying toolchain"
Invoke-Bash "set -e; echo -n 'gcc     : '; gcc --version | head -n1; echo -n 'g++     : '; g++ --version | head -n1; echo -n 'cmake   : '; cmake --version | head -n1; echo -n 'ninja   : '; ninja --version; echo -n 'pkgconf : '; pkg-config --modversion libavformat; echo -n 'qt6     : '; qmake6 --version 2>/dev/null | tail -n1 || true; echo -n 'gdb     : '; gdb --version | head -n1"

Write-Host ""
Write-Host "Toolchain ready: $Root" -ForegroundColor Green
Write-Host "Build with: powershell -ExecutionPolicy Bypass -File scripts\build.ps1" -ForegroundColor Green
