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
TESTS=${*:-T0 S1 T1 T2 T3 T4 T5 T6 T7 T8 T9 T10 T11}
TIMEOUT=${TIMEOUT:-90}
ROM=$ROMDIR/PGBOOT.ROM
ROM88=$ROMDIR/PGBOOT88.ROM
# The ROM's message when no card answers (T6); a regex for grep -E.
ROM_NOTFOUND_RE=${ROM_NOTFOUND_RE:-PicoGUS.*not found|not found|no PicoGUS}

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
    sleep 3; screen_dump "$W" screen-early
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
    [ "$(dd if="$img" bs=1 skip=510 count=2 2>/dev/null | xxd -p)" = 55aa ] || miss="$miss MBR-signature"
    [ "$(dd if="$img" bs=1 count=64 2>/dev/null | tr -d '\000' | wc -c | tr -d ' ')" -gt 0 ] || miss="$miss MBR-code"
    off=$((HD_S * 512))
    [ "$(dd if="$img" bs=1 skip=$((off + 510)) count=2 2>/dev/null | xxd -p)" = 55aa ] || miss="$miss boot-signature"
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
    if [ $ok = 0 ]; then
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
    sleep 3; screen_dump "$W" screen-early
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

# ---- T10: PGUSDFS next to PGBOOT: the stick as D:, the open image protected -
T10_AUTO='@ECHO OFF
ECHO PGBOOT-T10-START > COM1
PGUSDFS D: > COM1
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
    mk_floppy "$W/fd.img" "$T10_AUTO" "$DOS/PGUSDFS.EXE"
    echo hello-from-the-stick > "$W/hello.txt"
    mk_stick "$W/stick.img" "$W/fd.img" FD.IMG "$W/hello.txt" HELLO.TXT
    boot_rom_test T10 "$ROM" PGBOOT-T10-DONE --fd FD.IMG; ok=$?
    del=$(grep "REQ.*DELETE" -A3 "$W/sim.log" | grep -m1 "ANS" | sed 's/.*AX=\([0-9a-f]*\).*/\1/')
    ren=$(grep "REQ.*RENAME" -A3 "$W/sim.log" | grep -m1 "ANS" | sed 's/.*AX=\([0-9a-f]*\).*/\1/')
    mcopy -n -i "$W/stick.img" ::/FD.IMG "$W/fd-after.img" 2>/dev/null || ok=1
    if [ $ok = 0 ] && [ "$del" = 0005 ] && [ "$ren" = 0005 ] &&
       mtype -i "$W/fd-after.img" ::/HELLO2.TXT 2>/dev/null | grep -q hello-from-the-stick &&
       ! mdir -i "$W/stick.img" ::/GONE.IMG >/dev/null 2>&1; then
        report T10 PASS "PGUSDFS D: after a ROM boot; D:->A: copy; DEL/REN of the open FD.IMG refused (AX=05h)"
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

for t in $TESTS; do
    case $t in
        S1|T0|T1|T2|T3|T4|T5|T6|T7|T8|T9|T10|T11) "t_$t" ;;
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
