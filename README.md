# gig

Windows-native streaming client for Frigate: it discovers every camera from the Frigate API and renders them all in a live Direct3D 11 grid, with click-to-zoom.

The native pipeline:

1. Discovery fetches `/api/config` (Boost.Beast over the app's own TLS stack) and maps each camera to its go2rtc `stream.ts` URL. A shared TLS session cache resumes handshakes across all connections, and a shared cookie jar carries the Frigate auth token everywhere once login succeeds.
2. One FFmpeg decoder per camera. The app terminates TLS itself and feeds FFmpeg the decrypted MPEG-TS through a custom AVIO callback — FFmpeg does no networking of its own. Each stream decodes H.264/H.265 through D3D11VA on the renderer's shared D3D11 device when available, falling back to software decode automatically.
3. A health supervisor polls go2rtc byte counters and is the authority on liveness: it tears a decoder down when its bytes stop advancing and brings it back when they resume. FFmpeg handles its own transient reconnects in between.
4. SDL owns the window. A thin D3D11 renderer keeps per-camera GPU state and draws each decoded frame letterboxed into its grid cell (or one cell filling the window in focus mode), sampling D3D11 NV12 textures directly with CPU NV12/YUV420P/BGRA fallbacks.

## Build

The Windows build script uses vcpkg manifest mode, CMake, Ninja, and the static triplet by default. It creates an isolated private vcpkg checkout under `build\vcpkg`, installs packages under `build\vcpkg-installed`, and keeps the vcpkg binary cache/downloads under `build`. FFmpeg is built from a trimmed overlay port ([`vcpkg-ports/ffmpeg`](vcpkg-ports/ffmpeg/portfile.cmake)) that compiles only the surface gig uses — h264/hevc decoders + parsers, the mpegts demuxer, and the D3D11VA hwaccels — which roughly halves the exe.

```powershell
.\scripts\build_windows.ps1
```

To configure without compiling:

```powershell
.\scripts\build_windows.ps1 -ConfigureOnly
```

Useful overrides:

```powershell
.\scripts\build_windows.ps1 -BuildType RelWithDebInfo
```

## Run

The app is GUI-only — there are no command-line options or subcommands. Settings are stored in the Windows registry under `HKCU\Software\gig` (the password is DPAPI-encrypted). A native settings dialog is in progress; until it lands, set values with `regedit` or PowerShell. A minimal config for a Frigate with authentication enabled:

```powershell
New-Item -Path 'HKCU:\Software\gig' -Force | Out-Null
Set-ItemProperty 'HKCU:\Software\gig' -Name base -Value 'https://frigate.example:8971/'
Set-ItemProperty 'HKCU:\Software\gig' -Name user -Value 'viewer'
# password is DPAPI-wrapped (CurrentUser) and stored as REG_BINARY:
Add-Type -AssemblyName System.Security
$blob = [Security.Cryptography.ProtectedData]::Protect([Text.Encoding]::UTF8.GetBytes('secret'), $null, 'CurrentUser')
Set-ItemProperty 'HKCU:\Software\gig' -Name password -Value $blob -Type Binary
```

Launch `gig.exe`. Each tile is labeled with its camera, and a synthetic diagnostics tile shows live camera counts, frame rate, ingest bandwidth, and CPU; clicking it toggles a full-window log view (Esc or the ✕ closes it). Click a camera tile to zoom it to fill the window; click again (or press Esc) to return to the grid. Esc in the grid quits.

### Authentication

Two independent layers; mix them as your setup requires:

- **Frigate login** (`user` / `password`) — POSTs `{base}/api/login` at startup, then carries the returned `frigate_token` cookie on every API and video request, re-logging-in every `login-refresh` seconds (default 600) to keep the token fresh. This is the normal way to talk to Frigate's authenticated port directly.
- **TLS / client certificates** — with no `ca`/`cert`/`key` set, server certificates are verified against the Windows certificate store, and a client certificate (from CurrentUser\MY, signed via CNG so the private key never leaves the store) is offered only if a server's TLS handshake actually requests one — the cert picker and consent prompt appear just then, once per run. Set `ca` (and `cert`/`key` for client auth) to use PEM files instead, e.g. behind a client-cert-checking reverse proxy.

### Settings (registry value names)

`base`, `user`, `password` (encrypted), `login-refresh` (`REG_QWORD`); `ca`, `cert`, `key`, `url` (single-stream mode without discovery), `stream-url` (override the discovered stream URL template) — all `REG_SZ`; `software` (force software decode; decode already auto-falls-back when no hardware path is usable), `overlay` (diagnostics tile on/off), `insecure` (skip server-cert verification, testing only) — `REG_DWORD` 0/1; `poll-interval` (health poll seconds), `rw-timeout-us` (per-connection I/O timeout) — `REG_QWORD`.

## Next Steps

- Justified/aspect-aware layout and a hero+spotters mode beyond the uniform grid.
- A renderer abstraction for non-Windows backends.
