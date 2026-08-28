#!/usr/bin/env bash
# Build SvnSyncDrive for macOS arm64 and produce a self-contained .app + dmg.
#
# Environment:
#   QT_ROOT     root of a Qt 6.8 install (contains bin/qmake, bin/macdeployqt)
#   SVN_STAGE   root of the staged libsvnplus (contains include/ and lib/)
#   VERSION     app version for artifact naming (default: parsed from CMakeLists.txt)
#
# Run from the repository root:
#   QT_ROOT=... SVN_STAGE=... bash scripts/publish-macos.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-macos"
APP_DIR="$BUILD/svnsyncdrive.app"
BIN="$APP_DIR/Contents/MacOS/svnsyncdrive"
FRAMEWORKS="$APP_DIR/Contents/Frameworks"
PUBLISH="$BUILD/publish"

ARCH="${HOMEBREW_ARCH:-$(uname -m)}"
echo "Detected macOS architecture: $ARCH"

[ -n "${QT_ROOT:-}" ] || { echo "QT_ROOT not set" >&2; exit 1; }
[ -n "${SVN_STAGE:-}" ] || { echo "SVN_STAGE not set" >&2; exit 1; }
[ -x "$QT_ROOT/bin/qmake" ] || { echo "qmake not found under QT_ROOT=$QT_ROOT" >&2; exit 1; }
[ -d "$SVN_STAGE/lib" ] || { echo "SVN_STAGE/lib missing under $SVN_STAGE" >&2; exit 1; }

if [ -z "${VERSION:-}" ]; then
    VERSION="$(sed -n 's/^project(SvnSyncDrive VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
fi
echo "Building SvnSyncDrive $VERSION for macOS $ARCH"

rm -rf "$BUILD"
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DLIBSVNPLUS_ROOT="$SVN_STAGE" \
    -DOPENSSL_ROOT_DIR="$SVN_STAGE" \
    -DCMAKE_BUILD_RPATH="$SVN_STAGE/lib"
cmake --build "$BUILD" --parallel

# Core sanity check: console unit tests (no server needed, exits non-zero on
# failure). The build rpath above lets it load the staged libsvn dylibs.
"$BUILD/synccoretest"

# Bundle the Qt frameworks and plugins into the .app.
"$QT_ROOT/bin/macdeployqt" "$APP_DIR"

# Ship the libsvn stack alongside the app. The staged dylibs already carry
# @rpath install names plus an @loader_path rpath, so dropping them all into
# Contents/Frameworks is enough: every dependency resolves relative to the
# dylib's own location.
mkdir -p "$FRAMEWORKS"
cp -R "$SVN_STAGE"/lib/*.dylib "$FRAMEWORKS/"

# The app binary must resolve its @rpath references from Contents/Frameworks.
# Drop the CI-only absolute build rpath first.
if otool -l "$BIN" | grep -q "path $SVN_STAGE/lib"; then
    install_name_tool -delete_rpath "$SVN_STAGE/lib" "$BIN"
fi
if ! otool -l "$BIN" | grep -q "path @executable_path/../Frameworks"; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$BIN"
fi

codesign --force --deep --sign - "$APP_DIR"

echo "Load commands of $BIN:"
otool -l "$BIN" | grep -E "path | name " || true

mkdir -p "$PUBLISH"
DMG="$PUBLISH/SvnSyncDrive-${VERSION}-macos-${ARCH}.dmg"
hdiutil create -volname "SvnSyncDrive" -srcfolder "$APP_DIR" -ov -format UDZO "$DMG"

echo "Built $DMG"
