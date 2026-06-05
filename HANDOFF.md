# Handoff — start here

Notes for the next session. The grid app is done and working; the next work is a
**TLS re-plumb in two phases, done in this order: Phase 2 first, then Phase 1**
(yes, backwards — read on). `PLAN.md` has the grid-milestone history.

## What the app is

Native Windows multi-camera client for Frigate: D3D11 + FFmpeg + SDL3, vcpkg
static, built as `gig.exe`. Discovers all cameras from Frigate's API and renders
them in a live grid with click-to-zoom, a per-camera label + a synthetic
diagnostics tile (cams good/bad, fps, kbps, cpu%), and an authoritative health
supervisor that starts/stops decoders off a go2rtc byte-delta poll.

Build: `.\scripts\build_windows.ps1` (Release; warm cache is fast, a dep change is
a slow one-time rebuild). Check results with
`... | Select-String "error C|error LNK|Built:"`.

**Everything is uncommitted in the working tree.** The user commits; don't commit
unless asked. End commit messages with the `Co-Authored-By: Claude ...` line.

## Module map

- `src/main.cpp` — CLI (`run` / `probe` / `discover` / `certstore`), run loop,
  supervisor wiring, live-resize `SDL_AddEventWatch`, `GetProcessTimes` cpu sampler.
- `src/app/camera_supervisor.*` — owns decoder lifecycle from the health poll.
- `src/decode/ffmpeg_decoder.*` — per-camera decode thread. **Phase 2 edits here.**
- `src/discovery/frigate_discovery.*` — `/api/config` → cameras + stream URLs.
- `src/health/stream_health.*` — bulk `/api/go2rtc/api/streams` byte-delta.
- `src/net/http_client.*` — Boost.Beast mTLS client (control plane). **Phase 2 & 1 extend here.**
- `src/net/tls_session_cache.*` — TLS resumption pool (ported from `../hitsc`), shared.
- `src/net/win_cert_store.*` — Win32/CNG cert picker + RSA & EC signers. **Phase 1 uses.**
- `src/probe/cert_probe.*` — the `certstore` probe + the OpenSSL↔CNG bridge to relocate in Phase 1.
- `src/render/*` — D3D11 per-tile grid renderer, `grid_layout`, `text_overlay` (GDI font-atlas HUD).

## TLS / cert R&D — DONE and PROVEN (read before touching TLS)

Today there are **two TLS stacks**, both on PEM files (`--ca/--cert/--key`):
1. Our Boost.Beast client (`http_client.cpp`, OpenSSL we configure) — control plane.
2. FFmpeg's own OpenSSL — the video `stream.ts` connections.

Findings (all verified against the user's Frigate):
- **Server CA from the Windows store works**: `SSL_CTX_load_verify_store(ctx, "org.openssl.winstore://")`. Prereq: the private CA must be in the Windows store (it is — Trusted Root).
- **The OpenSSL `capi` ENGINE is DEAD on 3.x** — it loads + selects the cert, but signing fails `error:0A080006 SSL routines::EVP lib / function not supported`. Do **not** pursue it.
- **The working client-cert path is our custom CNG bridge** — proven end-to-end with the Windows consent prompt firing:
  - `src/net/win_cert_store.*`: cert picker (`CryptUIDlgSelectCertificateFromStore`, CurrentUser\MY) + `NCryptSignHash` signers — `cngSignPkcs1` (RSA) and `cngSignEcdsa` (EC). Walled off from OpenSSL (wincrypt/openssl headers collide), exposes plain types only.
  - `cert_probe.cpp`: builds an OpenSSL `EVP_PKEY` whose sign is delegated to the bridge — `RSA_METHOD` for RSA, `EC_KEY_METHOD` for EC — and auto-detects via `EVP_PKEY_base_id`.
  - **RSA** ⇒ proven, but pinned to **TLS 1.2 + PKCS#1** (the legacy `RSA_METHOD` only gets the bare digest on the PKCS#1 path; TLS 1.3's RSA-PSS would need a full OpenSSL 3.x *provider* — a someday item).
  - **EC** ⇒ proven on **TLS 1.3** (ECDSA has no padding, so no version pin).
- Probe: `gig certstore --base URL [--server-only | --capi]` (default = CNG bridge).
- Libs added for this: `crypt32 cryptui ncrypt` (in `CMakeLists.txt`).

## Phase 2 — terminate TLS ourselves, feed FFmpeg via custom AVIO (DO FIRST)

**Why first:** it's auth-agnostic (keep PEM), so it's testable without the
consent prompt, and it leaves a single TLS stack for Phase 1 to flip.

**Goal:** we open the HTTPS `stream.ts` connection (Boost.Beast, our OpenSSL) and
hand FFmpeg the decrypted bytes through an `avio_alloc_context` read callback, so
FFmpeg needs no TLS. Wins: one unified TLS config; session resumption for video
(faster starts, and later **one** consent not ten); ~5 MB smaller exe.

**Where:** `ffmpeg_decoder.cpp` `decodeOnce()` — today it calls
`avformat_open_input(url)` (FFmpeg does HTTPS). Replace the open with:
- A **streaming** HTTPS GET. The current `HttpClient::get()` buffers the whole
  body — no good for an endless stream. Add a streaming path (new
  `HttpClient::getStreaming()` or a small `TlsStream` class) using
  `http::response_parser<http::buffer_body>` + `http::read_some` (Beast de-chunks
  transparently). Reuse the existing `configureSslContext` mTLS + the shared
  `TlsSessionCache`.
- `avio_alloc_context(buf, bufSize, /*write=*/0, opaque, &readPacket, nullptr, nullptr)`;
  set `avioCtx->seekable = 0`; `fmt->pb = avioCtx`; then
  `avformat_open_input(&fmt, nullptr, av_find_input_format("mpegts"), &opts)` —
  **force the mpegts demuxer** (stream is non-seekable; don't let it probe-by-seek).
- `readPacket(opaque, buf, size)` → blocking read of body bytes from our Beast
  stream into `buf`, returns count or `AVERROR_EOF` / `AVERROR_EXIT`. It owns the
  `rw_timeout` (Beast `expires_after`) and checks `stopRequested_`.
- **Reconnect is now ours** — but the existing `run()` retry loop already wraps
  `decodeOnce()`; on read error/EOF the callback returns EOF → FFmpeg unwinds →
  outer loop reconnects (re-establishing our TLS, fast via the session cache).
- `vcpkg.json`: drop `openssl` from the ffmpeg feature list; keep avcodec/avformat/
  swscale + HW accels. Full rebuild (slow, one-time). Can also drop
  `avformat_network_init()` (no FFmpeg protocols left). Measure the size delta.

**Test (a fresh Claude CAN run this — PEM, no consent):** existing PEM args, e.g.
`gig --base https://frigate.lan/security --software --ca ... --cert ... --key ...`
→ grid still comes up, now with TLS terminated by us.

## Phase 1 — flip the (now single) TLS stack to the Windows store (DO SECOND)

**Goal:** swap PEM → Windows store for the real client: winstore CA + CNG client cert.
- Relocate the OpenSSL↔CNG bridge helpers (the `RSA_METHOD`/`EC_KEY_METHOD` build
  funcs + key-type detect) out of `cert_probe.cpp` into a reusable module
  (e.g. `src/net/cng_tls.*` or into the TLS-context setup). `win_cert_store.*` is
  already reusable.
- In the SSL_CTX setup (`configureSslContext` + the Phase-2 video TlsStream):
  - Server CA: `SSL_CTX_load_verify_store(ctx, "org.openssl.winstore://")` + verify_peer.
  - Client cert: `selectClientCertFromStore()` **once** at startup (or remember the
    thumbprint in config to skip the picker), build the bridge EVP_PKEY,
    `SSL_CTX_use_certificate` + `SSL_CTX_use_PrivateKey`. **Reuse the one
    `WinClientCert` across all connections** → one consent, then resumption.
  - RSA: pin TLS 1.2 + PKCS#1 sigalgs (see probe). EC: no pin.
- New flags (e.g. `--winstore`); keep PEM (`--ca/--cert/--key`) as default/fallback.
- **Lifetime gotcha:** the `WinClientCert` owns the live `NCRYPT_KEY_HANDLE` used by
  the sign callback — it must outlive the SSL_CTX and all handshakes (park it
  somewhere app-lifetime, e.g. the supervisor or a TLS holder).
- **CONSENT-PROMPT CONSTRAINT:** once the app uses a store cert, *running it pops
  the prompt*. A fresh Claude must hand runs to the user — build, then ask.

## Operational constraints for the next Claude

- **Cannot run unattended:** `certstore` (default), or the real app once on store
  certs — both pop a Windows consent prompt; hand to the user.
- **Can run solo:** builds, PEM-based grid runs, `certstore --server-only`,
  `discover`, `probe`.
- **This dev VM has no usable GPU** (D3D adapter is software/WARP) — HW decode
  auto-falls-back to software; use `--software` or rely on the fallback. The user
  validates HW decode on a separate GPU box.
- GUI headless-check pattern: `Start-Process gig.exe -ArgumentList ... -RedirectStandardError $log -PassThru`; `Start-Sleep N`; `Stop-Process`; then grep the log for `live N/10`, `frames total`, `Decoder error`.

## Paths / facts

- Frigate base: `https://frigate.lan/security` (host `frigate.lan:443`). 10 cameras.
- PEM (CurrentUser): CA `C:\Users\Marton\Desktop\winuty\gig\myca.pem`,
  client `...\marton@mars11.crt` / `.key` (RSA).
- Windows store: CA in Trusted Root; client certs `marton@mars11` (RSA) and
  `marton@devbox2` (EC) in CurrentUser\MY.
- Stream URL template (proxied go2rtc): `{base}/api/go2rtc/api/stream.ts?src={name}`.
- Reusable hitsc code lived at `../hitsc` (the TLS session cache came from there).
