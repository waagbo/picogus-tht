# PGBOOT: booting floppy and hard disk images from the PicoGUS USB drive

Experimental. An option ROM (`PGBOOT.ROM`, in an EEPROM on a
third-party ROM card such as the Monotech DoubleROM, or loaded into RAM by
`PGBOOT.COM`) presents a disk image file on the PicoGUS USB drive to the BIOS
as a floppy drive (A:) and/or a hard disk (C:), so the machine can boot from
it. The PicoGUS itself only decodes I/O ports, so it cannot carry the ROM.
The ROM's boot menu lists the disk images on the drive (BDLIST) and selects
them through the same registers `pgusinit` writes.

The card serves 512-byte sectors of the image files over the existing PGDFS
transport (`sw/dfs/PROTOCOL.md`: knock, `CMD_DFSREQ`/`EXEC`/`STAT`/`RESP`, the
word-wide data window at `CMD_DFSPORT`). Every firmware mode built with PGDFS
(GUS, AdLib, MPU, PSG, SB, USB) serves it, because it lives in the shared DFS
server on core 1.

## Units

| Unit | Presented as | Notes |
|---|---|---|
| 0 | BIOS drive 00h (A:) | Floppy image. Replaces the physical A: while an image is configured. Can be swapped at run time (media change). |
| 1 | BIOS drive 80h (C:) | Hard disk image. Physical hard disks move up one (80h -> 81h, ...). Changes take effect at the next boot. |

## Frames

All requests use the PGDFS frame header (`LL LL DD AL`, `DD` = 0). The card
handles these AL values before the PGDFS drive checks (`DD` is not looked
at, and they are answered with or without a USB drive, like `DIAG`). Firmware
without PGBOOT answers AX = 0001h (invalid function) to all of them, and
firmware from before the boot menu to BDLIST. A request
payload shorter than the fields below, an unknown unit or a count of 0 is
answered AX = 0001h without a payload. The transport accepts these frames whether or not a USB
drive is mounted: `CMD_DFSSTAT` reads NODRIVE (FFh) only in place of IDLE or
ABORTED, so after `CMD_DFSEXEC` the status goes BUSY -> READY as usual and
BDINFO answers state 4.

### F3h BDINFO

Request payload: `UU FF`
* `UU` unit (0 or 1)
* `FF` flags: bit 0 OPEN = close the unit's image and reopen it from the
  current settings (the ROM sets it once per boot, before presenting the
  drive). Other bits 0.

Answer: AX = 0000h and the record below (AX = 0001h: unknown unit / old firmware).

| Off | Size | Field |
|---|---|---|
| 0 | u8 | record version, 1 |
| 1 | u8 | state: 0 no image configured (or none opened yet since the card booted), 1 ready, 2 file not found, 3 unusable (size or format not recognised, dynamic VHD, ...), 4 USB drive not mounted, 5 image file too fragmented (copy it to a freshly formatted stick) |
| 2 | u8 | type: 0 none, 1 floppy, 2 hard disk |
| 3 | u8 | flags: bit 0 read-only, bit 1 fixed VHD (footer stripped), bit 2 media changed since the previous BDINFO for this unit (cleared by this call) |
| 4 | u16 | cylinders (hard disks: at most 1024, the CHS view) |
| 6 | u16 | heads |
| 8 | u16 | sectors per track |
| 10 | u16 | reserved, 0 |
| 12 | u32 | total sectors (the LBA size; may exceed C*H*S) |
| 16 | u8 | floppy drive type for INT 13h AH=08h BL: 01h 360K, 02h 1.2M, 03h 720K, 04h 1.44M, 06h 2.88M; 0 for hard disks |
| 17 | u8 | media generation, incremented whenever the unit's image changes |
| 18 | u8 | n = length of the display name that follows at offset 32 (at most 63) |
| 19 | u8 | reserved, 0 |
| 20 | u32 | image token: identifies this card boot and this image file (hash of a per-boot nonce, the path, the start cluster and the size); 0 when the unit is not ready. BDREAD/BDWRITE must echo it. A USB replug of the same stick keeps it; a card reboot, a different file or a floppy swap changes it. |
| 24..31 | | reserved, 0 |
| 32 | n | display name (the active path, no terminator) |

Notes on the record:

* The path is shown as the card uses it: `/` turned into `\`, doubled and
  trailing separators and a leading drive letter (`E:`) dropped, a leading
  `\` added (`E:/disks//dos.img` shows as `\disks\dos.img`). A path longer
  than 63 characters is shown as `...` and its last 60 characters.
* For unit 1 it is the path the open image came from, which differs from
  `CMD_BDHDNAME` after a commit until the next OPEN.
* Type is 0 only in state 0; a unit in use reports its type in states
  1-5. Geometry, total sectors, drive type and token are 0 unless the state
  is 1. Flag bit 0 is set in states 1-5 when the unit is read-only by option
  or (state 1) by the file's attribute.
* After a card boot every unit is state 0 with an empty name until its first
  BDINFO with OPEN (unit 0 also on a floppy name commit, an explicit swap by
  the user): the card never opens an image nobody asked for. While no USB
  drive is mounted, a unit with a configured name reads state 4 instead, so
  the ROM's wait loop (BDINFO without OPEN) can tell "not mounted yet" from
  "mounted, not opened yet".
* The token hashes (FNV-1a) the per-boot nonce, the unit, the count of
  explicit opens of the unit (BDINFO OPEN, floppy commit), the normalised
  path (case-insensitive), the file's start cluster and its size. So every
  OPEN and every floppy commit (the same name again included) gives a new
  token; a USB replug reopens the file and gets the same token back as long
  as it is the same file (same start cluster and size); a file replaced on
  the stick while it was out gets a new one.
* The media-changed flag and the generation move together: on a floppy name
  commit, a BDINFO with OPEN (that same call already reports it), and a USB
  replug that brings back a different token (other file, or not openable any
  more). An unplug alone changes neither (the unit reads state 4, token 0,
  requests get 80h). The flag is cleared by every BDINFO for the unit; the
  generation wraps at 256.
* The answer is clipped to `CMD_DFSMAXLEN` (the name first); a frame
  capacity below 32 bytes gets AX = 0001h.

### F4h BDREAD

Request payload: `UU NN LL LL LL LL TT TT TT TT` (unit, sector count 1..255, LBA u32, the image token from the last BDINFO).
Answer: AX = INT 13h status (low byte; high byte 0), payload = NN*512 bytes
when AX = 0, nothing otherwise. The ROM never asks for more sectors than fit:
`NN*512 <= CMD_DFSMAXLEN` (8 sectors with the default 4096); a larger count
is answered 01h.

### F5h BDWRITE

Request payload: `UU NN LL LL LL LL TT TT TT TT` + NN*512 bytes (`10 + NN*512 <= CMD_DFSMAXLEN`,
so at most 7 sectors with the default 4096). Fewer data bytes than NN*512:
01h. Answer: AX = INT 13h status, no payload. The sectors are on the drive
when the answer comes (FatFs writes whole aligned sectors straight to the
disk, never through a buffer); the file's directory entry (time stamp,
archive bit; the size never changes) is written back on the first write after
an open, then after 1 s without writes, and when the image is closed.

Checks in this order: request shape (01h), unit ready (80h), token (floppy: 06h media changed, the ROM then refreshes BDINFO and DOS rereads; hard disk: 80h, the system must be rebooted), write
protection (03h, BDWRITE only), range (04h), then the transfer (20h on a
FatFs or USB error). After a 20h the next request tries the drive again.

### F6h BDLIST

One screenful of a list for the ROM's boot menu. The ROM is a list browser:
which lists there are and what their entries do is the card's business, so
firmware can add to the menu without a new ROM.

Request payload: `SS SS NN LL` + path
* `SS SS` index of the first entry wanted (u16)
* `NN` most entries wanted; the card returns fewer when the frame is smaller
  (`(CMD_DFSMAXLEN - 4) / 48`, 85 with the default 4096). 0 asks for the
  total alone.
* `LL` the list:

  | LL | List | Needs the USB drive |
  |---|---|---|
  | 00h | a folder: its folders and all its disk images | yes |
  | 01h | a folder: its folders and its floppy images | yes |
  | 02h | a folder: its folders and its hard disk images | yes |
  | 10h | the main menu | no |
  | 11h | the start-up tunes | no |
  | 12h | the card's modes | no |
  | 13h | the colours of the menu | no |
  | 14h | the boot order | no |

  Firmware before the lists ignored this byte (the ROM sent 0) and answers
  every value with list 00h. Any other value: 01h.
* path (lists 00h-02h; ignored by the others): the folder, without a
  terminator, at most 127 characters, spelled like an image name
  (`\DISKS\DOS`, `/disks/dos`, `E:\disks\dos\`). Empty or a separator
  alone: the root.

Answer: AX = 00h and `TT TT CC 00` followed by `CC` entries of 48 bytes.
`TT TT` is the number of entries the list has (at most 65535), `CC` the
number in this answer.

| Off | Size | Field |
|---|---|---|
| 0 | u8 | kind, see below |
| 1 | u8 | flags: bit 0 the image file is read-only; bit 1 this is what is in effect (the image in the drive, the tune, mode or colours selected) |
| 2 | u32 | images: size in 512-byte sectors, rounded down. Menu entries: byte 2 = a, byte 3 = b (see the kinds), the rest 0. Folders: 0 |
| 6 | u8 | length of the display name |
| 7 | u8 | reserved, 0 |
| 8 | 13 | 8.3 name, zero padded, always terminated: what a path is built from (`..` for the way up); empty for menu entries |
| 21 | 27 | display name, zero padded; a longer one is cut to 24 characters and `...` |

| Kind | Entry | What Enter does in the ROM |
|---|---|---|
| 0 | folder | lists that folder (`..`: the one above) |
| 1 | floppy image | writes its path (the folder, `\`, the 8.3 name) to `CMD_BDFDNAME`, returns to the main menu |
| 2 | hard disk image | the same to `CMD_BDHDNAME` |
| 3 | menu | opens list a; the entry's display name is that list's title |
| 4 | choice | writes the value b to control register a (`DATA_PORT_HIGH`), waits a quarter of a second and until the card answers again, then draws everything anew and reads the list again. After a write to `CMD_BDTUNE` it plays the tune; after one to `CMD_SAVE` it only says so; after one to `CMD_BOOTMODE` (a card mode) it goes on as `pgusinit /mode` does: a write to `CMD_SAVE`, then one to `CMD_REBOOT`, and the card restarts in that mode |
| 5 | boot | leaves the menu |

A ROM leaves the menu on any kind it does not know.

Folder lists (00h-02h): the way up (`..`, not in the root), the folders,
then the disk images, each group in the order the entries have in the
directory. At the top of the root of lists 01h and 02h comes `(none)`: a
choice that writes 0 to the image name register, which empties the drive.
Nothing is kept on the card between requests: every request reads the
directory again, so the listing follows the drive. Listed are folders and
files that are neither hidden nor system. A file is a disk image by its
extension: `IMG`, `IMA`, `VFD`, `FLP`, `DSK` (a floppy image up to 2,949,120
bytes = 2.88 MB, a hard disk image above), `VHD` and `HDD` (always a hard
disk image). The kind is a guess for the menu from the name and size alone;
whether the image is usable shows when it is opened (BDINFO state). An image
is flagged as the one in the drive when the configured name is this folder's
path with this entry's 8.3 name, which is how the ROM writes it.

The menu's own lists:

* 10h, main menu: Floppy Image (menu 01h), HDD Image (menu 02h), Boot Order
  (menu 14h), Boot Melody (menu 11h), Card Mode (menu 12h; left out when the firmware has one mode),
  Colours (menu 13h), Save Settings (choice: `CMD_SAVE` <- 1), Boot.
* 11h, tunes: Off, the firmware's tunes by name, then the user's melody when
  there is one; choices `CMD_BDTUNE` <- the tune's number.
* 12h, modes: the modes the firmware can restart in, by `card_mode_t`;
  choices `CMD_BOOTMODE` <- the mode. The card is off the bus for a second
  or two when it restarts; its USB drive takes longer to come back.
* 13h, colours: Setup, Commander, Mono; choices `CMD_BDTHEME` <- 0, 1, 2.
* 14h, boot order: Floppy A: first, Hard disk C: first; choices `CMD_BDOPTS`
  <- the options as they are with bit 3 clear, or set. The ROM reads
  `CMD_BDOPTS` again when the menu ends.

Other answers, without a payload: 01h request shorter than 4 bytes, path
longer than 127 characters or no such list, 02h no such folder, 20h FatFs
or USB error, 80h no USB drive mounted (folder lists).

### Status codes (AX)

| AX | Meaning (as INT 13h AH) |
|---|---|
| 00h | success |
| 01h | invalid unit or request |
| 02h | BDLIST only: no such folder |
| 03h | write protected (read-only image) |
| 04h | sector not found (LBA + count beyond the image) |
| 06h | media changed: floppy image token mismatch (image swapped since the last BDINFO) |
| 20h | controller failure (FatFs or USB error underneath) |
| 80h | not ready: no image configured or opened for the unit, USB drive gone, or a hard disk token mismatch |

After the card boots it opens no image until the ROM's BDINFO with OPEN, so a
running system whose card rebooted (mode switch, firmware update) gets 80h
instead of a different image. A USB replug reopens the units that were open.

## Configuration registers (control port, core 0)

| Reg | Name | Access | Meaning |
|---|---|---|---|
| 88h | `CMD_BDFDNAME` | string, `DATA_PORT_HIGH` | Floppy image path relative to the USB drive root (`\` or `/`, at most 127 chars; more are dropped; a leading drive letter is ignored). Selecting the register rewinds both the write and the read position. Writes append (the stored name is the new one from the first byte on); a 0 byte commits and rewinds: the card (core 1) reopens unit 0 and marks a media change. An empty string removes the image. Reads return the current name then 0 (and rewind). |
| 89h | `CMD_BDHDNAME` | string, `DATA_PORT_HIGH` | Hard disk image path, same rules. A commit only stores the name; it is opened at the next BDINFO with OPEN (next boot), never under a running system. |
| 8Ah | `CMD_BDOPTS` | byte, `DATA_PORT_HIGH` | bit 0 floppy image read-only, bit 1 hard disk image read-only, bit 2 ROM disabled (the ROM installs nothing), bit 3 boot the hard disk image even when a floppy image is configured. Reads return the value with bit 7 set and bit 6 clear (signature: firmware without PGBOOT reads FFh). |

| 8Bh | `CMD_BDMENU` | byte, `DATA_PORT_HIGH` | The ROM's boot menu. Bits 0-1: 0 off, 1 offered at boot (the ROM waits for M), 2 opened at every boot. Bits 2-5: the seconds it waits in mode 1, 1-15. The card stores mode 3 as 1 and 0 seconds as 3. Reads return the value with bit 6 set and bit 7 clear (firmware without the boot menu reads FFh: the ROM then shows no menu). Default 0Dh: mode 1, 3 s. |
| 8Ch | `CMD_BDTUNE` | byte, `DATA_PORT_HIGH` | Start-up tune the ROM plays on the PC speaker: 0 none, 1-8 the melodies that come with the firmware (`bd_tune.c`; later firmware may have more), 62 the user's (`CMD_BDMELODY`; only while there is one). Other values are not stored. 3Fh written: the card moves on to the next tune it has (0, 1 .. 8, 62, 0). Reads return the value with bit 6 set and bit 7 clear. Default 1. |
| 8Dh | `CMD_BDTHEME` | byte, `DATA_PORT_HIGH` | Colours of the ROM's boot menu, as the themes of the web page: 0 Setup, 1 Commander, 2 Mono. Other values are not stored; 3Fh written: the next one. Reads return the value with bit 6 set and bit 7 clear. Default 0. |
| 8Eh | `CMD_BDMELODY` | string, `DATA_PORT_HIGH` | The user's melody as RTTTL text (`name:d=4,o=5,b=120:c,e,g`; the name and the defaults may be left out), at most 255 characters. Same rules as the image names: select rewinds, writes append, a 0 byte commits, reads return the text then 0. |
| 8Fh | `CMD_BDTUNEDATA` | read stream, `DATA_PORT_HIGH` | The selected tune, compiled for the ROM. Selecting the register rewinds; reads return the stream below, then FFh. |

### The tune stream (`CMD_BDTUNEDATA`)

```
n                 length of the tune's name, 0-24
n bytes           the name ("Off" for tune 0, "Custom" for a melody without one)
per entry:
  u16 divisor     timer channel 2 divisor: 1193182 Hz / frequency; 0 = silence
  u16 length      in units of 1/32 BIOS timer tick (1716.42 us), 1-4000
FFh FFh           end
```

The card compiles the RTTTL text of the selected tune on core 1 whenever the
setting or the user's melody changes (and before core 1 starts, at power-on).
While it does, and with firmware that has no tunes, the register reads FFh,
which is no name length: the ROM then plays nothing. Octave 4 holds the A of
440 Hz; lengths are exact to the unit over the whole tune, so rounding does
not add up. A note gives up its last eighth, 20 ms at most, to a silent
entry, so that two notes of one pitch are heard as two. A tune has at most
160 entries and is cut after about 20 s. The ROM takes the time from timer
channel 0 (see `bootrom/tune.inc`).

All of them live in the persisted settings (`Settings.BootDisk`: settings
version 8, and version 9 for the colours and the user's melody; the settings
are three flash pages long) and are saved by
`CMD_SAVE` (`pgusinit /save`, or S in the boot menu: a write of any value to
`DATA_PORT_HIGH` with `CMD_SAVE` selected, after which the card is busy
writing its flash for a moment). `CMD_DEFAULTS` clears them; the floppy image
is ejected at once, an open hard disk image stays until the next boot.
`pgusinit` commands: `/fdimage <path|->`, `/hdimage <path|->`,
`/bdopts <0-15>`, `/bdmenu <0-2>`, `/bdwait <1-15>`, `/bdtheme <setup|commander|mono>`,
`/bdtune <off|1-8|custom>`, `/bdmelody <text|@file|->`, and a status block
after the PGDFS one.

## Card behaviour

* Images open on BDINFO with OPEN, for unit 0 also on a floppy name commit,
  and again on a USB mount for the units that were in use before the
  unplug (none after a card boot). USB unmount closes both (state 4). A file
  that cannot be opened is state 2 when FatFs reports no such file, path or
  name (a directory included), state 3 otherwise.
* Formats: raw sector images of any extension; fixed VHD (512-byte
  `conectix` footer with a valid checksum, disk type 2): footer geometry,
  size without the footer (the footer's current size when it is smaller than
  the file allows). Dynamic/differencing VHD: state 3. A `conectix` footer
  with a bad checksum is treated as the last sector of a raw image.
* Floppy geometry from the size: 160K 40/1/8, 180K 40/1/9, 320K 40/2/8,
  360K 40/2/9, 720K 80/2/9, 1.2M 80/2/15, 1.44M 80/2/18, 2.88M 80/2/36,
  1.68M DMF 80/2/21, 1.72M 82/2/21 (drive types 01h for the 5.25" 40-track
  sizes, 02h 1.2M, 03h 720K, 04h 1.44M/DMF/1.72M, 06h 2.88M); otherwise from
  a valid FAT BPB in sector 0 with 1-2 heads and 1-255 cylinders (drive type
  from the sectors per track: up to 9 01h/03h by cylinders, 15 02h, 21 04h,
  more 06h); otherwise state 3. A valid BPB: jump (EBh/E9h), 512 bytes per
  sector, a power-of-two cluster size, reserved sectors, 1-2 FATs, media
  F0h+, 1-63 sectors per track, 1-255 heads, a sector count.
* Hard disk geometry, first that applies: VHD footer (when its sectors per
  track are 1-63; large VHDs carry 255); MBR (55AA, at least one non-empty
  partition entry, every non-empty entry with status 00h/80h, a start LBA and
  length, an end sector) with heads = max(end head)+1 (at most 255) and sectors = the
  largest end sector of the partition entries; a FAT BPB in sector 0
  (superfloppy); else 16 heads, 63 sectors. Cylinders = min(1024,
  total / (heads*sectors)), for a VHD also at most the footer's cylinders,
  and at least 1.
* Reads/writes go through FatFs with fast seek, a cluster link map per open
  image, so random access does not walk the FAT chain. The maps share a 2 KB
  pool sized per file (2 DWORDs per fragment + 2, about 250 fragments in
  all); the floppy may use at most 128 DWORDs (63 fragments). A floppy image
  whose map does not fit is served with plain seeks, which walk its FAT chain
  (short: at most 2.88 MB). A hard disk image whose map does not fit what the
  floppy left is state 5 (fragmented): copy it to a freshly formatted stick.
  Writes stay inside the file (never extend it).
* An image is opened read/write unless it has the AM_RDO attribute or the
  drive is write-protected. A read-only image (option bit, checked at every
  write, or the file) answers 03h to writes and reports flag bit 0.
* While an image is open, PGDFS answers 05h (access denied) to requests that
  would change that file: DELETE (a wildcard DELETE skips it and answers 05h
  after deleting the rest), RENAME of the file or of a directory above it,
  OPEN/SPOPNFIL for writing or truncation, CREATE, SETATTR, and WRITEFILE or
  SETFILETIMESTAMP through a handle opened before the image was. Reading it
  through PGDFS works. The file is recognised by the configured path and by
  its 8.3 alias path, case-insensitively.

## ROM behaviour (summary; details in `bootrom/README.md`)

* 8 KB option ROM, 8088-compatible (word I/O with `rep insw`/`outsw` on 186+,
  byte-pair loops on 8086/8088). At POST it only detects the card and hooks
  INT 19h; everything else happens in its INT 19h handler (late init) so the
  USB drive has time to enumerate: reserve 1 KB at the top of conventional
  memory for state, play the start-up tune (`CMD_BDTUNEDATA`), wait for the
  drive (Esc skips), offer or open the boot menu (`CMD_BDMENU`; BDLIST, the
  image name registers, `CMD_SAVE`), BDINFO with OPEN for both
  units, hook INT 13h, adjust the BIOS data area (hard disk count 40:75h,
  equipment word 40:10h), print what it presents, and boot the floppy image
  (or the hard disk image, per option bit 3) from sector 0 at 0000:7C00.
  Nothing configured or nothing ready: chain to the original INT 19h.
* Card detection: knock CCh on 1D0h, `CMD_MAGIC` reads DDh, `CMD_PROTOCOL`
  >= 5, `CMD_DFSMAXLEN` in 512..32768, `CMD_DFSPORT` != 0, `CMD_BDOPTS` with
  the signature and bit 2 clear.
