#!/bin/sh
# PGBOOT / PGDFS end-to-end tests: QEMU (pgbridge) + pgcard-sim (real card code).
#
#   tools/emu/run-tests.sh              all tests
#   tools/emu/run-tests.sh T0 T3 T4     selected tests
#   tools/emu/run-tests.sh P4           only prepare the native (no ROM) reference hard disk
#   KEEP=1 ...                          keep the work directories of passing tests
#   SIM_LOG=3 ...                       more detailed card trace (see pgcard-sim --help)
#   SIM_EXTRA='--busy-polls 500' ...     extra pgcard-sim options for every test (a slow card)
#   ROMDIR=/path ...                    where PGBOOT.ROM / PGBOOT88.ROM are (default bootrom/)
#
# Results: one PASS/FAIL/SKIP line per test and a summary; evidence per test in
# $EMU_CACHE/work/<test>/: serial.txt (COM1), sim.log (card trace), screen*.txt
# (text screen dumps), qemu.log.
set -u
. "$(cd "$(dirname "$0")" && pwd)/lib.sh"

WORK=$EMU_CACHE/work
TESTS=${*:-T0 S1 T1 T2 T3 T4 T5 T6 T7 T8 T9 T10 T11 T12 T13 T14 T15 T16 T17 T18 T19 T20 T21 T22}
TIMEOUT=${TIMEOUT:-90}
ROM=$ROMDIR/PGBOOT.ROM
ROM88=$ROMDIR/PGBOOT88.ROM
# The ROM's message when no card answers (T6); a regex for grep -E.
ROM_NOTFOUND_RE=${ROM_NOTFOUND_RE:-PicoGUS.*not found|not found|no PicoGUS}

# The ROM's last line before it hands over: to the boot sector of an image,
# or to the BIOS boot (the early screen dump waits for it).
ROM_DONE_RE='PGBOOT: (booting [AC]:|continuing with the BIOS boot|no |skipped)'

[ -x "$QEMU" ] || die "no QEMU at $QEMU: run tools/emu/build-qemu.sh"
build_sim
fetch_freedos
mkdir -p "$WORK"
trap 'cleanup_procs' EXIT
trap 'cleanup_procs; exit 130' INT TERM

SUMMARY=
report() {  # report TEST STATUS MESSAGE
    printf '%-4s %-4s %s\n' "$1" "$2" "$3"
    SUMMARY="$SUMMARY$(printf '%-4s %-4s %s' "$1" "$2" "$3")
"
}
newwork() { rm -rf "$WORK/$1"; mkdir -p "$WORK/$1"; W=$WORK/$1; }
finish() {  # finish TEST: stop both processes, final screen dump already taken
    cleanup_procs
    rm -f "$SOCK" "$MON" 2>/dev/null
}
excerpt() { sed -e 's/\r$//' "$1/serial.txt" | grep -v '^ *$' | head -${2:-12} | sed 's/^/      | /'; }

# DOS side of the tests: batch files (CRLF added by mcopy -t)
FD_AUTO='@ECHO OFF
ECHO PGBOOT-FD-BOOTED > COM1
VER > COM1
DIR A:\ > COM1
ECHO PGBOOT-FD-OK > COM1
ECHO floppy-write-ok > A:\FDWRITE.TXT
TYPE A:\FDWRITE.TXT > COM1
ECHO PGBOOT-FD-WRITE-DONE > COM1'

HD_AUTO='@ECHO OFF
ECHO PGBOOT-HD-BOOTED > COM1
VER > COM1
DIR C:\ > COM1
ECHO hd-write-ok > C:\HDWRITE.TXT
TYPE C:\HDWRITE.TXT > COM1
ECHO PGBOOT-HD-OK > COM1'

# Installs FreeDOS on C: through INT 13h: MBR boot code (FDISK /MBR), boot
# sector + KERNEL.SYS + COMMAND.COM (SYS), then the hard disk's own batch files.
SYS_AUTO='@ECHO OFF
ECHO PGBOOT-SYS-START > COM1
FDISK /MBR > COM1
SYS C: > COM1
COPY A:\HDAUTO.BAT C:\AUTOEXEC.BAT > COM1
COPY A:\HDCONFIG.SYS C:\FDCONFIG.SYS > COM1
DIR C:\ > COM1
ECHO PGBOOT-SYS-DONE > COM1'

NATIVE_AUTO='@ECHO OFF
ECHO PGBOOT-NATIVE-OK > COM1'

T0_AUTO='@ECHO OFF
ECHO PGBOOT-T0-START > COM1
DFSDIAG /INFO > COM1
DFSDIAG /ECHO 3 > COM1
DFSDIAG /DIR > COM1
DFSDIAG /LDIR \SUBDIR > COM1
DFSDIAG /MKDIR \NEWDIR > COM1
DFSDIAG /DIR > COM1
ECHO PGBOOT-T0-DONE > COM1'

# sys_floppy OUT: the T3 installer floppy
sys_floppy() {
    printf '%s\n' "$HD_AUTO" > "$W/HDAUTO.BAT.lf"
    printf 'FILES=20\nBUFFERS=20\n' > "$W/HDCONFIG.SYS.lf"
    mk_floppy "$1" "$SYS_AUTO" "$FREEDOS/SYS.COM" "$FREEDOS/FDISK.EXE"
    mcopy -t -i "$1" "$W/HDAUTO.BAT.lf" ::/HDAUTO.BAT
    mcopy -t -i "$1" "$W/HDCONFIG.SYS.lf" ::/HDCONFIG.SYS
    rm -f "$W/HDAUTO.BAT.lf" "$W/HDCONFIG.SYS.lf"
}

need_rom() {  # need_rom TEST FILE
    [ -f "$2" ] && return 0
    report "$1" SKIP "no ROM image $2 (bootrom/ not built yet)"
    return 1
}

# boot_rom_test TEST ROM MARKER SIM_ARGS... : common body of the ROM boot tests
# (stick already in $W/stick.img)
boot_rom_test() {
    local t rom marker st
    t=$1; rom=$2; marker=$3; shift 3
    start_sim "$W" --stick "$W/stick.img" "$@" || return 1
    start_qemu "$W" -option-rom "$rom" -boot menu=off $(bridge_args)
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    if wait_marker "$W" "$marker" "$TIMEOUT"; then st=0; else st=1; fi
    sleep 1; screen_dump "$W" screen
    finish
    return $st
}

# ---- T0: bridge + transport self-test (no ROM): native floppy, DFSDIAG ------
t_T0() {
    local ok s
    newwork T0
    build_dos_tools
    echo hello > "$W/hello.txt"
    mk_stick "$W/stick.img" "$W/hello.txt" HELLO.TXT
    mmd -i "$W/stick.img" ::/SUBDIR
    mcopy -i "$W/stick.img" "$W/hello.txt" "::/SUBDIR/Long File Name.txt"
    mk_floppy "$W/fd.img" "$T0_AUTO" "$DOS/DFSDIAG.EXE"
    start_sim "$W" --stick "$W/stick.img" || { report T0 FAIL "sim did not start"; return; }
    start_qemu "$W" -drive "file=$W/fd.img,format=raw,if=floppy" -boot a $(bridge_args)
    wait_marker "$W" PGBOOT-T0-DONE "$TIMEOUT"; ok=$?
    finish
    s=$W/serial.txt
    if [ $ok = 0 ] && grep -q "PicoGUS detected" "$s" && grep -q "ECHO test passed" "$s" &&
       grep -q "HELLO.TXT" "$s" && grep -q "Long File Name.txt" "$s" &&
       mdir -i "$W/stick.img" ::/NEWDIR >/dev/null 2>&1; then
        report T0 PASS "DFSDIAG /INFO /ECHO /DIR /LDIR /MKDIR over the bridge ($(grep -c '^\[.*REQ #' "$W/sim.log") frames); NEWDIR on the stick"
    else
        report T0 FAIL "see $W/serial.txt and $W/sim.log"
        excerpt "$W" 40
    fi
}

# ---- S1: card PGBOOT server at bus level, no QEMU, no ROM (buscheck.py) ----
t_S1() {
    local ok
    newwork S1
    mk_floppy "$W/fd.img" "$FD_AUTO"
    hd_src=$WORK/hd-native.img
    [ -f "$hd_src" ] || { mk_hd "$W/hd.img"; hd_src=$W/hd.img; }
    cp "$hd_src" "$W/hd-in.img"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG "$W/hd-in.img" HD.IMG
    start_sim "$W" --stick "$W/stick.img" --fd FD.IMG --hd HD.IMG || { report S1 FAIL "sim did not start"; return; }
    python3 "$EMU_DIR/buscheck.py" "$SOCK" "$W/fd.img" "$W/hd-in.img" > "$W/buscheck.txt" 2>&1; ok=$?
    finish
    if [ $ok = 0 ]; then
        report S1 PASS "card PGBOOT server: registers, BDINFO/BDREAD/BDWRITE over the bus ($(grep -c '  ok ' "$W/buscheck.txt") checks)"
    else
        report S1 FAIL "see $W/buscheck.txt and $W/sim.log"
        sed 's/^/      | /' "$W/buscheck.txt"
    fi
}

# ---- T1/T2: boot a floppy image from the stick through the ROM --------------
t_fdboot() {
    local t rom extra ok wr
    t=$1; rom=$2; extra=${3:-}
    newwork "$t"
    need_rom "$t" "$rom" || return
    mk_floppy "$W/fd.img" "$FD_AUTO"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG
    # shellcheck disable=SC2086
    boot_rom_test "$t" "$rom" PGBOOT-FD-OK --fd FD.IMG $extra; ok=$?
    wr=
    mcopy -n -i "$W/stick.img" ::/FD.IMG "$W/fd-after.img" 2>/dev/null &&
        mtype -i "$W/fd-after.img" ::/FDWRITE.TXT 2>/dev/null | grep -q floppy-write-ok &&
        wr="; floppy write (A:\\FDWRITE.TXT) verified in FD.IMG"
    if [ $ok = 0 ]; then
        report "$t" PASS "$(basename "$rom"): FreeDOS booted from FD.IMG$wr"
        excerpt "$W" 6
    else
        report "$t" FAIL "$(basename "$rom"): no PGBOOT-FD-OK within ${TIMEOUT}s; see $W"
        [ -f "$W/screen.txt" ] && sed 's/^/      # /' "$W/screen.txt"
    fi
}
t_T1() { t_fdboot T1 "$ROM"; }
t_T2() { t_fdboot T2 "$ROM88"; }

# ---- T5: NODRIVE wait loop: the stick mounts 3 s after power-on -------------
t_T5() {
    local nd
    t_fdboot T5 "$ROM" "--mount-delay-ms 3000"
    [ -f "$WORK/T5/sim.log" ] || return
    nd=$(grep -c "status -> ff" "$WORK/T5/sim.log")
    echo "      sim: $(grep 'USB: stick mounted' "$WORK/T5/sim.log" | sed 's/^\[[^]]*\] //'), NODRIVE status transitions seen: $nd"
}

# ---- T3: FDISK /MBR + SYS C: onto the hard disk image through the ROM -------
t_T3() {
    local ok v
    newwork T3
    rm -f "$WORK/hd-from-T3.img"
    need_rom T3 "$ROM" || return
    sys_floppy "$W/fd.img"
    mk_hd "$W/hd.img"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG "$W/hd.img" HD.IMG
    boot_rom_test T3 "$ROM" PGBOOT-SYS-DONE --fd FD.IMG --hd HD.IMG; ok=$?
    mcopy -n -i "$W/stick.img" ::/HD.IMG "$W/hd-after.img" || ok=1
    v=$(verify_hd "$W/hd-after.img")
    if [ $ok = 0 ] && [ -z "$v" ]; then
        cp "$W/hd-after.img" "$WORK/hd-from-T3.img"
        report T3 PASS "SYS C: through the ROM; HD.IMG verified with mtools (MBR code, FreeDOS boot sector, KERNEL.SYS, COMMAND.COM, AUTOEXEC.BAT)"
        excerpt "$W" 30
    else
        report T3 FAIL "${v:-no PGBOOT-SYS-DONE}; see $W"
        excerpt "$W" 30
        [ -f "$W/screen.txt" ] && sed 's/^/      # /' "$W/screen.txt"
    fi
}

# verify_hd IMG: prints what is missing (nothing = a bootable FreeDOS disk)
verify_hd() {
    local img miss off f
    img=$1; miss=
    [ "$(dd if="$img" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d " \n")" = 55aa ] || miss="$miss MBR-signature"
    [ "$(dd if="$img" bs=1 count=64 2>/dev/null | tr -d '\000' | wc -c | tr -d ' ')" -gt 0 ] || miss="$miss MBR-code"
    off=$((HD_S * 512))
    [ "$(dd if="$img" bs=1 skip=$((off + 510)) count=2 2>/dev/null | od -An -tx1 | tr -d " \n")" = 55aa ] || miss="$miss boot-signature"
    dd if="$img" bs=1 skip=$((off + 3)) count=8 2>/dev/null | grep -q "FRDOS\|FreeDOS" || miss="$miss FreeDOS-boot-sector"
    for f in KERNEL.SYS COMMAND.COM AUTOEXEC.BAT FDCONFIG.SYS; do
        hd_mtools "$img" mdir "c:/$f" >/dev/null 2>&1 || miss="$miss $f"
    done
    [ -n "$miss" ] && echo "missing on HD.IMG:$miss"
}

# The fallback hard disk for T4 when T3 has not produced one: the same
# installer floppy, run natively (QEMU's own floppy and IDE disk, no ROM).
prep_hd_native() {
    local out ok v
    out=$WORK/hd-native.img
    [ -f "$out" ] && [ "$out" -nt "$EMU_DIR/run-tests.sh" ] && return 0
    echo "   (preparing the fallback hard disk image natively: FDISK /MBR + SYS C: without the ROM)"
    newwork P4
    sys_floppy "$W/fd.img"
    mk_hd "$W/hd.img"
    start_qemu "$W" -drive "file=$W/fd.img,format=raw,if=floppy" -drive "file=$W/hd.img,format=raw,if=ide" -boot a
    wait_marker "$W" PGBOOT-SYS-DONE "$TIMEOUT"; ok=$?
    sleep 1
    finish
    v=$(verify_hd "$W/hd.img")
    if [ $ok = 0 ] && [ -z "$v" ]; then
        cp "$W/hd.img" "$out"
        return 0
    fi
    echo "   fallback preparation failed: ${v:-no marker}; see $W" >&2
    return 1
}

# ---- T4: boot the hard disk image through the ROM ---------------------------
t_T4() {
    local src what ok wr
    need_rom T4 "$ROM" || return
    if [ -f "$WORK/hd-from-T3.img" ]; then src=$WORK/hd-from-T3.img; what="the image T3 installed through the ROM"
    else prep_hd_native || { report T4 FAIL "no hard disk image"; return; }
        src=$WORK/hd-native.img; what="the natively prepared fallback image"; fi
    newwork T4
    cp "$src" "$W/hd.img"
    mk_stick "$W/stick.img" "$W/hd.img" HD.IMG
    boot_rom_test T4 "$ROM" PGBOOT-HD-OK --hd HD.IMG; ok=$?
    wr=
    mcopy -n -i "$W/stick.img" ::/HD.IMG "$W/hd-after.img" 2>/dev/null &&
        hd_mtools "$W/hd-after.img" mtype c:/HDWRITE.TXT 2>/dev/null | grep -q hd-write-ok &&
        wr="; C:\\HDWRITE.TXT verified in HD.IMG"
    if [ $ok = 0 ] && grep -q "too large" "$W/screen.txt"; then
        report T4 FAIL "booted C:, but FreeDOS takes the 32 MB image for a drive over 8 GB (INT 13h AH=48h); see $W"
    elif [ $ok = 0 ]; then
        report T4 PASS "booted C: from $what$wr"
        excerpt "$W" 8
    else
        report T4 FAIL "no PGBOOT-HD-OK from $what; see $W"
        [ -f "$W/screen.txt" ] && sed 's/^/      # /' "$W/screen.txt"
    fi
}

# ---- T6: no card: the ROM must say so and the BIOS must boot on -------------
t_T6() {
    local ok
    newwork T6
    need_rom T6 "$ROM" || return
    mk_floppy "$W/fd.img" "$NATIVE_AUTO"
    SOCK=; start_qemu "$W" -option-rom "$ROM" -drive "file=$W/fd.img,format=raw,if=floppy" -boot menu=off
    sleep 2; screen_dump "$W" screen-early
    wait_marker "$W" PGBOOT-NATIVE-OK "$TIMEOUT"; ok=$?
    sleep 1; screen_dump "$W" screen
    finish
    if [ $ok = 0 ] && cat "$W"/screen*.txt 2>/dev/null | grep -Eqi "$ROM_NOTFOUND_RE"; then
        report T6 PASS "ROM reported no card; native floppy booted"
        cat "$W"/screen*.txt | grep -Ei "$ROM_NOTFOUND_RE" | sort -u | sed 's/^/      # /'
    elif [ $ok = 0 ]; then
        report T6 FAIL "native floppy booted but no not-found message on screen (ROM_NOTFOUND_RE='$ROM_NOTFOUND_RE')"
        sed 's/^/      # /' "$W/screen-early.txt" 2>/dev/null
    else
        report T6 FAIL "BIOS did not boot the native floppy with the ROM installed; see $W"
        sed 's/^/      # /' "$W/screen.txt" 2>/dev/null
    fi
}

# ---- T7: card present, configured image missing: the ROM chains to the BIOS
t_T7() {
    local ok
    newwork T7
    need_rom T7 "$ROM" || return
    mk_floppy "$W/fd.img" "$NATIVE_AUTO"
    echo x > "$W/x.txt"
    mk_stick "$W/stick.img" "$W/x.txt" OTHER.TXT
    start_sim "$W" --stick "$W/stick.img" --fd MISSING.IMG || { report T7 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -drive "file=$W/fd.img,format=raw,if=floppy" -boot menu=off $(bridge_args)
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    wait_marker "$W" PGBOOT-NATIVE-OK "$TIMEOUT"; ok=$?
    sleep 1; screen_dump "$W" screen
    finish
    if [ $ok = 0 ]; then
        report T7 PASS "FD image missing: ROM chained to the BIOS, native floppy booted"
        grep -h "PGBOOT" "$W/screen-early.txt" | sed 's/^/      # /'
    else
        report T7 FAIL "native floppy did not boot after the ROM found no image; see $W"
        sed 's/^/      # /' "$W/screen.txt" 2>/dev/null
    fi
}

# ---- T8: floppy and hard disk images, option bit 3: boot C:, A: present ---
t_T8() {
    local src ok
    need_rom T8 "$ROM" || return
    if [ -f "$WORK/hd-from-T3.img" ]; then src=$WORK/hd-from-T3.img
    else prep_hd_native || { report T8 FAIL "no hard disk image"; return; }; src=$WORK/hd-native.img; fi
    newwork T8
    cp "$src" "$W/hd.img"
    mk_floppy "$W/fd.img" "$FD_AUTO"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG "$W/hd.img" HD.IMG
    boot_rom_test T8 "$ROM" PGBOOT-HD-OK --fd FD.IMG --hd HD.IMG --opts 8; ok=$?
    if [ $ok = 0 ] && grep -q "A:" "$W/screen-early.txt" && grep -q "C:" "$W/screen-early.txt"; then
        report T8 PASS "both images presented, option bit 3 booted C:"
        grep -h "PGBOOT" "$W/screen-early.txt" | sed 's/^/      # /'
    else
        report T8 FAIL "no PGBOOT-HD-OK or not both drives presented; see $W"
        sed 's/^/      # /' "$W/screen.txt" 2>/dev/null
    fi
}

# ---- T9: floppy swap at run time (pgusinit /fdimage): DOS sees the new disk -
# FreeCOM re-opens AUTOEXEC.BAT by name for every line, at the byte offset it
# reached. The second floppy's AUTOEXEC.BAT repeats the first one up to the
# swap and continues with the checks, at a different cluster (a padding file
# comes first): the checks only run if DOS noticed the media change and
# re-read the FAT and the directory of the new disk.
T9_PRE='@ECHO OFF
ECHO PGBOOT-T9-START > COM1
PGUSINIT /fdimage FD2.IMG > COM1'
T9_POST='ECHO PGBOOT-T9-SWAPPED > COM1
DIR A:\ > COM1
TYPE A:\SECOND.TXT > COM1
ECHO PGBOOT-T9-DONE > COM1'
t_T9() {
    local ok
    newwork T9
    need_rom T9 "$ROM" || return
    build_dos_tools
    mk_floppy "$W/fd.img" "$T9_PRE" "$DOS/PGUSINIT.EXE"
    head -c 20000 /dev/zero > "$W/PADDING.BIN"
    echo second-floppy-content > "$W/SECOND.TXT"
    mk_floppy "$W/fd2.img" "x" "$DOS/PGUSINIT.EXE" "$W/PADDING.BIN" "$W/SECOND.TXT"
    mdel -i "$W/fd2.img" ::/AUTOEXEC.BAT
    printf '%s\n%s\n' "$T9_PRE" "$T9_POST" > "$W/auto2.bat"
    mcopy -t -i "$W/fd2.img" "$W/auto2.bat" ::/AUTOEXEC.BAT
    mlabel -i "$W/fd2.img" ::SECOND
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG "$W/fd2.img" FD2.IMG
    boot_rom_test T9 "$ROM" PGBOOT-T9-DONE --fd FD.IMG; ok=$?
    if [ $ok = 0 ] && grep -q "second-floppy-content" "$W/serial.txt" && grep -q "SECOND" "$W/serial.txt"; then
        report T9 PASS "pgusinit /fdimage swapped A: under DOS; new label, directory and file seen"
        excerpt "$W" 14
    else
        report T9 FAIL "DOS did not see the swapped floppy; see $W"
        excerpt "$W" 30
        sed 's/^/      # /' "$W/screen.txt" 2>/dev/null
    fi
}

# ---- T10: PGDFS next to PGBOOT: the stick as D:, the open image protected -
T10_AUTO='@ECHO OFF
ECHO PGBOOT-T10-START > COM1
PGDFS D: > COM1
DIR D:\ > COM1
TYPE D:\HELLO.TXT > COM1
COPY D:\HELLO.TXT A:\HELLO2.TXT > COM1
TYPE A:\HELLO2.TXT > COM1
DEL D:\FD.IMG > COM1
REN D:\FD.IMG GONE.IMG > COM1
ECHO PGBOOT-T10-DONE > COM1'
t_T10() {
    local ok del ren
    newwork T10
    need_rom T10 "$ROM" || return
    build_dos_tools
    mk_floppy "$W/fd.img" "$T10_AUTO" "$DOS/PGDFS.EXE"
    echo hello-from-the-stick > "$W/hello.txt"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG "$W/hello.txt" HELLO.TXT
    boot_rom_test T10 "$ROM" PGBOOT-T10-DONE --fd FD.IMG; ok=$?
    del=$(grep "REQ.*DELETE" -A3 "$W/sim.log" | grep -m1 "ANS" | sed 's/.*AX=\([0-9a-f]*\).*/\1/')
    ren=$(grep "REQ.*RENAME" -A3 "$W/sim.log" | grep -m1 "ANS" | sed 's/.*AX=\([0-9a-f]*\).*/\1/')
    mcopy -n -i "$W/stick.img" ::/FD.IMG "$W/fd-after.img" 2>/dev/null || ok=1
    if [ $ok = 0 ] && [ "$del" = 0005 ] && [ "$ren" = 0005 ] &&
       mtype -i "$W/fd-after.img" ::/HELLO2.TXT 2>/dev/null | grep -q hello-from-the-stick &&
       ! mdir -i "$W/stick.img" ::/GONE.IMG >/dev/null 2>&1; then
        report T10 PASS "PGDFS D: after a ROM boot; D:->A: copy; DEL/REN of the open FD.IMG refused (AX=05h)"
        excerpt "$W" 16
    else
        report T10 FAIL "DELETE AX=${del:-none} RENAME AX=${ren:-none}; see $W"
        excerpt "$W" 30
    fi
}

# ---- T11: PGDFS data window moved to 2A0h: the ROM follows CMD_DFSPORT ------
t_T11() {
    local ok
    newwork T11
    need_rom T11 "$ROM" || return
    mk_floppy "$W/fd.img" "$FD_AUTO"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG
    DATAPORT=0x2a0 boot_rom_test T11 "$ROM" PGBOOT-FD-OK --fd FD.IMG --dfsport 0x2a0; ok=$?
    if [ $ok = 0 ] && grep -q "2A0h" "$W/screen-early.txt"; then
        report T11 PASS "data window at 2A0h (bridge dataport=0x2a0, card CMD_DFSPORT 2A0h): floppy boot"
        grep -h "PGBOOT 0" "$W/screen-early.txt" | sed 's/^/      # /'
    else
        report T11 FAIL "see $W"
        sed 's/^/      # /' "$W/screen.txt" 2>/dev/null
    fi
}

# ---- T12-T17: the boot menu, the start-up tune, a ROM seen four times --------
SUB_AUTO='@ECHO OFF
ECHO PGBOOT-SUB-OK > COM1'

# menu_stick: the stick the menu tests browse. In the root: README.TXT (no
# image), FD.IMG (bootable floppy), HD.IMG (the bootable hard disk of T3/P4)
# and the folder "Boot Disks" with a second bootable floppy under a long name.
# The menu lists folders first: Boot Disks, FD.IMG, HD.IMG.
menu_stick() {
    local src
    if [ -f "$WORK/hd-from-T3.img" ]; then src=$WORK/hd-from-T3.img
    else prep_hd_native || return 1; src=$WORK/hd-native.img; fi
    cp "$src" "$W/hd.img"
    mk_floppy "$W/fd.img" "$FD_AUTO"
    mk_floppy "$W/sub.img" "$SUB_AUTO"
    echo x > "$W/x.txt"
    mk_stick "$W/stick.img" "$W/x.txt" README.TXT "$W/fd.img" FD.IMG "$W/hd.img" HD.IMG
    mmd -i "$W/stick.img" "::/Boot Disks"
    mcopy -i "$W/stick.img" "$W/sub.img" "::/Boot Disks/DOS Boot Disk.img"
}

# on_screen NAME REGEX...: every REGEX matches a line of the dump NAME.txt;
# prints the ones that do not
on_screen() {
    local n re miss
    n=$1; shift; miss=
    for re in "$@"; do
        grep -Eq -- "$re" "$W/$n.txt" 2>/dev/null || miss="$miss [$re]"
    done
    [ -z "$miss" ] && return 0
    echo "$miss"
    return 1
}

menu_fail() {  # menu_fail TEST MESSAGE: report, show the last screens
    report "$1" FAIL "$2; see $W"
    for f in "$W"/menu*.txt "$W/screen.txt"; do
        [ -f "$f" ] && { echo "      # --- $(basename "$f")"; sed 's/^/      # /' "$f"; }
    done
    finish
}

# T12: the menu at every boot, nothing configured: browse into a folder and
# boot the floppy image there (its 8.3 path goes to the card)
t_T12() {
    local m
    need_rom T12 "$ROM" || return
    newwork T12
    menu_stick || { report T12 FAIL "no hard disk image"; return; }
    start_sim "$W" --stick "$W/stick.img" --menu 2 || { report T12 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'HD\.IMG' 30 || { menu_fail T12 "the menu did not come up"; return; }
    m=$(on_screen menu1 'PicoGUS Boot +PGBOOT 0\.[0-9]+$' ' Drives$' ' A: +-$' ' C: +-$' '^ +\\$' ' 1/3$' \
        '<DIR> +Boot Disks$' 'A: +1440K +FD\.IMG$' 'C: +31M +HD\.IMG$' 'Enter=Boot Space=Insert') ||
        { menu_fail T12 "root listing: missing$m"; return; }
    grep -q 'README' "$W/menu1.txt" && { menu_fail T12 "README.TXT is listed"; return; }
    send_keys end
    wait_screen "$W" menu2 ' 3/3' 10 || { menu_fail T12 "End did not reach entry 3"; return; }
    send_keys up pgup ret           # 2/3, 1/3 (the folder), open it
    wait_screen "$W" menu3 'DOS Boot Disk\.img' 10 || { menu_fail T12 "the folder did not open"; return; }
    m=$(on_screen menu3 '^ +\\BOOTDI~1$' ' 1/2$' '<DIR> +\.\.$' 'A: +1440K +DOS Boot Disk\.img$') ||
        { menu_fail T12 "folder listing: missing$m"; return; }
    grep -q 'HD\.IMG' "$W/menu3.txt" && { menu_fail T12 "the root's rows were not cleared"; return; }
    send_keys ret                   # "..": back up
    wait_screen "$W" menu4 ' 1/3$' 10 || { menu_fail T12 "'..' did not lead back to the root"; return; }
    send_keys ret down ret          # into the folder again, the image, boot it
    if wait_marker "$W" PGBOOT-SUB-OK "$TIMEOUT"; then
        sleep 1; screen_dump "$W" screen
        finish
        if grep -q 'BDFDNAME commit "\\BOOTDI~1\\DOSBOO~1.IMG"' "$W/sim.log"; then
            report T12 PASS "boot menu: browsed into \"Boot Disks\", booted \"DOS Boot Disk.img\" as A:"
            sed 's/^/      # /' "$W/menu3.txt"
        else
            report T12 FAIL "booted, but the card was not given \\BOOTDI~1\\DOSBOO~1.IMG; see $W"
        fi
    else
        menu_fail T12 "the image picked in the menu did not boot"
    fi
}

# T13: insert the floppy image without booting it, save, boot the hard disk
t_T13() {
    local m
    need_rom T13 "$ROM" || return
    newwork T13
    menu_stick || { report T13 FAIL "no hard disk image"; return; }
    start_sim "$W" --stick "$W/stick.img" --menu 2 || { report T13 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'HD\.IMG' 30 || { menu_fail T13 "the menu did not come up"; return; }
    send_keys down spc              # FD.IMG into A:
    wait_screen "$W" menu2 ' A: +\\FD\.IMG$' 10 || { menu_fail T13 "Space did not insert FD.IMG"; return; }
    send_keys s
    wait_screen "$W" menu3 'Saved' 10 || { menu_fail T13 "S did not save"; return; }
    send_keys down ret              # HD.IMG: boot it
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    if wait_marker "$W" PGBOOT-HD-OK "$TIMEOUT"; then
        sleep 1; screen_dump "$W" screen
        finish
        m=$(on_screen screen-early 'PGBOOT: A: \\FD\.IMG, 1440K' 'PGBOOT: C: \\HD\.IMG, 31M' 'PGBOOT: booting C:') ||
            { report T13 FAIL "boot messages: missing$m; see $W"; return; }
        if grep -q 'CMD_SAVE: settings saved (fd="\\FD.IMG" hd=""' "$W/sim.log" &&
           grep -q 'BDHDNAME commit "\\HD.IMG"' "$W/sim.log"; then
            report T13 PASS "boot menu: FD.IMG inserted into A: and saved, HD.IMG booted as C:"
            grep -h "PGBOOT" "$W/screen-early.txt" | sed 's/^/      # /'
        else
            report T13 FAIL "booted, but the card trace lacks the save or the hard disk name; see $W"
        fi
    else
        menu_fail T13 "the hard disk image picked in the menu did not boot"
    fi
}

# T14: a configured floppy image, M at the prompt, eject it, Esc: nothing is
# left to boot, so the BIOS boots the native floppy
t_T14() {
    need_rom T14 "$ROM" || return
    newwork T14
    menu_stick || { report T14 FAIL "no hard disk image"; return; }
    mk_floppy "$W/native.img" "$NATIVE_AUTO"
    start_sim "$W" --stick "$W/stick.img" --fd FD.IMG --menu $((8 * 4 + 1)) || { report T14 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -drive "file=$W/native.img,format=raw,if=floppy" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'press M for the boot menu' 30 || { menu_fail T14 "no prompt"; return; }
    send_keys m
    wait_screen "$W" menu2 ' A: +FD\.IMG$' 10 || { menu_fail T14 "M did not open the menu with FD.IMG in A:"; return; }
    send_keys a
    wait_screen "$W" menu3 ' A: +-$' 10 || { menu_fail T14 "A did not eject the floppy image"; return; }
    send_keys esc
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    if wait_marker "$W" PGBOOT-NATIVE-OK "$TIMEOUT" && grep -q 'PGBOOT: no disk image configured' "$W/screen-early.txt"; then
        finish
        report T14 PASS "M at the prompt opened the menu; A ejected FD.IMG; Esc: nothing configured, BIOS boot"
    else
        menu_fail T14 "no BIOS boot after the image was ejected in the menu"
    fi
}

# T15: nothing configured, default settings: the tune plays, the prompt runs
# out, the BIOS boots. The speaker gate (port 61h) is traced by QEMU.
t_T15() {
    local on off ok
    need_rom T15 "$ROM" || return
    newwork T15
    mk_floppy "$W/native.img" "$NATIVE_AUTO"
    echo x > "$W/x.txt"
    mk_stick "$W/stick.img" "$W/x.txt" README.TXT
    start_sim "$W" --stick "$W/stick.img" || { report T15 FAIL "sim did not start"; return; }
    QEMU_EXTRA="-trace pcspk_io_write -D $W/trace.log" \
        start_qemu "$W" -option-rom "$ROM" -drive "file=$W/native.img,format=raw,if=floppy" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'press M for the boot menu' 30
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    wait_marker "$W" PGBOOT-NATIVE-OK "$TIMEOUT"; ok=$?
    finish
    # tune 1 is four notes: four writes with timer gate and speaker data on
    # (bits 0 and 1), then one that takes both away
    on=$(grep -c 'pcspk_io_write.*<- 0x[0-9a-f]*[37bf]$' "$W/trace.log" 2>/dev/null)
    off=$(tail -1 "$W/trace.log" 2>/dev/null | grep -c '<- 0x[0-9a-f]*[048c]$')
    if [ $ok = 0 ] && grep -q 'press M for the boot menu' "$W/menu1.txt" &&
       grep -q 'PGBOOT: no disk image configured' "$W/screen-early.txt" && [ "${on:-0}" = 4 ] && [ "${off:-0}" = 1 ]; then
        report T15 PASS "defaults, nothing configured: tune played ($on notes, speaker off after), prompt ran out, BIOS boot"
    else
        report T15 FAIL "marker $ok, speaker on $on times, off at the end $off; see $W"
        sed 's/^/      # /' "$W/screen-early.txt" 2>/dev/null
    fi
}

# T16: Esc at the prompt skips PGBOOT although an image is configured
t_T16() {
    need_rom T16 "$ROM" || return
    newwork T16
    mk_floppy "$W/fd.img" "$FD_AUTO"
    mk_floppy "$W/native.img" "$NATIVE_AUTO"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG
    start_sim "$W" --stick "$W/stick.img" --fd FD.IMG --menu $((8 * 4 + 1)) --tune 0 || { report T16 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -drive "file=$W/native.img,format=raw,if=floppy" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'press M for the boot menu' 30 || { menu_fail T16 "no prompt"; return; }
    send_keys esc
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    if wait_marker "$W" PGBOOT-NATIVE-OK "$TIMEOUT" && grep -q 'PGBOOT: skipped' "$W/screen-early.txt"; then
        finish
        report T16 PASS "Esc at the prompt: PGBOOT skipped, the BIOS booted the native floppy"
    else
        menu_fail T16 "Esc at the prompt did not skip PGBOOT"
    fi
}

# T17: the BIOS finds the ROM four times (a 28C256 with the image in every
# quarter behind a 32 KB window): one banner, one hook, a normal boot
t_T17() {
    local n ok
    need_rom T17 "$ROM" || return
    newwork T17
    mk_floppy "$W/fd.img" "$FD_AUTO"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG
    start_sim "$W" --stick "$W/stick.img" --fd FD.IMG --menu 0 --tune 0 || { report T17 FAIL "sim did not start"; return; }
    for n in 1 2 3 4; do cp "$ROM" "$W/COPY$n.ROM"; done    # QEMU wants a name per ROM
    start_qemu "$W" -option-rom "$W/COPY1.ROM" -option-rom "$W/COPY2.ROM" -option-rom "$W/COPY3.ROM" \
        -option-rom "$W/COPY4.ROM" -boot menu=off $(bridge_args)
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    wait_marker "$W" PGBOOT-FD-OK "$TIMEOUT"; ok=$?
    finish
    n=$(grep -c 'PGBOOT 0.*PicoGUS found' "$W/screen-early.txt" 2>/dev/null)
    if [ $ok = 0 ] && [ "${n:-0}" = 1 ]; then
        report T17 PASS "four copies of the ROM: one banner, FreeDOS booted from FD.IMG"
    else
        report T17 FAIL "marker $ok, $n banners; see $W"
        sed 's/^/      # /' "$W/screen-early.txt" 2>/dev/null
    fi
}

# T18: pgusinit sets the boot menu, the colours, the tune and the user's
# melody, and shows them
T18_AUTO='@ECHO OFF
ECHO PGBOOT-T18-START > COM1
PGUSINIT /bdmenu 2 /bdwait 7 /bdtune 3 > COM1
PGUSINIT > COM1
PGUSINIT /bdmenu 1 /bdtheme commander /bdtune 7 > COM1
PGUSINIT > COM1
PGUSINIT /bdmelody Mine:d=4,o=5,b=120:c,e,g /bdtune custom > COM1
PGUSINIT > COM1
PGUSINIT /bdmelody @A:\MEL.TXT /bdtheme 2 > COM1
PGUSINIT > COM1
PGUSINIT /bdtune off > COM1
PGUSINIT > COM1
ECHO PGBOOT-T18-DONE > COM1'
t_T18() {
    local ok m
    newwork T18
    need_rom T18 "$ROM" || return
    build_dos_tools
    printf 'From a file:d=8,o=5,b=140:c,d,e,f,g\r\nnot part of it\r\n' > "$W/MEL.TXT"
    mk_floppy "$W/fd.img" "$T18_AUTO" "$DOS/PGUSINIT.EXE" "$W/MEL.TXT"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG
    boot_rom_test T18 "$ROM" PGBOOT-T18-DONE --fd FD.IMG --menu 0 --tune 0; ok=$?
    sed -e 's/\r$//' "$W/serial.txt" > "$W/serial.lf"
    m=
    grep -q 'PGBOOT boot menu: opens at every boot; colours: Setup' "$W/serial.lf" || m="$m [status after /bdmenu 2]"
    grep -q 'PGBOOT start-up tune: 3 (Sweep)' "$W/serial.lf" || m="$m [status after /bdtune 3]"
    grep -q 'PGBOOT boot menu: offered at boot (press M), for 7 s; colours: Commander' "$W/serial.lf" || m="$m [status after /bdmenu 1 /bdtheme commander]"
    grep -q 'PGBOOT start-up tune: 7 (Mountain King)' "$W/serial.lf" || m="$m [status after /bdtune 7]"
    grep -q 'PGBOOT start-up tune: custom (Mine)' "$W/serial.lf" || m="$m [status after /bdmelody and /bdtune custom]"
    grep -q 'PGBOOT start-up tune: custom (From a file)' "$W/serial.lf" || m="$m [status after /bdmelody @file]"
    grep -q 'colours: Mono' "$W/serial.lf" || m="$m [status after /bdtheme 2]"
    grep -q 'PGBOOT start-up tune: off' "$W/serial.lf" || m="$m [status after /bdtune off]"
    grep -q 'BDMENU <- 0e' "$W/sim.log" && grep -q 'BDMENU <- 1e' "$W/sim.log" && grep -q 'BDMENU <- 1d' "$W/sim.log" ||
        m="$m [CMD_BDMENU writes 0e, 1e, 1d]"
    grep -q 'BDMELODY commit "From a file:d=8,o=5,b=140:c,d,e,f,g"' "$W/sim.log" || m="$m [the melody from the file, first line only]"
    if [ $ok = 0 ] && [ -z "$m" ]; then
        report T18 PASS "pgusinit /bdmenu /bdwait /bdtheme /bdtune /bdmelody set the card's registers and the status shows them"
        grep 'PGBOOT boot menu\|PGBOOT start-up' "$W/serial.lf" | sort -u | sed 's/^/      | /'
    else
        report T18 FAIL "missing:$m; see $W"
        excerpt "$W" 40
    fi
}

# T19: seven folders deep: the folder row shows the end of a path that is too
# long for it, R reads the folder again, the image at the bottom boots
t_T19() {
    local d i m
    need_rom T19 "$ROM" || return
    newwork T19
    mk_floppy "$W/sub.img" "$SUB_AUTO"
    echo x > "$W/x.txt"
    mk_stick "$W/stick.img" "$W/x.txt" README.TXT
    d=
    for i in 1 2 3 4 5 6 7; do d=$d/DEEPDIR$i; mmd -i "$W/stick.img" "::$d"; done
    mcopy -i "$W/stick.img" "$W/sub.img" "::$d/BOTTOM.IMG"
    start_sim "$W" --stick "$W/stick.img" --menu 2 || { report T19 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 '<DIR> +DEEPDIR1$' 30 || { menu_fail T19 "the menu did not come up"; return; }
    send_keys ret down ret down ret down ret down ret down ret down ret
    wait_screen "$W" menu2 'BOTTOM\.IMG' 15 || { menu_fail T19 "did not reach the seventh folder"; return; }
    # 63 characters of path in a frame that has room for 38: "..." and its last 35
    m=$(on_screen menu2 '^ +\.\.\.DEEPDIR4\\DEEPDIR5\\DEEPDIR6\\DEEPDIR7$' ' 1/2$' \
        '<DIR> +\.\.$' 'A: +1440K +BOTTOM\.IMG$') || { menu_fail T19 "deep folder: missing$m"; return; }
    send_keys r
    sleep 1
    wait_screen "$W" menu3 'BOTTOM\.IMG' 10 || { menu_fail T19 "R lost the listing"; return; }
    send_keys down ret
    if wait_marker "$W" PGBOOT-SUB-OK "$TIMEOUT"; then
        finish
        if grep -q 'BDFDNAME commit "\\DEEPDIR1\\DEEPDIR2\\DEEPDIR3\\DEEPDIR4\\DEEPDIR5\\DEEPDIR6\\DEEPDIR7\\BOTTOM.IMG"' "$W/sim.log" &&
           [ "$(grep -c 'BDLIST *len=71 ' "$W/sim.log")" -ge 2 ]; then   # 8 + the 63 characters of the path, twice
            report T19 PASS "seven folders deep: path cut to its end on the folder row, R reread it, BOTTOM.IMG booted"
        else
            report T19 FAIL "booted, but the card trace lacks the full path or the second listing; see $W"
        fi
    else
        menu_fail T19 "the image seven folders deep did not boot"
    fi
}

# T20: the menu at every boot on a stick without images: it says so, Enter
# looks again, Esc goes on to the BIOS boot
t_T20() {
    need_rom T20 "$ROM" || return
    newwork T20
    mk_floppy "$W/native.img" "$NATIVE_AUTO"
    echo x > "$W/x.txt"
    mk_stick "$W/stick.img" "$W/x.txt" README.TXT
    start_sim "$W" --stick "$W/stick.img" --menu 2 --tune 0 || { report T20 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -drive "file=$W/native.img,format=raw,if=floppy" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'No disk images or folders here' 30 || { menu_fail T20 "no message for an empty stick"; return; }
    send_keys down ret
    sleep 1
    wait_screen "$W" menu2 'No disk images or folders here' 10 || { menu_fail T20 "Enter on the empty list did not read it again"; return; }
    send_keys esc
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    if wait_marker "$W" PGBOOT-NATIVE-OK "$TIMEOUT" && grep -q 'PGBOOT: no disk image configured' "$W/screen-early.txt" &&
       [ "$(grep -c 'BDLIST.*start=0 max=16' "$W/sim.log")" -ge 2 ]; then
        finish
        report T20 PASS "empty stick: the menu says so, Enter reads it again, Esc: BIOS boot"
    else
        menu_fail T20 "no BIOS boot after the menu on an empty stick"
    fi
}

# T21: the three themes of the web page: T moves on, the colours follow, S saves.
# Cells looked at: the title bar, the list window's frame, the selected entry,
# the screen behind the windows, the drives window, a key on the status line.
theme_cells() {  # theme_cells NAME: their attributes
    echo "$(attr_at "$W" "$1" 0 40) $(attr_at "$W" "$1" 1 19) $(attr_at "$W" "$1" 2 30) $(attr_at "$W" "$1" 10 2)" \
         "$(attr_at "$W" "$1" 21 30) $(attr_at "$W" "$1" 24 1)"
}
t_T21() {
    local c
    need_rom T21 "$ROM" || return
    newwork T21
    menu_stick || { report T21 FAIL "no hard disk image"; return; }
    start_sim "$W" --stick "$W/stick.img" --menu 2 --tune 0 || { report T21 FAIL "sim did not start"; return; }
    start_qemu "$W" -option-rom "$ROM" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'HD\.IMG' 30 || { menu_fail T21 "the menu did not come up"; return; }
    c=$(theme_cells menu1)
    [ "$c" = "70 1f 4f 10 70 71" ] || { menu_fail T21 "Setup colours are '$c', not '70 1f 4f 10 70 71'"; return; }
    send_keys t
    wait_attr "$W" menu2 0 40 30 10 || { menu_fail T21 "T did not bring the Commander title bar"; return; }
    c=$(theme_cells menu2)
    [ "$c" = "30 30 0f 07 70 0f" ] || { menu_fail T21 "Commander colours are '$c', not '30 30 0f 07 70 0f'"; return; }
    grep -Eq 'A: +1440K +FD\.IMG$' "$W/menu2.txt" || { menu_fail T21 "the list was lost with the theme"; return; }
    send_keys s
    wait_screen "$W" menu3 'Saved' 10 || { menu_fail T21 "S did not save"; return; }
    send_keys t
    wait_attr "$W" menu4 1 19 07 10 || { menu_fail T21 "T did not bring the Mono list window"; return; }
    c=$(theme_cells menu4)
    [ "$c" = "70 07 70 08 07 70" ] || { menu_fail T21 "Mono colours are '$c', not '70 07 70 08 07 70'"; return; }
    send_keys t
    wait_attr "$W" menu5 1 19 1f 10 || { menu_fail T21 "T did not come round to Setup again"; return; }
    send_keys esc
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    finish
    if grep -q 'CMD_SAVE: settings saved .*theme=1 ' "$W/sim.log" && [ "$(attr_at "$W" screen-early 0 40)" = "07" ]; then
        report T21 PASS "themes: Setup, Commander (saved), Mono and round again; plain screen after the menu"
    else
        report T21 FAIL "the saved theme is not 1, or the screen kept the menu's colours; see $W"
    fi
}

# T22: tunes: off at boot, M moves on and plays, with the right lengths; the
# user's melody after the last built-in one. QEMU traces the speaker gate
# with the time of every write.
gate_ms() {  # gate_ms FILE: milliseconds from the first speaker-on write to the last write
    python3 - "$1" <<'PY'
import re, sys
t = []
for line in open(sys.argv[1]):
    m = re.match(r'\S*T(\d\d):(\d\d):(\d\d)\.(\d{6})Z pcspk_io_write .*<- 0x([0-9a-f]+)', line)
    if m:
        h, mi, sec, us = (int(m.group(i)) for i in range(1, 5))
        t.append((((h * 60 + mi) * 60 + sec) * 1000 + us // 1000, int(m.group(5), 16) & 3))
on = [x for x in t if x[1] == 3]
print(t[-1][0] - on[0][0] if on else -1, len(on))
PY
}
t_T22() {
    local r ms n
    need_rom T22 "$ROM" || return
    newwork T22
    menu_stick || { report T22 FAIL "no hard disk image"; return; }
    start_sim "$W" --stick "$W/stick.img" --menu 2 --tune 0 --melody 'Beep:d=4,o=5,b=120:c,p,c' ||
        { report T22 FAIL "sim did not start"; return; }
    QEMU_EXTRA="-trace pcspk_io_write -msg timestamp=on -D $W/trace.log" \
        start_qemu "$W" -option-rom "$ROM" -boot menu=off $(bridge_args)
    wait_screen "$W" menu1 'HD\.IMG' 30 || { menu_fail T22 "the menu did not come up"; return; }
    [ -s "$W/trace.log" ] && { menu_fail T22 "the speaker was touched although the tune is off"; return; }
    send_keys m                     # tune 1: Rising, 550 ms
    wait_screen "$W" menu2 'Start-up tune: Rising$' 10 || { menu_fail T22 "M did not show tune 1"; return; }
    sleep 2
    r=$(gate_ms "$W/trace.log"); ms=${r% *}; n=${r#* }
    { [ "$n" = 4 ] && [ "$ms" -ge 480 ] && [ "$ms" -le 700 ]; } ||
        { menu_fail T22 "tune 1 took $ms ms with $n notes; 550 ms and 4 expected"; return; }
    # on to the user's melody: each M ends the tune before it
    send_keys m m m m m m m m m m m m m
    wait_screen "$W" menu3 'Start-up tune: Beep$' 20 || { menu_fail T22 "M did not reach the user's melody"; return; }
    : > "$W/trace.log"
    sleep 1
    send_keys m                     # off
    wait_screen "$W" menu4 'Start-up tune: Off$' 10 || { menu_fail T22 "M after the user's melody is not off"; return; }
    send_keys s esc
    wait_screen "$W" screen-early "$ROM_DONE_RE" 30
    finish
    if grep -q 'tune is now 15' "$W/sim.log" && grep -q 'CMD_SAVE: settings saved .*tune=0 ' "$W/sim.log"; then
        report T22 PASS "tunes: off at boot, M played Rising in $ms ms, reached the user's melody, off again and saved"
    else
        report T22 FAIL "the card trace lacks tune 15 or the save with tune 0; see $W"
    fi
}

for t in $TESTS; do
    case $t in
        S1|T0|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11|T12|T13|T14|T15|T16|T17|T18|T19|T20|T21|T22) "t_$t" ;;
        P4) rm -f "$WORK/hd-native.img"
            if prep_hd_native; then report P4 PASS "native FDISK /MBR + SYS C: reference image ($WORK/hd-native.img)"
            else report P4 FAIL "see $WORK/P4"; fi ;;
        *) die "unknown test $t" ;;
    esac
done

echo
echo "== summary"
printf '%s' "$SUMMARY"
if [ "${KEEP:-0}" != 1 ]; then
    # Keep the evidence of failures only; stick images are large.
    printf '%s' "$SUMMARY" | while read -r t st _; do
        [ "$st" = PASS ] && rm -f "$WORK/$t"/*.img
    done
fi
printf '%s' "$SUMMARY" | grep -q ' FAIL ' && exit 1
exit 0
