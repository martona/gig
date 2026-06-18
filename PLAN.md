# Plan: Settings store, native config UI, cert pinning (June 2026)

Make the app configurable at runtime instead of from a read-only `gig.ini`: a
read/write settings store (registry now, plists/Keychain when iOS happens), a
native (not ImGui) config dialog with **live apply — never restart-to-apply**,
Win32 dark mode, persisted window geometry, and trust-on-first-use cert pinning.
ImGui stays for the log view; native is specifically for settings + dialogs so
the eventual tvOS/iPadOS UI doesn't look alien.

## Locked decisions (from review)

- **Secrets:** the settings interface takes an `encrypt` flag per value. On
  Windows, flagged values are DPAPI-wrapped (`CryptProtectData`, CurrentUser) and
  stored as `REG_BINARY`. `password` is the only secret today; pins are not
  secret (integrity, not confidentiality). macOS approach deferred — open question
  is *secrets-in-Keychain* vs *enc-key-in-Keychain + our own crypto*.
- **No restart-to-apply.** Editing base/credentials/TLS and hitting Apply must
  reconnect live (the wrong-password case is the whole point). This requires
  M4 (session extraction) — see there.
- **Cert pinning is fail → handle-at-leisure → retry, not async prompting.** The
  verify callback only accepts/rejects + records; the *existing* retry loop
  (decoder 2s, health re-poll, login retry) brings the connection back after the
  user decides. Login is the first and only connection at startup (single
  request), so it's the natural gatekeeper — no startup stampede. Dedupe is
  insurance for a cert changing mid-session, not the common path.
- **Pin over SPKI-SHA256** (survives same-key renewal). **Hostname mismatch is
  just another pinnable cert error** — no special refusal (user controls the CA).
- **No ini import.** Empty registry → first-run opens the config dialog. The ini
  path is deleted, not migrated.
- iOS backends are **interface-only now**; implement Windows, don't let `HKEY`
  leak through the abstraction.

## Dependency order

```
M1 settings store ─┬─ M4 app-session/live-reconfigure ── M5 config dialog
                   ├─ M6 cert pinning (+ hostname verify)
                   └─ (geometry, M5)
M2 wm_umbra dark ──┬─ M3 dark message box
                   └─ M5 config dialog
```

M1 and M2 are independent foundations (parallel). M3 is tiny (after M2). The
hostname-verification half of M6 is independent and can land any time.

---

## M1 — Platform settings store ✅ DONE (June 2026)

**Goal:** one read/write, thread-safe, platform-neutral settings facade; replace
ini loading entirely.

**Landed:** `src/platform/settings_store.{hpp,cpp(win)}` exactly as designed below
(registry `HKCU\Software\gig`, DPAPI-encrypted `password`, `schema-version` stamped
on first run). `main.cpp` `loadConfig(SettingsStore&)` replaced all ini code; dead
`DefaultUrl` removed and the control-plane UA renamed `frigate-d3d-poc` → `gig`;
`gig.ini`/`gig.ini.example` deleted. Verified end-to-end against the user's Frigate:
PowerShell DPAPI-seeded the registry, the app read it + decrypted the password
cross-process → login + 10/10 live; schema-version write confirmed. The user's live
config now lives in the registry (migrated from the ini once, externally — no in-app
import, per decision). **Not yet exercised:** a C++-side *encrypt-write* (only
read/decrypt is proven live) — naturally covered when M5's dialog saves the password.

**Build:**
- `src/platform/settings_store.hpp` — neutral interface, no Win32 types:
  ```cpp
  class SettingsStore {
  public:
      virtual ~SettingsStore() = default;
      virtual std::optional<std::string> getString(std::string_view key, bool encrypted = false) const = 0;
      virtual void setString(std::string_view key, std::string_view value, bool encrypt = false) = 0;
      virtual std::optional<int64_t>  getInt(std::string_view key) const = 0;
      virtual void setInt(std::string_view key, int64_t value) = 0;
      virtual std::optional<bool> getBool(std::string_view key) const = 0;
      virtual void setBool(std::string_view key, bool value) = 0;
      virtual void remove(std::string_view key) = 0;
      // subtree for pins, e.g. listKeys("pins") -> host names
      virtual std::vector<std::string> listKeys(std::string_view subkey) const = 0;
  };
  ```
  `encrypt`/`encrypted` is symmetric (caller passes it on both set and get; the
  getter unwraps DPAPI). Internally locked (mutex, like `CookieJar`).
- `src/platform/settings_store_win.cpp` — `HKCU\Software\gig`. Plain →
  `REG_SZ`/`REG_DWORD`; encrypted → `CryptProtectData` → `REG_BINARY`. Pins live
  under a `pins\` subkey (one value per host).
- Write a `schema-version` value on first save (cheap future-proofing).
- Replace `ProgramOptions`/`applyIniConfig`/`applyIniSetting` in `main.cpp` with
  a `Config` struct populated from the store. Keep the same keys (base, user,
  password[encrypt], login-refresh, ca/cert/key, software, overlay, insecure,
  poll-interval, rw-timeout-us). Ride-along cleanups while in here: delete the
  dead `DefaultUrl` (`/security-go2rtc/...`, [main.cpp:32](src/main.cpp:32)) and
  rename the `frigate-d3d-poc` control-plane User-Agent.

**Watch-outs:** DPAPI blobs are opaque bytes — the *caller's* `encrypted` flag is
how the getter knows to unwrap (don't auto-detect by reg type). CurrentUser DPAPI
scope means the value is readable only by this Windows user (fine; matches intent).

---

## M2 — umbra (WM_UMBRA) via git registry + Win32 dark mode ✅ DONE (June 2026)

**Landed:** umbra git registry in `vcpkg-configuration.json` (baseline `206822a…`,
resolved umbra 1.1.3) + `"umbra"` dep; `find_package(umbra)` + `umbra::umbra` linked
with `uxtheme dwmapi comctl32`. App manifest added (`resources/gig.manifest`, Common
Controls v6 + supportedOS, **silent on DPI** so SDL keeps owning it) embedded via
`gig.rc` `RT_MANIFEST` + `target_link_options(/MANIFEST:NO)`. `main.cpp`:
`umbra::initDarkMode()` first thing in `try` (before any window, so fatal boxes are
dark too), `umbra::setDarkWndNotifySafe(hwnd)` on the SDL window. Verified no-regression:
manifest embeds (comctl6 string in exe), DPI still `scale: 2`, 10/10 live, 0.19s clean
shutdown (umbra's subclass coexists with SDL). **Dark appearance itself is visual — user
confirms.** **Not wired:** `WM_SETTINGCHANGE` live re-theme (SDL owns the message loop) —
startup theme only; live light/dark flip needs an `SDL_SetWindowsMessageHook`, deferred.

**Goal:** dark title bar + dark native common dialogs/message boxes.

**Consumption (confirmed against `../WM_NIGHT`, which already pulls it):** umbra is
**not** an overlay port — the `WM_UMBRA` repo *is its own vcpkg git registry*, no
third-party deps. Merge into gig's existing `vcpkg-configuration.json` (keep the
ffmpeg `overlay-ports`, add a `registries` entry); gig's `builtin-baseline` covers
the default registry, so no `default-registry` needed.
```json
"registries": [
  { "kind": "git", "repository": "https://github.com/martona/WM_UMBRA",
    "baseline": "206822a27974256728404cf612ebc8ac72d1f9e5", "packages": ["umbra"] }
]
```
Then `"umbra"` in `vcpkg.json` deps; `find_package(umbra CONFIG REQUIRED)` +
`umbra::umbra` in CMake. Static lib, CRT follows the triplet — our
`x64-windows-static` → `/MT`, matches.

**API (`#include <umbra.h>`):** `umbra::initDarkMode()` once at startup before
windows; `umbra::setDarkWndNotifySafe(hWnd)` after creating a top-level window/
dialog; on `WM_SETTINGCHANGE`, `umbra::handleSettingChange(lParam)` then re-theme;
`umbra::DarkMessageBox(hWnd, L"…", L"…", flags)`. **Strings are wide (LPCWSTR).**

**Build:** call `initDarkMode()` at startup, `setDarkWndNotifySafe()` on the SDL
HWND (the one we already grab for `setConsentParentWindow`,
[main.cpp:296](src/main.cpp:296)) and on the config dialog.

**Watch-outs:**
- **Common Controls v6 manifest required** (umbra themes comctl6). An SDL3 app may
  not ship one — likely need to add the comctl6 dependency to `resources/gig.rc`'s
  manifest. Verify before assuming umbra "does nothing."
- Wide-string boundary: gig is UTF-8 internally; M3/M5 convert at the umbra edge.
- SDL owns the window/message loop — `WM_SETTINGCHANGE` live re-theme needs an SDL
  Windows message hook (`SDL_SetWindowsMessageHook`) or we skip live re-theme for
  v1 and only theme at startup.

---

## M3 — MessageBox → umbra dark message box ✅ DONE (June 2026)

**Landed:** the single `MessageBoxA` (fatal handler) → `umbra::DarkMessageBox(nullptr,
widen(error.what()).c_str(), L"gig", …)` with a UTF-8→UTF-16 `widen()` helper in
`main.cpp`. Null parent (window may not exist at fatal time) is fine — `initDarkMode()`
ran first. CryptUI cert picker/consent dialogs are untouched (not MessageBox).

**Goal:** the one fatal dialog renders dark.

**Build:** swap the single call site ([main.cpp:523](src/main.cpp:523)) to
`umbra::darkMessageBox`. Depends on M2.

**Watch-outs:** that call fires in the startup catch block where the window may
not exist yet — the wrapper must tolerate a null parent. (The CryptUI cert
picker/consent dialogs are *not* MessageBox; they're untouched here.)

---

## M4 — App session extraction + live reconfigure ✅ DONE (June 2026)

**Landed:** `src/app/app_session.{h,cpp}` — `AppSession` owns auth + supervisor +
the camera set behind `applyConfig(AppConfig)` = stop → login → discover → build
supervisor → start. It **never throws** for login/discovery/config errors (returns
`ApplyResult{ok,error}`, leaving a clean stopped session) so a live reconfigure can
fail without taking the app down. `main()` is now thin: `StartupConfig` = `AppConfig
session` + UI-only `showOverlay`; it builds one `AppSession`, calls `applyConfig`
once (a startup failure is still fatal — no dialog yet), pushes `cameraLabels()` to
the renderer, and the run loop reads `session.*` pass-throughs. **F5 = live reconnect**
(re-login, re-discover, rebuild — verified: 2nd login + rediscover + all 10 decoders
restart + relive, no app restart; the running frame-delta is reset so the rebuilt
supervisor's 0-counter doesn't underflow the fps). Renderer rebind is just
`setCameraLabels` (it already resizes `tiles_` per-frame). `FrigateAuth::loginOrThrow`
removed (AppSession uses `login()`). Verified: startup 10/10, F5 reconnect clean,
steady-state 10/10 ~241fps, 0.21s shutdown. The fatal box is `umbra::DarkMessageBox`;
the reconnect-failed box is owned to the window via `mainHwnd`.

**Goal:** make login→discover→supervisor a thing that can be torn down and rebuilt
on command, so the config dialog applies without a restart. **This is the hidden
scope behind "no restart-to-apply."**

**What exists:** the teardown/build primitives are already clean — `auth->stop()`,
`supervisor.stop()` (joins threads, tears down decoders), `start()`, and the
shared `CookieJar`/`TlsSessionCache` are `shared_ptr`. What's missing is an owner
that runs the sequence on demand instead of once in `main()`.

**Build:**
- `src/app/app_session.{h,cpp}` — `AppSession` owns auth + supervisor + the shared
  jar/cache + the current camera set. `applyConfig(Config)` = stop auth+supervisor,
  rebuild `TlsOptions`/`FrigateAuth`, re-login, re-discover, rebuild supervisor,
  restart. Runs synchronously on the UI thread (stop() joins, so no races).
- Failure is **surfaced, not fatal**: a bad login or failed discovery returns an
  error to the caller (→ M5 shows it inline; first-run empty config opens the
  dialog). Removes the `loginOrThrow` fatal path.
- Renderer must accept a **new camera set** at runtime: today `setCameraLabels` +
  grid are configured once. A base change → different cameras → relabel + relayout.
  Creds-only/same-base changes keep the set stable (cheaper path, but full
  teardown→rebuild is the clean default; a creds-only fast path — just re-login and
  let the 2s stream retries pick up the new cookie — is a possible optimization,
  not the baseline).

**Watch-outs:** the renderer rebind is the one piece that reaches outside the
network layer. Keep `applyConfig` on the main thread; don't call it from a network
callback (M6's pin-accept must post to the UI thread, not reconfigure inline).

---

## M5 — Native config dialog ✅ DONE (June 2026)

**Landed:** `src/ui/settings_dialog.{h,cpp}` + `src/ui/resource.h` + an `IDD_SETTINGS`
`DIALOGEX` in `resources/gig.rc`. Modal Win32 dialog, **dark via `umbra::setDarkWndNotifySafe`**
in `WM_INITDIALOG`; fields for base/user/password(masked)/login-refresh/ca/cert/key(+**Browse**
via `IFileOpenDialog`, `ole32`)/poll-interval/url/stream-url + software/overlay/insecure
checkboxes. `showSettingsDialog(parent, AppConfig&, bool&)` pre-fills, returns edited values on
OK. `main.cpp`: `saveConfig()` mirrors `loadConfig`'s keys (password DPAPI-encrypted, or removed
when blank; `useWindowsStore` re-derived on reload). **F2** opens it → `saveConfig` → reload →
`applyAndReport` (the shared F5/apply lambda: live reconnect, rebind labels, reset frame deltas).
**First-run / unusable config opens the dialog at startup** instead of dying (loop until it
applies or the user cancels → exit). Verified headless (drove F2 + `GW_ENABLEDPOPUP` + `WM_COMMAND
IDOK`): dialog opens, OK applies (2nd login + rediscover), first-run opens-not-fatal, 10/10
steady-state, graceful exit. **This also proved the M1-deferred C++ `CryptProtectData` encrypt-WRITE
round-trip** (OK re-saved the password encrypted; the reload decrypted it and re-login succeeded).
Visual appearance (dark theme, layout) is the user's to confirm. Invocation is F2 for now (no UI to
tie it to yet); the `WM_SETTINGCHANGE` live-retheme gap from M2 still applies to the dialog.

**Goal:** edit Frigate server specs + client behavior, live-applied.

**Build:**
- Win32 dialog (programmatic or DLGTEMPLATE), dark via umbra. Fields: base, user,
  password (masked), login-refresh, ca/cert/key with **Browse** (`IFileOpenDialog`
  owned to the HWND, same modality discipline as the consent prompts),
  software, overlay, poll-interval, insecure; advanced: url, stream-url.
- Invocation: a Settings hotkey (and/or a click target); auto-opens on first run
  (empty store) and on an apply/startup failure with the error shown inline.
- Apply → write settings (password `encrypt=true`) → `AppSession.applyConfig`.
  Cheap runtime knobs (overlay, poll interval, login-refresh) apply live without a
  reconnect; connection fields trigger the reconnect path.

**Depends on:** M1 (store), M2 (dark), M4 (apply path).

---

## M6 — Cert pinning + hostname verification

**Goal:** TOFU pinning that overrides chain/hostname failures the user accepts,
with loud detection when a *pinned* cert changes.

**Build:**
- **Hostname verification first** (independent, do it early): plumb the connect
  host into `configureSslContext` (today it only gets `TlsOptions` —
  [tls_context.cpp:45](src/net/tls_context.cpp:45)) and set
  `ssl::host_name_verification(host)`. Both call sites (control plane + video)
  change. Per decision, a mismatch is just another pinnable error, not a hard fail.
- **Pin store:** SPKI-SHA256 per host in the settings store under `pins\`. Entry:
  `{ spki_sha256, subject, not_after, pinned_on }` (cert fingerprint too, for
  display).
- **Verify callback** (`set_verify_callback`, replacing the bare `verify_peer`):
  on `preverify_ok == 0`, compute the leaf SPKI hash; matches a stored pin for
  this host → return 1 (accept). Else return 0 (reject) **and** record a pending
  decision `{ host, spki, X509_V_ERR code, subject, not_after }`.
- **Flow (fail → leisure → retry):** rejection surfaces as a normal connection
  failure; the existing retry loop reconnects after the user decides. The pending
  decision is shown out-of-band (in the dialog or a dark message box): first
  contact → "untrusted cert [reason]; pin SPKI abc…?"; **pin exists but differs →
  louder** "PINNED CERT CHANGED for <host> — was abc…, now def… (renewal or
  interception)". Accept → store pin → next retry succeeds.
- **Dedupe** the pending-decision slot by host+spki so a mid-session change that
  hits several streams at once yields one prompt; one acceptance re-trusts all.

**Watch-outs:**
- The callback runs on **network threads**; pin *reads* there, pin *writes* +
  prompts on the UI thread. The pending slot needs its own small lock; acceptance
  posts to the UI thread (do not reconfigure from inside the handshake).
- `insecure = true` → `verify_none` → no callback → pinning off. Document the
  interaction; the dialog should make "insecure" visibly defeat pinning.
- Scope the override to verification failures only — don't let a pin paper over a
  malformed-cert/protocol error.

## Open items

- ~~**wm_umbra** consumption~~ — resolved: git registry, see M2.
- **macOS secrets:** Keychain-holds-secret vs Keychain-holds-key + our crypto.
- **Settings hotkey / invocation affordance** for the dialog (and the tvOS focus
  model later).

---

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
- [x] HUD: per-camera labels + a synthetic diagnostics tile (cams good/bad, fps, ingest kbps, cpu%).
      GDI-baked monospace font atlas + alpha-blended overlay pass; `--no-overlay` toggles it (placeholder
      for a future settings dialog). kbps = supervisor ingest byte-delta; cpu% = GetProcessTimes normalized.

All milestones build and run clean in `--software` mode; stats verified sane (~33 Mbps ingest, ~1 core).
Still needs a human at the window: the on-screen grid appearance, label/HUD legibility, and the
click-to-zoom interaction -- plus the shared-device D3D11VA zero-copy path on a GPU box.
