# Releasing gig

Versioning is **tag-driven**. There is no committed version file: the tag (or the
manual-workflow input) is the source of truth, and CMake stamps it into
`gig.exe`'s `VERSIONINFO` and the MSIX identity. Plain local builds fall back to
`0.0.0.0`.

Version format is `W.X.Y.Z` (four integers), e.g. `0.1.0.0`.

## Cut a release

### Option A — push a tag (recommended)

```powershell
git tag v0.1.0.0
git push origin v0.1.0.0
```

`Release (tag)` builds amd64 + arm64, signs `gig.exe`, packages the MSIX + portable
zip, attests build provenance, and creates a **draft** release. Review it, then
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

Installable assets are version-less so `releases/latest/download/...` URLs stay
stable; the version still lives in the binary's `VERSIONINFO`, the MSIX identity,
and the build-provenance attestation (which binds by SHA-256, not filename).

Install the MSIX (no cert import needed — Trusted Signing chains to a trusted root):

```powershell
Add-AppxPackage gig-windows-amd64.msix
# remove later: Get-AppxPackage *gig* | Remove-AppxPackage
```

## Local builds

```powershell
# Stamp a version (omit for a 0.0.0.0 dev build):
.\scripts\build_windows.ps1 -Version 0.1.0.0

# Package an MSIX locally (signs if you have `az login` + the ARTIFACT_SIGNING_*
# vars + sign.exe; otherwise pass -NoSign):
.\scripts\package_windows_msix.ps1 -Arch amd64 -Version 0.1.0.0
```

## Signing setup (one-time, per repo)

Releases sign with **Azure Trusted Signing** via OIDC — no cert files in the repo.
Configure once in the gig repo settings:

- An environment named **`release-signing`**.
- Secrets: `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`.
- Variables: `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`,
  `ARTIFACT_SIGNING_CERTIFICATE_PROFILE`.
- On the Azure side, a federated credential whose subject is
  `repo:martona/gig:environment:release-signing`.

The MSIX `Publisher` is derived automatically from the signed `gig.exe`'s
certificate subject, so it always matches the signing identity.

## Not yet: winget

A winget manifest needs a published release to point at (download URLs + hashes),
so it's deferred until the first signed release exists.
