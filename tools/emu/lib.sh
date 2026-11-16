# Shared helpers for the PGBOOT / PGDFS emulator tests (sourced, POSIX sh).
#
# Layout: everything generated lives under $EMU_CACHE (default ~/pico/emu-cache):
#   qemu/bin/qemu-system-i386   build-qemu.sh
#   sim-build/pgcard-sim        make -C tools/emu/sim
#   freedos/                    FreeDOS 1.3 system files (fetch_freedos)
#   dos/                        DFSDIAG.EXE, PGUSDFS.EXE, PGUSINIT.EXE (build_dos_tools)
#   work/<test>/                per-test images, serial output, sim trace, screen dumps

EMU_CACHE=${EMU_CACHE:-$HOME/pico/emu-cache}
EMU_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$EMU_DIR/../.." && pwd)
QEMU=${QEMU:-$EMU_CACHE/qemu/bin/qemu-system-i386}
SIM=$EMU_CACHE/sim-build/pgcard-sim
FREEDOS=$EMU_CACHE/freedos
ROMDIR=${ROMDIR:-$ROOT/bootrom}
export MTOOLS_SKIP_CHECK=1
unset MTOOLSRC

FD13_URL=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip
FD13_SHA256=75a4e11a7fce6f124e20927b3022b4b715a2a3f7324c5f5bfea42d90d80eb072

die() { echo "error: $*" >&2; exit 1; }

# ---- inputs ------------------------------------------------------------------

# FreeDOS 1.3 FloppyEdition: its 1.44M boot disk is the template for every
# floppy (FreeDOS FAT12 boot sector that loads KERNEL.SYS by name), and the
# source of KERNEL.SYS, COMMAND.COM, SYS.COM, FDISK.EXE, FDAPM.COM.
fetch_freedos() {
    local zip img f
    [ -f "$FREEDOS/x86BOOT.img" ] && [ -f "$FREEDOS/SYS.COM" ] && return 0
    mkdir -p "$FREEDOS"
    zip=$EMU_CACHE/FD13-FloppyEdition.zip
    if [ ! -f "$zip" ]; then
        echo "== downloading FreeDOS 1.3 FloppyEdition"
        curl -fL -o "$zip.part" "$FD13_URL" && mv "$zip.part" "$zip"
    fi
    echo "$FD13_SHA256  $zip" | shasum -a 256 -c - >/dev/null || die "FD13-FloppyEdition.zip checksum mismatch"
    unzip -o -q -j "$zip" 144m/x86BOOT.img -d "$FREEDOS"
    img=$FREEDOS/x86BOOT.img
    mcopy -n -i "$img" ::/KERNEL.SYS "$FREEDOS/KERNEL.SYS"
    for f in COMMAND.COM SYS.COM FDAPM.COM FDISK.EXE FORMAT.EXE MEM.EXE; do
        mcopy -n -i "$img" "::/FREEDOS/BIN/$f" "$FREEDOS/$f"
    done
}

# The DOS tools, built with OpenWatcom out of tree (objects and executables in
# $EMU_CACHE/dos, nothing is written into pgusdfs/ or pgusinit/), with the
# flags of their Makefiles: DFSDIAG.EXE (T0), PGUSDFS.EXE (T10),
# PGUSINIT.EXE (T9). Rebuilt when a source is newer than the executable.
DOS=$EMU_CACHE/dos
build_dos_tools() {
    local W_ PATH_ newest
    WATCOM=${WATCOM:-$HOME/pico/watcom}
    [ -d "$WATCOM" ] || die "OpenWatcom not found in $WATCOM (needed for the DOS tools)"
    mkdir -p "$DOS/obj"
    newest=$(ls -t "$ROOT"/pgusdfs/*.c "$ROOT"/pgusdfs/*.h "$ROOT"/pgusdfs/*.asm \
                   "$ROOT"/pgusinit/*.c "$ROOT"/pgusinit/*.h "$ROOT"/common/picogus.h | head -1)
    for f in DFSDIAG.EXE PGUSDFS.EXE PGUSINIT.EXE; do
        [ -f "$DOS/$f" ] && [ "$DOS/$f" -nt "$newest" ] || { rm -f "$DOS"/*.EXE; break; }
    done
    [ -f "$DOS/PGUSINIT.EXE" ] && return 0
    echo "== building DFSDIAG.EXE, PGUSDFS.EXE, PGUSINIT.EXE (OpenWatcom, out of tree)"
    ( export WATCOM PATH="$WATCOM/armo64:$WATCOM/binl64:$WATCOM/binl:$PATH" INCLUDE="$WATCOM/h"
      cd "$DOS/obj" &&
      flags="-q -bcl=dos -0 -s -d0 -ms -os -wx -we -dPICOGUS_NO_MODENAMES" &&
      wcl $flags -za99 "$ROOT/pgusdfs/dfsdiag.c" "$ROOT/pgusdfs/xport.c" -fe="$DOS/DFSDIAG.EXE" &&
      wasm -q -0 "$ROOT/pgusdfs/chint086.asm" -fo=chint.obj -ms &&
      wcl $flags -k1024 chint.obj "$ROOT/pgusdfs/pgusdfs.c" -fe="$DOS/PGUSDFS.EXE" &&
      wcl -q -bcl=dos -za99 -os "$ROOT/pgusinit/pgusinit.c" -fe="$DOS/PGUSINIT.EXE" ) >"$DOS/build.log" 2>&1 ||
        { cat "$DOS/build.log"; die "DOS tool build failed"; }
}

build_sim() {
    make -s -C "$EMU_DIR/sim" EMU_CACHE="$EMU_CACHE" >/dev/null || die "pgcard-sim build failed"
    make -s -C "$EMU_DIR/sim" EMU_CACHE="$EMU_CACHE" info
}

# ---- images ------------------------------------------------------------------

# mk_floppy OUT AUTOEXEC_TEXT [FILE...]
# A bootable FreeDOS 1.44M floppy: the FloppyEdition boot disk with its
# installer removed, KERNEL.SYS kept in place, plus COMMAND.COM, FDAPM.COM,
# a minimal FDCONFIG.SYS, the given AUTOEXEC.BAT (LF -> CRLF) and extra files.
mk_floppy() {
    local out auto f
    out=$1; auto=$2; shift 2
    cp "$FREEDOS/x86BOOT.img" "$out"
    mdeltree -i "$out" ::/FREEDOS
    mdel -i "$out" ::/FDAUTO.BAT ::/FDCONFIG.SYS ::/SETUP.BAT
    printf 'FILES=20\nBUFFERS=20\nLASTDRIVE=Z\n' > "$out.cfg"
    printf '%s\n' "$auto" > "$out.bat"
    mcopy -t -i "$out" "$out.cfg" ::/FDCONFIG.SYS
    mcopy -t -i "$out" "$out.bat" ::/AUTOEXEC.BAT
    rm -f "$out.cfg" "$out.bat"
    mcopy -i "$out" "$FREEDOS/COMMAND.COM" "$FREEDOS/FDAPM.COM" ::/
    for f in "$@"; do mcopy -i "$out" "$f" ::/; done
}

# mk_hd OUT: 65 cyl x 16 heads x 63 sectors (32 MB), MBR with one active
# FAT16 partition from sector 63, formatted, empty, no boot code anywhere.
HD_C=65; HD_H=16; HD_S=63
mk_hd() {
    local out rc
    out=$1
    rm -f "$out"
    dd if=/dev/zero of="$out" bs=512 count=0 seek=$((HD_C * HD_H * HD_S)) 2>/dev/null
    rc=$out.mtoolsrc
    printf 'drive c: file="%s" partition=1\n' "$out" > "$rc"
    MTOOLSRC=$rc mpartition -I c: 2>/dev/null
    MTOOLSRC=$rc mpartition -c -a -t $HD_C -h $HD_H -s $HD_S c: >/dev/null
    MTOOLSRC=$rc mformat -H $HD_S -v PGHD c:
    rm -f "$rc"
}

# hd_mtools IMG CMD ARGS...: run an mtools command on the partition of IMG as c:
hd_mtools() {
    local img cmd rc st
    img=$1; cmd=$2; shift 2
    rc=$img.mtoolsrc
    printf 'drive c: file="%s" partition=1\n' "$img" > "$rc"
    MTOOLSRC=$rc "$cmd" "$@"
    st=$?
    rm -f "$rc"
    return $st
}

# mk_stick OUT [SRC DST]...: a 96 MB FAT32 superfloppy ("USB stick") with files.
mk_stick() {
    local out
    out=$1; shift
    rm -f "$out"
    mformat -C -i "$out" -F -T 196608 -h 64 -s 32 -v PGSTICK ::
    while [ $# -ge 2 ]; do
        mcopy -i "$out" "$1" "::/$2"
        shift 2
    done
}

# ---- running -----------------------------------------------------------------

SIM_PID=; QEMU_PID=; SOCK=; MON=

cleanup_procs() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    [ -n "$SIM_PID" ] && kill "$SIM_PID" 2>/dev/null
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null
    [ -n "$SIM_PID" ] && wait "$SIM_PID" 2>/dev/null
    QEMU_PID=; SIM_PID=
}

# start_sim WORKDIR ARGS...: pgcard-sim on WORKDIR/card.sock, trace in WORKDIR/sim.log
start_sim() {
    local w i
    w=$1; shift
    SOCK=/tmp/pgemu-$$-$(basename "$w").sock   # sun_path is short: keep it in /tmp
    rm -f "$SOCK"
    # shellcheck disable=SC2086
    "$SIM" --socket "$SOCK" --log=${SIM_LOG:-1} --logfile "$w/sim.log" ${SIM_EXTRA:-} "$@" &
    SIM_PID=$!
    i=0
    while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    [ -S "$SOCK" ] || { echo "pgcard-sim did not start" >&2; return 1; }
}

bridge_args() { echo "-device pgbridge,path=$SOCK${DATAPORT:+,dataport=$DATAPORT}"; }

# start_qemu WORKDIR ARGS...: headless PC, COM1 -> WORKDIR/serial.txt,
# HMP monitor on WORKDIR/mon.sock (for screen dumps), no NIC, no reboot.
start_qemu() {
    local w
    w=$1; shift
    : > "$w/serial.txt"
    MON=/tmp/pgemu-$$-$(basename "$w").mon
    rm -f "$MON"
    "$QEMU" -M pc -m 16 -display none -nic none -no-reboot \
        -serial "file:$w/serial.txt" -monitor "unix:$MON,server,nowait" \
        ${QEMU_EXTRA:-} "$@" > "$w/qemu.log" 2>&1 &
    QEMU_PID=$!
}

# screen_dump WORKDIR NAME: text-mode screen (B800:0000) -> WORKDIR/NAME.txt
screen_dump() {
    local w n
    w=$1; n=$2
    [ -S "$MON" ] || return 0
    python3 - "$MON" "$w/$n.bin" "$w/$n.txt" <<'PY' 2>/dev/null
import socket, sys, time, os
mon, binf, txt = sys.argv[1:4]
s = socket.socket(socket.AF_UNIX); s.settimeout(2); s.connect(mon)
time.sleep(0.2)
s.sendall(('pmemsave 0xb8000 4000 "%s"\n' % binf).encode())
for _ in range(20):
    time.sleep(0.1)
    if os.path.exists(binf) and os.path.getsize(binf) == 4000: break
s.close()
d = open(binf, 'rb').read()
lines = []
for r in range(25):
    row = d[r*160:(r+1)*160:2]
    lines.append(''.join(chr(c) if 32 <= c < 127 else ' ' for c in row).rstrip())
while lines and not lines[-1]: lines.pop()
open(txt, 'w').write('\n'.join(lines) + '\n')
os.remove(binf)
PY
}

# wait_marker WORKDIR MARKER TIMEOUT_S: 0 when MARKER shows up on COM1
wait_marker() {
    local w m t end
    w=$1; m=$2; t=$3
    end=$(( $(date +%s) + t ))
    while [ "$(date +%s)" -lt "$end" ]; do
        if grep -q "$m" "$w/serial.txt" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            grep -q "$m" "$w/serial.txt" 2>/dev/null
            return $?
        fi
        sleep 0.5
    done
    return 1
}

# wait_qemu_exit SECONDS: give the guest time to power off (FDAPM POWEROFF)
wait_qemu_exit() {
    local t i
    t=$1; i=0
    while kill -0 "$QEMU_PID" 2>/dev/null && [ $i -lt $((t * 10)) ]; do sleep 0.1; i=$((i + 1)); done
}
