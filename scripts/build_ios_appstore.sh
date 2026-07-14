#!/bin/bash
set -euo pipefail

# Builds, archives, signs, and (optionally) uploads the gig iOS app to App
# Store Connect -- fully headless, no Xcode GUI, no Xcode Apple-ID session.
#
# Signing model (deliberate, learned the hard way):
#   - The ARCHIVE signs automatically with the local development identity
#     (pbxproj is CODE_SIGN_STYLE=Automatic with the team baked in); the API
#     key only lets xcodebuild refresh dev profiles, which any key role may.
#   - The EXPORT re-signs for distribution MANUALLY: the Apple Distribution
#     identity in the login keychain + the iOS App Store provisioning profile
#     pointed at by APPLE_IOS_GIG_PROVISIONING_PROFILE (app-scoped env name,
#     same convention as APPLE_MAS_GIG_PROVISIONING_PROFILE). Cloud signing
#     is NOT used here: minting profiles on the fly needs an Admin-role API
#     key ("Cloud signing permission error"), which we deliberately avoid.
#   - The API key trio authenticates the UPLOAD only (any role works).
#
# Version: gig's version is tag-canonical everywhere (no in-tree stamp);
# --version W.X.Y.Z is REQUIRED and should match the release tag. It is
# applied at archive time (MARKETING_VERSION = W.X.Y, CURRENT_PROJECT_VERSION
# = W.X.Y.Z); the placeholder values in the pbxproj never ship.
#
# First-ever upload note: App Store Connect must already have an app record
# for the bundle id (stream.gig.app) or the upload is rejected -- create it
# once at appstoreconnect.apple.com (My Apps -> +).

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

PROJECT="ios/gig.xcodeproj"
SCHEME="gig"
CONFIG="Release"
BUILD_DIR="$REPO_ROOT/build/ios-appstore"
ARCHIVE_PATH="$BUILD_DIR/gig.xcarchive"
EXPORT_PATH="$BUILD_DIR/export"
VERSION=""
SETUP_VCPKG=1
clean=0
upload=0

# App-scoped profile path (see build_macos_mas.sh for the rationale: a shared
# env name once shipped clipp's profile inside gig, ITMS-90286).
PROVISION_PROFILE="${APPLE_IOS_GIG_PROVISIONING_PROFILE:-}"
# The distribution identity to sign with. Override only if your certificate
# predates the unified naming (e.g. "iPhone Distribution: ...").
SIGNING_IDENTITY="${APPLE_CODESIGN_IDENTITY_IOS:-Apple Distribution}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --version W.X.Y.Z [--clean] [--skip-vcpkg] [--upload]

Archives the gig iOS app for the App Store and exports a signed .ipa
(or uploads it with --upload). Always Release configuration.

Requires in env:
  APPLE_IOS_GIG_PROVISIONING_PROFILE  (path to the iOS App Store .mobileprovision)
  APPLE_API_KEY_PATH                  (path to AuthKey_XXXX.p8)
  APPLE_API_KEY_ID                    (10-char key id)
  APPLE_API_ISSUER_ID                 (issuer UUID)
Optional:
  APPLE_CODESIGN_IDENTITY_IOS         (default "Apple Distribution")

Also requires an Apple Distribution certificate + private key in the login
keychain (check: security find-identity -v -p codesigning).

Options:
  --version VER    REQUIRED. Stamp this build with VER (W.X.Y.Z, should match
                   the release tag).
  --clean          Remove $BUILD_DIR first.
  --skip-vcpkg     Do not run setup_ios_vcpkg.sh first.
  --upload         Upload to App Store Connect instead of exporting the .ipa.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) clean=1; shift ;;
        --skip-vcpkg) SETUP_VCPKG=0; shift ;;
        --upload) upload=1; shift ;;
        --version)
            [[ -z "${2:-}" ]] && { echo "[!] --version requires a value" >&2; exit 2; }
            VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "[!] Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# Fail fast before burning the dependency build. Every invocation produces a
# store artifact, so a real version is mandatory -- the pbxproj placeholders
# (0.1.0 / 1) must never ship.
if [[ -z "$VERSION" ]]; then
    echo "[!] Fatal: --version W.X.Y.Z is required (should match the release tag)." >&2
    usage >&2
    exit 2
fi
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[!] Fatal: --version must be W.X.Y.Z (4 dot-separated non-negative integers); got '$VERSION'" >&2
    exit 2
fi
if [[ -z "$PROVISION_PROFILE" ]]; then
    echo "[!] Fatal: APPLE_IOS_GIG_PROVISIONING_PROFILE is required (path to the iOS App Store .mobileprovision)." >&2
    exit 2
fi
if [[ ! -f "$PROVISION_PROFILE" ]]; then
    echo "[!] Fatal: APPLE_IOS_GIG_PROVISIONING_PROFILE does not point at a readable file: $PROVISION_PROFILE" >&2
    exit 2
fi
: "${APPLE_API_KEY_PATH:?requires APPLE_API_KEY_PATH (path to .p8)}"
: "${APPLE_API_KEY_ID:?requires APPLE_API_KEY_ID}"
: "${APPLE_API_ISSUER_ID:?requires APPLE_API_ISSUER_ID}"
if [[ ! -f "$APPLE_API_KEY_PATH" ]]; then
    echo "[!] Fatal: APPLE_API_KEY_PATH does not point at a readable file: $APPLE_API_KEY_PATH" >&2
    exit 2
fi
# xcodebuild resolves the key path relative to its own cwd quirks; absolutize.
APPLE_API_KEY_PATH="$(cd -- "$(dirname -- "$APPLE_API_KEY_PATH")" && pwd)/$(basename -- "$APPLE_API_KEY_PATH")"

# Manual export signing needs the distribution identity's PRIVATE KEY in the
# keychain -- a portal-listed cert alone is not enough. Catch it now.
if ! security find-identity -v -p codesigning 2>/dev/null | grep -qF "$SIGNING_IDENTITY"; then
    echo "[!] Fatal: no '$SIGNING_IDENTITY' signing identity in the keychain." >&2
    echo "    Create one: Keychain Access -> Certificate Assistant -> Request a" >&2
    echo "    Certificate From a Certificate Authority (save to disk), upload the" >&2
    echo "    CSR at developer.apple.com -> Certificates -> + -> Apple Distribution," >&2
    echo "    download the .cer and double-click it. Or set APPLE_CODESIGN_IDENTITY_IOS" >&2
    echo "    if your identity is named differently (security find-identity -v -p codesigning)." >&2
    exit 2
fi

need_tool() {
    command -v "$1" >/dev/null 2>&1 || { echo "[!] Fatal: $1 is required but was not found in PATH." >&2; exit 1; }
}
need_tool xcodebuild
need_tool xcrun
xcrun --sdk iphoneos --show-sdk-path >/dev/null

if [[ "$clean" == "1" ]]; then
    echo "[*] Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [[ "$SETUP_VCPKG" == "1" ]]; then
    # Device (arm64-ios) static libs. MUST run from a terminal, never as an
    # Xcode build phase (Xcode's env poisons vcpkg) -- which this is.
    echo "[*] Setting up iOS device vcpkg dependencies..."
    "$SCRIPT_DIR/setup_ios_vcpkg.sh" --device-only
fi

mkdir -p "$BUILD_DIR"

# Decode the profile and pull out what the export needs. Done BEFORE the
# archive so a wrong-app profile dies in seconds, not after a full build.
PROFILE_DECODED="$BUILD_DIR/profile.plist"
security cms -D -i "$PROVISION_PROFILE" -o "$PROFILE_DECODED"
PROFILE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :Name' "$PROFILE_DECODED")"
PROFILE_UUID="$(/usr/libexec/PlistBuddy -c 'Print :UUID' "$PROFILE_DECODED")"
PROFILE_APP_ID="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:application-identifier' "$PROFILE_DECODED" 2>/dev/null || true)"
if [[ -z "$PROFILE_APP_ID" ]]; then
    PROFILE_APP_ID="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.application-identifier' "$PROFILE_DECODED" 2>/dev/null || true)"
fi
BUNDLE_ID="stream.gig.app"
if [[ "$PROFILE_APP_ID" != *".${BUNDLE_ID}" ]]; then
    echo "[!] Fatal: provisioning profile is not for this app (see ITMS-90286 in the MAS flow)." >&2
    echo "    Profile's application-identifier: $PROFILE_APP_ID" >&2
    echo "    Expected suffix:                  .$BUNDLE_ID" >&2
    echo "    Check APPLE_IOS_GIG_PROVISIONING_PROFILE: $PROVISION_PROFILE" >&2
    exit 1
fi
echo "[*] Distribution profile: '$PROFILE_NAME' ($PROFILE_UUID)"

# Install the profile where xcodebuild's manual signing looks for it (the
# classic location plus the Xcode 16+ one; filename convention is the UUID).
for dir in \
    "$HOME/Library/MobileDevice/Provisioning Profiles" \
    "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
do
    mkdir -p "$dir"
    cp "$PROVISION_PROFILE" "$dir/$PROFILE_UUID.mobileprovision"
done

# Upload-only auth (any API-key role). The archive step additionally gets
# -allowProvisioningUpdates so automatic DEV signing can refresh profiles;
# the export step deliberately does NOT, so cloud signing is never attempted.
AUTH_ARGS=(
    -authenticationKeyPath "$APPLE_API_KEY_PATH"
    -authenticationKeyID "$APPLE_API_KEY_ID"
    -authenticationKeyIssuerID "$APPLE_API_ISSUER_ID"
)

VERSION_ARGS=(
    MARKETING_VERSION="${VERSION%.*}"
    CURRENT_PROJECT_VERSION="$VERSION"
)
echo "[*] Version: MARKETING_VERSION=${VERSION%.*} CURRENT_PROJECT_VERSION=$VERSION"

echo "[*] Archiving ($CONFIG, generic iOS device)..."
rm -rf "$ARCHIVE_PATH"
xcodebuild archive \
    -project "$PROJECT" \
    -scheme "$SCHEME" \
    -configuration "$CONFIG" \
    -destination 'generic/platform=iOS' \
    -archivePath "$ARCHIVE_PATH" \
    -derivedDataPath "$BUILD_DIR/DerivedData" \
    -allowProvisioningUpdates \
    "${AUTH_ARGS[@]}" \
    "${VERSION_ARGS[@]}"

if [[ ! -d "$ARCHIVE_PATH" ]]; then
    echo "[!] Fatal: archive not found at $ARCHIVE_PATH" >&2
    exit 1
fi

ARCHIVED_APP="$ARCHIVE_PATH/Products/Applications/gig.app"

# The bundle id assumption above must hold for the archived app too.
ARCHIVED_BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$ARCHIVED_APP/Info.plist")"
if [[ "$ARCHIVED_BUNDLE_ID" != "$BUNDLE_ID" ]]; then
    echo "[!] Fatal: archived app bundle id '$ARCHIVED_BUNDLE_ID' != expected '$BUNDLE_ID'." >&2
    exit 1
fi

# Belt-and-braces: the privacy manifest must sit at the app bundle root or
# App Store validation rejects the build; cheaper to fail here.
if [[ ! -f "$ARCHIVED_APP/PrivacyInfo.xcprivacy" ]]; then
    echo "[!] Fatal: PrivacyInfo.xcprivacy missing from the archived app bundle root." >&2
    echo "    Expected: $ARCHIVED_APP/PrivacyInfo.xcprivacy" >&2
    exit 1
fi

if [[ "$upload" == "1" ]]; then
    DESTINATION="upload"
else
    DESTINATION="export"
fi

EXPORT_OPTIONS="$BUILD_DIR/exportOptions.plist"
cat > "$EXPORT_OPTIONS" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key>
    <string>app-store-connect</string>
    <key>destination</key>
    <string>$DESTINATION</string>
    <key>signingStyle</key>
    <string>manual</string>
    <key>signingCertificate</key>
    <string>$SIGNING_IDENTITY</string>
    <key>provisioningProfiles</key>
    <dict>
        <key>$BUNDLE_ID</key>
        <string>$PROFILE_NAME</string>
    </dict>
    <key>uploadSymbols</key>
    <true/>
</dict>
</plist>
EOF

if [[ "$upload" == "1" ]]; then
    echo "[*] Exporting + uploading to App Store Connect (may take several minutes)..."
else
    echo "[*] Exporting signed .ipa to $EXPORT_PATH ..."
fi
rm -rf "$EXPORT_PATH"
xcodebuild -exportArchive \
    -archivePath "$ARCHIVE_PATH" \
    -exportOptionsPlist "$EXPORT_OPTIONS" \
    -exportPath "$EXPORT_PATH" \
    "${AUTH_ARGS[@]}"

if [[ "$upload" == "1" ]]; then
    echo "[*] Upload complete. Check App Store Connect / TestFlight for processing status."
else
    echo "[*] Export complete:"
    ls -la "$EXPORT_PATH"
    echo "[*] Ship it later with: $(basename "$0") --version $VERSION --skip-vcpkg --upload   (rebuilds; or upload this .ipa via Transporter)"
fi
echo "[*] Archive kept at: $ARCHIVE_PATH"
