# Handoff — start here

Notes for the next session. The grid app is done and working, **and the TLS re-plumb
is COMPLETE** — both Phase 2 (we terminate all TLS ourselves; FFmpeg does no
networking) and Phase 1 (Windows store + CNG client cert) are done and verified
against the user's Frigate. `PLAN.md` has the grid-milestone history.

**Default auth is now the Windows store** (store trust roots + a CurrentUser\MY
client cert via the CNG bridge) — implicit whenever no `--ca/--cert/--key` PEM args
are given; pops the Windows cert picker + one consent prompt on first use. PEM is the
explicit fallback. No `--winstore` flag — it's derived from the absence of PEM args.

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
  the one shared `TlsSessionCache` + `CookieJar`. Reads **`gig.ini`** next to the exe
  (`applyIniConfig`) as defaults before CLI parse — flags override. `gig.ini.example`
  in the repo documents the keys.
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
  the custom-AVIO byte source).
- `src/net/tls_context.*` — **`configureSslContext`** — the single place every TLS
  consumer (control plane + video) is configured: winstore CA + CNG client cert when no
  PEM args, else PEM.
- `src/net/cng_tls.*` — **`useWindowsStoreClientCert(SSL_CTX*)`**: the OpenSSL↔CNG
  signing bridge (RSA `RSA_METHOD` / EC `EC_KEY_METHOD` → `NCryptSignHash`) + a
  process-wide cached `WinClientCert` (one picker + one consent). RSA pins TLS 1.2 +
  PKCS#1; EC is TLS 1.3.
- `src/net/cookie_jar.*` — thread-safe cookie jar shared by control plane + video
  (seam for dropping mTLS → native Frigate auth later; empty under mTLS today).
- `src/net/tls_session_cache.*` — TLS resumption pool (ported from `../hitsc`), shared
  across control plane + all video connections.
- `src/net/win_cert_store.*` — Win32/CNG cert picker + RSA & EC signers (used by `cng_tls`)
  + `setConsentParentWindow` (owns the picker/consent/key-access dialogs to the app
  window so it can't be closed mid-prompt — else shutdown's join deadlocks against the
  thread stuck in the prompt). `main.cpp` feeds it the SDL HWND.
- `src/probe/cert_probe.*` — the `certstore` probe; its CNG path calls `cng_tls`.
- `src/probe/http_probe.*` — the `probe` command (self-contained Beast, own anon-namespace TLS).
- `src/render/*` — D3D11 per-tile grid renderer, `grid_layout`, `text_overlay` (GDI font-atlas HUD).

## TLS architecture (final)

There is **one TLS stack**: our Boost.Beast + OpenSSL. Both planes go through
`configureSslContext` (`tls_context.cpp`):
1. Control plane (`http_client.cpp`) — discovery + health.
2. Video (`tls_client.cpp` `TlsClient`/`MediaStream`) — we open the HTTPS `stream.ts`
   ourselves and hand FFmpeg the decrypted MPEG-TS via `avio_alloc_context`. FFmpeg
   is built **without** the `openssl` feature (`--disable-openssl --enable-schannel`)
   and we dropped `avformat_network_init()` — FFmpeg uses zero protocols now.

Auth: **Windows store by default** (store trust roots + a CurrentUser\MY client cert
via the CNG bridge), or PEM when `--ca/--cert/--key` are given. Control plane and video
each build their own `ssl::context` (both via `configureSslContext`) but share the one
`TlsSessionCache`, `CookieJar`, and — via `cng_tls`'s process-wide cache — the one
selected `WinClientCert`, so the picker + consent fire once.

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

## Phase 1 — DONE ✅ (Windows store + CNG client cert, default)

Built + verified against the user's Frigate (full grid on store certs + the `certstore`
probe). What landed:
- `cng_tls.*` — the OpenSSL↔CNG bridge relocated out of `cert_probe.cpp`, exposing
  `useWindowsStoreClientCert(SSL_CTX*)`: selects the cert **once** via a process-wide
  cached `WinClientCert` (one picker + one consent across both SSL contexts), builds the
  bridge `EVP_PKEY` (RSA `RSA_METHOD` / EC `EC_KEY_METHOD` → `NCryptSignHash`), pins RSA
  to TLS 1.2 + PKCS#1, leaves EC on TLS 1.3.
- `tls_context.cpp` (`configureSslContext`, the single chokepoint): when
  `useWindowsStore`, server CA via `SSL_CTX_load_verify_store("org.openssl.winstore://")`
  + `useWindowsStoreClientCert`; else PEM.
- `tls_options.h`: `useWindowsStore` is **derived, not a flag** — set in `main.cpp` when
  no `--ca/--cert/--key` is given. (There is intentionally no `--winstore`.)
- `cert_probe.cpp`: de-duped — its CNG path now calls `cng_tls` (single source of truth).
- Verified: `certstore --server-only` validates the server cert via the store over TLS
  1.3 (`verify result: 0 (ok)`); the full grid runs on the store cert with one consent
  (session resumption + the startup stagger keep it to one).

## Possible future work (not started)

- **Shrink the exe for real:** the Phase-2 `openssl`-feature drop did NOT shrink it (see
  the size note above). To actually cut size, trim FFmpeg's own surface via custom vcpkg
  port options — disable its network/protocol layer (we use zero FFmpeg protocols) and
  the demuxers/codecs we don't need (only h264/h265 + mpegts + swscale).
- **Native Frigate auth:** the shared `CookieJar` is the seam — add a login flow that
  populates it, then mTLS becomes optional.
- **TLS 1.3 for RSA store certs:** would need a full OpenSSL 3.x *provider* (RSA-PSS),
  replacing the legacy `RSA_METHOD` bridge. EC already does TLS 1.3.

## Operational constraints for the next Claude

- **Cannot run unattended:** the app or `discover` with **no PEM args** (the default →
  Windows store → cert picker + consent prompt), and `certstore` (default CNG). Hand to
  the user.
- **Can run solo:** builds, **PEM grid runs / `discover`** (explicit `--ca/--cert/--key`
  → no consent), `certstore --server-only`, `probe`.
- **This dev VM has no usable GPU** (D3D adapter is software/WARP) — HW decode
  auto-falls-back to software; use `--software` or rely on the fallback. The user
  validates HW decode on a separate GPU box.
- GUI headless-check pattern: `Start-Process gig.exe -ArgumentList ... -RedirectStandardError $log -PassThru`; `Start-Sleep N`; then `$p.CloseMainWindow()` (WM_CLOSE → SDL_EVENT_QUIT → graceful shutdown, exercises the stop/cancel path) or `Stop-Process` (hard kill). Grep the log for `live N/10`, `frames total`, `Decoder error`, `video tls handshake ... reused=`.

## Paths / facts

- Config: `gig.ini` next to the exe (keys: base, url, stream-url, ca, cert, key,
  software, overlay, insecure, poll-interval, rw-timeout-us). With no ca/cert/key →
  Windows store. Template: `gig.ini.example` (tracked); the live one lives in the
  gitignored `build/` so it isn't committed.
- Frigate base: `https://frigate.lan/security` (host `frigate.lan:443`). 10 cameras.
- PEM (CurrentUser): CA `C:\Users\Marton\Desktop\winuty\gig\myca.pem`,
  client `...\marton@mars11.crt` / `.key` (RSA).
- Windows store: CA in Trusted Root; client certs `marton@mars11` (RSA) and
  `marton@devbox2` (EC) in CurrentUser\MY.
- Stream URL template (proxied go2rtc): `{base}/api/go2rtc/api/stream.ts?src={name}`.
- Reusable hitsc code lived at `../hitsc` (the TLS session cache came from there).
