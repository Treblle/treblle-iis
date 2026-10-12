#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs the Treblle IIS Agent on Windows Server 2022/2025.

.DESCRIPTION
    - Copies the x64 and/or x86 build of the agent to C:\iismodules\treblle\
    - Creates treblle.config if it doesn't already exist
    - Registers each build globally in IIS behind a bitness precondition, so a
      64-bit worker process loads the x64 DLL and a 32-bit one loads the x86 DLL
    - Restarts IIS to activate the module

    Both builds are installed when both are available. IIS picks the matching one
    per worker process, which means mixed-bitness servers work without hand
    registration, and a pool that is switched to 32-bit later keeps being
    monitored instead of silently dropping off.

.PARAMETER DllPathX64
    Path to the x64 build. Defaults to searching the script directory and the
    local build output.

.PARAMETER DllPathX86
    Path to the x86 build. Defaults to searching the script directory and the
    local build output.

.PARAMETER DllPath
    A single DLL to install. Its architecture is read from the PE header and it
    is registered accordingly. Use this to install only one of the two builds.

.PARAMETER ConfigPath
    Destination for treblle.config. Defaults to C:\iismodules\treblle\treblle.config.
#>

param(
    [string]$DllPathX64 = "",
    [string]$DllPathX86 = "",
    [string]$DllPath    = "",
    [string]$ConfigPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Resolve paths -----------------------------------------------------------
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot    = Split-Path -Parent $ScriptDir
$InstallDir  = "C:\iismodules\treblle"
$DllDestX64  = Join-Path $InstallDir "TreblleAgent-x64.dll"
$DllDestX86  = Join-Path $InstallDir "TreblleAgent-x86.dll"
$ConfigDest  = if ($ConfigPath) { $ConfigPath } else { Join-Path $InstallDir "treblle.config" }

# Installs before the split put a single DLL here, under whichever architecture
# the installer happened to be handed. On a server migrated per MIGRATING-TO-X86.md
# that file is the x86 build. It is replaced by the two files above.
$LegacyDll   = Join-Path $InstallDir "TreblleAgent.dll"

# --- Architecture detection --------------------------------------------------
# Read the Machine field out of the PE header. Filenames are not trustworthy here:
# release archives ship the x64 build as plain TreblleAgent.dll, and the Win32
# build output is ALSO called TreblleAgent.dll (MSBuild writes it to Release\
# rather than x64\Release\). Registering a DLL behind the wrong bitness
# precondition fails every matching worker process with ERROR_BAD_EXE_FORMAT and
# rapid-fail protection then disables the pool, so only the header decides.
function Get-DllArchitecture {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)

        if ($stream.Length -lt 0x40) { return "unknown" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -lt 0x40 -or ($peOffset + 6) -gt $stream.Length) { return "unknown" }

        $stream.Position = $peOffset
        $signature = $reader.ReadBytes(4)
        if ($signature[0] -ne 0x50 -or $signature[1] -ne 0x45 -or
            $signature[2] -ne 0x00 -or $signature[3] -ne 0x00) { return "unknown" }

        switch ($reader.ReadUInt16()) {
            0x8664  { "x64" }
            0x014C  { "x86" }
            default { "unknown" }
        }
    } finally {
        $stream.Dispose()
    }
}

# Returns the first candidate that exists AND actually is the requested architecture.
function Find-AgentDll {
    param(
        [string[]]$Candidates,
        [ValidateSet("x64", "x86")][string]$Architecture
    )

    foreach ($candidate in $Candidates) {
        if ((Test-Path $candidate -PathType Leaf) -and
            ((Get-DllArchitecture $candidate) -eq $Architecture)) {
            return (Resolve-Path $candidate).Path
        }
    }
    return ""
}

# Confirms an explicitly supplied path is the architecture we are about to treat it as.
function Assert-DllArchitecture {
    param([string]$Path, [string]$Architecture, [string]$ParameterName)

    if (-not (Test-Path $Path -PathType Leaf)) {
        Write-Error "$ParameterName : file not found - $Path"
        exit 1
    }
    $actual = Get-DllArchitecture $Path
    if ($actual -ne $Architecture) {
        Write-Error "$ParameterName was given a $actual DLL ($Path) but must be $Architecture. Installing it under the wrong bitness precondition would disable the app pools it loads into."
        exit 1
    }
    return (Resolve-Path $Path).Path
}

# --- Locate the builds -------------------------------------------------------
if ($DllPath) {
    if ($DllPathX64 -or $DllPathX86) {
        Write-Error "-DllPath installs a single build and cannot be combined with -DllPathX64 or -DllPathX86. Pass the architecture-specific parameters instead."
        exit 1
    }
    if (-not (Test-Path $DllPath -PathType Leaf)) {
        Write-Error "Cannot find $DllPath"
        exit 1
    }
    switch (Get-DllArchitecture $DllPath) {
        "x64"   { $DllPathX64 = (Resolve-Path $DllPath).Path }
        "x86"   { $DllPathX86 = (Resolve-Path $DllPath).Path }
        default {
            Write-Error "$DllPath is not a recognizable x64 or x86 Windows DLL."
            exit 1
        }
    }
} else {
    $candidates = @(
        (Join-Path $ScriptDir "TreblleAgent-x64.dll"),
        (Join-Path $ScriptDir "TreblleAgent-x86.dll"),
        (Join-Path $ScriptDir "TreblleAgent.dll"),
        (Join-Path $RepoRoot  "x64\Release\TreblleAgent.dll"),
        (Join-Path $RepoRoot  "Release\TreblleAgent-x86.dll"),
        (Join-Path $RepoRoot  "Release\TreblleAgent.dll")
    )

    if ($DllPathX64) {
        $DllPathX64 = Assert-DllArchitecture $DllPathX64 "x64" "-DllPathX64"
    } else {
        $DllPathX64 = Find-AgentDll -Candidates $candidates -Architecture "x64"
    }

    if ($DllPathX86) {
        $DllPathX86 = Assert-DllArchitecture $DllPathX86 "x86" "-DllPathX86"
    } else {
        $DllPathX86 = Find-AgentDll -Candidates $candidates -Architecture "x86"
    }
}

if (-not $DllPathX64 -and -not $DllPathX86) {
    Write-Error "Cannot find an x64 or x86 build of TreblleAgent.dll. Build the project first, then re-run this script."
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

# The x64 build is registered under the original module name so that upgrades over
# an existing install stay recognizable to anything that references it.
$builds = @()
if ($DllPathX64) {
    $builds += [pscustomobject]@{
        Architecture = "x64"
        ModuleName   = "TreblleAgent"
        PreCondition = "bitness64"
        Source       = $DllPathX64
        Destination  = $DllDestX64
    }
}
if ($DllPathX86) {
    $builds += [pscustomobject]@{
        Architecture = "x86"
        ModuleName   = "TreblleAgentX86"
        PreCondition = "bitness32"
        Source       = $DllPathX86
        Destination  = $DllDestX86
    }
}

foreach ($build in $builds) {
    Write-Host ("Found {0} build: {1}" -f $build.Architecture, $build.Source) -ForegroundColor Green
}
if ($builds.Count -eq 1) {
    $missing = if ($DllPathX64) { "x86" } else { "x64" }
    Write-Host ("No $missing build supplied - $missing app pools will not be monitored.") -ForegroundColor Yellow
}

# --- Unregister anything already installed -----------------------------------
# Done before the files are touched: a registered module is held open by every
# running worker process, and a globalModules entry whose image has moved or
# disappeared takes those pools down with ERROR_MOD_NOT_FOUND.
$hadPrevious = $false
foreach ($moduleName in @("TreblleAgent", "TreblleAgentX86")) {
    $existing = & $appcmd list module /name:$moduleName 2>$null
    if ($existing) {
        & $appcmd uninstall module /module.name:$moduleName | Out-Null
        Write-Host "Removed previous registration: $moduleName" -ForegroundColor Gray
        $hadPrevious = $true
    }
}

# Worker processes keep the old DLL open until they recycle, so release the handles
# before overwriting anything. Only needed when something was already installed.
if ($hadPrevious -or (Test-Path $LegacyDll) -or (Test-Path $DllDestX64) -or (Test-Path $DllDestX86)) {
    Write-Host "Releasing the previous agent from running worker processes..." -ForegroundColor Gray
    iisreset /noforce 2>&1 | Out-Null
}

# --- Copy the DLLs -----------------------------------------------------------
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir | Out-Null
    Write-Host "Created $InstallDir" -ForegroundColor Green
}

foreach ($build in $builds) {
    Copy-Item -Path $build.Source -Destination $build.Destination -Force
    Write-Host ("Copied {0} build to {1}" -f $build.Architecture, $build.Destination) -ForegroundColor Green
}

if (Test-Path $LegacyDll) {
    try {
        Remove-Item -Path $LegacyDll -Force
        Write-Host "Removed superseded $LegacyDll" -ForegroundColor Gray
    } catch {
        Write-Warning "Could not remove the superseded $LegacyDll - it is no longer registered, so it is inert, but delete it once IIS has fully restarted to avoid confusion about which build is loaded."
    }
}

# --- Create config if missing ------------------------------------------------
if (-not (Test-Path $ConfigDest)) {
    Write-Host ""
    Write-Host "No treblle.config found. Let's set it up now." -ForegroundColor Yellow
    Write-Host "(You can edit $ConfigDest at any time - changes take effect immediately.)"
    Write-Host ""

    $apiKey   = Read-Host "  Enter your Treblle API Key"
    $sdkToken = Read-Host "  Enter your Treblle SDK Token"

    Write-Host ""
    Write-Host "  All JSON API traffic is monitored by default."
    Write-Host "  Optionally exclude hosts or paths (e.g. health checks, internal endpoints)."
    Write-Host "  Press Enter with no input when done."
    Write-Host "  Format: hostname[:port][/path]"
    Write-Host "  Examples: internal.example.com  |  api.example.com/health  |  api.example.com/metrics"
    Write-Host ""

    $routes = @()
    while ($true) {
        $entry = Read-Host "  Exclude route (or Enter to skip)"
        if ([string]::IsNullOrWhiteSpace($entry)) { break }

        if ($entry -match '^([^/]+)(/.*)$') {
            $routes += @{ host = $matches[1].Trim(); path = $matches[2].Trim() }
        } else {
            $routes += @{ host = $entry.Trim() }
        }
    }

    if ($routes.Count -eq 0) {
        Write-Host ""
        Write-Host "  No exclusions configured - all JSON API traffic will be monitored." -ForegroundColor Gray
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
  "disabled": false,
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

# --- Register modules in IIS -------------------------------------------------
Write-Host ""
Write-Host "Registering module with IIS..." -ForegroundColor Cyan

foreach ($build in $builds) {
    # install module registers the native DLL globally and enables it
    $result = & $appcmd install module /name:$($build.ModuleName) /image:"$($build.Destination)" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "appcmd failed to register $($build.ModuleName): $result"
        exit 1
    }

    # The precondition is what keeps the other bitness safe. Without it IIS tries to
    # load the DLL into every worker process, and the mismatched ones fail to start
    # (IIS-W3SVC-WP event 2282, ERROR_BAD_EXE_FORMAT) rather than simply skipping
    # the module - which eventually disables the pool and takes the site down.
    & $appcmd set config /section:system.webServer/globalModules `
        "/[name='$($build.ModuleName)'].preCondition:$($build.PreCondition)" /commit:apphost | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Registered $($build.ModuleName) but failed to apply the $($build.PreCondition) precondition. Removing the registration - leaving it in place would fail every worker process of the other bitness."
        & $appcmd uninstall module /module.name:$($build.ModuleName) 2>&1 | Out-Null
        exit 1
    }

    Write-Host ("  {0} registered as {1} ({2})." -f $build.Architecture, $build.ModuleName, $build.PreCondition) -ForegroundColor Green
}

# --- Report which app pools are covered --------------------------------------
$pools32 = @()
$pools64 = @()
foreach ($pool in @(& $appcmd list apppools /text:name)) {
    if ((& $appcmd list apppool "$pool" /text:enable32BitAppOnWin64) -eq "true") {
        $pools32 += $pool
    } else {
        $pools64 += $pool
    }
}

$uncovered = @()
if (-not $DllPathX64) { $uncovered += $pools64 }
if (-not $DllPathX86) { $uncovered += $pools32 }

Write-Host ""
Write-Host ("App pools: {0} 64-bit, {1} 32-bit." -f $pools64.Count, $pools32.Count) -ForegroundColor Cyan
if ($uncovered.Count -gt 0) {
    Write-Warning "No matching build is installed for the following app pools, so they will NOT be monitored (the module is skipped for them, not loaded):"
    $uncovered | ForEach-Object { Write-Host "    - $_" -ForegroundColor Yellow }
}

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
foreach ($build in $builds) {
    Write-Host ("  Agent DLL    : {0} ({1} pools)" -f $build.Destination, $build.Architecture)
}
Write-Host "  Config file  : $ConfigDest"
Write-Host ""
Write-Host "  To change settings, edit $ConfigDest" -ForegroundColor White
Write-Host "  Changes take effect immediately - no restart needed." -ForegroundColor White
Write-Host ""
