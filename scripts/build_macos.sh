#!/bin/bash
set -e

# Builds the gig macOS app bundle (gig.app). Software-decode target for now; no
# code signing or notarization (that comes later, mirroring clipp's
# sign_notarize_macos_app.sh). vcpkg is bootstrapped into gig's own cache so this
# is fully self-contained -- it never reuses another project's vcpkg/baseline.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

CONFIG="Release"
VERSION=""
clean=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--debug | --release] [--clean] [--version W.X.Y.Z]

Builds the gig macOS app (software decode; unsigned).

Options:
  --debug          Build the Debug configuration.
  --release        Build the Release configuration (default).
  --clean          Remove the build directory before building.
  --version VER    Stamp the bundle with this version (W.X.Y.Z). Omit for 0.0.0.0.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --clean) clean=1; shift ;;
        --version)
            [[ -z "${2:-}" ]] && { echo "[!] --version requires a value" >&2; exit 2; }
            VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# gig's own vcpkg + caches (do not share with sibling projects). Overridable via env.
GIG_CACHE_DIR="${GIG_CACHE_DIR:-$HOME/Library/Caches/gig}"
VCPKG_ROOT="${VCPKG_ROOT:-$GIG_CACHE_DIR/vcpkg}"
VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$GIG_CACHE_DIR/vcpkg-binary-cache}"
VCPKG_INSTALLED_DIR="$GIG_CACHE_DIR/vcpkg-installed"
export VCPKG_ROOT
export VCPKG_DEFAULT_BINARY_CACHE

DEV_DIR="$(xcode-select -p 2>/dev/null)" || {
    echo "[!] Fatal: Xcode Command Line Tools not installed. Run: xcode-select --install" >&2
    exit 1
}

# Full Xcode -> the Xcode generator (multi-config, artifacts under $CONFIG/);
# CLT only -> Ninja (single-config).
if [[ "$DEV_DIR" == */Xcode.app/Contents/Developer ]]; then
    USE_XCODE=1
    BUILD_DIR="build/macos-xcode"
    APP_PATH="$BUILD_DIR/$CONFIG/gig.app"
else
    USE_XCODE=0
    BUILD_DIR="build/macos-${CONFIG,,}"
    APP_PATH="$BUILD_DIR/gig.app"
fi

if [[ "$clean" == "1" ]]; then
    echo "[*] Removing $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
fi

need_tool() {
    command -v "$1" &>/dev/null && return
    command -v brew &>/dev/null || { echo "[!] Fatal: $1 not installed and Homebrew unavailable." >&2; exit 1; }
    echo "[*] Installing $1 via Homebrew..."
    brew install "$1"
}

echo "[*] Checking tools..."
need_tool cmake
[[ "$USE_XCODE" == "1" ]] || need_tool ninja

echo "[*] gig cache:        $GIG_CACHE_DIR"
echo "[*] vcpkg root:       $VCPKG_ROOT"
echo "[*] vcpkg bin cache:  $VCPKG_DEFAULT_BINARY_CACHE"
mkdir -p "$(dirname "$VCPKG_ROOT")" "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_INSTALLED_DIR"

TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    if [[ ! -e "$VCPKG_ROOT" || -z "$(ls -A "$VCPKG_ROOT" 2>/dev/null)" ]]; then
        echo "[*] Cloning vcpkg into $VCPKG_ROOT ..."
        git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    else
        echo "[!] Fatal: $VCPKG_ROOT exists but is not a vcpkg checkout." >&2
        exit 1
    fi
fi
[[ -f "$TOOLCHAIN_FILE" ]] || { echo "[!] Fatal: vcpkg.cmake not found at $TOOLCHAIN_FILE" >&2; exit 1; }
[[ -x "$VCPKG_ROOT/vcpkg" ]] || { echo "[*] Bootstrapping vcpkg..."; "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics; }

# Manifest + overlay ports/triplets live at the repo root (vcpkg.json,
# vcpkg-configuration.json). The CMakeLists Apple block derives the static
# arm64-osx-macos14 / x64-osx-macos14 triplet from `uname -m`.
CMAKE_ARGS=(
    -DVCPKG_MANIFEST_DIR="$REPO_ROOT"
    -DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DVCPKG_INSTALL_OPTIONS="--clean-buildtrees-after-build;--clean-packages-after-build"
)
[[ -n "$VERSION" ]] && CMAKE_ARGS+=(-DGIG_VERSION="$VERSION")

if [[ "$USE_XCODE" == "1" ]]; then
    echo "[*] Configuring (Xcode generator)..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode "${CMAKE_ARGS[@]}"
    echo "[*] Building gig ($CONFIG)..."
    cmake --build "$BUILD_DIR" --config "$CONFIG"
else
    echo "[*] Configuring (Ninja generator)..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "${CMAKE_ARGS[@]}"
    echo "[*] Building gig..."
    cmake --build "$BUILD_DIR"
fi

echo "[*] Build complete: $APP_PATH"
echo "[*] Run it with:    open \"$APP_PATH\"   (or: \"$APP_PATH/Contents/MacOS/gig\" for stderr logs)"
