#!/usr/bin/env bash
# Produce the Linux release artifacts for SvnSyncDrive from an existing
# cmake --build in build/:
#   build/publish/svnsyncdrive-<version>-linux64.tar.gz   (self-contained bundle)
#   build/publish/svnsyncdrive_<version>_amd64.deb        (Debian package)
#
# Run from the repository root after building against the SVN stack to bundle
# (point LIBSVNPLUS_ROOT at the libsvnplus stage that sits on top of that
# stack, e.g. a custom-built Subversion prefix):
#   cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DLIBSVNPLUS_ROOT=/path/to/svn-prefix
#   cmake --build build
#   bash scripts/publish-linux.sh
# The SVN stack shared libraries are bundled into both artifacts, so the app
# always runs with the exact libsvn it was built against.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
OUT="$ROOT/build/publish"

VERSION="$(sed -n 's/^project(SvnSyncDrive VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
ARCH="amd64"
PKG_NAME="svnsyncdrive-$VERSION-linux64"
DEB_NAME="svnsyncdrive_${VERSION}_${ARCH}"

BIN="$BUILD/svnsyncdrive"
if [ ! -x "$BIN" ]; then
    echo "Binary not found at $BIN - run cmake --build first." >&2
    exit 1
fi

QTPLUGINS="/usr/lib/x86_64-linux-gnu/qt6/plugins"
if [ ! -d "$QTPLUGINS" ]; then
    echo "Qt plugin dir not found: $QTPLUGINS" >&2
    exit 1
fi

mkdir -p "$OUT"

# --- self-contained bundle ----------------------------------------------------
GREEN="$BUILD/green"
rm -rf "$GREEN"
mkdir -p "$GREEN/$PKG_NAME/lib" "$GREEN/$PKG_NAME/plugins"

cp "$BIN" "$GREEN/$PKG_NAME/svnsyncdrive.bin"

# Iteratively collect every shared library the app and its Qt plugins need,
# so the bundle stays self-contained (keep glibc itself on the host).
collect_libs() {
    ldd "$@" 2>/dev/null | awk '$3 ~ /^\// {print $3}' | sort -u
}
for i in 1 2 3 4 5; do
    BEFORE="$(ls "$GREEN/$PKG_NAME/lib" | sort | md5sum)"
    (echo "$BIN"; find "$GREEN/$PKG_NAME/lib" "$GREEN/$PKG_NAME/plugins" \
        -name '*.so*' -type f) | while read -r f; do
        collect_libs "$f" | while read -r lib; do
            case "$lib" in
                */libc.so.6|*/libm.so.6|*/libdl.so.2|*/libpthread.so.0|*/librt.so.1|*/ld-linux*.so.2) ;;
                *) cp -L "$lib" "$GREEN/$PKG_NAME/lib/" 2>/dev/null || true;;
            esac
        done
    done
    AFTER="$(ls "$GREEN/$PKG_NAME/lib" | sort | md5sum)"
    [ "$BEFORE" = "$AFTER" ] && break
done

cp -r "$QTPLUGINS/." "$GREEN/$PKG_NAME/plugins/"
cp "$ROOT/src/resources/icon_256.png" "$GREEN/$PKG_NAME/svnsyncdrive.png"

cat > "$GREEN/$PKG_NAME/svnsyncdrive.sh" <<EOF
#!/usr/bin/env sh
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
export LD_LIBRARY_PATH="\$DIR/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="\$DIR/plugins"
exec "\$DIR/svnsyncdrive.bin" "\$@"
EOF
chmod +x "$GREEN/$PKG_NAME/svnsyncdrive.sh"

TGZ="$OUT/$PKG_NAME.tar.gz"
rm -f "$TGZ"
tar -C "$GREEN" -czf "$TGZ" "$PKG_NAME"
echo "Created $TGZ ($(du -h "$TGZ" | cut -f1))"

# --- deb package ---------------------------------------------------------------
DEB="$BUILD/$DEB_NAME"
rm -rf "$DEB"
DEB_LIBDIR="usr/lib/svnsyncdrive"
mkdir -p "$DEB/DEBIAN" \
         "$DEB/$DEB_LIBDIR" \
         "$DEB/$DEB_LIBDIR/lib" \
         "$DEB/usr/bin" \
         "$DEB/usr/share/applications" \
         "$DEB/usr/share/icons/hicolor/256x256/apps" \
         "$DEB/usr/share/doc/svnsyncdrive"

# Bundle the SVN stack this app was built against (e.g. a custom-built
# Subversion 1.15) so the installed app never picks up a mismatched system
# libsvn. Resolve the stack location from the binary's RUNPATH and ship the
# libsvn_* / libserf* shared libraries in the app's private directory.
SVN_LIB_DIR="$(readelf -d "$BIN" | sed -n 's/.*(RUNPATH).*\[\(.*\)\].*/\1/p' | head -1)"
if [ -z "$SVN_LIB_DIR" ]; then
    echo "Cannot locate bundled SVN lib dir (no RUNPATH on binary)" >&2
    exit 1
fi
for f in "$SVN_LIB_DIR"/libsvn*.so* "$SVN_LIB_DIR"/libserf*.so*; do
    [ -f "$f" ] && cp -L "$f" "$DEB/$DEB_LIBDIR/lib/"
done

cp "$BIN" "$DEB/$DEB_LIBDIR/svnsyncdrive"
cp "$ROOT/src/resources/icon_256.png" "$DEB/usr/share/icons/hicolor/256x256/apps/svnsyncdrive.png"
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$DEB/usr/share/doc/svnsyncdrive/copyright"
[ -f "$ROOT/README.md" ] && cp "$ROOT/README.md" "$DEB/usr/share/doc/svnsyncdrive/README.md"

# Wrapper: point LD_LIBRARY_PATH at the bundled SVN libs so the app always
# runs with the exact libsvn it was built against.
cat > "$DEB/usr/bin/svnsyncdrive" <<EOF
#!/bin/sh
export LD_LIBRARY_PATH=/usr/lib/svnsyncdrive/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}
exec /usr/lib/svnsyncdrive/svnsyncdrive "\$@"
EOF
chmod +x "$DEB/usr/bin/svnsyncdrive"

cat > "$DEB/usr/share/applications/svnsyncdrive.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=SvnSyncDrive
Comment=Two-way SVN directory sync client
Exec=/usr/bin/svnsyncdrive
Icon=svnsyncdrive
Terminal=false
Categories=Utility;Network;
StartupNotify=true
EOF

# Compute Depends from the binary and the bundled SVN libs (so their
# transitive system deps such as curl/ssl are captured). The bundled SVN
# libraries themselves have no dpkg shlibs mapping, so they never appear in
# Depends - which is correct, they ship inside the package.
mkdir -p "$BUILD/debian"
cat > "$BUILD/debian/control" <<EOF
Source: svnsyncdrive
Package: svnsyncdrive
Version: $VERSION
Architecture: $ARCH
EOF
EXTRA_E=""
for f in "$DEB/$DEB_LIBDIR/lib"/*.so*; do
    EXTRA_E="$EXTRA_E -e$f"
done
DEPS="$(cd "$BUILD" && dpkg-shlibdeps -O --ignore-missing-info -e"$BIN" $EXTRA_E 2>/dev/null \
    | sed 's/^shlibs:Depends=//')"
rm -f "$BUILD/debian/control"
rmdir "$BUILD/debian" 2>/dev/null || true

INSTALLED_SIZE="$(du -sk "$DEB/usr" | cut -f1)"
cat > "$DEB/DEBIAN/control" <<EOF
Package: svnsyncdrive
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: $DEPS
Installed-Size: $INSTALLED_SIZE
Maintainer: SvnSyncDrive
Description: Two-way SVN directory sync client
 Qt-based desktop client that keeps local folders in two-way live sync
 with SVN repositories: watches working copies, auto-commits changes and
 pulls updates, with conflict handling and encrypted credential storage.
EOF

(
    cd "$DEB"
    find usr -type f -print0 | xargs -0 md5sum > DEBIAN/md5sums
)

DEB_PATH="$OUT/$DEB_NAME.deb"
rm -f "$DEB_PATH"
dpkg-deb --build --root-owner-group "$DEB" "$DEB_PATH" >/dev/null
echo "Created $DEB_PATH ($(du -h "$DEB_PATH" | cut -f1))"
