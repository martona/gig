#!/bin/bash
set -e

# Builds the gig macOS app bundle (gig.app), optionally signed + notarized. Set
# APPLE_CODESIGN_IDENTITY (a Developer ID Application cert) to sign; add --notarize
# to also submit to Apple's notary service and staple the ticket. vcpkg is
# bootstrapped into gig's own cache so this is fully self-contained -- it never
# reuses another project's vcpkg/baseline.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

CONFIG="Release"
VERSION=""
clean=0
notarize=0
# Sign with a stable Developer ID so the keychain "Always Allow" persists across
# rebuilds (set APPLE_CODESIGN_IDENTITY, e.g. "Developer ID Application: Name (TEAMID)").
IDENTITY="${APPLE_CODESIGN_IDENTITY:-}"
TEAM_ID="${APPLE_TEAM_ID:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--debug | --release] [--clean] [--notarize] [--version W.X.Y.Z]

Builds the gig macOS app. Signs it when APPLE_CODESIGN_IDENTITY is set.

Options:
  --debug          Build the Debug configuration.
  --release        Build the Release configuration (default).
  --clean          Remove the build directory before building.
  --version VER    Stamp the bundle with this version (W.X.Y.Z). Omit for 0.0.0.0.
  --notarize       After signing, submit to Apple's notary service and staple.
                   Requires APPLE_CODESIGN_IDENTITY (Developer ID Application) plus
                   the App Store Connect API key in env:
                     APPLE_API_KEY_PATH   (path to AuthKey_XXXX.p8)
                     APPLE_API_KEY_ID     (10-char key id)
                     APPLE_API_ISSUER_ID  (team issuer UUID)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --clean) clean=1; shift ;;
        --notarize) notarize=1; shift ;;
        --version)
            [[ -z "${2:-}" ]] && { echo "[!] --version requires a value" >&2; exit 2; }
            VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Fail fast on a misconfigured --notarize before burning a build.
if [[ "$notarize" == "1" ]]; then
    if [[ -z "$IDENTITY" ]]; then
        echo "[!] Fatal: --notarize requires APPLE_CODESIGN_IDENTITY to be set." >&2
        exit 2
    fi
    : "${APPLE_API_KEY_PATH:?--notarize requires APPLE_API_KEY_PATH (path to .p8)}"
    : "${APPLE_API_KEY_ID:?--notarize requires APPLE_API_KEY_ID}"
    : "${APPLE_API_ISSUER_ID:?--notarize requires APPLE_API_ISSUER_ID}"
    if [[ ! -f "$APPLE_API_KEY_PATH" ]]; then
        echo "[!] Fatal: APPLE_API_KEY_PATH does not point at a readable file: $APPLE_API_KEY_PATH" >&2
        exit 2
    fi
fi

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
    # Lowercase via tr, not ${CONFIG,,}: macOS's /bin/bash is 3.2 (no case expansion),
    # and CI may invoke this with that bash.
    BUILD_DIR="build/macos-$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
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
    SIGN_ARGS=(-DGIG_MACOS_ENABLE_CODE_SIGNING=OFF)
    if [[ -n "$IDENTITY" ]]; then
        SIGN_ARGS=(
            -DGIG_MACOS_ENABLE_CODE_SIGNING=ON
            -DGIG_MACOS_CODE_SIGN_IDENTITY="$IDENTITY"
            -DGIG_MACOS_DEVELOPMENT_TEAM="$TEAM_ID"
            -DGIG_MACOS_ENABLE_HARDENED_RUNTIME=ON
        )
    fi
    echo "[*] Configuring (Xcode generator)..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode "${CMAKE_ARGS[@]}" "${SIGN_ARGS[@]}"
    echo "[*] Building gig ($CONFIG)..."
    cmake --build "$BUILD_DIR" --config "$CONFIG"
else
    echo "[*] Configuring (Ninja generator)..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "${CMAKE_ARGS[@]}"
    echo "[*] Building gig..."
    cmake --build "$BUILD_DIR"
    if [[ -n "$IDENTITY" ]]; then
        # gig.app has no nested frameworks (static link), so a plain sign of the
        # bundle covers the executable -- no --deep needed.
        echo "[*] Signing gig.app with '$IDENTITY' (hardened runtime)..."
        codesign --force --options=runtime --timestamp \
            --entitlements "$REPO_ROOT/resources/gig.entitlements" \
            --sign "$IDENTITY" "$APP_PATH"
    fi
fi

if [[ -n "$IDENTITY" ]]; then
    echo "[*] Verifying signature..."
    codesign --verify --strict --verbose=2 "$APP_PATH"
    codesign --display --verbose=2 "$APP_PATH" 2>&1 | grep -E 'Authority|TeamIdentifier|flags' || true

    # Confirm the hardened-runtime flag actually landed; cheaper to fail here than
    # minutes into a notary round trip. Capture + here-string match (not a pipe into
    # grep -q) so pipefail can't flip on a SIGPIPE to codesign.
    CODESIGN_INFO="$(codesign --display --verbose=4 "$APP_PATH" 2>&1 || true)"
    if ! grep -Eq 'flags=[^[:space:]]*runtime' <<<"$CODESIGN_INFO"; then
        if [[ "$notarize" == "1" ]]; then
            echo "[!] Fatal: hardened runtime flag not present on $APP_PATH; notarization would be rejected." >&2
            exit 1
        fi
        echo "[!] Warning: hardened runtime flag not present on $APP_PATH." >&2
    fi
else
    echo "[*] Unsigned build. Set APPLE_CODESIGN_IDENTITY (+ APPLE_TEAM_ID) to sign --"
    echo "    a stable Developer ID signature makes the keychain 'Always Allow' stick across rebuilds."
fi

if [[ "$notarize" == "1" ]]; then
    ZIP_PATH="$BUILD_DIR/gig.zip"
    echo "[*] Packaging signed app for notarization: $ZIP_PATH"
    # ditto (not zip) preserves macOS metadata; --sequesterRsrc keeps the archive
    # shape notarytool expects; --keepParent puts gig.app at the archive root.
    rm -f "$ZIP_PATH"
    /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$ZIP_PATH"

    echo "[*] Submitting $ZIP_PATH to Apple's notary service (this can take a few minutes)..."
    NOTARY_LOG="$BUILD_DIR/notarytool-submit.log"
    set +e
    xcrun notarytool submit "$ZIP_PATH" \
        --key "$APPLE_API_KEY_PATH" \
        --key-id "$APPLE_API_KEY_ID" \
        --issuer "$APPLE_API_ISSUER_ID" \
        --wait 2>&1 | tee "$NOTARY_LOG"
    NOTARY_STATUS=${PIPESTATUS[0]}
    set -e

    SUBMISSION_ID="$(grep -E '^[[:space:]]*id:' "$NOTARY_LOG" | head -n1 | awk '{print $2}')"
    # notarytool's exit code reflects upload success, not the verdict; the verdict is
    # the final "status:" line -- Accepted is the only good one.
    if [[ "$NOTARY_STATUS" -ne 0 ]] || ! grep -Eq '^[[:space:]]*status: Accepted[[:space:]]*$' "$NOTARY_LOG"; then
        echo "[!] Notarization did not return Accepted." >&2
        if [[ -n "$SUBMISSION_ID" ]]; then
            echo "[!] Fetching notary log for submission $SUBMISSION_ID..." >&2
            xcrun notarytool log "$SUBMISSION_ID" \
                --key "$APPLE_API_KEY_PATH" \
                --key-id "$APPLE_API_KEY_ID" \
                --issuer "$APPLE_API_ISSUER_ID" >&2 || true
        fi
        exit 1
    fi

    echo "[*] Notarization accepted. Stapling ticket to $APP_PATH..."
    xcrun stapler staple "$APP_PATH"
    xcrun stapler validate "$APP_PATH"

    echo "[*] Gatekeeper assessment:"
    spctl --assess --type execute --verbose "$APP_PATH"

    # The pre-staple zip is now stale (the .app inside lacks the ticket). Re-pack so
    # the on-disk archive matches the stapled bundle for distribution.
    echo "[*] Re-packing stapled bundle: $ZIP_PATH"
    rm -f "$ZIP_PATH"
    /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$ZIP_PATH"
    echo "[*] Distributable archive: $ZIP_PATH"
fi

echo "[*] Build complete: $APP_PATH"
echo "[*] Run it with:    open \"$APP_PATH\"   (or: \"$APP_PATH/Contents/MacOS/gig\" for stderr logs)"
