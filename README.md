# Treblle IIS Native HTTP Module

A native C++ IIS HTTP Module for **Windows Server 2022 / 2025** that passively monitors JSON API traffic and forwards structured telemetry to [Treblle](https://treblle.com). Zero impact on API availability or latency.

---

## What it does

The module hooks into the IIS request pipeline and, for every matching JSON API request:

1. Captures the request method, URL, headers, body, and client IP
2. Captures the response status code, headers, and body
3. Measures end-to-end request time
4. Sends the data asynchronously (off the IIS thread) to Treblle's ingress API

It **never** blocks a request, **never** modifies a response, and **never** crashes or throws errors that would affect your APIs. If anything goes wrong internally it silently discards the telemetry.

---

## Requirements

| Requirement | Version |
|-------------|---------|
| Windows Server | 2022 or 2025 |
| IIS | 10.0 |
| Architecture | x64 only |
| Privileges | Local Administrator (install only) |
| Visual Studio (to build) | 2022 with "Desktop development with C++" |
| Windows SDK (to build) | 10.0 |

---

## Installation

### Step 1 — Build the DLL

Open `TreblleModule.sln` in **Visual Studio 2022**, select **Release | x64**, and build. The output is:

```
x64\Release\TreblleModule.dll
```

Alternatively, build from the command line:

```powershell
msbuild TreblleModule.sln /p:Configuration=Release /p:Platform=x64
```

> **Pre-built release:** Download `TreblleModule.dll` from the [Releases](../../releases) page and skip this step.

### Step 2 — Run the installer

Copy `TreblleModule.dll` into the `installer\` directory (or build in place), then run:

```powershell
# Right-click PowerShell → Run as Administrator
cd path\to\treblle-iis\installer
.\install.ps1
```

The installer will:
- Copy `TreblleModule.dll` to `C:\iismodules\treblle\`
- Prompt you for your **API Key**, **SDK Token**, and optional exclusion patterns
- Write `C:\iismodules\treblle\treblle.config`
- Register the module globally in IIS
- Restart IIS

### Step 3 — Verify

```powershell
# List all registered IIS modules
%windir%\system32\inetsrv\appcmd.exe list module

# Should include:
# MODULE "TreblleModule" (image:C:\iismodules\treblle\TreblleModule.dll)
```

Make a request to one of your configured API endpoints and check your [Treblle dashboard](https://treblle.com).

---

## Configuration

The config file lives at `C:\iismodules\treblle\treblle.config`. **Edits take effect immediately** — no IIS restart needed. The module checks the file modification time on every request and reloads it if changed.

```json
{
  "api_key":    "YOUR_TREBLLE_API_KEY",
  "sdk_token":  "YOUR_TREBLLE_SDK_TOKEN",
  "treblle_url": "https://ingress.treblle.com",
  "debug": false,
  "exclude_routes": [
    { "host": "internal.yourdomain.com" },
    { "host": "api.yourdomain.com", "path": "/health" },
    { "host": "api.yourdomain.com", "path": "/metrics" }
  ],
  "masked_keywords": [
    "password", "pwd", "secret",
    "password_confirmation", "passwordConfirmation",
    "cc", "card_number", "cardNumber", "ccv",
    "credit_score", "creditScore", "ssn"
  ]
}
```

### Configuration reference

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `api_key` | string | — | **Required.** Your Treblle API key. |
| `sdk_token` | string | — | **Required.** Your Treblle SDK token. |
| `treblle_url` | string | `https://ingress.treblle.com` | Treblle ingress endpoint. Override only if directed by Treblle support. |
| `debug` | bool | `false` | When `true`, errors are written to the Windows Application Event Log (source: `Treblle`). Leave `false` in production. |
| `exclude_routes` | array | `[]` | List of route objects to exclude from monitoring. **Empty = monitor all JSON API traffic.** |
| `masked_keywords` | array | *(see below)* | List of field names whose values are redacted before sending to Treblle. Omit to use the built-in defaults. Set to `[]` to disable masking entirely. |

Each object in `exclude_routes`:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `host` | string | Yes | Hostname to exclude (case-insensitive). Must match the HTTP `Host` header, excluding port. |
| `path` | string | No | URL path prefix to exclude (case-insensitive). If omitted, the entire host is excluded. |

---

## exclude_routes explained

The module monitors **all JSON API traffic by default**. Use `exclude_routes` to opt specific hosts or paths out.

A request is tracked unless:
- Its `Host` header matches an entry's `host` field, **and**
- Its URL path starts with the entry's `path` prefix (if specified)

All non-JSON responses (HTML, CSS, JS, images) are automatically ignored — no configuration needed for those.

A request is tracked only when:
- The response `Content-Type` contains `application/json`
- The HTTP method is one of: `GET POST PUT PATCH DELETE HEAD OPTIONS`
- It does not match any `exclude_routes` entry

**Common scenarios:**

```json
// Exclude an entire internal host
{ "host": "internal.yourdomain.com" }

// Exclude health/metrics endpoints on a specific host
{ "host": "api.yourdomain.com", "path": "/health" },
{ "host": "api.yourdomain.com", "path": "/metrics" }

// Exclude a legacy API version
{ "host": "api.yourdomain.com", "path": "/v1" }
```

If `exclude_routes` is omitted or empty, all JSON API traffic across all hosts is monitored.

---

## Sensitive data masking

The module redacts the values of fields whose names match `masked_keywords` before any data leaves the server. Masking is applied to:

- Request body (JSON)
- Response body (JSON)
- Request headers
- Response headers

Each character of a matched value is replaced with `*` so the length is preserved:

```
"password": "hunter2"   →   "password": "*******"
"cc": "4111111111111111" →   "cc": "****************"
```

### Default masked keywords

The following fields are masked automatically when `masked_keywords` is omitted from the config:

`password`, `pwd`, `secret`, `password_confirmation`, `passwordConfirmation`, `cc`, `card_number`, `cardNumber`, `ccv`, `credit_score`, `creditScore`, `ssn`

### Customising the list

Add or remove keywords by specifying your own list. The list replaces the defaults entirely, so include any defaults you want to keep:

```json
"masked_keywords": [
  "password", "pwd", "secret",
  "password_confirmation", "passwordConfirmation",
  "cc", "card_number", "cardNumber", "ccv",
  "credit_score", "creditScore", "ssn",
  "api_token", "access_token", "private_key"
]
```

To disable masking completely:

```json
"masked_keywords": []
```

### Masking behaviour reference

| Value type | Input | Output |
|------------|-------|--------|
| String | `"hunter2"` | `"*******"` |
| Number | `4242` | `"****"` |
| Boolean / null | `true` | `"****"` |
| Nested object or array | `{"a":1}` | `"*"` |

Matching is **case-insensitive** and applies at **any nesting depth** in the JSON body. Bodies larger than **500 KB** are sent unmasked to avoid performance impact — ensure sensitive fields are not present in very large payloads.

> **Changes to `masked_keywords` take effect immediately** without an IIS restart, just like all other config changes.

---

## How it works

```
IIS Worker Process (w3wp.exe)
  └── TreblleModule.dll
        ├── OnBeginRequest   → route check → capture request headers + body
        ├── OnSendResponse   → capture response headers + body chunks
        └── OnEndRequest     → build JSON payload → push to background queue
                                    │
                                    └── Background thread
                                          └── WinHTTP HTTPS POST → ingress.treblle.com
```

- **Zero blocking:** The IIS request thread only pushes a string to an in-memory queue. All network I/O happens on a dedicated background thread.
- **Bounded memory:** The queue holds at most 5,000 pending payloads. If it fills up, the oldest entry is dropped rather than blocking.
- **No dependencies:** The module uses only Windows built-in APIs (WinHTTP, Windows SDK). No VC++ Redistributable or third-party packages are required.
- **Static CRT:** The module links the C runtime statically (`/MT`) so it works on any Windows Server without additional installs.

### Request/response body limits

| Condition | Behaviour |
|-----------|-----------|
| Body ≤ 2 MB | Captured and sent to Treblle |
| Body > 2 MB | Replaced with `{"treblle_error": "Payload exceeds 2MB and will not be tracked by Treblle"}` |
| Non-JSON body | Stored as `{}` |

---

## Performance impact

The module's work on the hot path is minimal:
- Config check: one `GetFileAttributesEx` call + mutex read (sub-microsecond)
- Exclusion check: O(n) string comparison over `exclude_routes` entries (skipped entirely when list is empty)
- Body reading (request): buffered synchronous read + re-insert for downstream
- Body reading (response): direct memory access to already-buffered chunks
- JSON serialisation and network send happen entirely off the IIS thread

In benchmarks on a 32-core Windows Server 2022 machine, the overhead per request is **< 5 µs** on the IIS thread.

---

## Updating

1. Build or download the new `TreblleModule.dll`
2. Run the installer again — it handles re-registration automatically:
   ```powershell
   .\installer\install.ps1
   ```
   The installer removes the old module registration and adds the new one.

Alternatively, for a manual update:
```powershell
iisreset /stop
Copy-Item new\TreblleModule.dll C:\iismodules\treblle\TreblleModule.dll
iisreset /start
```

**Config changes never require a restart** — just edit `treblle.config` and the next request picks up the changes.

---

## Uninstalling

```powershell
.\installer\uninstall.ps1
```

This removes the module from IIS and optionally deletes `C:\iismodules\treblle\` including your `treblle.config`.

---

## Debug mode

To diagnose issues, set `"debug": true` in `treblle.config`. The module will write errors to the **Windows Application Event Log** under source `Treblle`.

Open Event Viewer → **Windows Logs → Application**, filter by source `Treblle`.

Common log entries:
- `Treblle: WinHttpCrackUrl failed for URL: ...` — check `treblle_url` format
- `Treblle: WinHttpSendRequest failed (0x...)` — network connectivity issue
- `Treblle: ingress returned HTTP 401` — check `api_key` and `sdk_token`

**Always set `debug` back to `false`** in production — Event Log writes have a small overhead.

---

## Troubleshooting

**Module doesn't appear in `appcmd list module`**

- Confirm you ran the installer as Administrator
- Check `C:\Windows\System32\inetsrv\` for `appcmd.exe`
- Look in Event Viewer → Windows Logs → System for IIS startup errors

**Module is registered but no data appears in Treblle**

1. Confirm the API returns `Content-Type: application/json` in the response
2. Check that the host/path is not matched by an `exclude_routes` entry
3. Enable `debug: true` and check the Application Event Log
4. Verify network access from the server to `ingress.treblle.com:443`

**IIS fails to start after installing the module**

- Confirm the DLL was compiled for **x64** (not x86)
- Confirm the DLL path in `C:\Windows\System32\inetsrv\config\applicationHost.config` matches the actual file location
- Temporarily rename `TreblleModule.dll` and restart IIS to confirm it is the cause

**Request bodies are empty in Treblle**

- Confirm the request includes `Content-Type: application/json`
- Bodies larger than 2 MB are intentionally not captured

---

## License

MIT — see [LICENSE](LICENSE).
