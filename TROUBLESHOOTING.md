# Treblle IIS Agent - Troubleshooting & FAQ

**"The agent is installed but no data is showing up in Treblle."**

This document walks the request path end-to-end, from the IIS worker process to
Treblle's ingress API, with a copy-paste PowerShell check at every step. Run
everything from an **elevated PowerShell** session (Run as Administrator) **on the
web server itself**.

> **The single fastest thing you can do:** set `"debug": true` in
> `C:\iismodules\treblle\treblle.config`, make one API call, and read the Windows
> Application Event Log. The agent logs the exact reason it skipped a request.
> Jump to [Step 4](#step-4--is-the-request-actually-being-tracked).

---

## Contents

- [How the agent works (and where it can break)](#how-the-agent-works-and-where-it-can-break)
- [60-second triage](#60-second-triage)
- [Step 1 - Is the agent registered with IIS?](#step-1--is-the-agent-registered-with-iis)
- [Step 2 - Is the DLL actually loaded into the worker process?](#step-2--is-the-dll-actually-loaded-into-the-worker-process)
- [Step 3 - Is the config file found and valid?](#step-3--is-the-config-file-found-and-valid)
- [Step 4 - Is the request actually being tracked?](#step-4--is-the-request-actually-being-tracked)
- [Step 5 - Is the agent sending data out?](#step-5--is-the-agent-sending-data-out)
- [Step 6 - Network: firewall, proxy, DNS, TLS](#step-6--network-firewall-proxy-dns-tls)
- [Step 7 - Is Treblle accepting the payload?](#step-7--is-treblle-accepting-the-payload)
- [Symptom index](#symptom-index)
- [Reference: every reason a request is skipped](#reference-every-reason-a-request-is-skipped)
- [Reference: event log messages](#reference-event-log-messages)
- [One-shot diagnostic script](#one-shot-diagnostic-script)
- [What to send to Treblle support](#what-to-send-to-treblle-support)

---

## How the agent works (and where it can break)

The agent is **not a Windows service and not a process**. It is a native IIS
module - a DLL loaded *inside* each IIS worker process (`w3wp.exe`). There is
nothing to "start"; if IIS is running and the module is registered, the agent is
running.

```
Client request
      │
      ▼
HTTP.SYS (kernel)  ─── kernel-cached response? ──▶ never reaches the agent  [BREAK 0]
      │
      ▼
w3wp.exe (application pool)
      │
      ├─ TreblleAgent.dll loaded?  ────────────── no ──▶ nothing happens     [BREAK 1,2]
      │
      ├─ OnBeginRequest
      │     ├─ config loaded + not disabled? ──── no ──▶ skip                [BREAK 3]
      │     ├─ path not in built-in exclusions?  no ──▶ skip                 [BREAK 4]
      │     ├─ host/path not in exclude_routes?  no ──▶ skip                 [BREAK 4]
      │     ├─ method tracked?                   no ──▶ skip                 [BREAK 4]
      │     └─ capture request headers + body
      │
      ├─ OnSendResponse
      │     ├─ response Content-Type is JSON? ─── no ──▶ skip                [BREAK 4]
      │     └─ capture response headers + body chunks
      │
      └─ OnEndRequest
            └─ build payload ──▶ in-memory queue (max 5,000)                 [BREAK 5]
                                        │
                                        ▼
                          Background worker thread (one per w3wp)
                                        │
                                        └─ WinHTTP POST https://ingress.treblle.com
                                              ├─ firewall / proxy / DNS / TLS  [BREAK 6]
                                              └─ HTTP 401 / 403 / 429 / 5xx    [BREAK 7]
```

Each numbered break maps to a step below.

---

## 60-second triage

Paste this whole block into an elevated PowerShell on the web server. It answers
"is it installed, is it loaded, is the config there, is the network open" in one go.

```powershell
$appcmd = "$env:windir\System32\inetsrv\appcmd.exe"

Write-Host "`n--- 1. Module registered? ---" -ForegroundColor Cyan
& $appcmd list module /name:TreblleAgent

Write-Host "`n--- 2. DLL loaded into w3wp? ---" -ForegroundColor Cyan
tasklist /m TreblleAgent.dll

Write-Host "`n--- 3. Files on disk ---" -ForegroundColor Cyan
Get-ChildItem C:\iismodules\treblle | Select-Object Name, Length, LastWriteTime

Write-Host "`n--- 4. Config (secrets redacted) ---" -ForegroundColor Cyan
(Get-Content C:\iismodules\treblle\treblle.config -Raw) -replace '("(?:api_key|sdk_token)"\s*:\s*")[^"]*', '$1***REDACTED***'

Write-Host "`n--- 5. Recent Treblle event log entries ---" -ForegroundColor Cyan
Get-EventLog -LogName Application -Source Treblle -Newest 15 -ErrorAction SilentlyContinue |
    Format-Table TimeGenerated, Message -AutoSize -Wrap

Write-Host "`n--- 6. Can the server reach Treblle? ---" -ForegroundColor Cyan
Test-NetConnection ingress.treblle.com -Port 443 |
    Select-Object ComputerName, RemoteAddress, TcpTestSucceeded

Write-Host "`n--- 7. WinHTTP proxy (what the agent uses) ---" -ForegroundColor Cyan
netsh winhttp show proxy
```

**Reading the result:**

| Output | Meaning | Go to |
|---|---|---|
| Step 1 prints nothing | Module is not registered in IIS | [Step 1](#step-1--is-the-agent-registered-with-iis) |
| Step 2 says "No tasks are running" or lists no `w3wp.exe` | DLL is not loaded into any worker process | [Step 2](#step-2--is-the-dll-actually-loaded-into-the-worker-process) |
| Step 4 fails / keys look wrong | Config missing or invalid | [Step 3](#step-3--is-the-config-file-found-and-valid) |
| Step 5 shows `config not loaded` | Bad/missing `api_key` or `sdk_token` | [Step 3](#step-3--is-the-config-file-found-and-valid) |
| Step 5 shows `skip - ...` | Requests are being filtered out | [Step 4](#step-4--is-the-request-actually-being-tracked) |
| Step 5 shows `WinHttpSendRequest failed` | Network blocked | [Step 6](#step-6--network-firewall-proxy-dns-tls) |
| Step 5 shows `ingress returned HTTP 401` | Wrong credentials | [Step 7](#step-7--is-treblle-accepting-the-payload) |
| Step 6 `TcpTestSucceeded: False` | Firewall / egress blocked | [Step 6](#step-6--network-firewall-proxy-dns-tls) |
| Everything looks fine, still no data | [Symptom index](#symptom-index) | |

---

## Step 1 - Is the agent registered with IIS?

### Check

```powershell
$appcmd = "$env:windir\System32\inetsrv\appcmd.exe"

# Is the module registered at all?
& $appcmd list module /name:TreblleAgent
# Expected:
# MODULE "TreblleAgent" ( native, image:C:\iismodules\treblle\TreblleAgent.dll )

# Where does IIS think the DLL lives?
& $appcmd list config /section:system.webServer/globalModules | Select-String Treblle

# Is it in the enabled-modules list (registration has two halves)?
& $appcmd list config /section:system.webServer/modules | Select-String Treblle
```

A native module must appear in **both** `globalModules` (the DLL is loadable) and
`modules` (the DLL is enabled for the request pipeline). `install.ps1` writes both.

You can also read `applicationHost.config` directly:

```powershell
Select-String -Path "$env:windir\System32\inetsrv\config\applicationHost.config" -Pattern "Treblle"
```

### If it is missing

```powershell
cd C:\path\to\treblle-iis\installer
.\install.ps1
```

Common install failures:

| Cause | Fix |
|---|---|
| Installer not run as Administrator | Right-click PowerShell → Run as Administrator |
| PowerShell execution policy blocked the script | `Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process` then re-run |
| DLL not next to `install.ps1` | Copy `TreblleAgent.dll` into `installer\` first |
| Installer ran but `appcmd` errored | Re-run and read the output; IIS role may not be installed |

### If it is registered for the *wrong* path

The config file is read from **the directory the DLL lives in**, not from a fixed
location. If `globalModules` points at, say, `D:\modules\TreblleAgent.dll`, then the
config must be `D:\modules\treblle.config`. Mismatched paths are a common cause of
"installed but silent".

### Is it disabled at the site level?

A site's `web.config` can remove the module for that site only:

```powershell
# Check the effective module list for a specific site
& $appcmd list config "Default Web Site/" /section:system.webServer/modules | Select-String -Pattern "Treblle|clear"
```

If the site's `web.config` contains `<modules><clear /></modules>` or
`<remove name="TreblleAgent" />`, the agent will not run for that site. Remove those
lines, or re-add the module inside that site's `<modules>` section:

```xml
<system.webServer>
  <modules>
    <add name="TreblleAgent" />
  </modules>
</system.webServer>
```

---

## Step 2 - Is the DLL actually loaded into the worker process?

Registration is not the same as loading. This is the definitive "is the agent
running" check.

### Check

```powershell
# Fast check - lists every process that has the DLL mapped
tasklist /m TreblleAgent.dll

# Detailed - which app pool, which PID
$appcmd = "$env:windir\System32\inetsrv\appcmd.exe"
& $appcmd list wp

Get-Process w3wp -ErrorAction SilentlyContinue | ForEach-Object {
    $p = $_
    $mod = $p.Modules | Where-Object { $_.ModuleName -eq 'TreblleAgent.dll' }
    [pscustomobject]@{
        PID    = $p.Id
        Loaded = [bool]$mod
        Path   = $mod.FileName
    }
}
```

> `w3wp.exe` only exists once the app pool has served (or been warmed for) a
> request. If no `w3wp.exe` is running, hit the API once and re-check.

### Confirm the agent initialised

The agent writes a startup line to the Application Event Log **on every worker
process start, regardless of the `debug` setting**:

```powershell
Get-EventLog -LogName Application -Source Treblle -Newest 30 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -like "*RegisterModule*" -or $_.Message -like "*registered successfully*" } |
    Format-List TimeGenerated, Message
```

Expected, in order:

```
[TREBLLE]: Treblle: RegisterModule starting - DLL: C:\iismodules\treblle\TreblleAgent.dll
[TREBLLE]: Treblle: agent registered successfully - DLL: C:\iismodules\treblle\TreblleAgent.dll
```

**If you see neither line, the DLL was never loaded.** Recycle the app pool and
watch the log:

```powershell
Restart-WebAppPool -Name "YourAppPoolName"      # or: iisreset
```

### Why a native module fails to load

| Cause | How to confirm | Fix |
|---|---|---|
| **32-bit app pool** - the DLL is x64-only | see command below | set a bitness precondition, or switch the pool to 64-bit |
| **File ACLs** - app pool identity can't read `C:\iismodules\treblle` | Event Viewer → System → WAS/w3wp errors; site returns 503 | `icacls` grant (below) |
| **DLL blocked** - downloaded from the internet (Zone.Identifier) | `Get-Item ... -Stream *` | `Unblock-File` |
| **Antivirus / EDR** blocked injection into `w3wp.exe` | AV console; Defender detection log | add an exclusion for `C:\iismodules\treblle` |
| **Wrong architecture** built | `dumpbin /headers` shows `x86` | rebuild `Release|x64` |

**32-bit app pool check and fix:**

```powershell
$appcmd = "$env:windir\System32\inetsrv\appcmd.exe"
& $appcmd list apppools /text:name | ForEach-Object {
    $b32 = (& $appcmd list apppool "$_" /text:enable32BitAppOnWin64)
    "{0,-35} 32-bit={1}" -f $_, $b32
}
```

If any pool reports `32-bit=true`, tell IIS to load the agent only into 64-bit
pools (otherwise the load failure can take the 32-bit pool down):

```powershell
& $appcmd set config /section:system.webServer/globalModules `
    "/[name='TreblleAgent'].preCondition:bitness64" /commit:apphost
iisreset
```

To instead run the pool as 64-bit:

```powershell
Set-ItemProperty "IIS:\AppPools\YourAppPoolName" -Name enable32BitAppOnWin64 -Value $false
Restart-WebAppPool -Name "YourAppPoolName"
```

**ACL fix:**

```powershell
icacls "C:\iismodules\treblle" /grant "IIS_IUSRS:(OI)(CI)RX" /T
iisreset
```

**Unblock check/fix:**

```powershell
Get-Item C:\iismodules\treblle\TreblleAgent.dll -Stream * | Select-Object Stream
Unblock-File -Path C:\iismodules\treblle\TreblleAgent.dll
```

**Check for IIS-level load errors:**

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WAS'} -MaxEvents 20 |
    Format-List TimeCreated, Id, Message
```

---

## Step 3 - Is the config file found and valid?

The agent reads `treblle.config` **from the same folder as the DLL**. With the
default install that is:

```
C:\iismodules\treblle\treblle.config
```

### Check

```powershell
$cfg = "C:\iismodules\treblle\treblle.config"

Test-Path $cfg
Get-Item $cfg | Select-Object FullName, Length, LastWriteTime

# Is it valid JSON?
try { Get-Content $cfg -Raw | ConvertFrom-Json | Out-Null; "JSON OK" }
catch { "JSON INVALID: $($_.Exception.Message)" }

# Are both credentials present and non-empty?
$j = Get-Content $cfg -Raw | ConvertFrom-Json
[pscustomobject]@{
    api_key_len   = $j.api_key.Length
    sdk_token_len = $j.sdk_token.Length
    treblle_url   = $j.treblle_url
    debug         = $j.debug
    disabled      = $j.disabled
    exclusions    = $j.exclude_routes.Count
}

# Can the app pool identity read it?
icacls $cfg
```

### Rules the agent enforces

| Condition | Result |
|---|---|
| File missing, empty, or unreadable | Config not loaded → **agent tracks nothing** |
| Invalid JSON (trailing comma, smart quotes, UTF-16 encoding) | Config not loaded → **agent tracks nothing** |
| `api_key` empty **or** `sdk_token` empty | Config rejected → **agent tracks nothing** |
| `"disabled": true` | Agent tracks nothing (by design) |
| Valid | Tracking active |

If the config is not loaded you will see this in the event log on **every**
request (it is logged unconditionally, so it will be noisy):

```
[TREBLLE]: Treblle: OnBeginRequest - config not loaded, skipping
[TREBLLE]: Treblle: config must have non-empty 'sdk_token' and 'api_key' - module disabled
[TREBLLE]: Treblle: config parse error: <details>
```

### Gotchas

- **Encoding.** Save as UTF-8. Notepad's "Unicode" (UTF-16) will not parse. A UTF-8 BOM is tolerated.
- **Smart quotes.** Pasting from Word/Slack/email can replace `"` with `“ ”`. Retype the quotes.
- **Trailing commas.** `"disabled": false,` before `}` is invalid JSON.
- **Keys swapped.** `api_key` and `sdk_token` are different values from the Treblle dashboard. Swapping them produces a valid-looking config that gets rejected by ingress (HTTP 401/403). See [Step 7](#step-7--is-treblle-accepting-the-payload).
- **Editing does not need a restart.** The agent re-reads the file when its modification time changes, checked on every request. Save the file and the next request picks it up.
- **Config next to the wrong DLL.** If you ever installed to a second location, both DLLs will look for their own config. Confirm the path from `globalModules` (Step 1).

### Minimal known-good config

```json
{
  "api_key": "YOUR_TREBLLE_API_KEY",
  "sdk_token": "YOUR_TREBLLE_SDK_TOKEN",
  "treblle_url": "https://ingress.treblle.com",
  "debug": true,
  "disabled": false,
  "exclude_routes": []
}
```

Write it from PowerShell without encoding surprises:

```powershell
@'
{
  "api_key": "YOUR_TREBLLE_API_KEY",
  "sdk_token": "YOUR_TREBLLE_SDK_TOKEN",
  "treblle_url": "https://ingress.treblle.com",
  "debug": true,
  "disabled": false,
  "exclude_routes": []
}
'@ | Set-Content -Path C:\iismodules\treblle\treblle.config -Encoding UTF8
```

---

## Step 4 - Is the request actually being tracked?

This is where most "installed but no data" cases end up. The agent deliberately
ignores anything that is not JSON API traffic.

### Turn on debug and read the reasons

```powershell
# 1. Enable debug (takes effect on the next request - no restart)
$cfg = "C:\iismodules\treblle\treblle.config"
(Get-Content $cfg -Raw) -replace '"debug"\s*:\s*false', '"debug": true' |
    Set-Content $cfg -Encoding UTF8

# 2. Note the time, then make exactly one call to the API
$t0 = Get-Date
curl.exe -i https://api.yourdomain.com/v1/your-endpoint

# 3. Read what the agent decided
Get-EventLog -LogName Application -Source Treblle -After $t0 -ErrorAction SilentlyContinue |
    Sort-Object TimeGenerated | Format-List TimeGenerated, Message
```

> **If `Get-EventLog` returns nothing**, the source may not be registered in the
> registry. Fall back to a message-text search:
> ```powershell
> Get-WinEvent -LogName Application -MaxEvents 300 |
>     Where-Object { $_.Message -like '*TREBLLE*' } |
>     Select-Object TimeCreated, Message -First 40 | Format-List
> ```
> Optionally register the source once so Event Viewer renders it cleanly:
> ```powershell
> New-EventLog -LogName Application -Source Treblle
> ```
> Event Viewer may show *"The description for Event ID 0 ... cannot be found"* - that
> is cosmetic. The actual `[TREBLLE]: ...` text is printed underneath it.

**Remember to set `"debug": false` when you are done.** With debug on, the agent
writes an Event Log entry for every skipped request, which floods the Application
log on a busy server.

### What each skip message means

| Event log message | Why | Fix |
|---|---|---|
| `skip - default excluded path prefix "/health": /health` | Built-in exclusion (see list below) | Test against a real API endpoint instead |
| `skip - matched exclude_routes: api.x.com/v1` | Your `exclude_routes` config | Remove the entry from `treblle.config` |
| `skip - method not tracked: TRACE` | Only GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS are tracked | n/a |
| `skip - response Content-Type "text/html" is not JSON: ...` | The response is not JSON | see below |
| `OnBeginRequest - agent disabled, skipping` | `"disabled": true` | set it to `false` |
| *(no message at all for your request)* | The request never reached the module | Steps 1, 2, and "Request never reaches the agent" below |

### Built-in exclusions you cannot override

These URL path prefixes are **always** skipped, on every host, case-insensitively:

| Prefix | Also matches |
|---|---|
| `/.well-known/` | OIDC discovery, JWKS, `security.txt` |
| `/health` | `/healthz`, `/health/live`, `/health/ready`, `/healthcheck` |
| `/swagger` | `/swagger-ui`, `/swagger/v1/swagger.json` |
| `/openapi` | `/openapi.json`, `/openapi/v1` |
| `/api-docs` | any JSON docs path |

> **Very common false alarm:** the customer tests with `/health` or a Swagger URL,
> sees nothing in Treblle, and concludes the agent is broken. Always test with a
> real business endpoint.

### The response Content-Type rule

A request is only sent to Treblle if the **response** `Content-Type` header
*contains* the literal string `application/json`.

| Content-Type | Tracked? |
|---|---|
| `application/json` | ✅ |
| `application/json; charset=utf-8` | ✅ |
| `text/json` | ❌ |
| `application/problem+json` (ASP.NET Core `ProblemDetails`) | ❌ |
| `application/vnd.api+json`, `application/hal+json`, `application/ld+json` | ❌ |
| `text/plain`, `text/html`, `application/xml` | ❌ |
| *no Content-Type* (typical for `204 No Content`, `304 Not Modified`) | ❌ |
| An unhandled `500` rendered by IIS as HTML | ❌ |

Check what your API actually returns - from the server itself, so you bypass any
load balancer or CDN:

```powershell
curl.exe -i -s -o NUL -D - https://api.yourdomain.com/v1/your-endpoint
# or, hitting the loopback with an explicit Host header:
curl.exe -i --resolve api.yourdomain.com:443:127.0.0.1 https://api.yourdomain.com/v1/your-endpoint
```

If your framework emits `text/json` or `application/problem+json`, configure it to
emit `application/json`, or contact Treblle support - the accepted list is a
one-line change in the agent.

### Exclusion rules in your config

```powershell
(Get-Content C:\iismodules\treblle\treblle.config -Raw | ConvertFrom-Json).exclude_routes |
    Format-Table host, path
```

Matching semantics:

- `host` must equal the request's `Host` header (case-insensitive, **port excluded**).
- `path` is a **prefix** match on the URL path (case-insensitive). `"path": "/v1"` also excludes `/v1anything`.
- An entry with no `path` excludes the entire host.
- An empty or absent `exclude_routes` array means **monitor everything** (that is the default and the recommended starting point while debugging).

Reset to "track everything" while debugging:

```powershell
$cfg = "C:\iismodules\treblle\treblle.config"
$j = Get-Content $cfg -Raw | ConvertFrom-Json
$j.exclude_routes = @()
$j.debug = $true
$j | ConvertTo-Json -Depth 5 | Set-Content $cfg -Encoding UTF8
```

### Request never reaches the agent at all

If debug is on and you see **no event at all** for your test request:

1. **Are you hitting this server?** Behind a load balancer, your call may land on a
   different node. Test from the server against `127.0.0.1` with an explicit `Host`
   header, or install the agent on every node.
2. **Is the agent installed on all nodes?** Data from a farm only appears if each
   node has the agent.
3. **HTTP.SYS kernel-mode cache.** Cached responses are served from kernel mode and
   never enter `w3wp.exe`.
   ```powershell
   netsh http show cachestate
   ```
   If your endpoint appears there, disable kernel caching for that site/URL to test:
   ```powershell
   $appcmd = "$env:windir\System32\inetsrv\appcmd.exe"
   & $appcmd set config "Default Web Site/" /section:system.webServer/caching /enabled:false /commit:apphost
   ```
   (Kernel caching normally does not apply to authenticated requests or requests with query strings.)
4. **The request is terminated before the managed pipeline** - e.g. by URL Rewrite
   returning a response, IP restrictions, or Request Filtering. Those still hit
   `OnBeginRequest`; if you see the begin-request events but no send-response event,
   the response was not JSON.
5. **Wrong app pool recycled.** Ensure the pool that serves the API is the one you
   restarted.

---

## Step 5 - Is the agent sending data out?

Once a request is tracked, the payload goes into an in-memory queue and a
background thread POSTs it. With `"debug": true` you get:

```
[TREBLLE]: Treblle: sending payload (2841 bytes)
```

```powershell
Get-EventLog -LogName Application -Source Treblle -Newest 50 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -like "*sending payload*" } |
    Format-Table TimeGenerated, Message -AutoSize -Wrap
```

**If you see `sending payload` and no error after it, the payload was accepted
(HTTP 2xx) and the problem is on the dashboard side** - see
[Step 7](#step-7--is-treblle-accepting-the-payload).

Things that can silently drop payloads at this stage:

| Behaviour | Detail |
|---|---|
| **Queue full** | Max 5,000 pending payloads per worker process. Oldest is dropped. Only reachable if the network is down for a sustained period at high traffic. |
| **Circuit breaker** | After **5 consecutive** network failures or 5xx responses, sends pause for **30 seconds**, then one probe is allowed. Logged as `circuit breaker tripped - pausing sends for 30s`. |
| **429 backoff** | On HTTP 429 the agent honours `Retry-After` (default 60s) and sends nothing during the window. |
| **App pool recycle** | On shutdown the worker gets 8 seconds to drain. Payloads still queued after that are lost. Frequent recycling (default idle timeout is 20 minutes) can lose the tail end of a burst. |
| **Worker thread failed to start** | Logged as `CreateThread failed - worker will not run`. Recycle the pool. |

Watch actual outbound connections from the worker process:

```powershell
Get-Process w3wp | ForEach-Object {
    Get-NetTCPConnection -OwningProcess $_.Id -ErrorAction SilentlyContinue |
        Where-Object { $_.RemotePort -eq 443 } |
        Select-Object @{n='PID';e={$_.OwningProcess}}, RemoteAddress, RemotePort, State
} | Format-Table -AutoSize

# Resolve which of those is Treblle
Resolve-DnsName ingress.treblle.com | Select-Object Name, IPAddress
```

---

## Step 6 - Network: firewall, proxy, DNS, TLS

The agent uses **WinHTTP** (not .NET's HTTP stack, not WinINET). That matters for
proxy configuration.

### 6.1 DNS and TCP

```powershell
Resolve-DnsName ingress.treblle.com
Test-NetConnection ingress.treblle.com -Port 443 -InformationLevel Detailed
```

`TcpTestSucceeded : True` is required. `False` means egress is blocked by a
firewall, a security group, or a proxy-only network policy.

### 6.2 Test the exact stack the agent uses

`Invoke-WebRequest` and `curl.exe` do **not** use WinHTTP proxy settings, so a
passing test there can hide a proxy problem. Use the WinHTTP COM object instead -
this is the closest possible simulation of the agent's send path:

```powershell
$token = (Get-Content C:\iismodules\treblle\treblle.config -Raw | ConvertFrom-Json).sdk_token

$w = New-Object -ComObject WinHttp.WinHttpRequest.5.1
$w.SetTimeouts(5000,5000,5000,5000)
$w.Open("POST", "https://ingress.treblle.com", $false)
$w.SetRequestHeader("Content-Type", "application/json")
$w.SetRequestHeader("x-api-key", $token)
try {
    $w.Send('{"connectivity_test":true}')
    "HTTP $($w.Status) $($w.StatusText)"
    $w.ResponseText
} catch {
    "WinHTTP ERROR: $($_.Exception.Message)"
}
```

Interpreting the result:

| Result | Meaning |
|---|---|
| Any HTTP status at all (200, 400, 401, 422…) | **Network path is open.** Move to Step 7. |
| `The server name or address could not be resolved` (12007) | DNS blocked or no resolver |
| `Cannot connect` / timeout (12002 / 12029) | Firewall or proxy blocking egress on 443 |
| `A security error occurred` (12175) | TLS failure - see 6.4 |
| `407 Proxy Authentication Required` | Authenticating proxy - see 6.3 |

### 6.3 Proxy

```powershell
# What WinHTTP (and therefore the agent) will use:
netsh winhttp show proxy
```

If the environment requires a proxy for outbound HTTPS, it must be configured at
the **machine WinHTTP** level. Per-user Internet Options and .NET
`<system.net><defaultProxy>` settings have **no effect** on this agent.

```powershell
# Set explicitly
netsh winhttp set proxy proxy-server="http=proxy.corp.local:8080;https=proxy.corp.local:8080" bypass-list="<local>"

# Or import the current user's IE settings
netsh winhttp import proxy source=ie

# Remove
netsh winhttp reset proxy
```

**After changing the proxy, recycle the app pools** - the agent creates its WinHTTP
session when the worker process starts, so it picks up proxy config at that point:

```powershell
iisreset
```

**Authenticating proxies (407):** the agent does not send proxy credentials. Ask the
network team to allow `ingress.treblle.com:443` to bypass authentication, or to
allowlist the server.

### 6.4 TLS

Treblle's ingress requires **TLS 1.2 or higher**. Hardened servers sometimes disable
protocols system-wide via SCHANNEL.

```powershell
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\SecurityProviders\SCHANNEL\Protocols' -Recurse |
    ForEach-Object {
        $p = $_.PSPath
        [pscustomobject]@{
            Key      = $_.Name
            Enabled  = (Get-ItemProperty $p -Name Enabled -ErrorAction SilentlyContinue).Enabled
            Disabled = (Get-ItemProperty $p -Name DisabledByDefault -ErrorAction SilentlyContinue).DisabledByDefault
        }
    } | Format-Table -AutoSize
```

`TLS 1.2\Client` must not be disabled (`Enabled = 0` or `DisabledByDefault = 1`).

Also check the machine clock - a skewed clock breaks certificate validation:

```powershell
w32tm /query /status
Get-Date
```

And confirm root certificate chain / TLS inspection: if the network does HTTPS
interception, the interception CA must be in the machine's Trusted Root store, or
`ingress.treblle.com` must be excluded from inspection.

### 6.5 Firewall

```powershell
# Outbound default action per profile - 'Allow' is the Windows default
Get-NetFirewallProfile | Select-Object Name, Enabled, DefaultOutboundAction

# Any outbound block rules that could match
Get-NetFirewallRule -Direction Outbound -Enabled True -Action Block |
    Select-Object DisplayName, Profile | Format-Table -AutoSize
```

**What to ask the network team to allow:**

| Field | Value |
|---|---|
| Direction | Outbound |
| Destination | `ingress.treblle.com` |
| Port | `443/TCP` |
| Protocol | HTTPS (TLS 1.2+, HTTP/2 with HTTP/1.1 fallback) |
| Source process | `C:\Windows\System32\inetsrv\w3wp.exe` |
| Data direction | Egress only - the agent never receives inbound connections |

> Treblle's ingress sits behind a CDN, so its IP addresses change. Allowlist the
> **hostname**, not an IP. If the firewall is IP-only, contact Treblle support for
> current ranges.

---

## Step 7 - Is Treblle accepting the payload?

With `"debug": true`, non-2xx responses from ingress are logged:

```
[TREBLLE]: Treblle: ingress returned HTTP 401
```

| Status | Meaning | Fix |
|---|---|---|
| **401 / 403** | Bad or swapped credentials | Re-copy `api_key` and `sdk_token` from the Treblle dashboard. They are two different values - confirm neither was pasted into the other's field, and that there is no trailing whitespace or newline. |
| **404** | Wrong `treblle_url` | It should be exactly `https://ingress.treblle.com` (no trailing path). |
| **413** | Payload too large | Bodies over 2 MB are already replaced with a marker; if you still see this, report it to support. |
| **422 / 400** | Payload rejected | Send the debug log to Treblle support. |
| **429** | Rate limited | The agent backs off automatically. Check your Treblle plan limits. |
| **5xx** | Treblle-side issue | The circuit breaker will retry. Check [status.treblle.com](https://status.treblle.com) and contact support. |

Verify credentials without touching the server config - run the WinHTTP test in
[6.2](#62-test-the-exact-stack-the-agent-uses) with the values from the config file
and compare the status.

### Data is sent (HTTP 2xx) but the dashboard looks empty

- **Check the right project.** `api_key` decides which API/project in Treblle the
  data lands in. If the customer created several, they may be looking at the wrong one.
- **Check the right workspace.** The `sdk_token` is workspace-scoped.
- **A new API entry may have been created.** The agent groups traffic by the
  request's `Host` header and reports the **IIS site name** as the service name. If
  the site name is `Default Web Site`, look for that.
- **Time range filter.** Widen the dashboard's time filter to "Last 24 hours".
- **Latency.** Delivery is near-real-time (seconds). If you have waited minutes and
  see `sending payload` with no error, escalate to support with the log.

---

## Symptom index

### "Nothing at all appears, and the event log has no Treblle entries"

The DLL is not loaded. → [Step 1](#step-1--is-the-agent-registered-with-iis),
[Step 2](#step-2--is-the-dll-actually-loaded-into-the-worker-process).

### "The event log shows `config not loaded` over and over"

`treblle.config` is missing, unreadable, invalid JSON, or has an empty `api_key`
/ `sdk_token`. → [Step 3](#step-3--is-the-config-file-found-and-valid).

### "The event log shows `skip - response Content-Type ... is not JSON`"

Your API is not returning `application/json`, or a framework is returning
`application/problem+json` / `text/json`. →
[The response Content-Type rule](#the-response-content-type-rule).

### "Some endpoints show up, others never do"

Check, in order: built-in path exclusions (`/health`, `/swagger`, `/openapi`,
`/api-docs`, `/.well-known/`), your `exclude_routes`, and the response Content-Type
of the missing endpoints (error paths often return HTML or `problem+json`).

### "Requests appear but request/response bodies are empty (`{}`)"

| Cause | Check | Fix |
|---|---|---|
| **Dynamic compression** - the agent sees gzip/brotli bytes, not JSON | `curl.exe -i -H "Accept-Encoding: identity" <url>` - if the body appears in Treblle for that call, compression is the cause | Disable dynamic compression for the API site, or contact support |
| Request had no `Content-Type: application/json` | Inspect the client's request headers | Only JSON and `multipart/form-data` request bodies are captured |
| Body larger than 2 MB | Treblle shows `{"treblle_error": "Payload exceeds 2MB..."}` | By design |
| Response served from a file handle (static `.json` file) | The handler is `StaticFileModule` | By design - only in-memory response chunks are captured |
| Response streamed via a mechanism that bypasses in-memory chunks | Rare | Report to support with the framework details |

Disable dynamic compression for a site to test:

```powershell
$appcmd = "$env:windir\System32\inetsrv\appcmd.exe"
& $appcmd set config "Default Web Site/" /section:urlCompression /doDynamicCompression:false /commit:apphost
```

### "It worked, then stopped"

| Cause | Check |
|---|---|
| Config was edited and is now invalid JSON | `Get-Content ... \| ConvertFrom-Json` |
| `"disabled": true` was set | Read the config |
| Credentials rotated in the Treblle dashboard | Event log shows `HTTP 401` |
| Plan limit reached | Event log shows `429 received - backing off` |
| Circuit breaker open due to network change | Event log shows `circuit breaker tripped` |
| App pool stopped / site down | `& $appcmd list wp`, `Get-Website` |
| Agent DLL replaced/upgraded without an `iisreset` | Compare `LastWriteTime` with the last `RegisterModule` event |

### "IIS started failing / 503s after installing the agent"

Stop the bleeding first:

```powershell
$appcmd = "$env:windir\System32\inetsrv\appcmd.exe"
& $appcmd uninstall module /module.name:TreblleAgent
iisreset
```

Then diagnose: 32-bit app pool ([Step 2](#step-2--is-the-dll-actually-loaded-into-the-worker-process)),
missing ACLs, or a blocked/corrupt DLL. Check Event Viewer → **Windows Logs → System**
for WAS events 5009/5011 and → **Application** for faulting-module entries.

### "Data shows up under an unexpected API / service name in Treblle"

The service name comes from the **IIS site name** (falling back to the app pool
name, then the `Host` header). Rename the site in IIS Manager, or ask Treblle
support to rename the API in the dashboard.

### "Only one server in the farm reports data"

The agent must be installed on every node. Verify with the 60-second triage on each.

### "Debug logging is flooding the Application event log"

Expected - with `debug: true` each skipped request logs a line. Set it back:

```powershell
$cfg = "C:\iismodules\treblle\treblle.config"
(Get-Content $cfg -Raw) -replace '"debug"\s*:\s*true', '"debug": false' | Set-Content $cfg -Encoding UTF8
```

Takes effect immediately - no restart. While debugging on a busy server, consider
temporarily raising the Application log size:

```powershell
Limit-EventLog -LogName Application -MaximumSize 128MB -OverflowAction OverwriteAsNeeded
```

---

## Reference: every reason a request is skipped

In evaluation order:

| # | Condition | Log message (debug) |
|---|---|---|
| 1 | Config not loaded (missing file / bad JSON / empty credentials) | `OnBeginRequest - config not loaded, skipping` *(always logged)* |
| 2 | `"disabled": true` | `OnBeginRequest - agent disabled, skipping` |
| 3 | URL path starts with `/.well-known/`, `/health`, `/swagger`, `/openapi`, `/api-docs` | `skip - default excluded path prefix "<p>": <path>` |
| 4 | Host + path matches an `exclude_routes` entry | `skip - matched exclude_routes: <host><path>` |
| 5 | HTTP method is not GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS | `skip - method not tracked: <method>` |
| 6 | Response `Content-Type` does not contain `application/json` | `skip - response Content-Type "<ct>" is not JSON: <name><path>` |
| 7 | Response never reached the send-response stage (connection aborted, request killed) | *(no message)* |

---

## Reference: event log messages

Source: **Treblle**, log: **Application**, all prefixed `[TREBLLE]:`.

| Message | Logged when | Meaning |
|---|---|---|
| `RegisterModule starting - DLL: <path>` | always | Worker process started, DLL loaded |
| `agent registered successfully - DLL: <path>` | always | Agent hooked into the IIS pipeline |
| `config failed to load - check api_key and sdk_token` | always | Config invalid at startup |
| `config must have non-empty 'sdk_token' and 'api_key' - module disabled` | always | Credentials missing |
| `config parse error: <detail>` | always | Malformed JSON |
| `OnBeginRequest - config not loaded, skipping` | always | Per-request; agent is inert |
| `failed to allocate AsyncQueue` / `CreateThread failed` | always | Startup resource failure - recycle the pool |
| `queue is null - payload dropped` | always | Startup failed earlier |
| `unhandled exception in <callback>` | always | Report to Treblle support with the surrounding entries |
| `skip - ...` | debug only | See the skip table above |
| `sending payload (N bytes)` | debug only | Payload leaving the server |
| `WinHttpCrackUrl failed for URL: <url>` | debug only | Malformed `treblle_url` |
| `WinHttpConnect failed` | debug only | DNS/TCP failure |
| `WinHttpOpenRequest failed` | debug only | WinHTTP handle failure |
| `WinHttpSendRequest failed (0x…)` | debug only | Network/TLS failure - decode the code below |
| `ingress returned HTTP <code>` | debug only | Non-2xx from Treblle |
| `429 received - backing off for N s` | debug only | Rate limited |
| `circuit breaker tripped - pausing sends for 30s` | debug only | 5 consecutive failures |
| `circuit breaker probing after cooldown` | debug only | Retrying after 30s |
| `worker thread did not exit within drain timeout` | always | Shutdown took longer than 8s; some payloads may be lost |

### Common WinHTTP error codes

| Hex | Decimal | Meaning |
|---|---|---|
| `0x00002EE2` | 12002 | Timeout - firewall silently dropping traffic |
| `0x00002EE5` | 12005 | Invalid URL |
| `0x00002EE7` | 12007 | Name not resolved - DNS |
| `0x00002EEC` | 12012 | Shutting down |
| `0x00002EFD` | 12029 | Cannot connect - port blocked |
| `0x00002F0C` | 12044 | Client certificate required (TLS-inspecting proxy) |
| `0x00002F7C` | 12172 | Secure channel support not available |
| `0x00002F8F` | 12175 | TLS/certificate failure - see [6.4](#64-tls) |

