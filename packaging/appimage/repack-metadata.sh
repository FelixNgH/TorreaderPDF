#!/usr/bin/env bash
set -euo pipefail

SRC="${1:?Usage: $0 <source-AppImage> <dest-AppImage>}"
DST="${2:?Usage: $0 <source-AppImage> <dest-AppImage>}"
APPIMAGETOOL="${APPIMAGETOOL:-$HOME/appimagetool.AppImage}"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

cp "$SRC" "$TMPDIR/input.AppImage"
chmod +x "$TMPDIR/input.AppImage"

(cd "$TMPDIR" && ./input.AppImage --appimage-extract)

APPDIR="$TMPDIR/squashfs-root"

# Record checksums before any change
BIN_SHA256=$(sha256sum "$APPDIR/usr/bin/TorReader" | cut -d' ' -f1)

mapfile -t LIB_FILES < <(find "$APPDIR/usr/lib" -type f | sort)
declare -A LIB_SHA256
for f in "${LIB_FILES[@]}"; do
  LIB_SHA256["$f"]=$(sha256sum "$f" | cut -d' ' -f1)
done

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DESKTOP_SRC="$SCRIPT_DIR/linux/cloud.torreader.TorReader.desktop"
METAINFO_SRC="$SCRIPT_DIR/linux/cloud.torreader.TorReader.metainfo.xml"

# Replace desktop file
rm -f "$APPDIR/usr/share/applications/torreader.desktop"
cp "$DESKTOP_SRC" "$APPDIR/usr/share/applications/cloud.torreader.TorReader.desktop"

# Rename icon
if [ -f "$APPDIR/usr/share/icons/hicolor/256x256/apps/torreader.png" ]; then
  mv "$APPDIR/usr/share/icons/hicolor/256x256/apps/torreader.png" \
     "$APPDIR/usr/share/icons/hicolor/256x256/apps/cloud.torreader.TorReader.png"
fi

# Install metainfo
mkdir -p "$APPDIR/usr/share/metainfo"
cp "$METAINFO_SRC" "$APPDIR/usr/share/metainfo/cloud.torreader.TorReader.metainfo.xml"

# Rebuild root symlinks
rm -f "$APPDIR/torreader.desktop" "$APPDIR/torreader.png"
ln -sf usr/bin/TorReader         "$APPDIR/AppRun"
ln -sf usr/share/applications/cloud.torreader.TorReader.desktop "$APPDIR/cloud.torreader.TorReader.desktop"
ln -sf usr/share/icons/hicolor/256x256/apps/cloud.torreader.TorReader.png "$APPDIR/cloud.torreader.TorReader.png"
ln -sf cloud.torreader.TorReader.png "$APPDIR/.DirIcon"

# Verify: binary unchanged
NEW_BIN_SHA256=$(sha256sum "$APPDIR/usr/bin/TorReader" | cut -d' ' -f1)
if [ "$BIN_SHA256" != "$NEW_BIN_SHA256" ]; then
  echo "FATAL: usr/bin/TorReader checksum changed! Was $BIN_SHA256, now $NEW_BIN_SHA256" >&2
  exit 1
fi

# Verify: lib files unchanged
for f in "${LIB_FILES[@]}"; do
  if [ ! -f "$f" ]; then
    echo "FATAL: $f went missing" >&2
    exit 1
  fi
  NEW_LIB_SHA256=$(sha256sum "$f" | cut -d' ' -f1)
  if [ "${LIB_SHA256[$f]}" != "$NEW_LIB_SHA256" ]; then
    echo "FATAL: $f checksum changed!" >&2
    exit 1
  fi
done

# Verify: AppRun points to usr/bin/TorReader
APP_RUN_TARGET=$(readlink "$APPDIR/AppRun")
if [ "$APP_RUN_TARGET" != "usr/bin/TorReader" ]; then
  echo "FATAL: AppRun symlink target is '$APP_RUN_TARGET', expected 'usr/bin/TorReader'" >&2
  exit 1
fi

# Verify: .DirIcon resolves to a real PNG file
if ! file "$(readlink -f "$APPDIR/.DirIcon")" | grep -qi png; then
  echo "FATAL: .DirIcon does not resolve to a PNG file" >&2
  exit 1
fi

# Repack
mkdir -p "$(dirname "$DST")"
ARCH=x86_64 "$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$DST"
chmod +x "$DST"

SRC_SIZE=$(stat -c %s "$SRC")
DST_SIZE=$(stat -c %s "$DST")

echo "=== repack-metadata summary ==="
echo "Source: $SRC ($SRC_SIZE bytes)"
echo "Dest:   $DST ($DST_SIZE bytes)"
echo "Binary sha256: $BIN_SHA256 (unchanged)"

# Static smoke test (GUI smoke test is in verify-in-docker.sh)
echo "--- Static smoke test ---"

VERIFY_TMP=$(mktemp -d)
trap 'rm -rf "$TMPDIR" "$VERIFY_TMP"' EXIT

cp "$DST" "$VERIFY_TMP/output.AppImage"
chmod +x "$VERIFY_TMP/output.AppImage"
(cd "$VERIFY_TMP" && ./output.AppImage --appimage-extract)

VDIR="$VERIFY_TMP/squashfs-root"

# 1. Binary sha256 matches original
VERIFY_BIN=$(sha256sum "$VDIR/usr/bin/TorReader" | cut -d' ' -f1)
if [ "$BIN_SHA256" != "$VERIFY_BIN" ]; then
  echo "FAIL: binary sha256 mismatch $VERIFY_BIN vs original $BIN_SHA256" >&2
  exit 1
fi
echo "  binary sha256: OK (unchanged)"

# 2. 4 root symlinks resolve to real files
for sym in AppRun cloud.torreader.TorReader.desktop cloud.torreader.TorReader.png .DirIcon; do
  if [ ! -e "$VDIR/$sym" ]; then
    echo "FAIL: symlink $sym does not resolve to real file" >&2
    exit 1
  fi
done
echo "  root symlinks: OK (all resolve)"

# 3. Required metadata files exist with correct names
for f in \
  usr/share/applications/cloud.torreader.TorReader.desktop \
  usr/share/metainfo/cloud.torreader.TorReader.metainfo.xml \
  usr/share/icons/hicolor/256x256/apps/cloud.torreader.TorReader.png
do
  if [ ! -f "$VDIR/$f" ]; then
    echo "FAIL: missing $f" >&2
    exit 1
  fi
done
echo "  metadata files: OK (all present)"

# 4. ldd check — no "not found" with bundled libs
LDD_OUT=$(LD_LIBRARY_PATH="$VDIR/usr/lib" ldd "$VDIR/usr/bin/TorReader" 2>&1 || true)
NOT_FOUND=$(echo "$LDD_OUT" | grep -i "not found" || true)
if [ -n "$NOT_FOUND" ]; then
  echo "FAIL: unresolved library dependencies:" >&2
  echo "$NOT_FOUND" >&2
  exit 1
fi
echo "  ldd check:    OK (no unresolved deps)"

echo "Smoke test (static): PASS
Note: GUI smoke test requires X display — run verify-in-docker.sh instead"
