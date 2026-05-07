#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Uninstalls the Treblle IIS Native HTTP Module.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$InstallDir = "C:\iismodules\treblle"
$appcmd     = "$env:SystemRoot\System32\inetsrv\appcmd.exe"

Write-Host ""
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  Treblle IIS Module Uninstaller" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host ""

# ── Remove module registration ────────────────────────────────────────────────
if (Test-Path $appcmd) {
    $existing = & $appcmd list module /name:TreblleModule 2>$null
    if ($existing) {
        & $appcmd delete module /name:TreblleModule | Out-Null
        Write-Host "Module unregistered from IIS." -ForegroundColor Green
    } else {
        Write-Host "Module was not registered in IIS." -ForegroundColor Yellow
    }
} else {
    Write-Host "appcmd.exe not found — skipping IIS module removal." -ForegroundColor Yellow
}

# ── Restart IIS ───────────────────────────────────────────────────────────────
Write-Host "Restarting IIS..." -ForegroundColor Cyan
iisreset /noforce 2>&1 | Out-Null

# ── Remove files ──────────────────────────────────────────────────────────────
if (Test-Path $InstallDir) {
    $answer = Read-Host "`nRemove $InstallDir (including treblle.config)? [y/N]"
    if ($answer -match '^[Yy]') {
        Remove-Item -Recurse -Force $InstallDir
        Write-Host "Removed $InstallDir" -ForegroundColor Green
    } else {
        Write-Host "Files kept at $InstallDir" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Uninstall complete." -ForegroundColor Green
Write-Host ""
