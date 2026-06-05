# Plan: Frigate multi-camera grid

Replace the single test camera with a live grid of all discovered Frigate cameras.

## Architecture: supervisor / worker / render

Three concerns, cleanly owned:

```
 StreamHealth poll (AUTHORITATIVE)        FfmpegDecoder x N (autonomous)        Renderer (main thread)
 -- bulk GET /api/go2rtc/api/streams      -- when running: pull stream.ts,      -- reads N frame slots + layout
 -- byte-delta per camera -> online?         decode, self-heal leg-2 blips      -- draws each into a grid cell
 -- drives create/destroy of decoders     -- reports frames + state             -- click-to-zoom = grid of 1
        |                                         |                                     ^
        +--------- CameraSupervisor: membership + per-camera slot {state, decoder, bytes, frame} ----------+
```

- **Health poll is authoritative on life/death.** Camera->go2rtc bytes advancing => decoder should run;
  stalled => tear the decoder down (no futile reconnect against a dead upstream); advancing again => bring
  it back. The poll cadence *is* the backoff, so no thundering herd of reconnects.
- **FFmpeg owns its own transient reconnects** (our connection dropped, camera fine) -- health stays green,
  supervisor leaves it alone.
- **Layout is a pure function of membership** (stable slots); liveness only repaints a slot, never moves it.

## Liveness (battle-tested byte-delta, now authoritative)

- One **bulk** `GET /api/go2rtc/api/streams` per poll (returns all cameras in one doc).
- Per stream: `online = sum(producers[].bytes_recv) > lastSum`.
  - `bytes_recv` -> fall back to `recv` (go2rtc <1.9) -> if a producer has **neither**, emit a **loud parse
    error** (do NOT silently report offline -- that is the go2rtc-reshuffle trap).
  - go2rtc restart resets the counter: tolerate as a one-cycle blip, never trigger a storm.
- **Poll interval: 5s.**

## TLS

- Reuse the `TlsSessionCache` from `../hitsc` (tiny, OpenSSL-only pool of `SSL_SESSION*`). Wire into the
  control-plane HTTP client (discovery + health) with keep-alive; resumption is the fallback when keep-alive
  drops. NOTE: only the control plane benefits -- the video streams use FFmpeg's own TLS stack.

## Logging

- Tiny thread-safe stderr logger (`log_info() << ...` stream style), used at every lifecycle step
  (discovery, decoder up/down, health transitions, layout). Never on the per-frame hot path.

## Modules

| File | Role |
|------|------|
| `src/log.{hpp,cpp}` | thread-safe stderr logger |
| `src/net/tls_options.h` | `TlsOptions` (extracted from ffmpeg_decoder.h) |
| `src/net/tls_session_cache.{hpp,cpp}` | ported from hitsc |
| `src/net/url.h` | URL parse / join / encode helpers (header-only) |
| `src/net/http_client.{hpp,cpp}` | persistent mTLS Beast client w/ keep-alive + session cache |
| `src/discovery/frigate_discovery.{h,cpp}` | `discoverCameras()` -> `CameraStream` list + stream URLs |
| `src/health/stream_health.{h,cpp}` | bulk byte-delta poller |
| `src/app/camera_supervisor.{h,cpp}` | membership + per-camera slots; drives decoders from health |
| `src/render/grid_layout.{h,cpp}` | column count by fill-score -> per-tile rects (pure) |

Renderer/decoder changes:
- `FfmpegDecoder`: `AVIOInterruptCB` (prompt teardown) + `Connecting/Live/Stale/Offline` status.
- `D3D11Renderer`: per-tile GPU state (`TileState` vector), grid draw, letterbox-in-tile, release the D3D11
  lock before `Present`, `focusedTile` for click-to-zoom.

## Sequencing (runnable grid early)

1. **Logger + net (cache + client)** -- infra; verify resumption logs.
2. **Discovery returns data** -- log the cameras + built URLs.
3. **Grid renderer** with the existing single decoder fanned to N (no health yet) -- first visible 10-up grid.
4. **Health supervisor** -- life/death authoritative; tiles go offline/online for real.
5. **Click-to-zoom + stale-dimming polish.**

## Open knobs / defaults

- Stream URL template: default `{base}/api/go2rtc/api/stream.ts?src={name}`; `--url` overrides as a template
  whose `src` is swapped (for the known-good `.../security-go2rtc/api/stream.ts?src=` path). Confirm the exact
  proxy path on first run (`probe --src <cam> --stream-check`).
- Poll interval: 5s.
- All 10 discovered cameras, config order (stable slots).

## Decode: hardware + software

- Hardware decode is shared-device D3D11VA only (zero-copy) -- D3D11VA is vendor-agnostic and covers
  every real GPU. No DXVA2/QSV/CUDA grab-bag: those add a sysmem round-trip, and on GPU-less VMs DXVA2
  "opens" successfully then decodes garbage ("Invalid data found when processing input").
- If the renderer is on a software adapter (WARP / Basic Render Driver -- e.g. this dev VM, confirmed via
  `DXGI_ADAPTER_FLAG_SOFTWARE`) or D3D11VA won't open, decode falls through cleanly to software. No flag
  needed; the binary auto-selects correctly on both a GPU box and this VM.
- `--software` (alias `--no-hwaccel`) forces software decode explicitly.
- Decode loop tolerates `AVERROR_INVALIDDATA` on send/receive (skip the packet, keep the stream) instead
  of tearing down and reconnecting -- fixes the mid-GOP-join error storm at startup.

## Progress

- [x] M1 logger + net (TLS session cache resumption confirmed live)
- [x] M2 discovery (10 cameras resolved from /api/config; stream.ts proxied path confirmed streaming)
- [x] M3 grid renderer + N decoders (software path verified: 10 cams, ~245 frames/s aggregate, 0 errors)
- [x] M4 health supervisor (verified: all cams unknown->online via byte-delta, started/reconciled, 0 churn)
- [x] M5 click-to-zoom (click a tile to fill the window; click or Esc returns to the grid; Esc in grid quits)

All five milestones build and run clean in `--software` mode. Still to verify on real hardware: the
shared-device D3D11VA zero-copy path (run without `--software` on a GPU box), the on-screen grid
appearance, and the click-to-zoom interaction (both need a human at the window).
