#!/bin/bash
# Generate a macOS .icns from a square PNG master (>= 1024x1024) using the
# preinstalled sips + iconutil. Usage: make_icns.sh <source.png> <out.icns>
set -e

SRC="$1"
OUT="$2"
[[ -f "$SRC" ]] || { echo "[!] make_icns: source PNG not found: $SRC" >&2; exit 1; }

WORK="$(mktemp -d)"
ICONSET="$WORK/gig.iconset"
mkdir -p "$ICONSET"
trap 'rm -rf "$WORK"' EXIT

# Apple's expected iconset sizes (point size + @2x retina variant).
sips -z 16   16   "$SRC" --out "$ICONSET/icon_16x16.png"      >/dev/null
sips -z 32   32   "$SRC" --out "$ICONSET/icon_16x16@2x.png"   >/dev/null
sips -z 32   32   "$SRC" --out "$ICONSET/icon_32x32.png"      >/dev/null
sips -z 64   64   "$SRC" --out "$ICONSET/icon_32x32@2x.png"   >/dev/null
sips -z 128  128  "$SRC" --out "$ICONSET/icon_128x128.png"    >/dev/null
sips -z 256  256  "$SRC" --out "$ICONSET/icon_128x128@2x.png" >/dev/null
sips -z 256  256  "$SRC" --out "$ICONSET/icon_256x256.png"    >/dev/null
sips -z 512  512  "$SRC" --out "$ICONSET/icon_256x256@2x.png" >/dev/null
sips -z 512  512  "$SRC" --out "$ICONSET/icon_512x512.png"    >/dev/null
sips -z 1024 1024 "$SRC" --out "$ICONSET/icon_512x512@2x.png" >/dev/null

iconutil -c icns "$ICONSET" -o "$OUT"
echo "[*] make_icns: wrote $OUT"
