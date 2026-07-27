#!/usr/bin/env bash
set -euo pipefail

# Note: expects an AppImage that has already been repacked by
# packaging/appimage/repack-metadata.sh (cloud.torreader.TorReader metadata).

APPIMAGE="${1:?Usage: $0 <path-to-AppImage> [version]}"
VERSION="${2:-2.2.4}"
PKGNAME="torreader"
ARCH="amd64"
DEB="${PKGNAME}_${VERSION}_${ARCH}.deb"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Extract AppImage (--appimage-extract has no --dest, must cd first)
cp "$APPIMAGE" "$TMPDIR/input.AppImage"
chmod +x "$TMPDIR/input.AppImage"
(cd "$TMPDIR" && ./input.AppImage --appimage-extract)

# Layout package
mkdir -p "$TMPDIR/deb/opt/torreader"
cp -r "$TMPDIR/squashfs-root/"* "$TMPDIR/deb/opt/torreader/"

mkdir -p "$TMPDIR/deb/usr/bin"
cat > "$TMPDIR/deb/usr/bin/torreader" <<'WRAPPER'
#!/bin/sh
exec /opt/torreader/usr/bin/TorReader "$@"
WRAPPER
chmod 755 "$TMPDIR/deb/usr/bin/torreader"

mkdir -p "$TMPDIR/deb/DEBIAN"

INSTALLED_SIZE=$(du -sk "$TMPDIR/deb/opt" | cut -f1)

cat > "$TMPDIR/deb/DEBIAN/control" <<CONTROL
Package: $PKGNAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: FelixNgH <felixnguyenhuy@gmail.com>
Depends: libc6 (>= 2.35), libglib2.0-0 | libglib2.0-0t64,
 libegl1, libgl1, libglx0, libopengl0,
 libice6, libsm6,
 libx11-6, libx11-xcb1, libxcb1,
 libfontconfig1, libfreetype6, libharfbuzz0b
Homepage: https://torreader.cloud
Installed-Size: $INSTALLED_SIZE
Description: Fast offline PDF reader, editor and merger
 TorReader PDF is a fast, offline, portable PDF reader and
 editor for large documents. Open 200 MB files in 1-2 seconds,
 scroll continuously, and view crisp text at any zoom level.
 .
 Features:
  * Merge PDFs with bookmark remapping
  * Split by page count or file size
  * Extract or insert pages from other files
  * Digital signature (PKCS#7 detached, SHA-256)
  * Free in-app text translation
  * Annotations: freehand, highlight, line/arrow, text, note, rect
  * Full-text search
  * Reorder / delete pages, dark mode
  * 100% offline, local - no account, no ads, no watermark
CONTROL

cat > "$TMPDIR/deb/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f /usr/share/icons/hicolor || true
fi
POSTINST
chmod 755 "$TMPDIR/deb/DEBIAN/postinst"

cat > "$TMPDIR/deb/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f /usr/share/icons/hicolor || true
fi
POSTRM
chmod 755 "$TMPDIR/deb/DEBIAN/postrm"

# Install desktop, metainfo, icon, license
mkdir -p "$TMPDIR/deb/usr/share/applications"
cp "$TMPDIR/squashfs-root/usr/share/applications/cloud.torreader.TorReader.desktop" \
  "$TMPDIR/deb/usr/share/applications/"

mkdir -p "$TMPDIR/deb/usr/share/metainfo"
cp "$TMPDIR/squashfs-root/usr/share/metainfo/cloud.torreader.TorReader.metainfo.xml" \
  "$TMPDIR/deb/usr/share/metainfo/"

mkdir -p "$TMPDIR/deb/usr/share/icons/hicolor/256x256/apps"
cp "$TMPDIR/squashfs-root/usr/share/icons/hicolor/256x256/apps/cloud.torreader.TorReader.png" \
  "$TMPDIR/deb/usr/share/icons/hicolor/256x256/apps/"

# Debian standard: copyright at /usr/share/doc/<package>/copyright
mkdir -p "$TMPDIR/deb/usr/share/doc/$PKGNAME"
cp "$(dirname "$0")/../../LICENSE" \
  "$TMPDIR/deb/usr/share/doc/$PKGNAME/copyright"

dpkg-deb --build --root-owner-group "$TMPDIR/deb" "$DEB"

echo "=== Package info ==="
dpkg-deb --info "$DEB"
echo "=== Contents (top) ==="
dpkg-deb --contents "$DEB" | head -n 15 || true

SIZE=$(du -h "$DEB" | cut -f1)
echo "Built: $DEB ($SIZE)"
echo "Run lintian (if available): lintian $DEB"
