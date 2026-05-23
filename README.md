# Frigate D3D PoC

Tiny Windows proof of concept for opening one Frigate/go2rtc camera stream with FFmpeg and rendering the latest decoded frame through Direct3D 11.

This intentionally skips camera discovery, motion/activity handling, and Windows certificate store integration. It is just enough to prove the native pipeline:

1. FFmpeg opens `https://.../api/stream.ts?src=...` with client cert files.
2. A background thread decodes H.264 through D3D11VA on the renderer's D3D11 device when available, then falls back to other FFmpeg hardware devices or software decode.
3. SDL owns the window.
4. A thin D3D11 renderer samples decoded D3D11 NV12 textures directly and presents them letterboxed, with CPU NV12/YUV420P/BGRA paths kept as fallbacks.

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

Example using PEM files:

```powershell
.\build\windows-release\frigate_d3d_poc.exe `
  --url "https://frigate.lan/security-go2rtc/api/stream.ts?src=frontgate" `
  --ca "C:\certs\myca.pem" `
  --cert "C:\certs\marton@mars11.crt" `
  --key "C:\certs\marton@mars11.key"
```

For a quick server-cert bypass while testing:

```powershell
.\build\windows-release\frigate_d3d_poc.exe --insecure --cert C:\certs\client.crt --key C:\certs\client.key
```

## Probe

Use `probe` to check whether a Frigate or raw go2rtc base URL exposes the endpoints this app can use for discovery/status:

```powershell
.\build\windows-release\frigate_d3d_poc.exe probe `
  --base "https://frigate.lan/security" `
  --src frontgate `
  --ca "C:\certs\myca.pem" `
  --cert "C:\certs\marton@mars11.crt" `
  --key "C:\certs\marton@mars11.key"
```

It probes Frigate config, Frigate-proxied go2rtc streams, and raw go2rtc stream status endpoints. Add `--stream-check` to read a small prefix from stream endpoints, `--dump` to print full fetched bodies, or `--endpoint /path` to test a custom path.

## Next Steps

- Replace PEM file options with a Windows certificate store backed TLS path.
- Add Frigate API discovery and websocket camera activity.
- Add a renderer abstraction for non-Windows backends.
- Replace the one-camera path with Frigate discovery and a dynamic grid.
