#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Uninstalls the Treblle IIS Agent.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$InstallDir = "C:\iismodules\treblle"
$appcmd     = "$env:SystemRoot\System32\inetsrv\appcmd.exe"

Write-Host ""
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  Treblle IIS Agent Uninstaller" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host ""

# --- Remove module registration ----------------------------------------------
# TreblleAgentX86 is the hand-registered 32-bit entry from the dual-registration
# recipe in TROUBLESHOOTING.md. It has to go too: this script deletes $InstallDir
# (including the x86 subfolder it points at), and a globalModules entry whose image
# is gone takes every matching worker process down with error 0x7E
# (ERROR_MOD_NOT_FOUND) until rapid-fail protection disables the pool.
if (Test-Path $appcmd) {
    $removed = $false
    foreach ($moduleName in @("TreblleAgent", "TreblleAgentX86")) {
        $existing = & $appcmd list module /name:$moduleName 2>$null
        if ($existing) {
            & $appcmd uninstall module /module.name:$moduleName | Out-Null
            Write-Host "Unregistered $moduleName from IIS." -ForegroundColor Green
            $removed = $true
        }
    }
    if (-not $removed) {
        Write-Host "Agent was not registered in IIS." -ForegroundColor Yellow
    }
} else {
    Write-Host "appcmd.exe not found - skipping IIS module removal." -ForegroundColor Yellow
}

# --- Restart IIS -------------------------------------------------------------
Write-Host "Restarting IIS..." -ForegroundColor Cyan
iisreset /noforce 2>&1 | Out-Null

# --- Remove files ------------------------------------------------------------
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
