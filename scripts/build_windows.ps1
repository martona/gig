[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$BuildType = "Release",

    [string]$Triplet = "",

    [string]$VcpkgRoot = "",

    [string]$VcVarsAll = $env:VCVARSALL,

    [string]$VcVarsArch = "amd64",

    [switch]$ConfigureOnly
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildRoot = Join-Path $repoRoot "build"

if (-not $Triplet) {
    $Triplet = "x64-windows-static"
}

function Find-Executable {
    param(
        [string]$Name,
        [string[]]$Candidates
    )

    $fromPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

function Resolve-Executable {
    param(
        [string]$Name,
        [string[]]$Candidates
    )

    $resolved = Find-Executable -Name $Name -Candidates $Candidates
    if ($resolved) {
        return $resolved
    }

    throw "Could not find $Name. Install it or add it to PATH."
}

function Invoke-NativeCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' failed with exit code $LASTEXITCODE."
    }
}

function Resolve-VcVarsAll {
    param([string]$RequestedPath)

    $candidates = @()

    if ($RequestedPath) {
        $candidates += $RequestedPath
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidates += Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
        }
    }

    $candidates += @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    throw "Could not find vcvarsall.bat. Pass -VcVarsAll C:\path\to\vcvarsall.bat."
}

function Import-VcVarsEnvironment {
    param(
        [string]$VcVarsAllPath,
        [string]$Arch
    )

    $resolvedVcVarsAll = Resolve-VcVarsAll -RequestedPath $VcVarsAllPath
    Write-Host "Importing Visual Studio environment ($Arch)..."

    $command = "call `"$resolvedVcVarsAll`" $Arch >nul && set"
    $environment = & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall.bat failed for architecture '$Arch'."
    }

    foreach ($line in $environment) {
        if ($line -match "^([^=]+)=(.*)$") {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

function Resolve-ProjectPath {
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-VcpkgBaseline {
    $manifestPath = Join-Path $repoRoot "vcpkg.json"
    if (-not (Test-Path $manifestPath)) {
        return ""
    }

    $manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
    if ($manifest.'builtin-baseline') {
        return $manifest.'builtin-baseline'
    }

    return ""
}

function Invoke-VcpkgBootstrap {
    param([string]$Root)

    $bootstrap = Join-Path $Root "bootstrap-vcpkg.bat"
    if (-not (Test-Path $bootstrap)) {
        throw "Could not find vcpkg bootstrap script at $bootstrap."
    }

    Push-Location $Root
    try {
        & cmd.exe /d /c "bootstrap-vcpkg.bat -disableMetrics"
        if ($LASTEXITCODE -ne 0) {
            throw "'$bootstrap' failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }
}

function Resolve-VcpkgRoot {
    param([string]$RequestedRoot)

    if ($RequestedRoot) {
        $root = Resolve-ProjectPath -Path $RequestedRoot
    }
    else {
        $root = Join-Path $buildRoot "vcpkg"
    }

    $toolchain = Join-Path $root "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $toolchain)) {
        if (Test-Path $root) {
            throw "vcpkg root exists but is incomplete: $root"
        }

        $gitExe = Resolve-Executable -Name "git" -Candidates @(
            "C:\Program Files\Git\cmd\git.exe",
            "C:\Program Files\Git\bin\git.exe"
        )

        New-Item -ItemType Directory -Force -Path (Split-Path $root -Parent) | Out-Null
        Write-Host "Cloning private vcpkg root into $root..."
        Invoke-NativeCommand -FilePath $gitExe -Arguments @(
            "clone",
            "https://github.com/microsoft/vcpkg.git",
            $root
        )

        $baseline = Get-VcpkgBaseline
        if ($baseline) {
            Write-Host "Checking out vcpkg baseline $baseline..."
            Invoke-NativeCommand -FilePath $gitExe -Arguments @(
                "-C", $root,
                "checkout",
                "--detach",
                $baseline
            )
        }
    }

    $vcpkgExe = Join-Path $root "vcpkg.exe"
    if (-not (Test-Path $vcpkgExe)) {
        Write-Host "Bootstrapping private vcpkg..."
        Invoke-VcpkgBootstrap -Root $root
    }

    if (-not (Test-Path $toolchain)) {
        throw "vcpkg toolchain file was not found at $toolchain."
    }

    return $root
}

$cmakeCandidates = @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

$ninjaCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)

if (-not (Find-Executable -Name "cmake" -Candidates $cmakeCandidates) -or -not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Import-VcVarsEnvironment -VcVarsAllPath $VcVarsAll -Arch $VcVarsArch
}

$resolvedVcpkgRoot = Resolve-VcpkgRoot -RequestedRoot $VcpkgRoot
$env:VCPKG_ROOT = $resolvedVcpkgRoot
$toolchainFile = Join-Path $resolvedVcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$buildDir = Join-Path $buildRoot "windows-$($BuildType.ToLowerInvariant())"
$vcpkgInstalledDir = Join-Path $buildRoot "vcpkg-installed"
$vcpkgCacheDir = Join-Path $buildRoot "vcpkg-binary-cache"
$vcpkgDownloadsDir = Join-Path $buildRoot "vcpkg-downloads"
$cmakeExe = Resolve-Executable -Name "cmake" -Candidates $cmakeCandidates
$ninjaExe = Resolve-Executable -Name "ninja" -Candidates $ninjaCandidates

New-Item -ItemType Directory -Force -Path $vcpkgCacheDir, $vcpkgDownloadsDir | Out-Null
$env:VCPKG_BINARY_SOURCES = "clear;files,$vcpkgCacheDir,readwrite"
$env:VCPKG_DOWNLOADS = $vcpkgDownloadsDir

$configureArgs = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$ninjaExe",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    "-DVCPKG_INSTALLED_DIR=$vcpkgInstalledDir",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)

if ($env:VCPKG_INSTALL_OPTIONS) {
    $configureArgs += "-DVCPKG_INSTALL_OPTIONS=$env:VCPKG_INSTALL_OPTIONS"
}

Write-Host "Configuring gig ($BuildType, $Triplet)..."
Invoke-NativeCommand -FilePath $cmakeExe -Arguments $configureArgs

if ($ConfigureOnly) {
    Write-Host "Configured: $buildDir"
    return
}

Write-Host "Building gig..."
Invoke-NativeCommand -FilePath $cmakeExe -Arguments @("--build", $buildDir, "--config", $BuildType)

Write-Host "Built: $buildDir"
