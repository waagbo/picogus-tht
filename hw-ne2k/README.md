# PicoGUS NE2000 v1.0 – WiFi network card based on PicoGUS v1.2

A third card in this repository: the PicoGUS **v1.2** ISA interface with **everything audio removed**,
built around a **Raspberry Pi Pico W** so it runs the PicoGUS NE2000 firmware (`pg-ne2k`: an NE2000
that talks to the network over WiFi). Added:

* **USB-A host port** at the bracket, wired to the Pico W's USB test points (TP2/TP3), for USB mice,
  joysticks and storage (the same idea as the PicoGUS 2.0 USB-A port)
* **8 MB PSRAM** (APS6404L) kept, for buffering/caching in future firmware
* **Boot ROM** on the ISA memory bus: an in-system-writable SST39SF010A flash with 4 × 32 KB images
  (e.g. iPXE for PXE boot, a disk-image boot ROM, XT-IDE BIOS), configured from the Pico W

> **Status: prototype design, not yet fabricated or tested.** The schematic passes ERC (no errors
> beyond those inherited from v1.2). The board was autorouted and checked with DRC in KiCad 10:
> **0 errors, 0 unconnected**, and it matches the schematic. Firmware support for the boot ROM
> (enable/base/bank set-up, ROM images, flashing tool) does not exist yet; the stock `pg-ne2k` firmware
> runs the NE2000 and leaves the ROM switched off.

| File | What |
|---|---|
| `PicoGUS-NE2K.kicad_pro/.kicad_sch/.kicad_pcb` | KiCad **10** project. Root sheet: the v1.2 ISA logic + Pico W + PSRAM |
| `bootrom.kicad_sch` | Boot ROM (U13–U16) and USB-A host port (J5, U17, F1) |
| `PicoGUS-NE2K-schematic.pdf` | Schematic PDF |
| `BOM-NE2K.csv` | BOM with a **DigiKey link** for every line, stock and NOK price |
| `BOM-NE2K-digikey-upload.csv` / `-10x.csv` | DigiKey cart/BOM upload for 1 / 10 boards (designators as Customer Reference) |
| `build/jlcpcb/` | Gerbers, drill, assembly BOM and placement files for JLCPCB (regenerate with `hw-common/tools/jlcpcb.sh`) |

## What is on the card

### Kept from v1.2 (unchanged)

U2 74LVC244, U3/U4 SN74CB3T3257, U5 74AHC14, U6 74LVC00, U7 74LVC1G00, U8 APS6404L PSRAM and
U10A 74LVC2G06 (IOCHRDY), with the same Pico GPIO assignment as v1.2. So the stock PicoGUS firmware
builds (`pg-ne2k`, the USB mouse/joystick build, `multifw`) work without changes; the sound modes of
`multifw` simply have no DAC to play through.

### Removed

* **Audio:** PCM510x DAC, the GY-PCM5102 option, line-out and the Waveblaster/mixer section of v1.3
* **MIDI out** (J5 jack, R3/R4, FB1/FB2). U10B stays on the board (its input is still driven) with the
  output unconnected.
* **ISA DMA:** the DRQ/DACK jumpers and the DRQ/DACK/TC paths. The NE2000 moves packet data with
  programmed I/O through its data port (its "remote DMA" is internal to the NE2000, not ISA DMA), so
  `pg-ne2k` never uses DRQ or DACK. DACK keeps its pull-up (R6), so the AEN gating (U7) works as before.
* **J7 (USB power jumper):** the USB-A port has its own VBUS supply.

### IRQ (J1)

J1 is now a **2×5 header**: one jumper selects **IRQ 2/9, 3, 4, 5 or 7** for the NE2000 (same layout as
the IRQ half of the v1.2 J1). IRQ 3 is the usual NE2000 choice if COM2 is not used.

### Pico W, antenna and USB-A

* **U1 must be a Pico W** (SC0918). The NE2000 firmware is built for the RP2040 + CYW43439; a
  Pico 2 W does not work.
* **Solder the Pico W flat** (castellated edges, or pins cut flush), not on header pins: the USB data
  lines are taken from its **TP2 (D−) and TP3 (D+)** test points on the underside. The footprint
  (`RPi_PicoW_SMD_TH_USB`) has plated 1.5 mm pads with a 0.6 mm hole there, so you solder them from the
  back of the card after the module is in place. TP1 (USB ground) is connected the same way. A Pico WH
  (pre-soldered headers) cannot be used.
* **The antenna end points at the bracket**, as far out as the board allows. No copper on either layer
  under the antenna (datasheet keep-out, 14 × 9 mm between the pin rows) or between the module end and
  the bracket edge. The footprint also drops the Pico's SWD pads, which would sit under the Pico W
  antenna.
* **USB-A (J5, Molex 67643)** sits at the bracket below the antenna, on the line of the v1.2 line-out jack.
  D+/D− run straight from the Pico's test points (the Pico's own 27 Ω series resistors are before them),
  with a USBLC6-2SC6 ESD protector (U17). VBUS comes from the ISA +5 V through a 0.5 A polyfuse (F1)
  with 10 µF + 0.1 µF. The Pico's own VBUS pin is not connected, so a PC on the micro-USB port (on the
  bench, for flashing) and the ISA supply never feed each other.
* **Firmware updates** go over ISA with `pgusinit` as usual. The micro-USB socket now faces into the card;
  it is still usable with the card out of the PC (BOOTSEL on the Pico).

### Boot ROM (bootrom.kicad_sch)

A real option ROM on the ISA memory bus, so the BIOS finds and runs it during POST.

* **U13 SST39SF010A** (128 KB, 5 V, PLCC-32), read through a **32 KB window**: SA0–SA14 go straight to the
  flash. Flash A15/A16 are the **bank** bits from the Pico, so the chip holds **4 images of 32 KB**.
* **U14 74HCT688** compares SA19..SA15 with `1 1 BASE2 BASE1 BASE0` and requires **ROM_EN = 1**. Its
  enable is `~MEMSTB` (U16: low during any ISA memory read or write), so the output `~ROM_CS` is only
  active in a memory cycle inside the window.
* **U15 74AHCT245** drives the flash data onto the ISA data bus (the flash alone is too weak for a loaded
  bus). Direction comes from `~SMEMW`, enable from `~ROM_CS`.
* **Base address** (BASE2..0): `001` C8000, `010` D0000, `011` D8000, `100` E0000, `101` E8000.
* **ROM_EN, BASE0–2, BANK0–1 are Pico GPIOs** (the pins freed by removing I2S and DMA):

  | GPIO | Pico pin | Signal |
  |---|---|---|
  | 16 | 21 | ROM_EN (pulled low by R3: ROM off until the firmware enables it) |
  | 17 | 22 | ROM_BASE0 |
  | 18 | 24 | ROM_BASE1 |
  | 19 | 25 | ROM_BASE2 |
  | 20 | 26 | ROM_BANK0 |
  | 22 | 29 | ROM_BANK1 |

  The firmware should set them from its stored settings first thing after reset. The Pico is running
  within tens of milliseconds of ISA RESET, while the BIOS scans for option ROMs only after its memory
  test. With the stock firmware the ROM stays invisible.
* **Writing the ROM from DOS:** fit **J2** (write enable: `~SMEMW` → flash WE#, R4 holds WE# high
  otherwise), then use the SST software command sequence (5555h/2AAAh lie inside the 32 KB window).
  Remove J2 afterwards.
* **What to put in the banks:** iPXE built with its `ne2k_isa` driver (PXE boot; iPXE needs a 386 or
  later), a small INT 13h ROM that boots a disk image the Pico fetches over WiFi (cached in the PSRAM; works
  on an 8088), or an XT-IDE BIOS. All of this is firmware work; the hardware only provides the ROM.

## Card dimensions and bracket

The card uses the **v1.2 outline: 103.7 × 74.2 mm** (bracket edge to far edge × fingers to top), with the
v1.2 edge connector and bracket holes H1/H2, so a v1.2 bracket screws on. The bracket needs:

* a **large opening in front of the antenna** (around the old MIDI-jack hole, y ≈ 80 mm on the board):
  metal close to the antenna is what costs range,
* a **USB-A cut-out** (about 14 × 7 mm) centred on the old line-out jack hole.

### Silkscreen

Front: IRQ numbers at J1, `ROM WE` at J2, `TX GND` at J6, `USB`, `WiFi ANTENNA` at the antenna end,
`REAR >`. Back: the PicoGUS artwork (`hw-common/art/picogus-artwork.svg`, 70 × 25 mm) over the lower half,
the dedication *This version is dedicated to Mila and Alisa, my two wonderful princesses, who fill my
world with joy.* behind the Pico, and the copyright/licence text (CERN-OHL-P v2).

## Building it

* Everything except U1, J1, J2, J5, J6 and the jumpers is SMD on the top side. `hw-common/tools/jlcpcb.sh
  hw-ne2k/PicoGUS-NE2K.kicad_pcb` writes gerbers, drill, assembly BOM and placement files to `build/jlcpcb/`
  (38 SMD placements). The parts have no LCSC numbers yet, so pick them in JLCPCB's part matcher (or order
  the parts from DigiKey and assemble by hand).
* Solder the **Pico W flat**, then solder **TP1, TP2, TP3 from the back** through the plated holes.
* Jumpers: one on J1 (IRQ). J2 only while writing the boot ROM.
* Firmware: flash `pg-ne2k` (or `multifw`) with `pgusinit`, then set the WiFi SSID/password and the NE2000
  base port with `pgusinit` as on any PicoGUS.

## Ordering

`BOM-NE2K.csv` lists a DigiKey part for every line with stock and price (new parts checked on
digikey.com on 2026-10-05; parts shared with the SMD board from its 2026-09-29 check).
DigiKey parts only (no PCB, no shipping): **about 264 kr excl. MVA (~330 kr incl. 25 % MVA, ~$28) per
board**; the Pico W (73 kr), the PLCC flash (19 kr), the USB-A connector (18 kr) and the two bus switches
(20 kr) are the largest items.

## Sources

* PicoGUS v1.2 schematic (`hw/`) and the v1.3.0 SMD root sheet (`hw-smd/`)
* Raspberry Pi Pico W datasheet (test points TP1–TP6, SMT footprint, antenna keep-out)
* Microchip SST39SF010A datasheet; TI CD74HCT688, SN74AHCT245, SN74AHCT1G08 datasheets
* PicoGUS firmware: `sw/ne2000play.cpp`, `sw/ne2000/ne2000.c`, `sw/CMakeLists.txt` (`NE2K` target)
