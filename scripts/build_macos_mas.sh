#!/bin/bash
set -e

# Builds gig.app sandboxed for the Mac App Store and (optionally) signs,
# packages, and uploads it to App Store Connect. This is a separate flow from
# build_macos.sh: MAS uses App Sandbox entitlements, an Apple Distribution /
# "3rd Party Mac Developer" certificate pair, an embedded provisioning
# profile, and a productbuild .pkg -- none of which apply to the Developer ID
# channel. Mirrors ../clipp/scripts/build_macos_mas.sh, gig-ified.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# Mac App Store distribution uses a different cert than Developer ID.
# APPLE_CODESIGN_IDENTITY stays reserved for build_macos.sh's Developer ID
# signing; this script reads the MAS variants via the _3RDPARTY names.
IDENTITY="${APPLE_CODESIGN_IDENTITY_3RDPARTY:-}"
INSTALLER_IDENTITY="${APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY:-}"
TEAM_ID="${APPLE_TEAM_ID:-}"
PROVISION_PROFILE="${APPLE_MAS_PROVISIONING_PROFILE:-}"
CONFIG="Release"
VERSION=""
clean=0
sign_for_distribution=0
package=0
upload=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--debug | --release] [--clean] [--sign] [--package] [--upload] [--version W.X.Y.Z]

Builds gig.app sandboxed for the Mac App Store.

Default (no flags, no env vars): build, then ad-hoc sign with the sandbox
entitlements. Result runs locally and exercises the sandbox; not
distributable. Useful for verifying nothing breaks under sandbox before
chasing certs.

Options:
  --debug          Build the Debug configuration.
  --release        Build the Release configuration (default).
  --clean          Remove the build directory before building.
  --version VER    Stamp the bundle with this version (W.X.Y.Z, should match
                   the release tag). Omit for 0.0.0.0 (local sandbox testing).
  --sign           Sign for App Store distribution. Requires:
                     APPLE_CODESIGN_IDENTITY_3RDPARTY (3rd Party Mac Developer Application cert)
                     APPLE_TEAM_ID                    (10-char Apple Team ID)
                     APPLE_MAS_PROVISIONING_PROFILE   (path to the Mac App Store .provisionprofile)
  --package        Wrap the signed app in a .pkg via productbuild. Implies --sign.
                   Also requires:
                     APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY (3rd Party Mac Developer Installer cert)
  --upload         Submit the .pkg to App Store Connect via xcrun altool. Implies --package.
                   Also requires:
                     APPLE_API_KEY_PATH    (path to AuthKey_XXXX.p8)
                     APPLE_API_KEY_ID      (10-char key id)
                     APPLE_API_ISSUER_ID   (issuer UUID)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --clean) clean=1; shift ;;
        --sign) sign_for_distribution=1; shift ;;
        --package) package=1; sign_for_distribution=1; shift ;;
        --upload) upload=1; package=1; sign_for_distribution=1; shift ;;
        --version)
            [[ -z "${2:-}" ]] && { echo "[!] --version requires a value" >&2; exit 2; }
            VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Fail fast on missing prerequisites before burning a vcpkg+cmake cycle.
# CMake validates GIG_VERSION too, but only after the full dependency build.
if [[ -n "$VERSION" ]] && ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[!] Fatal: --version must be W.X.Y.Z (4 dot-separated non-negative integers); got '$VERSION'" >&2
    exit 2
fi
if [[ "$sign_for_distribution" == "1" ]]; then
    if [[ -z "$IDENTITY" ]]; then
        echo "[!] Fatal: --sign/--package/--upload require APPLE_CODESIGN_IDENTITY_3RDPARTY ('3rd Party Mac Developer Application' cert)." >&2
        exit 2
    fi
    if [[ -z "$TEAM_ID" ]]; then
        echo "[!] Fatal: --sign/--package/--upload require APPLE_TEAM_ID (your 10-char Apple Team ID)." >&2
        exit 2
    fi
    if [[ -z "$PROVISION_PROFILE" ]]; then
        echo "[!] Fatal: --sign/--package/--upload require APPLE_MAS_PROVISIONING_PROFILE (path to the .provisionprofile)." >&2
        exit 2
    fi
    if [[ ! -f "$PROVISION_PROFILE" ]]; then
        echo "[!] Fatal: APPLE_MAS_PROVISIONING_PROFILE does not point at a file: $PROVISION_PROFILE" >&2
        exit 2
    fi
fi
if [[ "$package" == "1" && -z "$INSTALLER_IDENTITY" ]]; then
    echo "[!] Fatal: --package/--upload require APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY ('3rd Party Mac Developer Installer' cert)." >&2
    exit 2
fi
if [[ "$upload" == "1" ]]; then
    : "${APPLE_API_KEY_PATH:?--upload requires APPLE_API_KEY_PATH (path to .p8)}"
    : "${APPLE_API_KEY_ID:?--upload requires APPLE_API_KEY_ID}"
    : "${APPLE_API_ISSUER_ID:?--upload requires APPLE_API_ISSUER_ID}"
    if [[ ! -f "$APPLE_API_KEY_PATH" ]]; then
        echo "[!] Fatal: APPLE_API_KEY_PATH does not point at a readable file: $APPLE_API_KEY_PATH" >&2
        exit 2
    fi
fi

# gig's own vcpkg + caches, shared with build_macos.sh (same deps, same
# triplet -- only the post-build signing differs). Overridable via env.
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

# Separate build dir from build_macos.sh so the two flows can coexist without
# cmake reconfigure thrash when switching between Developer ID and MAS.
BUILD_DIR="build/macos-mas"
if [[ "$DEV_DIR" == */Xcode.app/Contents/Developer ]]; then
    USE_XCODE=1
    APP_PATH="$BUILD_DIR/$CONFIG/gig.app"
else
    USE_XCODE=0
    APP_PATH="$BUILD_DIR/gig.app"
fi

ENTITLEMENTS_FILE="$REPO_ROOT/resources/gig.mas.entitlements"
if [[ ! -f "$ENTITLEMENTS_FILE" ]]; then
    echo "[!] Fatal: MAS entitlements file missing: $ENTITLEMENTS_FILE" >&2
    exit 1
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

CMAKE_ARGS=(
    -DVCPKG_MANIFEST_DIR="$REPO_ROOT"
    -DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DVCPKG_INSTALL_OPTIONS="--clean-buildtrees-after-build;--clean-packages-after-build"
)
[[ -n "$VERSION" ]] && CMAKE_ARGS+=(-DGIG_VERSION="$VERSION")

# Always build unsigned; sign post-build so entitlements/identity swaps don't
# round-trip through a cmake reconfigure, and Xcode's auto-signing heuristics
# can't interfere with the MAS provisioning-profile embedding.
SIGN_ARGS=(-DGIG_MACOS_ENABLE_CODE_SIGNING=OFF)

if [[ "$USE_XCODE" == "1" ]]; then
    echo "[*] Configuring (Xcode generator)..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Xcode "${CMAKE_ARGS[@]}" "${SIGN_ARGS[@]}"
    echo "[*] Building gig ($CONFIG)..."
    cmake --build "$BUILD_DIR" --config "$CONFIG"
else
    echo "[*] Configuring (Ninja generator)..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" "${CMAKE_ARGS[@]}" "${SIGN_ARGS[@]}"
    echo "[*] Building gig..."
    cmake --build "$BUILD_DIR"
fi

if [[ ! -d "$APP_PATH" ]]; then
    echo "[!] Fatal: Expected app bundle not found at $APP_PATH" >&2
    exit 1
fi

# The build tree may live on a network share (SMB from the Windows host in
# this setup). SMB doesn't honor POSIX chmod and silently drops in-place plist
# edits, so post-build surgery done on the share doesn't survive into the pkg
# (learned the hard way in ../clipp: the CFBundleVersion rewrite,
# world-readable perms, and signed entitlements all reverted). When signing
# for distribution, copy the bundle onto local disk and do everything there.
# The ad-hoc local-test path stays in place -- it just needs to run.
if [[ "$sign_for_distribution" == "1" ]]; then
    STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gig-mas.XXXXXX")"
    trap 'rm -rf "$STAGE_DIR"' EXIT
    echo "[*] Staging app bundle to local disk (build tree may be on a network share): $STAGE_DIR"
    ditto "$APP_PATH" "$STAGE_DIR/gig.app"
    APP_PATH="$STAGE_DIR/gig.app"
fi

INFO_PLIST="$APP_PATH/Contents/Info.plist"

# MAS rejects a 4-part CFBundleVersion (error 90257: "at most three
# non-negative integers"). Our W.X.Y.Z scheme carries the monotonic build
# counter in the 4th component, so collapse CFBundleVersion down to just that
# integer -- unique per upload and always increasing. CFBundleShortVersionString
# (user-visible W.X.Y) is left untouched.
FULL_BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$INFO_PLIST")"
MAS_BUNDLE_VERSION="${FULL_BUNDLE_VERSION##*.}"
if [[ "$MAS_BUNDLE_VERSION" != "$FULL_BUNDLE_VERSION" ]]; then
    echo "[*] Rewriting CFBundleVersion $FULL_BUNDLE_VERSION -> $MAS_BUNDLE_VERSION for MAS (<=3 integers)..."
    /usr/libexec/PlistBuddy -c "Set :CFBundleVersion $MAS_BUNDLE_VERSION" "$INFO_PLIST"
fi
echo "[*] CFBundleVersion in bundle is now: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$INFO_PLIST")"

if [[ "$sign_for_distribution" == "1" ]]; then
    echo "[*] Embedding provisioning profile from $PROVISION_PROFILE..."
    cp "$PROVISION_PROFILE" "$APP_PATH/Contents/embedded.provisionprofile"
fi

# codesign rejects a bundle carrying extended-attribute "detritus" (quarantine
# flags, Finder info, resource forks). The most common source here is
# com.apple.quarantine on the just-copied provisioning profile -- it was
# downloaded via a browser, which tags it. Stripping xattrs only removes
# metadata, not content, so the profile stays valid.
xattr -cr "$APP_PATH"

# productbuild preserves on-disk POSIX permissions. If any file in the bundle
# is not world-readable, App Store Connect rejects the pkg (error 90255,
# "files only readable by the root user"). +X adds execute only where it
# already exists (dirs, the binary).
chmod -R a+rX "$APP_PATH"

if [[ "$sign_for_distribution" == "1" ]]; then
    # App Store signatures must embed com.apple.application-identifier
    # (AppIDPrefix.BundleID) and com.apple.developer.team-identifier. Xcode
    # injects these from the provisioning profile automatically; since we sign
    # manually with a static entitlements file, we add them ourselves. Without
    # application-identifier the signature mismatches the embedded profile and
    # ASC flags the build ineligible (warning 90886 / not valid for TestFlight).
    BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$INFO_PLIST")"

    # application-identifier must match the profile byte-for-byte. Its prefix
    # is the App ID Prefix -- usually the Team ID, occasionally a legacy seed
    # ID -- so read the authoritative value out of the profile itself.
    # Profiles store the key with or without the com.apple. prefix depending
    # on vintage; try both, then fall back to TeamID.BundleID.
    PROFILE_DECODED="$STAGE_DIR/embedded.profile.plist"
    security cms -D -i "$PROVISION_PROFILE" -o "$PROFILE_DECODED" 2>/dev/null || true
    APP_IDENTIFIER="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.application-identifier' "$PROFILE_DECODED" 2>/dev/null || true)"
    if [[ -z "$APP_IDENTIFIER" ]]; then
        APP_IDENTIFIER="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:application-identifier' "$PROFILE_DECODED" 2>/dev/null || true)"
    fi
    if [[ -z "$APP_IDENTIFIER" ]]; then
        APP_IDENTIFIER="${TEAM_ID}.${BUNDLE_ID}"
    fi

    # A profile for the WRONG APP signs cleanly and only fails at upload time
    # (ITMS-90286, application-identifier mismatch -- seen live with clipp's
    # profile left in APPLE_MAS_PROVISIONING_PROFILE). The identifier must be
    # <AppIDPrefix>.<our bundle id>; catch anything else before signing.
    if [[ "$APP_IDENTIFIER" != *".${BUNDLE_ID}" ]]; then
        echo "[!] Fatal: provisioning profile is not for this app." >&2
        echo "    Profile's application-identifier: $APP_IDENTIFIER" >&2
        echo "    Expected suffix:                  .$BUNDLE_ID" >&2
        echo "    Check APPLE_MAS_PROVISIONING_PROFILE: $PROVISION_PROFILE" >&2
        exit 1
    fi

    EFFECTIVE_ENTITLEMENTS="$STAGE_DIR/gig.effective.entitlements"
    cp "$ENTITLEMENTS_FILE" "$EFFECTIVE_ENTITLEMENTS"
    /usr/libexec/PlistBuddy -c "Add :com.apple.application-identifier string ${APP_IDENTIFIER}" "$EFFECTIVE_ENTITLEMENTS"
    /usr/libexec/PlistBuddy -c "Add :com.apple.developer.team-identifier string ${TEAM_ID}" "$EFFECTIVE_ENTITLEMENTS"

    # gig.app has no nested frameworks (static link), so a plain sign of the
    # bundle covers the executable -- no --deep needed (matches build_macos.sh).
    echo "[*] Signing gig.app for App Store distribution ($IDENTITY)..."
    echo "    application-identifier: $APP_IDENTIFIER"
    codesign --force --options=runtime --timestamp \
        --entitlements "$EFFECTIVE_ENTITLEMENTS" \
        --sign "$IDENTITY" "$APP_PATH"
else
    # Ad-hoc sign with the sandbox entitlements. macOS only enforces the
    # sandbox against an entitlement-bearing *signed* bundle; without this
    # step the entitlements file would be inert and the app would run
    # unrestricted, defeating the point of a "test the sandbox" build.
    # (No --timestamp: ad-hoc signatures can't be timestamped.)
    echo "[*] Ad-hoc signing for local sandbox testing (no distribution identity used)..."
    codesign --force --options=runtime \
        --entitlements "$ENTITLEMENTS_FILE" \
        --sign - "$APP_PATH"
fi

echo "[*] Verifying signature..."
codesign --verify --strict --verbose=2 "$APP_PATH"

# Confirm the entitlements actually landed in the signed bundle. Cheaper to
# fail here than at App Store Connect upload time. Capture + here-string match
# (not a pipe into grep -q) so a SIGPIPE to codesign can't skew the result.
SIGNED_ENTITLEMENTS="$(codesign --display --entitlements :- "$APP_PATH" 2>/dev/null || true)"
if ! grep -q "com.apple.security.app-sandbox" <<<"$SIGNED_ENTITLEMENTS"; then
    echo "[!] Fatal: app-sandbox entitlement missing from signed bundle." >&2
    exit 1
fi
if [[ "$sign_for_distribution" == "1" ]]; then
    # application-identifier must be present (and match the embedded profile)
    # or the upload is flagged ineligible (90886). Catch it here, not at
    # upload time.
    if ! grep -q "com.apple.application-identifier" <<<"$SIGNED_ENTITLEMENTS"; then
        echo "[!] Fatal: com.apple.application-identifier missing from signed bundle." >&2
        exit 1
    fi
    # MAS doesn't strictly require the hardened runtime, but Apple's review
    # pipeline complains when it's missing.
    CODESIGN_INFO="$(codesign --display --verbose=4 "$APP_PATH" 2>&1 || true)"
    if ! grep -Eq 'flags=[^[:space:]]*runtime' <<<"$CODESIGN_INFO"; then
        echo "[!] Warning: hardened runtime flag not present on $APP_PATH." >&2
    fi
fi

if [[ "$package" == "1" ]]; then
    PKG_PATH="$BUILD_DIR/gig.pkg"
    echo "[*] Wrapping signed app in .pkg via productbuild: $PKG_PATH"
    rm -f "$PKG_PATH"
    productbuild --component "$APP_PATH" /Applications \
        --sign "$INSTALLER_IDENTITY" \
        "$PKG_PATH"

    echo "[*] Verifying .pkg signature..."
    pkgutil --check-signature "$PKG_PATH"
fi

if [[ "$upload" == "1" ]]; then
    # altool's --apiKey expects the 10-char key id and looks for the matching
    # .p8 in ~/.appstoreconnect/private_keys/AuthKey_<id>.p8. Stage the
    # user-supplied .p8 path into that location if it isn't already there.
    KEYS_DIR="$HOME/.appstoreconnect/private_keys"
    EXPECTED_KEY="$KEYS_DIR/AuthKey_${APPLE_API_KEY_ID}.p8"
    if [[ ! -f "$EXPECTED_KEY" ]]; then
        mkdir -p "$KEYS_DIR"
        cp "$APPLE_API_KEY_PATH" "$EXPECTED_KEY"
        echo "[*] Staged API key at $EXPECTED_KEY"
    fi

    UPLOAD_LOG="$BUILD_DIR/altool-upload.log"
    echo "[*] Uploading $PKG_PATH to App Store Connect (may take several minutes)..."
    set +e
    xcrun altool --upload-app -f "$PKG_PATH" -t macos \
        --apiKey "$APPLE_API_KEY_ID" \
        --apiIssuer "$APPLE_API_ISSUER_ID" 2>&1 | tee "$UPLOAD_LOG"
    UPLOAD_STATUS=${PIPESTATUS[0]}
    set -e

    if [[ "$UPLOAD_STATUS" -ne 0 ]]; then
        echo "[!] Fatal: altool upload returned status $UPLOAD_STATUS. See $UPLOAD_LOG." >&2
        exit 1
    fi
    echo "[*] Upload complete. Check App Store Connect for processing status."
fi

echo "[*] Build complete: $APP_PATH"
