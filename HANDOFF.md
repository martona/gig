# Handoff — start here

Notes for the next session. The grid app is done and working, **and the Phase 2 TLS
re-plumb is now DONE and verified** (we terminate all TLS ourselves; FFmpeg no
longer does any networking). The remaining work is **Phase 1: flip the now-single
TLS stack to the Windows store + CNG client cert.** `PLAN.md` has the grid-milestone
history.

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

- `src/main.cpp` — CLI (`run` default / `probe` / `discover` / `certstore`; note
  there is **no** `run` token — Run is the default command), run loop, supervisor
  wiring, live-resize `SDL_AddEventWatch`, `GetProcessTimes` cpu sampler. Creates
  the one shared `TlsSessionCache` + `CookieJar`.
- `src/app/camera_supervisor.*` — owns decoder lifecycle from the health poll; owns
  the one app-lifetime `TlsClient` (built from `config_.tls` + shared cache + jar)
  and the `startupStagger` (default 50ms/camera).
- `src/decode/ffmpeg_decoder.*` — per-camera decode thread. Now feeds FFmpeg via a
  **custom AVIO** over a `MediaStream` (no FFmpeg networking). Interruptible
  startup-stagger wait (CV) + event-driven read cancel on stop.
- `src/discovery/frigate_discovery.*` — `/api/config` → cameras + stream URLs.
- `src/health/stream_health.*` — bulk `/api/go2rtc/api/streams` byte-delta.
- `src/net/http_client.*` — Boost.Beast mTLS client (control plane: discovery + health).
- `src/net/tls_client.*` — **`TlsClient`** (the app-lifetime TLS holder: one shared
  `ssl::context`) + **`MediaStream`** (streaming HTTPS GET, de-chunked `buffer_body`,
  the custom-AVIO byte source). **Phase 1 client cert lives here / via tls_context.**
- `src/net/tls_context.*` — **`configureSslContext`** — the single place every TLS
  consumer (control plane + video) is configured. **Phase 1 edits this one function.**
- `src/net/cookie_jar.*` — thread-safe cookie jar shared by control plane + video
  (seam for dropping mTLS → native Frigate auth later; empty under mTLS today).
- `src/net/tls_session_cache.*` — TLS resumption pool (ported from `../hitsc`), shared
  across control plane + all video connections.
- `src/net/win_cert_store.*` — Win32/CNG cert picker + RSA & EC signers. **Phase 1 uses.**
- `src/probe/cert_probe.*` — the `certstore` probe + the OpenSSL↔CNG bridge to relocate in Phase 1.
- `src/probe/http_probe.*` — the `probe` command (self-contained Beast, own anon-namespace TLS).
- `src/render/*` — D3D11 per-tile grid renderer, `grid_layout`, `text_overlay` (GDI font-atlas HUD).

## TLS architecture NOW (post Phase 2)

There is **one TLS stack**: our Boost.Beast + OpenSSL. Both planes go through
`configureSslContext` (`tls_context.cpp`):
1. Control plane (`http_client.cpp`) — discovery + health.
2. Video (`tls_client.cpp` `TlsClient`/`MediaStream`) — we open the HTTPS `stream.ts`
   ourselves and hand FFmpeg the decrypted MPEG-TS via `avio_alloc_context`. FFmpeg
   is built **without** the `openssl` feature (`--disable-openssl --enable-schannel`)
   and we dropped `avformat_network_init()` — FFmpeg uses zero protocols now.

Today still on PEM (`--ca/--cert/--key`). Control plane and video each build their own
`ssl::context` (both via `configureSslContext`) but share the one `TlsSessionCache`
and `CookieJar`.

## TLS / cert R&D — DONE and PROVEN (read before touching TLS for Phase 1)

Findings (all verified against the user's Frigate):
- **Server CA from the Windows store works**: `SSL_CTX_load_verify_store(ctx, "org.openssl.winstore://")`. Prereq: the private CA must be in the Windows store (it is — Trusted Root).
- **The OpenSSL `capi` ENGINE is DEAD on 3.x** — it loads + selects the cert, but signing fails `error:0A080006 ... function not supported`. Do **not** pursue it.
- **The working client-cert path is our custom CNG bridge** — proven end-to-end with the Windows consent prompt firing:
  - `src/net/win_cert_store.*`: cert picker (`CryptUIDlgSelectCertificateFromStore`, CurrentUser\MY) + `NCryptSignHash` signers — `cngSignPkcs1` (RSA) and `cngSignEcdsa` (EC). Walled off from OpenSSL (wincrypt/openssl headers collide), exposes plain types only.
  - `cert_probe.cpp`: builds an OpenSSL `EVP_PKEY` whose sign is delegated to the bridge — `RSA_METHOD` for RSA, `EC_KEY_METHOD` for EC — and auto-detects via `EVP_PKEY_base_id`.
  - **RSA** ⇒ proven, but pinned to **TLS 1.2 + PKCS#1** (legacy `RSA_METHOD` only gets the bare digest on the PKCS#1 path; TLS 1.3's RSA-PSS would need a full OpenSSL 3.x *provider* — a someday item).
  - **EC** ⇒ proven on **TLS 1.3** (ECDSA has no padding, so no version pin).
- Probe: `gig certstore --base URL [--server-only | --capi]` (default = CNG bridge).
- Libs added for this: `crypt32 cryptui ncrypt` (in `CMakeLists.txt`).

## Phase 2 — DONE ✅ (terminate TLS ourselves, feed FFmpeg via custom AVIO)

Built + verified (headless PEM run, 10 cams): grid up, **10/10 live**, ~36 Mbps,
~240 fps aggregate (software), **zero errors**, graceful shutdown **0.1s**. What landed:
- `tls_client.*` — `TlsClient` (one shared `ssl::context`, `enableSessionCache`) +
  `MediaStream`: streaming GET via `response_parser<buffer_body>` with
  `body_limit(boost::none)` (mandatory — the 1 MiB default kills an endless stream),
  redirects (≤4), cookies, SNI, session resumption. **Read/timeout/stop are
  event-driven, no polling**: one `async_read_some` + a one-shot `expires_after`
  timeout; `cancel()` does `asio::post(io, [socket close])` to wake the blocked
  `io.run()`.
- `ffmpeg_decoder.*` — `decodeOnce()` opens a `MediaStream`, `avio_alloc_context(buf,
  64K, /*write*/0, stream, &readPacket, …)`, `seekable=0`, forced `mpegts` demuxer
  (non-seekable, no probe-by-seek), `AVFMT_FLAG_CUSTOM_IO`. RAII destruction order is
  **fmt → avio → guard → stream** (FFmpeg stops reading before the socket dies); avio
  deleter does `av_freep(&buffer)` then `avio_context_free`; open uses the
  release()/open/reset() aliasing-safe idiom. `interrupt_callback` kept (covers
  `find_stream_info`). `readPacket` returns bytes / `AVERROR_EOF` / `AVERROR_EXIT`,
  never 0.
- `tls_context.*` (extracted `configureSslContext`) + `cookie_jar.*` (shared jar);
  `http_client` ctor now takes the shared `CookieJar`.
- **Staggered startup**: `SupervisorConfig.startupStagger` (50ms) × slot index, waited
  once per decoder via an interruptible CV (`startupCv_`; `stop()` flips the flag under
  `startupMutex_` then notifies — no lost wakeup). Took video handshake **reuse from
  4/10 → 10/10** (each connect resumes the prior's ticket instead of all racing).
- `vcpkg.json`: dropped `openssl` from the ffmpeg features (one-time full rebuild).

**Size note (important — the ~5 MB prediction was wrong):** exe went **25.91 → 26.59
MB (+0.68 MB)**, not smaller. OpenSSL is **still statically embedded** (verified:
`OpenSSL 3.6.2` string in the exe) because our Beast stack needs it; in a vcpkg
*static* build there was only ever one OpenSSL (FFmpeg + Beast linked the same one,
deduped by the linker), so dropping FFmpeg's `openssl` feature only switched FFmpeg's
now-unused TLS to SChannel. Real size savings would come from trimming **FFmpeg's own
surface** — disable its network/protocol layer (we use zero FFmpeg protocols) and
unused demuxers/codecs (we only need h264/h265 + mpegts + swscale) via custom vcpkg
port options. Separate, bigger effort; not done.

## Phase 1 — flip the (now single) TLS stack to the Windows store (THE REMAINING WORK)

**Goal:** swap PEM → Windows store for the real client: winstore CA + CNG client cert.
- Relocate the OpenSSL↔CNG bridge helpers (the `RSA_METHOD`/`EC_KEY_METHOD` build funcs
  + key-type detect) out of `cert_probe.cpp` into a reusable spot — cleanest is to fold
  them into **`tls_context.cpp`** (or a new `src/net/cng_tls.*` it calls).
  `win_cert_store.*` is already reusable.
- **`configureSslContext` (`tls_context.cpp`) is now the single chokepoint** — edit it:
  - Server CA: `SSL_CTX_load_verify_store(ctx, "org.openssl.winstore://")` + verify_peer.
  - Client cert: `selectClientCertFromStore()` **once** at startup (or remember the
    thumbprint in config to skip the picker), build the bridge EVP_PKEY,
    `SSL_CTX_use_certificate` + `SSL_CTX_use_PrivateKey`.
  - RSA: pin TLS 1.2 + PKCS#1 sigalgs (see probe). EC: no pin.
- New flag (e.g. `--winstore`); keep PEM (`--ca/--cert/--key`) as default/fallback.
- **Lifetime / one-consent gotcha:** `configureSslContext` is called **per context** —
  control plane (`HttpClient`) builds one, `TlsClient` builds another. To get **one**
  consent, select the `WinClientCert` **once** and reuse it across both contexts (e.g.
  a process-wide/lazy-static holding the live `NCRYPT_KEY_HANDLE`), or unify to a single
  shared context. The handle must outlive every SSL_CTX + handshake; session resumption
  (already shared) then avoids re-signing on reconnects. `TlsClient` is the natural
  app-lifetime home if you go the holder route.
- **CONSENT-PROMPT CONSTRAINT:** once the app uses a store cert, *running it pops the
  prompt*. A fresh Claude must hand runs to the user — build, then ask.

## Operational constraints for the next Claude

- **Cannot run unattended:** `certstore` (default), or the real app once on store
  certs — both pop a Windows consent prompt; hand to the user.
- **Can run solo:** builds, PEM-based grid runs, `certstore --server-only`,
  `discover`, `probe`.
- **This dev VM has no usable GPU** (D3D adapter is software/WARP) — HW decode
  auto-falls-back to software; use `--software` or rely on the fallback. The user
  validates HW decode on a separate GPU box.
- GUI headless-check pattern: `Start-Process gig.exe -ArgumentList ... -RedirectStandardError $log -PassThru`; `Start-Sleep N`; then `$p.CloseMainWindow()` (WM_CLOSE → SDL_EVENT_QUIT → graceful shutdown, exercises the stop/cancel path) or `Stop-Process` (hard kill). Grep the log for `live N/10`, `frames total`, `Decoder error`, `video tls handshake ... reused=`.

## Paths / facts

- Frigate base: `https://frigate.lan/security` (host `frigate.lan:443`). 10 cameras.
- PEM (CurrentUser): CA `C:\Users\Marton\Desktop\winuty\gig\myca.pem`,
  client `...\marton@mars11.crt` / `.key` (RSA).
- Windows store: CA in Trusted Root; client certs `marton@mars11` (RSA) and
  `marton@devbox2` (EC) in CurrentUser\MY.
- Stream URL template (proxied go2rtc): `{base}/api/go2rtc/api/stream.ts?src={name}`.
- Reusable hitsc code lived at `../hitsc` (the TLS session cache came from there).
