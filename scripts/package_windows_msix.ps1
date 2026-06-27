#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Packages the built gig.exe as a full-trust MSIX for signed sideload / direct
  download (not the Microsoft Store).

.DESCRIPTION
  Stages the single GUI gig.exe, generates the PNG logo set + a resources.pri index,
  fills in the AppxManifest template, runs makeappx, and (unless -NoSign) signs the
  package with Azure Trusted Signing -- the same ARTIFACT_SIGNING_* env vars + 'az
  login' that scripts/build_windows.ps1 uses. CI passes -NoSign and signs the .msix
  with the azure/trusted-signing-action instead.

  Logo PNGs are left in build\msix\layout-<arch>\Images for inspection.

.NOTES
  Requires the Windows 10/11 SDK (makeappx.exe, makepri.exe). For -NoSign you only
  need the SDK; for signing, also sign.exe (dotnet tool install --global --prerelease
  sign). Build the matching arch first, e.g.:
    .\scripts\build_windows.ps1 -Triplet x64-windows-static -VcVarsArch amd64
#>
[CmdletBinding()]
param(
    [ValidateSet('amd64', 'arm64')]
    [string]$Arch = 'amd64',

    # MSIX version (W.X.Y.Z). Omit to read it from the built gig.exe's VERSIONINFO.
    [string]$Version = '',

    [string]$BuildDir = 'build\windows-release',
    [string]$OutDir = 'build\msix',

    # Package identity. Publisher must equal the Trusted Signing certificate subject;
    # it's read automatically from the signed gig.exe, so normally leave it unset.
    [string]$IdentityName = 'gig',
    [string]$Publisher = '',
    [string]$PublisherDisplayName = 'gig',

    # Pack only, leaving the .msix unsigned -- for CI, which signs it with the Azure
    # Trusted Signing action. Locally you normally omit this (the script signs).
    [switch]$NoSign
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot
# Set-Location does NOT update [Environment]::CurrentDirectory; child tools (makeappx,
# makepri, sign) resolve relative path args against the .NET CWD. Keep them in sync and
# make our working paths absolute so nothing depends on it.
[Environment]::CurrentDirectory = $repoRoot
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repoRoot $BuildDir }
if (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $repoRoot $OutDir }
$masterImage = Join-Path $repoRoot 'resources\boat.png'
$manifestTemplate = Join-Path $repoRoot 'resources\msix\AppxManifest.xml.in'

# MSIX ProcessorArchitecture names differ from our vcpkg/vcvars arch names.
$msixArch = if ($Arch -eq 'amd64') { 'x64' } else { 'arm64' }

# Trusted Signing is mandatory unless -NoSign (CI signs the .msix with the action).
# Fail fast before building the layout and packing.
if (-not $NoSign) {
    if (-not (Get-Command sign.exe -ErrorAction SilentlyContinue)) {
        throw "sign.exe (the Trusted Signing CLI) is not on PATH. Install it: dotnet tool install --global --prerelease sign"
    }
    foreach ($name in 'ARTIFACT_SIGNING_ENDPOINT', 'ARTIFACT_SIGNING_ACCOUNT', 'ARTIFACT_SIGNING_CERTIFICATE_PROFILE') {
        if (-not [Environment]::GetEnvironmentVariable($name)) {
            throw "Signing requires $name (the same vars build_windows.ps1 uses). Run 'az login' and set the ARTIFACT_SIGNING_* vars."
        }
    }
}

function Find-SdkTool {
    param([Parameter(Mandatory)][string]$Name)
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path $kitsBin)) {
        throw "Windows SDK not found at '$kitsBin'. Install the Windows 10/11 SDK (ships makeappx.exe and makepri.exe)."
    }
    $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $versions = Get-ChildItem -Path $kitsBin -Directory |
        Where-Object { $_.Name -like '10.*' } | Sort-Object Name -Descending
    foreach ($v in $versions) {
        foreach ($a in @($hostArch, 'x64', 'x86')) {
            $candidate = Join-Path $v.FullName "$a\$Name"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "Could not locate $Name under '$kitsBin'."
}

function New-LogoPng {
    # Downscale the master app image to a Size x Size transparent PNG at an absolute path.
    param([Parameter(Mandatory)][string]$Master, [Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][int]$Size)
    $src = [System.Drawing.Image]::FromFile($Master)
    try {
        $dst = New-Object System.Drawing.Bitmap $Size, $Size
        try {
            $g = [System.Drawing.Graphics]::FromImage($dst)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $g.Clear([System.Drawing.Color]::Transparent)
            $g.DrawImage($src, 0, 0, $Size, $Size)
            $g.Dispose()
            $dst.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $dst.Dispose() }
    } finally { $src.Dispose() }
}

$makeappx = Find-SdkTool 'makeappx.exe'
$makepri = Find-SdkTool 'makepri.exe'
Write-Host "[*] makeappx: $makeappx"
Write-Host "[*] makepri:  $makepri"

# --- locate the built binary -------------------------------------------------
$guiExe = Join-Path $BuildDir 'gig.exe'
if (-not (Test-Path $guiExe)) {
    $triplet = if ($Arch -eq 'amd64') { 'x64-windows-static' } else { 'arm64-windows-static' }
    throw "gig.exe not found at '$guiExe'. Build the $Arch slice first: .\scripts\build_windows.ps1 -Triplet $triplet -VcVarsArch $Arch"
}
if (-not (Test-Path $masterImage)) { throw "Master app image not found: $masterImage" }
if (-not (Test-Path $manifestTemplate)) { throw "Missing manifest template: $manifestTemplate" }

# Default the package version to whatever the built exe was stamped with (VERSIONINFO),
# so it always matches the binary being packaged. Pass -Version to override.
if (-not $Version) {
    $vi = [Diagnostics.FileVersionInfo]::GetVersionInfo($guiExe)
    $Version = '{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
    Write-Host "[*] Version (from gig.exe): $Version"
}

# The manifest Publisher must equal the Trusted Signing cert subject. The exe is already
# signed by the same profile, so read the exact subject off it (avoids a hand-typed DN
# that won't match). Pass -Publisher only if the exe isn't signed yet.
$sig = Get-AuthenticodeSignature $guiExe -ErrorAction SilentlyContinue
if ($sig -and $sig.SignerCertificate -and $sig.Status -ne 'NotSigned') {
    $detected = $sig.SignerCertificate.Subject
    if ($Publisher -and $Publisher -ne $detected) {
        Write-Warning "Overriding -Publisher '$Publisher' with the signed exe's cert subject '$detected' (MSIX requires Publisher == signing cert subject)."
    }
    $Publisher = $detected
    Write-Host "[*] Publisher (from signed gig.exe): $Publisher"
}
elseif (-not $Publisher) {
    throw "gig.exe at '$guiExe' is not signed, so the Trusted Signing cert subject can't be auto-detected. Sign it first (build_windows.ps1 with ARTIFACT_SIGNING_* set), or pass the exact subject via -Publisher."
}

# --- stage the package layout ------------------------------------------------
$layout = Join-Path $OutDir "layout-$Arch"
if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
$layoutImages = Join-Path $layout 'Images'
New-Item -ItemType Directory -Force -Path $layoutImages | Out-Null

Copy-Item $guiExe (Join-Path $layout 'gig.exe') -Force

# --- generate the PNG logo set ----------------------------------------------
# MSIX uses PNG (never .ico) for tile/Start/taskbar. The manifest names the base logos;
# Windows resolves these scale-*/targetsize-*/altform-unplated variants via resources.pri.
if (-not ('System.Drawing.Bitmap' -as [type])) {
    try { Add-Type -AssemblyName System.Drawing -ErrorAction Stop }
    catch { throw "System.Drawing is unavailable in this PowerShell. Run this script in Windows PowerShell (powershell.exe), where it is built in." }
}
$scales = @(125, 150, 200, 400)
# logical name => base (scale-100) pixel size. All square (the wide tile is skipped).
$squareLogos = [ordered]@{
    'StoreLogo'         = 50
    'Square44x44Logo'   = 44
    'Square71x71Logo'   = 71
    'Square150x150Logo' = 150
}
foreach ($name in $squareLogos.Keys) {
    $base = $squareLogos[$name]
    New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "$name.png") -Size $base
    foreach ($s in $scales) {
        $px = [int][Math]::Round($base * $s / 100.0, [MidpointRounding]::AwayFromZero)
        New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "$name.scale-$s.png") -Size $px
    }
}
# Square44x44 app-icon target sizes, both plated and altform-unplated (the unplated set
# is what renders the taskbar/Start icon transparently, with no colored plate).
foreach ($t in @(16, 24, 32, 48, 256)) {
    New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "Square44x44Logo.targetsize-$t.png") -Size $t
    New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "Square44x44Logo.targetsize-${t}_altform-unplated.png") -Size $t
}
Write-Host "[*] Generated $((Get-ChildItem $layoutImages -Filter *.png).Count) logo PNGs in $layoutImages"

# --- substitute manifest tokens ----------------------------------------------
$manifest = (Get-Content -Raw $manifestTemplate).
    Replace('@IDENTITY_NAME@', $IdentityName).
    Replace('@PUBLISHER@', $Publisher).
    Replace('@PUBLISHER_DISPLAY_NAME@', $PublisherDisplayName).
    Replace('@VERSION@', $Version).
    Replace('@ARCH@', $msixArch)
Set-Content -Path (Join-Path $layout 'AppxManifest.xml') -Value $manifest -Encoding UTF8

# --- index resources (resources.pri) -----------------------------------------
# Without this, Windows ignores the scale/targetsize/unplated qualifier files and the
# Start/taskbar icon can render blank. makepri builds the index from the file names.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$priConfig = Join-Path $OutDir 'priconfig.xml'
& $makepri createconfig /cf $priConfig /dq en-US /o
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed ($LASTEXITCODE)." }
& $makepri new /pr $layout /cf $priConfig /mn (Join-Path $layout 'AppxManifest.xml') /of (Join-Path $layout 'resources.pri') /o
if ($LASTEXITCODE -ne 0) { throw "makepri new failed ($LASTEXITCODE)." }

# --- pack --------------------------------------------------------------------
$msix = Join-Path $OutDir "gig-$Version-$Arch.msix"
if (Test-Path $msix) { Remove-Item -Force $msix }
& $makeappx pack /d $layout /p $msix /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)." }
Write-Host "[*] Packed: $msix"

# --- sign (Azure Trusted Signing) --------------------------------------------
if ($NoSign) {
    Write-Host '[*] -NoSign: leaving the .msix unsigned (the workflow signs it).'
}
else {
    $signArgs = @(
        'code', 'artifact-signing',
        '-b', $OutDir,
        '-ase', $env:ARTIFACT_SIGNING_ENDPOINT,
        '-asa', $env:ARTIFACT_SIGNING_ACCOUNT,
        (Split-Path -Leaf $msix),
        '-v', 'Information',
        '-ascp', $env:ARTIFACT_SIGNING_CERTIFICATE_PROFILE
    )
    & sign.exe @signArgs
    if ($LASTEXITCODE -ne 0) { throw "sign.exe (Trusted Signing) failed ($LASTEXITCODE)." }
    Write-Host '[*] Signed with Azure Trusted Signing.'
}

Write-Host ''
Write-Host "[*] Done: $msix"
if ($NoSign) {
    Write-Host '[*] Package is UNSIGNED (-NoSign) -- sign it before install/distribution.'
    return
}
Write-Host ''
Write-Host 'Sideload-test (no cert import needed; Trusted Signing chains to a trusted root):'
Write-Host "  Add-AppxPackage '$msix'"
Write-Host 'Remove when done:'
Write-Host '  Get-AppxPackage *gig* | Remove-AppxPackage'
