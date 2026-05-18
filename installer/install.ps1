#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs the Treblle IIS Agent on Windows Server 2022/2025.

.DESCRIPTION
    - Copies TreblleAgent.dll to C:\iismodules\treblle\
    - Creates treblle.config if it doesn't already exist
    - Registers the module globally in IIS
    - Restarts IIS to activate the module

.PARAMETER DllPath
    Path to TreblleAgent.dll. Defaults to the directory containing this script.

.PARAMETER ConfigPath
    Destination for treblle.config. Defaults to C:\iismodules\treblle\treblle.config.
#>

param(
    [string]$DllPath   = "",
    [string]$ConfigPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Resolve paths -----------------------------------------------------------
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$InstallDir  = "C:\iismodules\treblle"
$DllDest     = Join-Path $InstallDir "TreblleAgent.dll"
$ConfigDest  = if ($ConfigPath) { $ConfigPath } else { Join-Path $InstallDir "treblle.config" }

if (-not $DllPath) {
    $candidates = @(
        (Join-Path $ScriptDir "TreblleAgent.dll"),
        (Join-Path (Split-Path -Parent $ScriptDir) "x64\Release\TreblleAgent.dll"),
        (Join-Path (Split-Path -Parent $ScriptDir) "Release\TreblleAgent.dll")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $DllPath = $c; break }
    }
}

if (-not $DllPath -or -not (Test-Path $DllPath)) {
    Write-Error "Cannot find TreblleAgent.dll. Build the project first, then re-run this script."
    exit 1
}

# --- Check IIS ---------------------------------------------------------------
$appcmd = "$env:SystemRoot\System32\inetsrv\appcmd.exe"
if (-not (Test-Path $appcmd)) {
    Write-Error "IIS does not appear to be installed (appcmd.exe not found). Install IIS first."
    exit 1
}

Write-Host ""
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  Treblle IIS Agent Installer" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host ""

# --- Copy DLL ----------------------------------------------------------------
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir | Out-Null
    Write-Host "Created $InstallDir" -ForegroundColor Green
}

Copy-Item -Path $DllPath -Destination $DllDest -Force
Write-Host "Copied TreblleAgent.dll to $DllDest" -ForegroundColor Green

# --- Create config if missing ------------------------------------------------
if (-not (Test-Path $ConfigDest)) {
    Write-Host ""
    Write-Host "No treblle.config found. Let's set it up now." -ForegroundColor Yellow
    Write-Host "(You can edit $ConfigDest at any time - changes take effect immediately.)"
    Write-Host ""

    $apiKey   = Read-Host "  Enter your Treblle API Key"
    $sdkToken = Read-Host "  Enter your Treblle SDK Token"

    Write-Host ""
    Write-Host "  Add API routes to monitor. Press Enter with no input when done."
    Write-Host "  Format: hostname[:port][/path]"
    Write-Host "  Examples: api.example.com  |  api.example.com/v1  |  service.example.com/api"
    Write-Host ""

    $routes = @()
    while ($true) {
        $entry = Read-Host "  Route (or Enter to finish)"
        if ([string]::IsNullOrWhiteSpace($entry)) { break }

        if ($entry -match '^([^/]+)(/.*)$') {
            $routes += @{ host = $matches[1].Trim(); path = $matches[2].Trim() }
        } else {
            $routes += @{ host = $entry.Trim() }
        }
    }

    if ($routes.Count -eq 0) {
        Write-Host ""
        Write-Host "  Warning: No routes configured. The module will not track anything until" -ForegroundColor Yellow
        Write-Host "  exclude_routes is populated in $ConfigDest" -ForegroundColor Yellow
    }

    $routeJson = ($routes | ForEach-Object {
        $h = $_.host
        if ($_.ContainsKey("path") -and $_.path) {
            "    { `"host`": `"$h`", `"path`": `"$($_.path)`" }"
        } else {
            "    { `"host`": `"$h`" }"
        }
    }) -join ",`n"

    $configContent = @"
{
  "api_key": "$apiKey",
  "sdk_token": "$sdkToken",
  "treblle_url": "https://ingress.treblle.com",
  "debug": false,
  "exclude_routes": [
$routeJson
  ]
}
"@
    Set-Content -Path $ConfigDest -Value $configContent -Encoding UTF8
    Write-Host ""
    Write-Host "Config written to $ConfigDest" -ForegroundColor Green
} else {
    Write-Host "Existing config found at $ConfigDest - leaving it unchanged." -ForegroundColor Green
}

# --- Register module in IIS --------------------------------------------------
Write-Host ""
Write-Host "Registering module with IIS..." -ForegroundColor Cyan

# Remove any previous registration first (idempotent)
$existing = & $appcmd list module /name:TreblleAgent 2>$null
if ($existing) {
    & $appcmd uninstall module /module.name:TreblleAgent | Out-Null
    Write-Host "  Removed previous registration." -ForegroundColor Gray
}

# install module registers the native DLL globally and enables it
$result = & $appcmd install module /name:TreblleAgent /image:"$DllDest" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "appcmd failed to register the module: $result"
    exit 1
}
Write-Host "  Agent registered successfully." -ForegroundColor Green

# --- Restart IIS -------------------------------------------------------------
Write-Host ""
Write-Host "Restarting IIS..." -ForegroundColor Cyan
iisreset /noforce 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Warning "iisreset returned a non-zero exit code. IIS may need a manual restart."
} else {
    Write-Host "  IIS restarted." -ForegroundColor Green
}

# --- Done --------------------------------------------------------------------
Write-Host ""
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  Installation complete!" -ForegroundColor Green
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Agent DLL    : $DllDest"
Write-Host "  Config file  : $ConfigDest"
Write-Host ""
Write-Host "  To change settings, edit $ConfigDest" -ForegroundColor White
Write-Host "  Changes take effect immediately - no restart needed." -ForegroundColor White
Write-Host ""
