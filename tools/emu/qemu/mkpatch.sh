#!/bin/sh
# Regenerate pgbridge.patch from pgbridge.c (the source of truth for the
# device) plus the Kconfig/meson wiring, against the pristine QEMU tarball.
#   tools/emu/qemu/mkpatch.sh      (then tools/emu/build-qemu.sh rebuilds)
set -eu
QEMU_VER=${QEMU_VER:-10.2.4}
EMU_CACHE=${EMU_CACHE:-$HOME/pico/emu-cache}
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
tar -C "$T" -xf "$EMU_CACHE/qemu-$QEMU_VER.tar.xz" \
    "qemu-$QEMU_VER/hw/misc/Kconfig" "qemu-$QEMU_VER/hw/misc/meson.build" "qemu-$QEMU_VER/hw/i386/Kconfig"
mkdir -p "$T/a" "$T/b"
cp -R "$T/qemu-$QEMU_VER/hw" "$T/a/"
cp -R "$T/qemu-$QEMU_VER/hw" "$T/b/"
cp "$HERE/pgbridge.c" "$T/b/hw/misc/pgbridge.c"
python3 - "$T/b" <<'PY'
import sys
b = sys.argv[1]
def edit(path, old, new):
    t = open(path).read()
    assert old in t, (path, old)
    open(path, 'w').write(t.replace(old, new, 1))
edit(b + '/hw/misc/Kconfig', "config ISA_DEBUG\n    bool\n    depends on ISA_BUS\n",
     "config ISA_DEBUG\n    bool\n    depends on ISA_BUS\n\nconfig PGBRIDGE\n    bool\n    depends on ISA_BUS\n")
edit(b + '/hw/misc/meson.build', "files('debugexit.c'))\n",
     "files('debugexit.c'))\nsystem_ss.add(when: 'CONFIG_PGBRIDGE', if_true: files('pgbridge.c'))\n")
edit(b + '/hw/i386/Kconfig', "    imply ISA_DEBUG\n", "    imply ISA_DEBUG\n    imply PGBRIDGE\n")
PY
(cd "$T" && diff -ruN --label a/hw/i386/Kconfig --label b/hw/i386/Kconfig a/hw/i386/Kconfig b/hw/i386/Kconfig;
            diff -ruN --label a/hw/misc/Kconfig --label b/hw/misc/Kconfig a/hw/misc/Kconfig b/hw/misc/Kconfig;
            diff -ruN --label a/hw/misc/meson.build --label b/hw/misc/meson.build a/hw/misc/meson.build b/hw/misc/meson.build;
            diff -ruN --label /dev/null --label b/hw/misc/pgbridge.c /dev/null b/hw/misc/pgbridge.c) > "$HERE/pgbridge.patch" || true
echo "wrote $HERE/pgbridge.patch"
