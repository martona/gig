# Releasing gig

Versioning is **tag-driven**. There is no committed version file: the tag (or the
manual-workflow input) is the source of truth, and CMake stamps it into
`gig.exe`'s `VERSIONINFO` + the MSIX identity (Windows) and the `gig.app`
`Info.plist` (macOS). Plain local builds fall back to `0.0.0.0`.

Version format is `W.X.Y.Z` (four integers), e.g. `0.1.0.0`.

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

## Not yet: winget

A winget manifest needs a published release to point at (download URLs + hashes),
so it's deferred until the first signed release exists.
