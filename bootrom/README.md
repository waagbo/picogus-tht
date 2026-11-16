# PGBOOT: boot floppy and hard disk images from the PicoGUS USB drive

**Experimental.** PGBOOT is an 8 KB x86 option ROM. It presents disk image
files on the USB drive plugged into a PicoGUS to the BIOS as a floppy drive
(A:, BIOS drive 00h) and/or a hard disk (C:, BIOS drive 80h), and it can boot
from them. The PicoGUS only decodes I/O ports and cannot carry a ROM itself,
so the ROM goes into an EEPROM on a separate ROM card, or it is loaded into
RAM by `PGBOOT.COM`.

The card side (`sw/bootdisk/`) serves 512-byte sectors of the image files
over the PGDFS transport. The wire protocol is described in
`sw/bootdisk/PROTOCOL.md` and `sw/dfs/PROTOCOL.md`.

| File | What it is |
|---|---|
| `PGBOOT.ROM` | The option ROM (8192 bytes: `55 AA 10`, init entry at offset 3, checksum byte at the end). |
| `PGBOOT88.ROM` | The same code, but it always uses the 8088 transfer loops. This is for testing; on an XT the normal ROM picks those loops by itself. |
| `PGBOOT.COM` | Loader for machines without a ROM board (experimental). |
| `PGFLASH.COM` | Writes the ROM into a 28C64/28C256 EEPROM in the machine (experimental). |
| `test/INT13TST.COM` | INT 13h exerciser used by the tests. It is not needed to boot. |

## Building

You need `nasm` (2.15 or later) and `python3`.

```
make            # PGBOOT.ROM PGBOOT88.ROM PGBOOT.COM PGFLASH.COM test/INT13TST.COM
make check      # size/header/checksum of both ROMs, and no 186+ opcode outside
                # the two 186+ transfer routines (cpucheck.py on the listing)
```

All sources assemble with `cpu 8086` and with every NASM warning treated as
an error. Only two routines are exceptions: `out186` (`rep outsw`) and
`in186` (`rep insw`). They are assembled with `cpu 186` and only run after the
CPU check has found an 80186 or later. `mkrom.py` pads the image, sets the
checksum byte and verifies it. The header word at 14h holds the number of
bytes in use: about 4.3 KB of the 8 KB.

## Setting up the card

Copy the image files to the USB drive and configure the card with pgusinit:

```
pgusinit /fdimage DOS622.IMG     floppy image -> A: (raw 160K..2.88M, DMF, or any size with a FAT BPB)
pgusinit /hdimage HD\DOS.VHD     hard disk image -> C: (raw with MBR, fixed VHD, superfloppy)
pgusinit /bdopts n               bit 0 floppy read-only, bit 1 hard disk read-only,
                                 bit 2 ROM disabled, bit 3 boot the hard disk even with a floppy image
pgusinit /save                   make it permanent
pgusinit /fdimage -              remove the floppy image (same for /hdimage)
```

You can change the floppy image while the system is running: the ROM
reports a media change (INT 13h AH=16h) to DOS. A new hard disk image takes
effect at the next boot.

## Installing the ROM

### On a ROM card (DoubleROM-style, XT-IDE boards with a free socket, ...)

1. Choose a free 8 KB window, for example C800h, D000h, D800h or E000h. It
   must not overlap the VGA BIOS (C000h-C7FFh), a network boot ROM or an
   XTIDE ROM. **Order matters with other disk ROMs.** The BIOS scans option
   ROMs from low to high segments, so each ROM that hooks INT 19h wraps the
   ones scanned before it, and the last one scanned runs first at boot.
   * Some ROMs replace INT 19h without chaining to the old handler: XT
     fixed-disk controller ROMs (the IBM/WD/Xebec family) and the XTIDE
     Universal BIOS in its default mode, which runs its own boot menu.
     PGBOOT must be scanned **after** such a ROM, so give it a higher
     segment. Scanned first, PGBOOT's INT 19h never runs.
   * The XTIDE Universal BIOS configured for late initialisation hooks
     INT 19h and INT 13h at boot time itself. Then both ROMs rearrange the
     drive numbers when their INT 19h runs, and the one that runs second
     sees the other's drives as physical. Use XUB in normal (POST-time)
     mode with PGBOOT above it, or expect the image at 80h to shift.
   * BIOSes with a BIOS Boot Specification boot manager (most 1996+ AT
     BIOSes, "boot device priority" menus) may restore their own INT 19h
     before booting, or boot through their own list without calling
     INT 19h. PGBOOT then never runs its late init. Such machines need a
     BIOS that keeps legacy option ROM INT 19h hooks, or `PGBOOT.COM`.
2. Program the EEPROM. You have three ways to do it:
   * `PGFLASH D000 PGBOOT.ROM` in the target machine. Boot clean first (no
     EMM386 or QEMM) and set the board's write-enable jumper. `/256` sends
     the 28C256 SDP command addresses, but only when the board decodes the
     chip's full 32 KB. `/NOSDP` writes without the unlock sequence. `/BYTE`
     is for chips without page mode. PGFLASH only reads until you confirm.
     It shows what the segment holds now and warns about other option ROMs
     in the same 32 KB window, which a 28C256 or a board decoding 32 KB may
     share. After you confirm, it checks that the segment is not RAM and
     writes. Interrupts stay off from each SDP unlock until the write cycle
     ends. It refuses to overwrite a PGBOOT that is running from that
     segment.
   * XTIDECFG (from the XTIDE Universal BIOS): load `PGBOOT.ROM` as a
     "BIOS image" and flash it to the segment. Skip its "configure" step,
     because that is XTIDE-specific.
   * An external programmer such as the TL866 or T48: write `PGBOOT.ROM` to
     the chip at address 0. On a 28C256 in a board that maps an 8 KB window,
     write it at the offset the board's address jumpers select.
3. Tell memory managers to keep out of the window, for example
   `DEVICE=EMM386.EXE X=D000-D1FF`. The same goes for QEMM, 386MAX and the
   UMB drivers.
4. If the BIOS shadows option ROMs, it may shadow this window too. Shadowing
   is harmless for PGBOOT, which never writes to its own segment. Only
   PGFLASH needs shadowing off for that window.

### Without a ROM card: PGBOOT.COM (experimental)

```
PGBOOT [path\PGBOOT.ROM] [/Y] [/C]
```

`PGBOOT.COM` reads the image (default: `PGBOOT.ROM` in the current directory)
and checks its size, header, signature and checksum. It copies the image to
the top of conventional memory: 40:13h is lowered by 8 KB, so the copy is KB
aligned. It tells the copy where the original BIOS INT 13h handler is, calls
the copy's init entry just as the BIOS would at POST, and then issues
INT 19h. If the ROM does not install (no card, disabled, ...), the loader
puts back the memory and vectors it touched and returns to DOS.

**Run it from a clean boot.** INT 19h does not undo what DOS and TSRs have
hooked. The booted system overwrites DOS in memory while a timer, keyboard
or disk IRQ vector may still point into it. The loader lists such vectors
(08h, 09h, 0Eh, 15h, 1Ch, 70h, 74h, 76h in conventional memory or the HMA)
and asks before it goes on. The fix is a minimal CONFIG.SYS: `STACKS=0,0`
(FreeDOS and MS-DOS hook the IRQ vectors for their stack switcher
otherwise), no TSRs, no HIMEM, no EMM386. The loader refuses to run in
virtual 8086 mode. `/Y` skips the question.

`/C` (catch mode, MS-DOS 3.2 or later only; untested) keeps DOS's own INT 19h
in charge, so that DOS restores the vectors it hooked. The loader points
INT 13h at a boot catcher in the ROM copy, directly and through INT 2Fh
AH=13h. The BIOS's first INT 13h call after DOS's INT 19h then runs the late
init. FreeDOS has no INT 2Fh AH=13h and restores INT 13h on its own, so `/C`
is refused there.

#### Loader <-> ROM convention (header fields)

| Offset | Size | Field |
|---|---|---|
| 06h | 4 | `PGBT` signature |
| 0Ah | 1 | loader interface version, 1 |
| 0Bh | 1 | flags. Bit 0: this image is a writable RAM copy, set by the loader. Bit 1: catch mode active, set by the catcher. |
| 0Ch | 4 | far pointer: the INT 13h handler the ROM chains to. 0:0 means "the vector at INT 19h time". The loader stores the BIOS handler from INT 2Fh AH=13h (ES:BX, else DS:DX, else the current vector). |
| 10h | 4 | previous INT 19h. Written by the init entry only when bit 0 is set. |
| 14h | 2 | bytes used, for information only |
| 16h | 1 | `IRET`. The loader points INT 1Bh (Ctrl-Break) at it. |
| 1Ch | 2 | offset of the boot catcher (`/C`) |

The loader fixes the checksum byte after it patches these fields.

## What happens at boot

1. **POST** (the BIOS far-calls offset 3). The ROM looks for the card with
   these checks: knock CCh on 1D0h; `CMD_MAGIC` = DDh; `CMD_PROTOCOL` >= 5;
   `CMD_DFSMAXLEN` in 512..32768; `CMD_DFSPORT` != 0; `CMD_BDOPTS` with
   bit 7 set, bit 6 clear and bit 2 (ROM disabled) clear. The ROM prints one
   line. If the card is there, it hooks INT 19h and nothing else. If
   nothing answers at all, it still hooks INT 19h: a fast AT POST can reach
   the option ROMs while the card is still booting. The late init then
   gives the card another 2 s and continues with the BIOS boot if it is
   still absent. If the card answers but reports old firmware, PGDFS off
   or the ROM disabled, nothing is hooked. A ROM
   cannot write to itself: an unprotected EEPROM would take the write. So
   the ROM keeps the previous INT 19h in interrupt vector **6Bh** (free
   between POST and boot) until the late init copies it to RAM. A loaded
   copy keeps it in its header instead.
2. **INT 19h (late init).** The ROM checks the card again and takes 1 KB at
   the top of conventional memory (40:13h - 1) for all its state. It then
   runs on its own stack at the top of that block. If no image name is
   configured, it continues with the BIOS boot at once. Otherwise it waits
   up to 15 s for the USB drive (`CMD_DFSSTAT` = FFh, or BDINFO state 4,
   means "not mounted yet"). A countdown is shown and Esc skips the wait
   (other keys, such as F5/F8 for DOS, stay in the keyboard buffer). The
   ROM then sends BDINFO with OPEN for unit 0 (floppy) and unit 1 (hard
   disk), prints a line per configured unit, adjusts the BIOS data area,
   hooks INT 13h and reads sector 0 of the boot image to 0000:7C00. It boots
   the floppy image first, or the hard disk image when option bit 3 is set or
   there is no floppy image. A hard disk needs 55AAh at the end of the
   sector. A floppy boot sector without the signature is accepted when it
   starts with a jump (EBh/E9h). The ROM then jumps to 0000:7C00 with
   DL = drive, DH = 0, SS:SP = 0000:7C00 and the other registers 0. If the
   first image does not boot, it tries the other one. If neither boots, it
   continues with the BIOS boot, and the images stay installed as A:/C:.
   When no image is ready, the ROM gives the memory back, unhooks INT 19h
   and continues with the BIOS boot.
3. **Running system.** The INT 13h handler serves drives 00h and 80h from the
   card. It passes other drives through to the previous handler, translating
   them as described in the next section.

### Drive mapping

| BIOS drive | Floppy image configured | Hard disk image configured |
|---|---|---|
| 00h | the floppy image (the physical A: is hidden) | physical A: |
| 01h | physical B: | physical B: |
| 80h | physical first disk | the hard disk image |
| 81h..80h+n | physical | physical disk 80h..80h+n-1 (DL-1 to the old handler, DL restored) |

When a floppy image is present, the equipment word (40:10h) is set to report
at least one floppy drive. A hard disk image adds one to 40:75h. INT 13h
AH=08h for 80h or a translated drive returns DL = the number of physical
hard disks the ROM saw at install + 1, taken from its own state rather than
from 40:75h after chaining. AH=15h returns CX:DX unchanged from the old
handler.

### INT 13h functions served

Floppy image (00h): 00 reset (also re-reads the image info), 01 status,
02/03/04 read/write/verify, 05 format (fills the track with F6h, 03h if
read-only), 08 parameters (BL type from the card, ES:DI -> a DPT in RAM with
the image's sectors per track), 15 (AH=02h, change line), 16 (06h + CF once
after the image changed), 17, 18 (0Ch if the requested geometry does not
match the image).

Hard disk image (80h): 00, 01, 02, 03, 04, 08, 09, 0C, 0D, 10, 11, 14,
15 (AH=03h, CX:DX = total sectors), 41h (EDD 1.1, CX=1: 42h-44h, 47h, 48h),
42h/43h/44h (disk address packet, LBA < 2^32, count written back),
47h (bounds check), 48h (1Ah or 1Eh bytes, CHS valid, no DPTE).

Any other function returns AH=01h with CF set. The last status goes to 40:41h
(floppy) or 40:74h (hard disk). CHS addresses outside the geometry, and LBAs
at or past the end of the image, return 04h. Transfers stop at the end of the
image, and AL (or the packet count) holds the number of sectors done. The
buffer (ES:BX or the packet's pointer) is tracked as a linear address. Each
frame gets the segment address/16, but at most FFFFh, so no DMA-boundary
errors are raised. A buffer in the HMA (FFFF:xxxx with A20 on) is served
without wrapping to low memory. A frame that would run past FFFF:FFFF is cut
there, and if not even one sector fits, the ROM returns 09h. Transfers are
split into frames of at most `CMD_DFSMAXLEN`/512 sectors for reads and
(`CMD_DFSMAXLEN`-10)/512 for writes. With the default 4096 that is 8 and 7
sectors. Card status codes pass through as INT 13h status (03h write
protected, 04h, 20h, 80h). A transport timeout or a missing USB drive gives
80h, and a frame the card rejects twice gives 20h.

Every BDREAD/BDWRITE carries the unit's image token from the last BDINFO.
If the floppy image was swapped in between, the card answers 06h. The ROM
passes 06h on (CF set, DOS re-reads the disk), refreshes the unit (new
token and geometry, so the next call works) and also reports the change at
the next AH=16h. For the hard disk, a token mismatch (card rebooted,
different file) is 80h "not ready" until the next boot.

A call that comes in while a request is in progress (an interrupt handler
calling INT 13h during the poll) gets AAh "drive not ready" with CF set, and
the transaction in flight is not disturbed.

### Transport

Every transaction knocks and selects its register again, because another
program may have changed it. On a 186 or later the ROM moves data with
`rep outsw`/`rep insw`. On an 8086/8088 it uses `lodsw/out dx,ax` and
`in ax,dx/stosw` loops, and an odd last byte moves as a single byte. The
CPU test is the shift-count mask, so a NEC V20 counts as an 8088. After
`CMD_DFSEXEC`, the ROM polls `CMD_DFSSTAT`. Reads, verifies and BDINFO get
about 10 s. Writes and BDINFO with OPEN get about 30 s, because the card's
USB write deadline is 10 s per operation and one BDWRITE (write plus sync)
can take several. The time is counted in changes of the BIOS tick count,
and a timeout also needs a minimum number of status reads (ticks x 1.2 x
65536; one ISA read takes at least about 0.7 us). A program that speeds up
the timer therefore cannot shorten the timeout. With interrupts off, twice
that many reads end the wait. On a timeout the ROM aborts the transaction
and returns 80h. An ABORTED status makes it send the request once more, and
NODRIVE returns 80h. Before it starts a request, the ROM reads
`CMD_DFSSTAT`. If another client's request is BUSY (PGUSDFS interrupted by
an INT 13h call), the ROM waits for it to finish, within the same timeout.

### RAM block (1 KB at segment `[40:13h]*64` after the late init)

| Offset | Content |
|---|---|
| 00h | `PGBT`, old INT 13h, old INT 19h, ROM segment, data port, sectors per read/write frame, CPU flag, installed units, last translated drive, saved 40:10h / 40:75h, options, media-change latch, scratch |
| 30h / 48h | unit structures (floppy, hard disk): cylinders, heads (at most 255), sectors, total, drive type, flags, state, card unit, generation, image token. The geometry is only replaced by a valid BDINFO record, so an ejected floppy keeps its last geometry. |
| 60h | diskette parameter table (AH=08h/18h) |
| 70h | INT 13h trampoline: `push cs / push cs / jmp far ROM:int13_entry`. The INT 13h vector points here. The handler reads its RAM segment from the pushed words and puts the caller's DS back before it returns or chains. |
| 78h-B3h | transaction and transfer parameters |
| C0h-13Fh | BDINFO answer buffer |
| up to 400h | stack for the late init (the INT 13h handler runs on the caller's stack, about 50 bytes) |

If INT 19h runs again (a reboot through INT 19h), the ROM finds its block
through the INT 13h vector, undoes the previous installation and reuses the
block.

## Expected screen output

No card (or firmware without PGDFS/PGBOOT):
```
PGBOOT 0.1: PicoGUS not found, will look again at boot   [then at INT 19h: PGBOOT: PicoGUS not found]
PGBOOT 0.1: PicoGUS firmware without PGBOOT support
PGBOOT 0.1: PGDFS disabled or unsupported on the card
PGBOOT 0.1: disabled (pgusinit /bdopts bit 2)
```
Card found, floppy and hard disk image, the stick mounts a few seconds late:
```
PGBOOT 0.1: PicoGUS found, data port 1D4h (186+)       [(8088) on an XT, (8088 forced) for PGBOOT88.ROM]
PGBOOT: waiting for the USB drive (Esc skips)... 12    [only while not mounted; counts down from 15]
PGBOOT: A: \FD.IMG, 1440K
PGBOOT: C: \HD.IMG, 31M                                [" (read-only)" appended when read-only]
PGBOOT: booting A:
```
Other late-init lines, each followed by the normal BIOS boot:
```
PGBOOT: no disk image configured
PGBOOT: skipped                                        (Esc)
PGBOOT: no USB drive                                   (15 s passed)
PGBOOT: A: \MISSING.IMG - file not found               (also: - unusable image, - no USB drive,
                                                        - too fragmented (copy it to a freshly formatted drive), - not ready)
PGBOOT: no image ready
PGBOOT: C: card error 80h
PGBOOT: C: boot sector read error 04h
PGBOOT: C: not bootable (no 55AAh signature)
PGBOOT: continuing with the BIOS boot
```

## Tests

`tools/emu/run-tests.sh` (QEMU with the pgbridge device and the card
simulator) covers these cases:
* T1: FreeDOS floppy image, with a write.
* T2: T1 with `PGBOOT88.ROM`.
* T3: `FDISK /MBR` and `SYS C:` onto a blank hard disk image through the ROM.
* T4: boot that hard disk image.
* T5: USB drive mounted 3 s late.
* T6: no card.

`test/INT13TST.COM [W]` prints AH=08h/15h for 00h/80h/81h. It also tests
EDD 41h/48h, compares CHS against LBA reads over several frames, reads into
an odd unnormalised buffer, checks error codes, reads a physical 81h through
the translation, and with `W` does write/readback/restore on A: and the last
LBA of 80h. The last line is `INT13TST DONE, n failures`.

## Limitations

* Every sector goes through the USB drive and FatFs on the card. The ROM
  waits for each write until the card has synced the file, so disk-heavy
  work is slower than on a real disk.
* SMARTDRV and other write-behind caches hold writes in RAM. Turn write-behind
  off for the image drives (`SMARTDRV C-` or `/X`), or flush before a power
  cycle, or the image can end up inconsistent.
* Windows 9x runs these drives in MS-DOS compatibility mode (real-mode
  INT 13h, no 32-bit disk access), which is slow but works. Windows 3.x
  32-bit disk access (WDCTRL) does not apply: keep 32BitDiskAccess off.
* The ROM and PGUSDFS (the PGDFS redirector) share the card's single
  transport. Both knock and select for every transaction, so they coexist as
  long as neither is called from an interrupt handler while the other is in
  the middle of a transaction. A disk cache that flushes from the timer
  interrupt could do exactly that.
* An image that is in use must not be changed through PGUSDFS. The card
  refuses to modify, rename or delete an open image file (access denied),
  but other files on the stick are fine.
* The hard disk image must fit the CHS view for old DOS: at most 1024
  cylinders, 256 heads, 63 sectors. Larger images are reachable through
  EDD (LBA) only.
* No INT 41h/46h fixed disk parameter table is installed for the image.
  Software that reads the geometry from there instead of INT 13h AH=08h sees
  the physical disk.
* The pre-POST INT 19h is kept in vector 6Bh between POST and the late init
  (ROM only). If another ROM uses that vector at POST, change `OLD19_VEC`
  in `defs.inc` and rebuild.
* `PGBOOT.COM` depends on a clean boot, and its `/C` mode is untested on
  MS-DOS.
* A floppy image that was ejected (`/fdimage -`) still answers AH=08h with
  the last geometry. Reads return 80h.
* PGFLASH is untested on real EEPROMs. Its refusal paths (RAM,
  read-only window, running ROM) have been checked in QEMU.
