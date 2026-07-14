# Releasing gig

Versioning is **tag-driven**. There is no committed version file: the tag (or the
manual-workflow input) is the source of truth, and CMake stamps it into
`gig.exe`'s `VERSIONINFO` + the MSIX identity (Windows) and the `gig.app`
`Info.plist` (macOS). The App Store scripts take the same value as a required
`--version` and stamp it at archive time (the placeholder values in the iOS
pbxproj never ship). Plain local builds fall back to `0.0.0.0`.

Version format is `W.X.Y.Z` (four integers), e.g. `0.1.0.0`. The 4th component
is the monotonic build counter: it must increase between store uploads of the
same `W.X.Y` (TestFlight/App Store reject a reused build number).

## Release channels

| Channel | Platforms | Driven by |
|---|---|---|
| **GitHub Releases** (+ Homebrew tap bump) | Windows amd64 + arm64, macOS arm64 | CI — tag push or manual dispatch |
| **iOS App Store / TestFlight** | iOS arm64 | Manual — `./scripts/build_ios_appstore.sh --upload` |
| **Mac App Store** | macOS arm64 (sandboxed) | Manual — `./scripts/build_macos_mas.sh --upload` |

The GitHub-release and Mac App Store macOS builds are *different binaries* from
the same source: the former is Developer ID, non-sandboxed, notarized; the
latter runs in the App Sandbox and is signed for the store. iOS and macOS both
use bundle ID `stream.gig.app`, so the store listing is one **Universal
Purchase**.

Everything store-side authenticates with the **App Store Connect API key** —
no step uses Xcode's Apple-ID session (its ASC auth is unreliable, and cloud
signing via API key would demand an Admin-role key; both flows sign with local
certificates instead).

## Cut a release

### Option A — push a tag (recommended)

```powershell
git tag v0.1.0.0
git push origin v0.1.0.0
```

`Release (tag)` builds Windows (amd64 + arm64) and macOS (arm64): it signs `gig.exe`,
packages the MSIX + portable zip, and signs + notarizes + staples `gig.app` into a
zip; then it attests build provenance and creates a **draft** release. Review it, then
publish:

```powershell
gh release edit v0.1.0.0 --draft=false
```

(or the Publish button in the GitHub Releases UI).

### Option B — manual workflow

Actions → **Release (manual)** → Run workflow → enter the version (no `v`), and
tick **Publish** to skip the draft step. Useful for re-running a release without
moving tags.

## What a release produces (per architecture)

| Asset | What it is |
|---|---|
| `gig-windows-<arch>.zip` | Portable: just the signed `gig.exe`. |
| `gig-windows-<arch>.msix` | Signed MSIX for sideload / direct download. |
| `gig-<version>-windows-<arch>-symbols.zip` | PDBs for crash symbolication. |
| `gig-macos-arm64.zip` | Signed + notarized + stapled `gig.app` (Apple Silicon). |

Installable assets are version-less so `releases/latest/download/...` URLs stay
stable; the version still lives in the binary's `VERSIONINFO`, the MSIX identity,
and the build-provenance attestation (which binds by SHA-256, not filename).

Install the MSIX (no cert import needed — Trusted Signing chains to a trusted root):

```powershell
Add-AppxPackage gig-windows-amd64.msix
# remove later: Get-AppxPackage *gig* | Remove-AppxPackage
```

On macOS, unzip `gig-macos-arm64.zip` and move `gig.app` to `/Applications`. It's
notarized + stapled, so Gatekeeper opens it with no warnings (and gig still needs
**Local Network** access granted on first launch to reach Frigate on the LAN).

## Local builds

```powershell
# Stamp a version (omit for a 0.0.0.0 dev build):
.\scripts\build_windows.ps1 -Version 0.1.0.0

# Package an MSIX locally (signs if you have `az login` + the ARTIFACT_SIGNING_*
# vars + sign.exe; otherwise pass -NoSign):
.\scripts\package_windows_msix.ps1 -Arch amd64 -Version 0.1.0.0
```

On macOS (signs when `APPLE_CODESIGN_IDENTITY` is set; add `--notarize` for the full
release flow, which also needs the App Store Connect API key vars in the environment):

```bash
./scripts/build_macos.sh --version 0.1.0.0
```

## Signing setup (one-time, per repo)

Both platforms sign under a single GitHub environment named **`release-signing`**.

### Windows (Azure Trusted Signing)

Signs via OIDC — no cert files in the repo.

- Secrets: `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`.
- Variables: `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`,
  `ARTIFACT_SIGNING_CERTIFICATE_PROFILE`.
- On the Azure side, a federated credential whose subject is
  `repo:martona/gig:environment:release-signing`.

The MSIX `Publisher` is derived automatically from the signed `gig.exe`'s
certificate subject, so it always matches the signing identity.

### macOS (Developer ID + notarization)

Signs `gig.app` with a **Developer ID Application** certificate and notarizes it via
the **App Store Connect API**. Add to the same `release-signing` environment:

- Secrets:
  - `APPLE_CERTIFICATE_P12` — base64 of the Developer ID Application `.p12`
    (export from Keychain Access, then `base64 -i cert.p12 | pbcopy`).
  - `APPLE_CERTIFICATE_P12_PASSWORD` — the `.p12` export password.
  - `APPLE_KEYCHAIN_PASSWORD` — any string; names the temporary CI keychain.
  - `APPLE_API_KEY_P8` — the full contents of the App Store Connect API key
    (`AuthKey_XXXX.p8`, including the `BEGIN/END PRIVATE KEY` lines).
- Variables:
  - `APPLE_CODESIGN_IDENTITY` — e.g. `Developer ID Application: Your Name (TEAMID)`.
  - `APPLE_TEAM_ID` — the 10-char Apple Team ID.
  - `APPLE_API_KEY_ID` — the 10-char App Store Connect key id.
  - `APPLE_API_ISSUER_ID` — the issuer UUID (App Store Connect → Users and Access →
    Integrations → App Store Connect API).

The API key only needs the **Developer** role (enough for notarization). gig is a
single static bundle with no nested code, so it's signed with the hardened runtime
(a notarization prerequisite) directly — no `--deep`.

## App Store releases (iOS + Mac App Store)

Both flows run from a terminal on the Mac and share this environment (put it
in your release shell profile):

```bash
export APPLE_TEAM_ID=XXXXXXXXXX
# App Store Connect API key -- authenticates uploads (and notarization);
# Developer role is enough, no Admin needed:
export APPLE_API_KEY_PATH="$HOME/.appstoreconnect/private_keys/AuthKey_XXXXXXXXXX.p8"
export APPLE_API_KEY_ID=XXXXXXXXXX
export APPLE_API_ISSUER_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
# iOS: App Store distribution profile (app-scoped var name -- a generic name
# once shipped the wrong app's profile, ITMS-90286):
export APPLE_IOS_GIG_PROVISIONING_PROFILE="$HOME/.appstoreconnect/Gig_iOS_App_Store.mobileprovision"
# Mac App Store: certs + profile:
export APPLE_CODESIGN_IDENTITY_3RDPARTY="3rd Party Mac Developer Application: … (XXXXXXXXXX)"
export APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY="3rd Party Mac Developer Installer: … (XXXXXXXXXX)"
export APPLE_MAS_GIG_PROVISIONING_PROFILE="$HOME/.appstoreconnect/Gig_Mac_App_Store.provisionprofile"
# Optional -- only if your iOS distribution identity is named differently:
# export APPLE_CODESIGN_IDENTITY_IOS="iPhone Distribution: … (XXXXXXXXXX)"
```

**One-time setup** (developer.apple.com + appstoreconnect.apple.com):

1. **App record**: App Store Connect → My Apps → **+** with bundle ID
   `stream.gig.app`. Adding both the iOS and macOS platforms to the same
   record makes it a Universal Purchase.
2. **Certificates** (all team-level — shared with other apps like clipp):
   - **Apple Distribution** (signs the iOS app). Needs the private key in the
     login keychain: Keychain Access → Certificate Assistant → *Request a
     Certificate From a Certificate Authority* (save to disk) → upload the CSR
     at Certificates → **+** → Apple Distribution → download the `.cer` and
     double-click it. Verify: `security find-identity -v -p codesigning`.
   - **3rd Party Mac Developer Application** + **Installer** (sign the MAS
     `.app` and `.pkg`) — same CSR dance if the team doesn't have them yet.
3. **Provisioning profiles** (per-app): Profiles → **+** → under
   *Distribution* pick **App Store Connect**, one for platform **iOS** and one
   for **macOS**, both against App ID `stream.gig.app`, each tied to its
   distribution certificate. Download both and point the two
   `..._GIG_PROVISIONING_PROFILE` vars at them.
4. **Export compliance**: both Info.plists declare
   `ITSAppUsesNonExemptEncryption = false` (OpenSSL = standard published
   algorithms, exempt). This is consistent **only with France excluded** from
   availability (App Store Connect → Pricing and Availability); shipping to
   France instead requires the French encryption declaration.

### iOS App Store / TestFlight

```bash
./scripts/build_ios_appstore.sh --version 1.2.3.4 --upload
```

Builds the device vcpkg deps (`--skip-vcpkg` when they're warm), archives with
`xcodebuild` (automatic *development* signing), re-signs for distribution
**manually** (Apple Distribution identity + the profile from
`APPLE_IOS_GIG_PROVISIONING_PROFILE`, auto-installed where xcodebuild looks),
verifies the privacy manifest landed at the bundle root, and uploads with
symbols. Omit `--upload` to export a signed `.ipa` into
`build/ios-appstore/export` instead (inspect it, or upload later via
Transporter).

The upload lands under TestFlight in App Store Connect after processing.
Internal testing needs no review; **external** TestFlight testing and App
Store submission are review-gated in the ASC UI.

### Mac App Store

```bash
./scripts/build_macos_mas.sh --version 1.2.3.4 --upload
```

Builds sandboxed (`resources/gig.mas.entitlements`: App Sandbox +
`network.client`), signs the `.app` with the 3rd Party Mac Developer
Application identity + embedded provisioning profile (staged to local disk
first — post-build surgery on the SMB build tree silently reverts), wraps it
in an Installer-signed `.pkg`, and uploads via `altool`. `CFBundleVersion` is
collapsed to the 4th version component in the bundle (ASC rejects 4-part
values on macOS, ITMS-90257).

With **no flags** it produces an ad-hoc-signed sandboxed build for local
testing — useful to shake out sandbox breakage without any certificates.

Both scripts fail fast on a profile that isn't for `stream.gig.app`
(ITMS-90286 guard), a malformed `--version`, or missing env/identities —
before the expensive build steps where they can.

## Not yet: winget

A winget manifest needs a published release to point at (download URLs + hashes),
so it's deferred until the first signed release exists.
