#!/bin/bash
set -euo pipefail

# Installs gig's iOS vcpkg dependencies (Boost, OpenSSL, the trimmed FFmpeg
# overlay) for the device and/or simulator triplet, and publishes them where the
# Xcode project (ios/project.yml) expects: vcpkg-installed/<triplet>. This is the
# iOS feasibility gate -- it proves FFmpeg/Boost/OpenSSL cross-compile for
# arm64-ios -- and it runs the same on a dev Mac and on the macos-latest CI runner.
#
# gig's manifest (vcpkg.json) + overlays (vcpkg-ports, vcpkg-triplets) live at the
# repo root, unlike clipp (src/). gig's own vcpkg + caches stay under the gig cache
# dir so this never reuses another project's vcpkg/baseline.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

# Run this from a normal shell (Terminal), NOT from inside an Xcode run-script build
# phase: Xcode exports its build settings (SDKROOT=iphonesimulator, etc.) into the
# environment, which poisons vcpkg's host-tool builds. Build the deps here once, then
# build the app in Xcode (or via scripts/build_ios.sh).

GIG_CACHE_DIR="${GIG_CACHE_DIR:-$HOME/Library/Caches/gig}"
VCPKG_ROOT="${VCPKG_ROOT:-$GIG_CACHE_DIR/vcpkg}"
VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$GIG_CACHE_DIR/vcpkg-binary-cache}"
# Final publish root consumed by Xcode (HEADER/LIBRARY_SEARCH_PATHS in project.yml).
VCPKG_INSTALLED_DIR="$REPO_ROOT/vcpkg-installed"
# Per-triplet staging root (vcpkg installs here, we ditto the result into place).
VCPKG_STAGING_INSTALLED_DIR="$GIG_CACHE_DIR/vcpkg-ios-installed"
OVERLAY_TRIPLETS="$REPO_ROOT/vcpkg-triplets"
OVERLAY_PORTS="$REPO_ROOT/vcpkg-ports"
IOS_DEVICE_TRIPLET="arm64-ios"
IOS_SIMULATOR_TRIPLET="arm64-ios-simulator"

export VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE
export VCPKG_BINARY_SOURCES="${VCPKG_BINARY_SOURCES:-clear;files,$VCPKG_DEFAULT_BINARY_CACHE,readwrite}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--device-only] [--simulator-only] [--triplet TRIPLET] [--clean]

Installs gig's iOS vcpkg dependencies into:
  $VCPKG_INSTALLED_DIR/<triplet>

Uses per-triplet staging roots under:
  $VCPKG_STAGING_INSTALLED_DIR

Default triplets:
  device:    $IOS_DEVICE_TRIPLET
  simulator: $IOS_SIMULATOR_TRIPLET
EOF
}

install_device=1
install_simulator=1
custom_triplets=()
clean=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device-only) install_device=1; install_simulator=0; shift ;;
        --simulator-only) install_device=0; install_simulator=1; shift ;;
        --triplet)
            [[ $# -ge 2 ]] || { echo "[!] --triplet requires a value." >&2; exit 2; }
            custom_triplets+=("$2"); shift 2 ;;
        --clean) clean=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

need_tool() {
    command -v "$1" >/dev/null 2>&1 || { echo "[!] Fatal: $1 is required but was not found in PATH." >&2; exit 1; }
}

need_tool git
need_tool xcrun
need_tool ditto

# FFmpeg's vcpkg port cross-compiles via autotools; a missing one fails deep in the
# port with a confusing error, so check up front.
if ! command -v autoconf >/dev/null 2>&1 \
    || ! command -v automake >/dev/null 2>&1 \
    || { ! command -v libtoolize >/dev/null 2>&1 && ! command -v glibtoolize >/dev/null 2>&1; }; then
    cat >&2 <<'EOF'
[!] Missing autotools required to cross-compile FFmpeg for iOS.
    With Homebrew:  brew install autoconf autoconf-archive automake libtool
EOF
    exit 1
fi

xcrun --sdk iphoneos --show-sdk-path >/dev/null
xcrun --sdk iphonesimulator --show-sdk-path >/dev/null

if [[ "$clean" == "1" ]]; then
    echo "[*] Cleaning iOS vcpkg install root: $VCPKG_INSTALLED_DIR"
    rm -rf "$VCPKG_INSTALLED_DIR"
    echo "[*] Cleaning iOS vcpkg staging root: $VCPKG_STAGING_INSTALLED_DIR"
    rm -rf "$VCPKG_STAGING_INSTALLED_DIR"
fi

mkdir -p "$(dirname "$VCPKG_ROOT")" "$VCPKG_DEFAULT_BINARY_CACHE" \
         "$VCPKG_INSTALLED_DIR" "$VCPKG_STAGING_INSTALLED_DIR"

TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    if [[ ! -e "$VCPKG_ROOT" || -z "$(ls -A "$VCPKG_ROOT" 2>/dev/null)" ]]; then
        echo "[*] Cloning vcpkg into $VCPKG_ROOT..."
        git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    else
        echo "[!] Fatal: $VCPKG_ROOT exists but does not look like a vcpkg checkout." >&2
        exit 1
    fi
fi

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    echo "[*] Bootstrapping vcpkg..."
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

triplets=()
if [[ "${#custom_triplets[@]}" -gt 0 ]]; then
    triplets=("${custom_triplets[@]}")
else
    [[ "$install_device" == "1" ]] && triplets+=("$IOS_DEVICE_TRIPLET")
    [[ "$install_simulator" == "1" ]] && triplets+=("$IOS_SIMULATOR_TRIPLET")
fi

extra_options=(--clean-buildtrees-after-build --clean-packages-after-build)

for triplet in "${triplets[@]}"; do
    echo "[*] Installing vcpkg manifest for $triplet..."
    triplet_install_root="$VCPKG_STAGING_INSTALLED_DIR/$triplet"
    (
        cd "$REPO_ROOT"
        "$VCPKG_ROOT/vcpkg" install \
            --triplet "$triplet" \
            --overlay-triplets "$OVERLAY_TRIPLETS" \
            --overlay-ports "$OVERLAY_PORTS" \
            --x-install-root="$triplet_install_root" \
            "${extra_options[@]}"
    )

    triplet_source_dir="$triplet_install_root/$triplet"
    triplet_target_dir="$VCPKG_INSTALLED_DIR/$triplet"
    [[ -d "$triplet_source_dir" ]] || {
        echo "[!] Fatal: expected vcpkg output was not created: $triplet_source_dir" >&2
        exit 1
    }

    echo "[*] Publishing $triplet to $triplet_target_dir..."
    rm -rf "$triplet_target_dir"
    ditto "$triplet_source_dir" "$triplet_target_dir"
done

echo "[*] iOS vcpkg dependencies are ready under $VCPKG_INSTALLED_DIR"
