# Upgrading the Treblle IIS Agent: 1.0.2 to 1.0.3

This guide walks through upgrading a production Treblle IIS Agent from **1.0.2**
to **1.0.3** and configuring dual-destination delivery so traffic is sent to an
on-prem ingress endpoint while a secondary copy is mirrored to Treblle Cloud as a
backup.

Target end state:

```json
{
  "treblle_url": "https://10.0.30.43:8083",
  "secondary_treblle_url": "https://ingress.treblle.com"
}
```

Run every command from an **elevated PowerShell** session (Run as Administrator)
on the web server itself.

---

## What changed in 1.0.3

1.0.3 adds the `secondary_treblle_url` config field. When set, every tracked
request is mirrored to a second ingress endpoint in addition to `treblle_url`.
Each destination has its own queue, worker thread, and circuit breaker, so an
outage on one endpoint never blocks or slows down the other. This is what lets
you point the primary destination at an on-prem ingress and use Treblle Cloud as
a resilient backup.

No config fields were removed or renamed between 1.0.2 and 1.0.3, so this is a
straightforward upgrade with one new optional field.

---

## Is a restart needed?

Yes, but only for the DLL swap, not for the config change.

| Change | Restart needed? | Why |
|---|---|---|
| Replacing `TreblleAgent.dll` with the 1.0.3 build | **Yes** | The DLL is loaded into every `w3wp.exe` worker process. Windows locks a loaded DLL file, so it cannot be overwritten until IIS releases it. |
| Editing `treblle.config` (new URLs) | **No** | The agent checks the config file's modification time on every request and hot-reloads it automatically. |

Because the DLL swap already requires a restart, do the config edit in the same
maintenance window so you only take one outage window instead of two.

---

## Step 1 - Get the 1.0.3 DLL

Download the new version of the Treblle Agent DLL from our Github Release page: https://github.com/Treblle/treblle-iis/releases/tag/v1.0.3

---

## Step 2 - Replace the DLL and restart IIS

The DLL is locked while IIS worker processes are running, so IIS has to be
stopped before the file can be overwritten.

```powershell
$installDir = "C:\iismodules\treblle"

# Stop IIS to release the file lock on TreblleAgent.dll
iisreset /stop

# Replace the DLL
Copy-Item -Path ".\TreblleAgent.dll" -Destination "$installDir\TreblleAgent.dll" -Force

# Start IIS back up
iisreset /start
```

If `Copy-Item` still reports the file is in use, a worker process did not fully
exit. Confirm with `tasklist /m TreblleAgent.dll` (should return nothing while
IIS is stopped) before retrying the copy.

> **Alternative:** re-running `installer\install.ps1` also handles this (it
> re-registers the module and calls `iisreset`), but it will prompt for a new
> config if one isn't already present. Since this server already has a config,
> the manual stop/copy/start above is simpler and avoids re-registration.

---

## Step 3 - Update the config

Edit `treblle.config` directly, or use PowerShell so you don't have to worry
about encoding:

```powershell
$cfg = "C:\iismodules\treblle\treblle.config"

$json = Get-Content $cfg -Raw | ConvertFrom-Json
$json.treblle_url = "https://10.0.30.43:8083"
$json | Add-Member -NotePropertyName secondary_treblle_url -NotePropertyValue "https://ingress.treblle.com" -Force
$json | ConvertTo-Json -Depth 5 | Set-Content -Path $cfg -Encoding UTF8
```

Resulting `treblle.config` should look like this (existing `api_key`,
`sdk_token`, `exclude_routes`, and `masked_keywords` are left as they were):

```json
{
  "api_key": "YOUR_TREBLLE_API_KEY",
  "sdk_token": "YOUR_TREBLLE_SDK_TOKEN",
  "treblle_url": "https://10.0.30.43:8083",
  "secondary_treblle_url": "https://ingress.treblle.com",
  "debug": false,
  "disabled": false,
  "exclude_routes": [ ... ],
  "masked_keywords": [ ... ]
}
```

Notes on the values:

- Both destinations use the **same** `api_key` and `sdk_token`. This mirrors
  identical traffic to two endpoints; it does not split traffic across two
  Treblle projects.
- If `secondary_treblle_url` ever ends up equal to `treblle_url`, the agent
  ignores it and logs a debug warning rather than sending duplicate traffic to
  the same place, so a typo here fails safe.
- This edit does **not** require a restart on its own, since the file's
  modification time changed and the agent reloads it on the next request. It's
  only bundled into this same maintenance window because the DLL swap already
  forced one.

