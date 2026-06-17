<#
.SYNOPSIS
  Seed the gig settings store (HKCU\Software\gig) for testing until the native
  settings dialog (PLAN.md M5) lands.

.DESCRIPTION
  Writes config the way the C++ SettingsStore reads it: plain values as
  REG_SZ/REG_DWORD/REG_QWORD, and the password DPAPI-wrapped (CurrentUser scope)
  as REG_BINARY. DPAPI is per-user, so run this as the same Windows user that
  runs gig.exe. Existing values are overwritten; unspecified ones are left alone.

.EXAMPLE
  .\scripts\seed_settings.ps1 -Base 'https://frigate.lan:9971/' -User viewbot -Password 's3cret' -Software

.EXAMPLE
  # Pull base/user/password from an existing gig.ini (the old config format):
  .\scripts\seed_settings.ps1 -FromIni .\build\windows-release\gig.ini -Software
#>
[CmdletBinding()]
param(
    [string]$Base,
    [string]$User,
    [string]$Password,
    [int]$LoginRefresh = 600,
    [switch]$Software,
    [string]$FromIni
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security

if ($FromIni) {
    foreach ($line in Get-Content $FromIni) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#') -or $t.StartsWith(';') -or -not $t.Contains('=')) { continue }
        $k, $v = $t -split '=', 2
        switch ($k.Trim()) {
            'base'     { if (-not $Base)     { $Base = $v.Trim() } }
            'user'     { if (-not $User)     { $User = $v.Trim() } }
            'password' { if (-not $Password) { $Password = $v.Trim() } }
        }
    }
}

$root = 'HKCU:\Software\gig'
New-Item -Path $root -Force | Out-Null

if ($Base) { Set-ItemProperty $root -Name base -Value $Base }
if ($User) { Set-ItemProperty $root -Name user -Value $User }
Set-ItemProperty $root -Name 'login-refresh' -Value $LoginRefresh -Type QWord
Set-ItemProperty $root -Name software -Value ([int][bool]$Software) -Type DWord

if ($Password) {
    $blob = [Security.Cryptography.ProtectedData]::Protect(
        [Text.Encoding]::UTF8.GetBytes($Password), $null,
        [Security.Cryptography.DataProtectionScope]::CurrentUser)
    Set-ItemProperty $root -Name password -Value $blob -Type Binary
}

Write-Host "seeded HKCU\Software\gig (base=$Base user=$User software=$([int][bool]$Software))"
