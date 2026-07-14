#!/bin/bash
# Bumps the project version everywhere it is stored in the tree.
#
# gig's canonical version is the git TAG: the release pipeline derives
# -DGIG_VERSION from it and local desktop builds default to 0.0.0.0, so the
# CMake side needs no stamping. The one place a version is baked into the repo
# is the iOS Xcode project -- MARKETING_VERSION (CFBundleShortVersionString)
# and CURRENT_PROJECT_VERSION (CFBundleVersion) in project.pbxproj -- and a
# manual Product -> Archive ships whatever is written there. This script
# rewrites those.
#
# MARKETING_VERSION gets the 3-part W.X.Y (Apple rejects a 4-part value
# there); CURRENT_PROJECT_VERSION gets the full 4-part W.X.Y.Z (mirrors
# ../clipp's shipped convention -- the 4th component is the monotonic build
# counter, and it must increase between TestFlight uploads of the same W.X.Y).
#
# The values live in the pbxproj itself (Info.plist just references the build
# settings), so plain sed suffices and this runs on any OS, not just macOS.
# The script does NOT commit or tag for you -- review with `git diff` first.

set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $(basename "$0") W.X.Y.Z

Bumps the project version. Touches:
  ios/gig.xcodeproj/project.pbxproj   (MARKETING_VERSION + CURRENT_PROJECT_VERSION,
                                       every build configuration)

The desktop platforms take their version from the git tag at release time and
need no stamping.
EOF
}

if [[ $# -ne 1 ]]; then
    usage
    exit 2
fi

if ! [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[!] Version must be W.X.Y.Z (4 dot-separated non-negative integers); got '$1'" >&2
    exit 2
fi

VERSION="$1"
VERSION_3PART="${VERSION%.*}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

PBXPROJ="ios/gig.xcodeproj/project.pbxproj"
if [[ ! -f "$PBXPROJ" ]]; then
    echo "[!] Missing expected file: $PBXPROJ" >&2
    exit 1
fi

# Count what we're about to replace so we can verify the sed landed everywhere.
# A future target (tvOS, extension) in the same project adds occurrences; that
# is fine -- all of them should carry the same version.
# Patterns are anchored to line start so a future setting whose name merely
# ends in the same suffix (FOO_MARKETING_VERSION = ...) can't be caught.
marketing_before="$(grep -cE '^[[:space:]]*MARKETING_VERSION = [^;]*;' "$PBXPROJ" || true)"
project_before="$(grep -cE '^[[:space:]]*CURRENT_PROJECT_VERSION = [^;]*;' "$PBXPROJ" || true)"
if [[ "$marketing_before" -eq 0 || "$project_before" -eq 0 ]]; then
    echo "[!] $PBXPROJ has no MARKETING_VERSION/CURRENT_PROJECT_VERSION lines." >&2
    echo "    Has the project's versioning setup been restructured?" >&2
    exit 1
fi

echo "[*] Bumping to $VERSION (MARKETING_VERSION uses 3-part: $VERSION_3PART)"

# BSD sed (macOS) requires an extension after -i; use .bak and remove it.
sed -i.bak -E \
    -e "s|^([[:space:]]*)MARKETING_VERSION = [^;]*;|\1MARKETING_VERSION = $VERSION_3PART;|" \
    -e "s|^([[:space:]]*)CURRENT_PROJECT_VERSION = [^;]*;|\1CURRENT_PROJECT_VERSION = $VERSION;|" \
    "$PBXPROJ"
rm -f "$PBXPROJ.bak"

marketing_after="$(grep -cF "MARKETING_VERSION = $VERSION_3PART;" "$PBXPROJ" || true)"
project_after="$(grep -cF "CURRENT_PROJECT_VERSION = $VERSION;" "$PBXPROJ" || true)"
if [[ "$marketing_after" -ne "$marketing_before" || "$project_after" -ne "$project_before" ]]; then
    echo "[!] sed did not produce the expected lines in $PBXPROJ" >&2
    echo "    (MARKETING_VERSION: $marketing_after of $marketing_before;" >&2
    echo "     CURRENT_PROJECT_VERSION: $project_after of $project_before)" >&2
    exit 1
fi
echo "  [+] $PBXPROJ (MARKETING_VERSION x$marketing_after, CURRENT_PROJECT_VERSION x$project_after)"

cat <<EOF

[*] Done. Review with:
      git diff

[*] Then commit + tag:
      git commit -am "Bump version to $VERSION"
      git tag v$VERSION
      git push && git push --tags   # tag push runs the desktop release pipeline

[*] iOS ships from Xcode (Product -> Archive); the Mac App Store build is
      ./scripts/build_macos_mas.sh --version $VERSION --upload
EOF
