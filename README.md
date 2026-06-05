# gig

Windows-native streaming client for Frigate: it discovers every camera from the Frigate API and renders them all in a live Direct3D 11 grid, with click-to-zoom.

The native pipeline:

1. Discovery fetches `/api/config` over mTLS (Boost.Beast) and maps each camera to its go2rtc `stream.ts` URL. A shared TLS session cache resumes handshakes across control-plane calls.
2. One FFmpeg decoder per camera opens `https://.../api/go2rtc/api/stream.ts?src=...` with client cert files. Each decodes H.264 through D3D11VA on the renderer's shared D3D11 device when available, falling back to other FFmpeg hardware devices or software decode (`--software`).
3. A health supervisor polls go2rtc byte counters and is the authority on liveness: it tears a decoder down when its bytes stop advancing and brings it back when they resume. FFmpeg handles its own transient reconnects in between.
4. SDL owns the window. A thin D3D11 renderer keeps per-camera GPU state and draws each decoded frame letterboxed into its grid cell (or one cell filling the window in focus mode), sampling D3D11 NV12 textures directly with CPU NV12/YUV420P/BGRA fallbacks.

## Build

The Windows build script uses vcpkg manifest mode, CMake, Ninja, and the static triplet by default. It creates an isolated private vcpkg checkout under `build\vcpkg`, installs packages under `build\vcpkg-installed`, and keeps the vcpkg binary cache/downloads under `build`.

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

** NOTE: This assumes that you're running Frigate behind a client-cert-verifying Nginx proxy, and that is the only
authentication path; Frigate's auth is OFF. This is most likely not going to match your setup. It matches mine,
but you've been warned.

Discover every camera and show the grid (pass `--base`, the Frigate root):

```powershell
.\build\windows-release\gig.exe `
  --base "https://frigate.lan/" `
  --ca "C:\certs\myca.pem" `
  --cert "C:\certs\client.crt" `
  --key "C:\certs\client.key"
```

Each tile is labeled with its camera, and a synthetic diagnostics tile shows live camera counts, frame rate, ingest bandwidth, and CPU. Click a tile to zoom it to fill the window; click again (or press Esc) to return to the grid. Esc in the grid quits.

`--ca` is the CA cert used to sign Frigate's TLS cert. `--cert` / `--key` are the client TLS keys used to auth to Nginx.

Flags:

- `--software` (alias `--no-hwaccel`) forces software decode. Decode already auto-falls-back to software when the renderer is on a software adapter (WARP / Basic Render Driver) or D3D11VA won't open, so this is only an explicit override.
- `--poll-interval N` sets the health poll period in seconds (default 5).
- `--no-overlay` hides the on-screen HUD (labels + diagnostics tile); it is on by default.
- `--url URL` runs a single camera instead of discovering (the legacy path); `--insecure` skips server-cert verification.

## Probe

Use `probe` to check whether a Frigate or raw go2rtc base URL exposes the endpoints this app can use for discovery/status:

```powershell
.\build\windows-release\gig.exe probe `
  --base "https://frigate.lan/security" `
  --src frontgate `
  --ca "C:\certs\myca.pem" `
  --cert "C:\certs\marton@mars11.crt" `
  --key "C:\certs\marton@mars11.key"
```

It probes Frigate config, Frigate-proxied go2rtc streams, and raw go2rtc stream status endpoints, then parses the Frigate response to list cameras and their go2rtc streams. Add `--stream-check` to read a small prefix from stream endpoints, `--dump` to print fetched bodies, `--max-bytes N` to raise the response cap, or `--endpoint /path` to test a custom path.

## Next Steps

- Verify the shared-device D3D11VA zero-copy path with all cameras on real GPU hardware.
- Justified/aspect-aware layout and a hero+spotters mode beyond the uniform grid.
- Replace PEM file options with a Windows certificate store backed TLS path.
- Add a renderer abstraction for non-Windows backends.
- Add proper Frigate authentication