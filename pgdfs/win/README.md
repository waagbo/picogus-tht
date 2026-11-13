# PGDFS for Windows - the PicoGUS USB drive as a Windows drive letter

Two drivers give Windows a drive letter for the USB drive plugged into a
PicoGUS, the same drive that `PGDFS.EXE` maps under DOS:

| Driver | For | Shows up as |
|---|---|---|
| `PGDFS.VXD` (`win9x/`) | Windows 95, 98, 98 SE, ME | a network drive, `P:` by default |
| `pgdfs.sys` (`winnt/`) | Windows NT 4.0, 2000, XP (x86) | a removable drive, `P:` by default |

Both are native file system drivers: Explorer, the command prompt, Win32,
Win16 and DOS programs (in a DOS box) all use the drive like any other,
with long file names, and the drive keeps working when the USB stick is
pulled and plugged in again. The card serves the files from its own FatFs
(FAT12, FAT16 or FAT32), so the drivers never see sectors.

`PGDFS.VXD` has been run on a real machine with Windows 98 SE (before it
learned the control requests PGTRAY uses). `pgdfs.sys` has not been on real
hardware yet, and PGTRAY has only met an emulated card so far (the host
tests, and `make demo` under Wine).

With them comes **PGTRAY** (`pgtray.exe`, the same file in both folders), a
small program in the notification area for what `PGUSINIT` does under DOS:
loading CD images, switching the card mode, setting up WiFi for the web
server, and switching an mt32-pi between its synthesizers. See
[PGTRAY](#pgtray) below.

## Requirements

* A PicoGUS with firmware that includes PGDFS, with PGDFS enabled (the
  default; `pgusinit` shows `PGDFS data port 1D4 ...`). The drivers read the
  data port from the card, as `PGDFS.EXE` does.
* A FAT-formatted USB drive in the PicoGUS (it can be plugged in later).
* The PicoGUS must be found when Windows starts: without it, or with PGDFS
  disabled on it, the driver does not load (and Windows starts without the
  drive letter).
* Do not load `PGDFS.EXE` in `AUTOEXEC.BAT` at the same time: the card serves
  one client. Remove it when you install the Windows 9x driver.

## Installing

### Windows 95, 98, ME

1. Copy `pgdfs.vxd` and `pgdfs9x.inf` from the `win9x` folder into one
   folder on the machine.
2. Right-click `pgdfs9x.inf` and choose **Install**.
3. Restart Windows.

The INF copies the driver to `WINDOWS\SYSTEM` and lists it as a static VxD
under `HKLM\System\CurrentControlSet\Services\VxD\PGDFS` (`StaticVxD`,
`Start` = 0). Instead of the INF, `device=pgdfs.vxd` in the `[386Enh]`
section of `SYSTEM.INI` (with the file in `WINDOWS\SYSTEM`) works as well.
Remove it again with Control Panel, Add/Remove Programs, "PicoGUS PGDFS".

### Windows 2000, XP

1. Copy `pgdfs.sys` and `pgdfsnt.inf` from the `winnt` folder into one
   folder on the machine.
2. Right-click `pgdfsnt.inf` and choose **Install** (administrator).
3. Restart Windows.

The INF copies the driver to `System32\drivers` and adds the service
`pgdfs` (file system driver, system start). Remove it with
`rundll32 setupapi,InstallHinfSection PGDFS.Remove 132 pgdfsnt.inf` from the
folder with the INF, then restart.

### Windows NT 4.0

1. Copy `pgdfs.sys` to `%SystemRoot%\System32\drivers`.
2. Double-click `pgdfsnt.reg` (or `regedit pgdfsnt.reg`) to add the service.
3. Restart Windows.

To remove it, delete the key `HKLM\SYSTEM\CurrentControlSet\Services\pgdfs`
with `regedit` and restart. (The INF route of Windows 2000 may also work on
NT 4.0 with a recent `setupapi.dll`, but the `.reg` file needs nothing.)

### The drive letter

`P:` unless the registry says otherwise: the string value `DriveLetter`
(`D` to `Z`) in `HKLM\System\CurrentControlSet\Services\VxD\PGDFS`
(Windows 9x) or in `HKLM\SYSTEM\CurrentControlSet\Services\pgdfs\Parameters`
(Windows NT). Both INF files set it to `P`; change it and restart. On
Windows 9x a letter that is already taken by a local, CD-ROM or network
drive is skipped for the next free one; on Windows NT the driver does not
start when the letter is taken (the System event log then lists `pgdfs` as
a driver that failed to load).

## The removable drive

The USB stick can come and go while Windows runs:

* **No stick**: the letter stays, every access fails with "The device is not
  ready" (Windows NT: "Please insert a disk into drive P:" in Explorer, the
  same as an empty floppy drive; DOS programs: "Not ready reading drive P").
* **The same stick plugged in again** (same volume label, serial number and
  size): open files carry on where they were, as FAT does with a floppy
  that goes back into the drive. The card counts its mounts (`DFSDIAG /INFO`
  shows the count), which is how the drivers notice a stick that was pulled
  and put back; with firmware older than that count they work it out from
  the card having forgotten its open files. Every write is written through to the
  stick by the card, so pulling the stick between two writes loses nothing
  that was reported as written.
* **A different stick**: files that were open on the previous one fail with
  "The wrong diskette is in the drive" (Windows 9x) or "The volume for a
  file has been externally altered so that the opened file is no longer
  valid" (Windows NT); everything opened afterwards sees the new stick.
* On Windows NT the driver looks at the card every two seconds and makes
  Explorer windows that show the drive refresh themselves when the stick
  comes or goes. On Windows 9x press F5.

## PGTRAY

`pgtray.exe` puts a compact disc into the notification area next to the
clock (the taskbar's "tray"). It is one program for Windows 95, 98, ME and
Windows NT 4.0, 2000, XP: copy it from `win9x` or `winnt` to the hard disk
(not to the USB drive of the PicoGUS: that drive goes away for a moment when
the card mode is switched) and start it.

* **Right-click the icon** for the menu: the CD images on the USB drive with
  a check mark on the loaded one, "No image (eject)", the two synthesizers of
  an mt32-pi, the card modes, and Exit.
* **Double-click the icon** for the window. Minimizing or closing the window
  hides it again; the icon stays. Exit ends the program.
* The icon is a blank disc while no CD image is loaded, and its tip says
  which image is, or which mode the card is in.
* **Start with Windows** in the window makes Windows start the program at
  logon, as the icon only (`pgtray /tray`, in `HKCU\...\Run`).

The window has four tabs:

| Tab | |
|---|---|
| **CD-ROM** | The images on the USB drive, as `pgusinit /cdlist` shows them. Load (or a double-click) puts one into the emulated CD-ROM drive, Eject takes it out, Refresh reads the list again after the USB stick was changed. The card's CD-ROM emulation is part of Sound Blaster and USB mode and needs its port set (`pgusinit /cdport`). |
| **MIDI** | For an [mt32-pi](https://github.com/dwhinham/mt32-pi) on the card's MIDI output: switch between its MT-32 and its SoundFont (General MIDI) synthesizer, choose the MT-32 ROM set (old, new, CM-32L) or a SoundFont by number. The messages are mt32-pi's own SysEx commands and go out through a MIDI output of Windows: choose the one of the card's MPU-401 at the top. It must not be in use by another program at that moment. |
| **Card mode** | The firmware version and the mode the card runs in. Switch restarts the card in another mode (GUS, Sound Blaster, AdLib, MPU-401, Tandy / CMS, USB), as `pgusinit /mode` does; "Keep this mode after power-off" saves it as well. The card becomes a different device: programs in a DOS box see the new one at once, but a Windows sound driver that was loaded for the old mode has no card until you switch back. The USB drive is "not ready" for the two or three seconds the card needs. |
| **Web server** | For firmware with the web server (Pico W and Pico 2 W boards): the card's WiFi status and the address of its web page, a button that opens the page in the browser, and the network name and password for it (`pgusinit /wifissid`, `/wifipass`). "Save to the card" keeps the network after power-off (it saves all of the card's settings, as `pgusinit /save`). |

How the program reaches the card:

* **With a PGDFS driver loaded**, through the driver (`\\.\PgDfs` under
  Windows NT, `\\.\PGDFS` under Windows 9x; `common/pgctl.h` describes the
  requests). The driver owns the card's control port while Windows runs and
  fits the program's requests in between its own. Use the drivers of the
  same release: older ones have no such requests.
* **Windows 9x without the driver** (also with firmware that has no PGDFS):
  the program uses the card's I/O ports itself.
* **Windows NT, 2000, XP without the driver**: not at all. Programs cannot
  reach I/O ports there, so PGTRAY needs `pgdfs.sys`, and with it a card
  with PGDFS.

The program looks at the card when its menu or window opens, every few
seconds while the window shows, and when the mouse rests on its icon;
otherwise it leaves the card alone. Close its window while `PGUSINIT` writes
new firmware from a DOS box: a firmware update must have the control port to
itself.

## What works how

* **Long file names** in both directions. The card returns 8.3 names in its
  listings; the drivers ask for every entry's long name (the PGDFS
  `LONGNAME` request, see `sw/dfs/PROTOCOL.md`), and a path may use long or
  8.3 names. DOS programs in a Windows 9x DOS box see the 8.3 names, as on
  any FAT drive. GetShortPathName/GetLongPathName work.
* **Names are in the card's code page**, `FATFS_CODE_PAGE` (437 by default),
  as for DOS: the drivers convert between it and Unicode with the OEM code
  page of Windows, so the two should match. A name with a character the
  code page does not have cannot be created ("The filename, directory name,
  or volume label syntax is incorrect").
* **Paths** may be at most 127 bytes long on the card (in the code page,
  from the root of the drive), and names at most 127 characters (FatFs on
  the card). Longer ones fail with "The filename or extension is too long".
* **Many open files**: the card keeps 8 files open at a time; the drivers
  hand the card's file slots to the files that are being read or written
  and give the least recently used one back when another file needs one.
  Any number of files can be open in Windows.
* **Programs and memory-mapped files** run from the drive (Windows NT pages
  them in through the driver; Windows 9x loads them through the VxD).
* **Free space** is exact, from the card's diagnostics record (DOS gets at
  most 2 GB, as with any big drive). The first free-space query after the
  stick is plugged in makes the card count the free clusters, which takes a
  few seconds on a big stick.
* **Timestamps**: the drivers send the local time to the card every minute,
  which stamps the files it creates and writes; times set by programs
  (`SetFileTime`, `COPY`) are stored. FAT keeps the modification time only,
  which the drivers report as creation and access time too.
* **Not there**: file locking (lock requests succeed, as in EtherDFS),
  security and extended attributes (as on FAT), formatting, changing the
  volume label, chkdsk/scandisk, and on Windows 9x renames with wildcards
  (`REN *.TXT *.BAK` in a DOS box). No cache: every read and write goes to
  the card, so programs that read a byte at a time are slow.

## Speed

Every request is one transaction over the ISA bus, at most 4 KB of data
each. Expect roughly what `DFSDIAG /ECHO` measures under DOS on the same
machine, a little less for small files (one request to open, one per 4 KB,
one to close) and for listings (two requests per directory entry: the entry
and its long name).

## Building

From this directory, `make` builds both drivers and PGTRAY, `make test` runs
the host tests of the shared client core, `make clean` removes the results.

* `9x/`: Open Watcom 2.0 (`wcc386`, `wasm`, `wlink`; `make INCLUDE="$WATCOM"/h`
  as for the DOS tools) and Python 3 for `fixle.py`, which applies the LE
  header fixes the VMM expects to wlink's output.
* `nt/`: the MinGW-w64 cross compiler for 32-bit Windows with its DDK
  headers (Debian/Ubuntu: `gcc-mingw-w64-i686`) and Python 3 for
  `pefix.py`, which clears the DLL flag and writes the PE checksum. The
  driver uses only kernel exports that Windows NT 4.0 has (`make imports`
  lists them).
* `tray/`: PGTRAY, with the same MinGW-w64 compiler (its `windres` compiles
  the resources, `mkicon.py` draws the icons). The program is linked without
  a C runtime and imports only what Windows 95 has (`make imports` lists
  it). `pgcard.c` is what it does with the card, `backend.c` the way there,
  `pgtray.c` the window. `make demo` builds `pgtray-demo.exe`, the same
  program with an emulated card inside, to work on the window without a
  PicoGUS (it also runs under Wine).
* `common/`: the client core both drivers share, `pgc.c`: the ISA transport,
  the PGDFS requests, the sharing of the card's eight file slots, the
  tracking of the removable drive (`pgc.h` explains how) and the control
  requests of a setup program such as PGTRAY (`pgctl.h`).
* `test/`: the core against an emulated card at the I/O port level, behind
  which run the real firmware transport and file server on a FatFs RAM
  disk: unplug, replug, swapped sticks, a hung card, more open files than
  the card has slots; and PGTRAY's card part through the core's control
  requests against the card's CD, mode and WiFi registers (`emuctl.c`).

`sw/scripts/build_release.sh` builds the drivers and PGTRAY with the DOS
tools into `win9x/` and `winnt/` of the release files.

## Credits

* The Windows 9x driver follows the network file system driver structure of
  the FSINFILE sample in Stan Mitchell's "Inside the Windows 95 File System"
  (O'Reilly, 1997), which connects a drive letter without a network client.
* The card side and the request format are those of the DOS driver, derived
  from Mateusz Viste's EtherDFS.

See `../LICENSE`.
