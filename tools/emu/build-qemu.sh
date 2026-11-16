#!/bin/sh
# Build a minimal qemu-system-i386 with the pgbridge ISA device
# (tools/emu/qemu/pgbridge.patch) for the PGBOOT / PGDFS emulator tests.
#
#   tools/emu/build-qemu.sh            download, patch, configure, build, install
#   EMU_CACHE=/elsewhere tools/emu/build-qemu.sh
#
# Everything lives under $EMU_CACHE (default ~/pico/emu-cache), never in the
# repository. Result: $EMU_CACHE/qemu/bin/qemu-system-i386 (+ SeaBIOS blobs).
# Needs: cc, make, ninja, pkg-config, glib, pixman, python3 >= 3.9
# (macOS: brew install ninja pkgconf glib pixman dtc). QEMU ships its own meson.
set -eu

QEMU_VER=${QEMU_VER:-10.2.4}
QEMU_SHA256=${QEMU_SHA256:-}
EMU_CACHE=${EMU_CACHE:-$HOME/pico/emu-cache}
HERE=$(cd "$(dirname "$0")" && pwd)
PATCH=$HERE/qemu/pgbridge.patch
SRC=$EMU_CACHE/qemu-$QEMU_VER
BUILD=$EMU_CACHE/qemu-build
PREFIX=$EMU_CACHE/qemu
TARBALL=$EMU_CACHE/qemu-$QEMU_VER.tar.xz

mkdir -p "$EMU_CACHE"

if [ ! -f "$TARBALL" ]; then
    echo "== downloading qemu $QEMU_VER"
    curl -fL -o "$TARBALL.part" "https://download.qemu.org/qemu-$QEMU_VER.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
fi
if [ -n "$QEMU_SHA256" ]; then
    echo "$QEMU_SHA256  $TARBALL" | shasum -a 256 -c -
fi

# When the patch changed since the last build: swap the old patch for the new
# one in place (incremental rebuild), or start from a fresh tree if that fails.
STAMP=$SRC/.pgbridge-patch
if [ -d "$SRC" ] && ! cmp -s "$PATCH" "$STAMP" 2>/dev/null; then
    echo "== patch changed: re-patching the source tree"
    if ! { [ -f "$STAMP" ] && (cd "$SRC" && patch -s -R -p1 < "$STAMP" && patch -s -p1 < "$PATCH"); }; then
        echo "== re-patching failed: re-extracting"
        rm -rf "$SRC" "$BUILD"
    else
        cp "$PATCH" "$STAMP"
    fi
fi
if [ ! -d "$SRC" ]; then
    echo "== extracting"
    tar -C "$EMU_CACHE" -xf "$TARBALL"
    (cd "$SRC" && patch -p1 < "$PATCH")
    cp "$PATCH" "$STAMP"
fi

# Homebrew libraries (libfdt) are not on the default linker path.
EXTRA_CFLAGS=""; EXTRA_LDFLAGS=""
if command -v brew >/dev/null 2>&1; then
    BREW=$(brew --prefix)
    EXTRA_CFLAGS="-I$BREW/include"; EXTRA_LDFLAGS="-L$BREW/lib"
fi

if [ ! -f "$BUILD/build.ninja" ]; then
    echo "== configuring"
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --prefix="$PREFIX" \
        --extra-cflags="$EXTRA_CFLAGS" --extra-ldflags="$EXTRA_LDFLAGS" \
        --enable-fdt=system \
        --target-list=i386-softmmu \
        --without-default-features \
        --enable-tcg \
        --enable-pixman \
        --enable-vnc \
        --disable-docs \
        --disable-werror \
        --disable-debug-info)
fi

echo "== building"
ninja -C "$BUILD" qemu-system-i386
ninja -C "$BUILD" install >/dev/null
# Keep only the firmware an i386 PC needs (SeaBIOS, VGA BIOS, option ROM
# helpers); the other targets' blobs (EDK2, OpenSBI, ...) are ~300 MB.
(cd "$PREFIX/share/qemu" && for f in *; do
    case "$f" in
        bios*.bin|vgabios*.bin|kvmvapic.bin|linuxboot*.bin|multiboot*.bin|pvh.bin|keymaps|firmware) ;;
        *) rm -rf "$f" ;;
    esac
done)
if [ "${CLEAN:-0}" = 1 ]; then
    echo "== CLEAN=1: removing the build and source trees (the tarball stays)"
    rm -rf "$BUILD" "$SRC"
fi
echo "== installed: $PREFIX/bin/qemu-system-i386"
"$PREFIX/bin/qemu-system-i386" -device help 2>/dev/null | grep pgbridge
