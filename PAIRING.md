# Plan: QR credential pairing (June 2026)

Move a Frigate credential set from one gig device to another by scanning a QR —
**zero typing on the consumer device** (especially tvOS, which has no keyboard and
no camera). The hard input (Frigate URL + user + password, maybe a client cert)
happens once, on a device where typing is easy (desktop, or a phone), and then
propagates to every other device by scanning.

The bootstrap chain this enables:

```
desktop (type once) ──scan──▶ iOS ──scan──▶ tvOS ──scan──▶ tvOS ...
   (easy input)               (camera)      (consumer)     (consumer)
```

## The fork we resolved: LAN-direct, no cloud relay

Two designs were on the table:

- **A — blind cloud relay** at `gig.stream`: both devices talk to a server that
  relays an E2E-encrypted blob. Pros: works cross-network, can complete pairing in
  a browser with no app installed.
- **B — embedded LAN server**: the device showing the QR binds a transient socket
  and serves the exchange directly over the local network. Pros: works fully
  offline, nothing to run/maintain forever, nothing leaves the LAN.

**Decision: B.** Rationale (locked):

1. **"Maintain it forever" is the real cost.** A relay is a stateful service + a
   domain + an abuse surface + a single point of failure that *rots the day
   interest lapses* — and every existing install's pairing silently breaks with
   it. gig is a personal, non-commercial project; the LAN path keeps working
   untended for a decade.
2. **A's cross-network advantage is largely illusory here.** `base` is almost
   always a LAN URL (`https://frigate.lan:9971/`). Credentials that point at
   `frigate.lan` are useless to a device that can't reach that LAN, so pairing an
   off-network device buys nothing. The exception (Frigate exposed via a public
   host / Tailscale / VPN) is a technical minority who can type or paste.
3. **B matches the local-NVR ethos and is arguably more private** — same E2E
   content protection, but no third party ever sees even ciphertext or metadata,
   and nothing leaves the LAN.
4. **The transport is reuse, not new code.** gig already links Boost.Asio/Beast +
   OpenSSL and already terminates TLS itself. The genuinely new surface is
   discovery + OS permissions, not a network stack.

The honest cost of B (see **Watch-outs**): multi-interface discovery, OS
local-network permission prompts, and "no app on the scanner = install-nudge
only" (you cannot complete the transfer in a browser). All acceptable for an
audience that already runs Frigate.

**We are not building a relay.** If real cross-network demand ever appears, add an
*optional, self-hostable* relay later — opt-in, pointed at the user's own host,
never a mandatory `gig.stream` dependency. YAGNI for now.

## The symmetric model (no "share" vs "receive" mode)

Every device behaves identically. There is **one role split by capability, not by
intent**:

- **Server (all platforms):** publish a QR, bind a socket, accept connections, and
  serve exactly two RPCs over the secure channel: `read_creds`, `write_creds`.
- **Client / scanner (camera devices only — iOS today):** scan the QR, connect,
  and drive the exchange. Desktop and tvOS have no camera, so they are
  **server-only**. iOS is both.

The device showing the QR is always the server; the scanner is always the client.
**Which way the credentials flow is decided by the scanner *after* it connects** —
the user never picks a direction. Direction only re-surfaces in the one genuine
conflict case (case 4 below), which is the correct place for it.

### The scanner's state machine

The scanner connects, calls `read_creds` first, then branches on
`(local, remote) ∈ {∅ blank, C has-creds}²`:

| local (scanner) ↓ \ remote (server) → | ∅ | C |
|---|---|---|
| **∅** | **case 5** → bail: "set up gig on one device first" | **case 1** → adopt remote (pull), happy |
| **C** | **case 3** → push local (`write_creds`), happy | **case 2** (equal) → no-op, happy · **case 4** (differ) → resolve conflict, converge, happy |

This is a complete enumeration of the state space. The model is naturally
**idempotent** (re-scanning a matched pair = case 2 = no-op) and always
**converges to consistency** (case 4 ends with both devices holding the same set).

## Locked decisions

- **Transport:** TCP + **TLS 1.3 with an external PSK in `psk_dhe_ke` mode**. The
  PSK (carried in the QR) authenticates both ends; the ECDHE half gives forward
  secrecy. No certs, no CA, no LAN-IP-cert problem. This *is* the "ECDH + the QR
  key for mutual auth" idea, named and vetted — **do not hand-roll the handshake.**
- **PSK wiring:** set the PSK callbacks on the OpenSSL `SSL_CTX` via
  `native_handle()` — the **same reach-into-OpenSSL pattern** already used by
  `installPinningVerify` in [cert_pin.cpp](src/net/cert_pin.cpp). Server:
  `SSL_CTX_set_psk_find_session_callback`; client:
  `SSL_CTX_set_psk_use_session_callback`; each builds an `SSL_SESSION` from the
  32-byte PSK + a fixed TLS 1.3 cipher (`TLS_AES_256_GCM_SHA384`).
- **RPC layer:** HTTP/1.1 over the PSK-TLS stream via Beast (reuse what's already
  linked). `GET /v1/creds` = `read_creds`; `PUT /v1/creds` = `write_creds`. Framing,
  methods, and status codes come for free; the confirm-on-hold gate (below) maps to
  holding the response open (async Beast).
- **Payload:** versioned `boost::json` `CredentialSet` (plaintext inside the
  channel — the PSK-TLS layer is the E2E protection). PEM blobs ride as strings.
- **One shared C++ implementation** for both server and client. iOS uses the same
  Beast/OpenSSL client (consistent with the locked "keep OpenSSL on Apple"
  decision); the **only** native iOS piece is the camera scan feeding
  `{addrs, port, psk}` into the shared client.
- **QR encodes a `gig.stream` universal link whose *fragment* carries the
  connection info** (see QR format). App installed → it intercepts and connects
  over the LAN (cloud untouched). No app → browser loads a **static** install-nudge
  page; the fragment is never transmitted to the server, so `gig.stream` never sees
  the LAN addresses or the PSK. A static page is near-zero maintenance and is *not*
  the forever-burden a relay would be.
- **Discovery = list-and-race.** The QR lists every plausible candidate address;
  the scanner connects to all in parallel and the first to complete the PSK
  handshake wins (cancel the rest). No mDNS dependency.
- **Credential identity is a decrypted, normalized *subset*** — not the stored
  blob (see Watch-outs: the non-deterministic-encryption trap).
- **Friction lands where input is easy.** The server confirms only when it has
  something to lose (serving a read of secrets, or being overwritten); a **blank**
  server adopts/serves with no prompt. This keeps the input-hostile device (a fresh
  tvOS) fully zero-touch and puts any confirmation on the device that has creds
  (and a keyboard/remote).

## What travels (the `CredentialSet`)

**In the payload (connection identity + trust):**
- `base` (normalized), `user`, `password`
- `ca` / `cert` / `key` (PEM, if present)
- `stream-url` (if non-default — it's part of reaching the server)
- **cert pins** (`pins/<host>` from [cert_pin.cpp](src/net/cert_pin.cpp)) — so the
  receiver trusts the same self-signed Frigate cert **without re-running TOFU**. A
  real win on tvOS, where confirming a pin prompt is painful.
- **provenance:** `{ origin_device_name, modified_at }` — used only to make the
  case-4 dialog answerable.

**Explicitly NOT in the payload:**
- Device-local prefs: `poll-interval`, `login-refresh`, `rw-timeout-us`, `overlay`,
  `cam-labels`, `software`.
- **`insecure`** — never transfer a "disable all verification" flag silently;
  transferring **pins** is the correct way to convey "I trust this cert."

**Equality (case 2 vs 4)** is computed over the **auth identity only**:
`normalize(base) + user + password + client-cert fingerprint`. `stream-url` and
pins are carried along but don't, by themselves, make two sets "different accounts."

**"Has creds" (C vs ∅)** means a *complete, usable* set: `base` AND
(`user`+`password` OR a client `cert`+`key`). A half-finished setup counts as ∅, so
it can't masquerade as a source — which also makes case 5's "finish setup first"
literally true.

## QR / universal-link format

```
https://gig.stream/p#v=1&a=<addr,addr,...>&p=<port>&k=<psk>
```

- `v` — protocol version.
- `a` — comma-separated candidate addresses. IPv4 dotted; IPv6 in brackets.
  **Exclude** loopback and IPv6 link-local (the `%zone` scope id is meaningless off
  the originating host).
- `p` — TCP port (ephemeral; bind to port 0, read back via `getsockname`).
- `k` — 32-byte PSK, base64url, no padding.

No direction field — the symmetric model decides direction after connecting. URL is
~100–110 chars → a comfortable QR at ECC level **Q** (resilient to a phone camera
reading it off a TV at a distance) with a 4-module quiet zone.

## The server-side confirmation gate

The scanner always reads before it (maybe) writes, so the server's two RPCs are
gated by **whether the server has something to lose**:

| Server state | `read_creds` (dispenses secrets) | `write_creds` (mutates config) |
|---|---|---|
| **∅ blank** | return blank — no prompt | adopt — **no prompt** (zero-touch) |
| **C has-creds** | **confirm**, then return | **confirm** (overwrite), then apply |

This single rule:
- keeps the **common consumer path zero-touch** (a blank tvOS receiving from a
  phone: scanner GETs → blank, no prompt → PUTs → adopt, no prompt);
- defends a configured device against **QR shoulder-surf** (someone who photographs
  the QR can't silently pull creds — the holder sees a confirm) and against
  **remote clobber**;
- and the confirm is a single button press — fine even on a tvOS remote (the "hard
  input" problem is *typing*, not pressing OK once).

A light **scanner-side** intent confirmation ("Send your credentials to *DESKTOP*?"
/ "Copy credentials from *DESKTOP*?") is good UX and lands on the easy-input device;
it's separate from this server-side security gate.

## Case 4 — the only screen that earns its keep

Both devices hold different, complete sets. This is where directionality legitimately
returns, so design it well:

- **Show provenance** so the choice is answerable: "Remote: `frigate.lan` as *viewer*
  — set on **DESKTOP**, 3 days ago" vs "Local: `frigate2.lan` as *admin* — set
  **here**, today."
- **Phrase in device terms**, never protocol terms: *"Use the account from the device
  you scanned"* vs *"Send this device's account to it."* Not "overwrite local."
- **Offer Cancel.** Resolving a conflict isn't mandatory just because you scanned.
- Either choice converges both devices to the same set, then runs the happy path.

## Completion (both sides)

- **Two-sided terminal state.** After a transfer, the server also reaches a happy
  state and **immediately runs its normal connect** — on tvOS, receiving creds kicks
  straight into login → grid (or the error banner). Don't make success scanner-only.
- **Celebrate on transfer, validate via the existing path.** Don't block the
  animation on a full Frigate login. Commit → `AppSession.applyConfig` (already
  returns `ApplyResult`, already never throws — [app_session.h](src/app/app_session.h)) →
  the existing status banner surfaces a bad login. Do only structural validation
  (non-empty `base`, parseable URL) before commit.

## Dependency order

```
P1 identity + payload codec ─┬─ P2 PSK-TLS channel + RPCs ─┬─ P4 server role ─┐
   (pure, platform-neutral)  │                             ├─ P5 scanner (iOS)├─ P6 state machine + UX
                             │                             │                  │
P3 discovery + QR encoding ──┴─────────────────────────────┘                  │
P7 web fallback + AASA  ──────────────────────────────────────────────────────┘  (independent; can land anytime)
Cross-cutting: OS local-network permissions (touches P4 + P5)
```

P1 and P3 are independent foundations. P7 (static page + AASA) is independent and
trivial — can land first or last.

---

## P1 — Credential identity & payload codec

**Goal:** a platform-neutral `CredentialSet` with canonical equality, a
"complete/usable" predicate, provenance, and versioned (de)serialization. No network.

**Build:**
- `src/pair/credential_set.{h,cpp}` — the struct, `boost::json` round-trip,
  `authIdentity()` (normalized base + user + password + cert fingerprint),
  `isComplete()`, `==` over auth identity.
- Read/write through the existing `SettingsStore`
  ([settings_store.hpp](src/platform/settings_store.hpp)); pull pins via
  `listKeys("pins")`. New plain keys: `device-name` (default = hostname / device
  model), `creds-modified-at` (stamped on every settings save), `creds-origin`.
- Normalize `base` (scheme/host/port/trailing slash) so trivial URL differences
  don't read as different accounts.

**Watch-outs:**
- **The non-deterministic-encryption trap.** `password` is DPAPI/Keychain-wrapped
  and re-encrypts to *different bytes every time*. Compare **decrypted** values, or
  case 2 (equal) never fires and every re-scan falls into case 4. Equality must be
  over plaintext auth identity, never stored blobs.

## P2 — Pairing channel (PSK-TLS + RPCs)

**Goal:** a mutually-authenticated, forward-secret channel from a shared 32-byte
PSK, carrying the two RPCs.

**Build:**
- `src/pair/pairing_channel.{h,cpp}` — PSK callbacks on `ctx.native_handle()`
  (mirror the `cert_pin` native-handle pattern), TLS 1.3 only,
  `TLS_AES_256_GCM_SHA384`. Reuse the Beast plumbing from
  [tls_client.cpp](src/net/tls_client.cpp) / [http_client.cpp](src/net/http_client.cpp).
- RPCs as Beast HTTP over the stream: `GET /v1/creds` / `PUT /v1/creds`.
- **`write_creds` is atomic + validated:** buffer the whole body, parse + validate,
  then commit to `SettingsStore` in one shot. A dropped connection must not leave a
  half-written config.

**Watch-outs:**
- TLS 1.3 external PSK is slightly fiddly in OpenSSL (you construct an `SSL_SESSION`
  in the callback). Pin the cipher; reject anything but TLS 1.3.
- Bind the protocol version into the PSK identity so a v1 client can't half-talk to
  a v2 server.

## P3 — Discovery & QR encoding

**Goal:** enumerate reachable addresses, bind, emit the QR; render it.

**Build:**
- `src/pair/lan_discovery.{h,cpp}` — `GetAdaptersAddresses` (Windows) /
  `getifaddrs` (mac/iOS); filter loopback + IPv6 link-local + down interfaces; keep
  RFC1918 / ULA / global. Bind TCP on `0.0.0.0` and `::`, port 0, read back the port.
- QR string builder (the format above) + the matrix: vendor Nayuki **qrcodegen**
  (single C++ file, public-domain) → boolean matrix → bake to a small texture →
  draw scaled with **nearest-neighbour** sampling. Both renderers already draw
  textured quads ([d3d11_renderer.cpp](src/render/d3d11_renderer.cpp),
  [metal_renderer.mm](src/render/metal_renderer.mm)).
- **Scanner-side race:** connect to all `a` candidates in parallel; first PSK
  handshake to complete wins; cancel the rest; overall timeout.

**Watch-outs:**
- **Your dev box will force this immediately** — it has Hyper-V/WSL virtual adapters,
  so "the bound IP" is a *list*, and several entries are unreachable from a phone.
  The race is what sorts it out; make sure unreachable candidates fail fast.

## P4 — Server role

**Goal:** publish the QR, accept connections, serve the gated RPCs.

**Build:** wire P2's RPC handlers to read/write the `SettingsStore`; implement the
**confirmation gate** (blank ⇒ auto; has-creds ⇒ confirm-then-serve/apply). Reuse the
native dialog patterns from [settings_dialog.cpp](src/ui/settings_dialog.cpp) (Win) /
`settings_dialog_mac.mm` and the existing dark/native message-box helpers; tvOS
confirm is a focus-model OK/Cancel.

## P5 — Scanner role (iOS)

**Goal:** camera → parse → race-connect → run the state machine.

**Build:** VisionKit `DataScannerViewController` (iOS 16+) — or `AVCaptureMetadataOutput`
for more control — parses `https://gig.stream/p#...`, extracts the fragment, hands
`{addrs, port, psk}` to the shared C++ client (P2/P3). Then the state machine (P6) +
the scanner-side intent confirm.

## P6 — State machine & UX

**Goal:** cases 1–5 end to end, the case-4 dialog, two-sided completion, happy/again/
error states.

**Build:** the branch table above; the conflict dialog (provenance + device-terms +
Cancel); two-sided terminal state + auto-connect via `AppSession.applyConfig`;
celebrate-on-transfer with validation through the existing `ApplyResult`/status-banner
machinery.

## P7 — Web fallback & deep linking

**Goal:** scanner without the app gets nudged; the app intercepts the link.

**Build:**
- AASA at `https://gig.stream/.well-known/apple-app-site-association` associating
  **`stream.gig.app`** (the locked bundle id) for path `/p`; entitlement
  `com.apple.developer.associated-domains = applinks:gig.stream`.
- Static `gig.stream/p` page (GitHub/Cloudflare Pages): platform-detect → App
  Store / TestFlight + a one-line explanation. It can do nothing with the
  fragment (and never receives it) — install-nudge only, by design.
- Universal-link handling in-app → straight into P5's connect path (so a tap on the
  link, not just a camera scan, can start pairing).
- (Android later: `assetlinks.json` + App Links — out of scope now.)

## Cross-cutting — OS local-network permissions

The friction unique to LAN-direct. Handle the **denied** path with a clear message
("gig needs local-network access to pair — enable it in Settings"), don't hang.

- **iOS / tvOS:** `NSLocalNetworkUsageDescription` (and `NSBonjourServices` only if
  mDNS is ever added). Runtime prompt on first listen/connect.
- **macOS 15+:** local-network prompt at runtime.
- **Windows:** Defender Firewall inbound prompt on first bind. Consider a firewall
  rule in the MSIX/installer; the portable exe will prompt.

## Watch-outs (consolidated)

- **Equality over decrypted auth identity**, never stored blobs (P1) — the single
  most likely bug.
- **"Has creds" = complete + usable**, not "any field set" (P1).
- **`write_creds` atomic + validated before commit** (P2).
- **`read_creds` dispenses secrets** and **`write_creds` mutates config** — both
  authorized only by PSK possession. The blank-vs-has-creds gate (P4) is what bounds
  this; don't skip it.
- **Multi-NIC discovery is a list + race**, not "the bound IP" (P3).
- **No-app web fallback can't complete the transfer** — `crypto.subtle` needs a
  secure context, and a LAN IP over HTTPS has no trustable cert. Install-nudge only.
- **Segmented networks** (AP/client isolation, guest VLANs) can't pair over LAN —
  fall back to manual entry. (Those usually can't reach LAN Frigate anyway.)
- **Don't transfer `insecure`**; transfer pins instead.

## Open items

- **Safety word** (HKDF over the PSK → a short word list, shown on both ends) as
  extra defense against a swapped/wrong QR. Cheap (both sides already hold the PSK,
  no round trip). Decide whether it's worth the UI in v1 or a later hardening pass.
- **mDNS / Bonjour** as a discovery enhancement on top of list-and-race. Not needed
  for correctness; Windows has no built-in responder, so it's net-new baggage.
- **Multi-server future.** This plan assumes **one credential set per device**
  (today's model). The handoff hints at future multi-server ("a single fixed
  Keychain item that doesn't grow as multiple servers are added"). When that lands,
  case 4 gains a third option ("keep both / add as second server"). Keep the payload
  and the conflict dialog shaped so that's an *additive* change, not a rewrite.
- **Raw-AEAD fallback.** If PSK-TLS plumbing in Beast proves annoying, raw
  AES-256-GCM with the PSK (+ a nonce exchange) is a simpler stand-in; it gives up
  forward secrecy, which is marginal for one-shot ephemeral creds. Prefer PSK-TLS.
