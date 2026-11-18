#Requires -Version 5.1
<#
  Remove the portable toolchain. Nothing else on the system is touched.
#>

[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Toolchain = "F:\091-Opencode\toolchain",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Toolchain)) {
    Write-Host "Nothing to remove: $Toolchain" -ForegroundColor Yellow
    return
}

$size = (Get-ChildItem -Recurse -Force $Toolchain -ErrorAction SilentlyContinue |
         Measure-Object -Property Length -Sum).Sum
Write-Host ("Target : {0}" -f $Toolchain)
Write-Host ("Size   : {0:N2} GB" -f ($size / 1GB))

if (-not $Force) {
    $answer = Read-Host "Delete the whole toolchain directory? [y/N]"
    if ($answer -notin @("y", "Y", "yes", "YES")) {
        Write-Host "Cancelled." -ForegroundColor Yellow
        return
    }
}

Remove-Item -Recurse -Force $Toolchain
Write-Host "Removed: $Toolchain" -ForegroundColor Green
