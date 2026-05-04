#!/usr/bin/env bash
# build-appimage.sh — Produces a Type 2 AppImage for process-lasso-qt.
#
# Usage (from the project root or packaging/ dir):
#   bash packaging/build-appimage.sh
#
# Requirements: cmake, qt6-base, curl, fuse2 (or fuse3 with fuse2 compat).
# Downloads linuxdeploy + linuxdeploy-plugin-qt + appimagetool on first run.
set -euo pipefail

# ── Paths ────────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-appimage"
TOOLS_DIR="$SCRIPT_DIR/appimage-tools"
APPDIR="$PROJECT_DIR/AppDir"

VERSION="$(grep -m1 'project.*VERSION' "$PROJECT_DIR/CMakeLists.txt" \
           | grep -oP '\d+\.\d+\.\d+')"
ARCH="x86_64"
OUTPUT="$PROJECT_DIR/process-lasso-qt-${VERSION}-${ARCH}.AppImage"

# ── Tool download ─────────────────────────────────────────────────────────────

mkdir -p "$TOOLS_DIR"

fetch() {
    local name="$1" url="$2" dst="$TOOLS_DIR/$1"
    if [[ ! -x "$dst" ]]; then
        echo "→ Downloading $name …"
        curl -fL --progress-bar "$url" -o "$dst"
        chmod +x "$dst"
    else
        echo "→ $name already present, skipping download."
    fi
}

fetch linuxdeploy \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch linuxdeploy-plugin-qt \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
fetch appimagetool \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

# Make sure linuxdeploy can locate its Qt plugin (must be on PATH).
export PATH="$TOOLS_DIR:$PATH"

# ── Patch linuxdeploy's bundled strip ─────────────────────────────────────────
#
# CachyOS / modern Arch glibc uses RELR relocations (.relr.dyn section, type
# 0x13). The strip binary bundled inside the linuxdeploy AppImage is too old to
# handle this and causes linuxdeploy to abort before deploying Qt plugins.
# Fix: extract linuxdeploy once, replace its strip with the system strip.

# Extract and patch linuxdeploy (both main tool and Qt plugin share the same
# problem: their bundled strip can't process .relr.dyn ELF sections used by
# modern Arch/CachyOS libraries).

patch_strip() {
    local name="$1" appimage="$2" unpackdir="$3"
    if [[ ! -d "$unpackdir" ]] || [[ "$appimage" -nt "$unpackdir/AppRun" ]]; then
        echo "→ Extracting and patching $name (strip too old for .relr.dyn) …"
        rm -rf "$unpackdir"
        (cd "$TOOLS_DIR" && "./$name" --appimage-extract >/dev/null 2>&1)
        mv "$TOOLS_DIR/squashfs-root" "$unpackdir"
        cp /usr/bin/strip "$unpackdir/usr/bin/strip"
        echo "   Patched: $(strip --version | head -1)"
    fi
}

LINUXDEPLOY_UNPACKED="$TOOLS_DIR/linuxdeploy-unpacked"
LINUXDEPLOY_QT_UNPACKED="$TOOLS_DIR/linuxdeploy-plugin-qt-unpacked"

patch_strip "linuxdeploy"            "$TOOLS_DIR/linuxdeploy"            "$LINUXDEPLOY_UNPACKED"
patch_strip "linuxdeploy-plugin-qt"  "$TOOLS_DIR/linuxdeploy-plugin-qt"  "$LINUXDEPLOY_QT_UNPACKED"

# Put the patched AppRun binaries first in PATH under their expected names so
# linuxdeploy finds the patched Qt plugin before the original AppImage.
PATCHED_BIN="$TOOLS_DIR/patched-bin"
mkdir -p "$PATCHED_BIN"
ln -sf "$LINUXDEPLOY_QT_UNPACKED/AppRun" "$PATCHED_BIN/linuxdeploy-plugin-qt"
export PATH="$PATCHED_BIN:$TOOLS_DIR:$PATH"

# ── Release build ─────────────────────────────────────────────────────────────

echo ""
echo "→ Configuring Release build …"
cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr

echo "→ Building ($(nproc) jobs) …"
cmake --build "$BUILD_DIR" -j"$(nproc)"

# ── Populate AppDir via cmake --install ───────────────────────────────────────

echo ""
echo "→ Populating AppDir …"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

# ── Icon ──────────────────────────────────────────────────────────────────────

ICON_SRC="$SCRIPT_DIR/process-lasso.png"
if [[ ! -f "$ICON_SRC" ]]; then
    echo "ERROR: icon not found at $ICON_SRC" >&2
    exit 1
fi

# Place icon in the hicolor hierarchy (linuxdeploy picks it up from here).
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$ICON_SRC" "$APPDIR/usr/share/icons/hicolor/256x256/apps/process-lasso.png"

# ── Bundle Qt libraries + plugins ─────────────────────────────────────────────
#
# linuxdeploy reads the .desktop file from usr/share/applications/, copies all
# shared-library dependencies, generates AppRun, and calls the Qt plugin which
# adds platform/imageformat/xcb plugins and a qt.conf.
#
# kimg_jxr.so (KDE JPEG XR image plugin) requires libjxrglue.so.0 which may not
# be installed. We build a qmake wrapper that points QT_INSTALL_PLUGINS to a
# temp copy of the plugin tree with kimg_jxr.so removed so linuxdeploy-plugin-qt
# can proceed without error.  This plugin is irrelevant to a process manager.

echo "→ Bundling Qt libraries and plugins …"

QMAKE=qmake6 \
"$LINUXDEPLOY_UNPACKED/AppRun" \
    --appdir        "$APPDIR" \
    --executable    "$APPDIR/usr/bin/process-lasso-qt" \
    --executable    "$APPDIR/usr/bin/process-lasso-helper" \
    --desktop-file  "$APPDIR/usr/share/applications/process-lasso.desktop" \
    --icon-file     "$ICON_SRC" \
    --plugin        qt

# ── Patch AppRun to expose $APPDIR data so QStandardPaths finds our files ─────
#
# Qt's QStandardPaths does not search inside the AppImage by default. Prepending
# $APPDIR/usr/share to XDG_DATA_DIRS makes QStandardPaths::AppDataLocation find
# install-helper.sh at runtime (belt-and-suspenders alongside the
# applicationDirPath fallback already coded in cpupark.cpp).

APPRUN="$APPDIR/AppRun"
if ! grep -q 'XDG_DATA_DIRS' "$APPRUN"; then
    # Insert before the final exec line
    sed -i 's|^exec "\$APPDIR/usr/bin/process-lasso-qt"|export XDG_DATA_DIRS="${APPDIR}/usr/share${XDG_DATA_DIRS:+:${XDG_DATA_DIRS}}"\nexec "${APPDIR}/usr/bin/process-lasso-qt"|' "$APPRUN"
    echo "→ AppRun patched (XDG_DATA_DIRS)."
fi

# ── Package as Type 2 AppImage ────────────────────────────────────────────────
#
# --updateinformation embeds a gh-releases-zsync block so Gear Lever / AppImageUpdate
# can find and apply delta updates automatically.  appimagetool also invokes
# zsyncmake (must be on PATH) to produce a companion .zsync file alongside the
# AppImage; upload both to the GitHub release.

ZSYNC_FILENAME="$(basename "$OUTPUT").zsync"
UPDATE_INFO="gh-releases-zsync|Tamalero|Process-lasso-linux-inC|latest|process-lasso-qt-*-${ARCH}.AppImage.zsync"

echo ""
echo "→ Packaging AppImage …"
ARCH="$ARCH" \
"$TOOLS_DIR/appimagetool" \
    --comp zstd \
    --updateinformation "$UPDATE_INFO" \
    "$APPDIR" \
    "$OUTPUT"

echo ""
echo "✓ AppImage ready: $OUTPUT"
echo "  Size: $(du -sh "$OUTPUT" | cut -f1)"
if [[ -f "$PROJECT_DIR/$ZSYNC_FILENAME" ]]; then
    echo "✓ zsync  ready: $PROJECT_DIR/$ZSYNC_FILENAME"
fi
