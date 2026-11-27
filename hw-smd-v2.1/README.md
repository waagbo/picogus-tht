# PicoGUS SMD v2.1 – the THT v2.1 card with surface-mount parts

The SMD counterpart of [PicoGUS THT v2.1](../hw-tht-v2.1/README.md), in the way [`hw-smd`](../hw-smd/README.md) is the
SMD counterpart of THT v1.3. It is **the same circuit with the same reference designators and the same nets**, so the
THT v2.1 README describes how it works and the same firmware runs on it (`core2350.uf2`, and `pg-ne2k-v21.uf2` with
the radio module). Only the packages change.

* Same card: the 102.0 × 99.95 mm outline, the bracket-side connectors and tab holes of THT v2.1, 4 layers.
* **Same Core2350B arrangement**: the Waveshare Core2350B2 module on four 2x8 sockets, same footprint and same place.
* **Parts picked for soldering by hand**: 0805 resistors and capacitors on KiCad's hand-solder lands (longer pads),
  SOIC (1.27 mm pitch) wherever the chip exists as SOIC, and everything on the front. Nothing is on the back.
* The radio module (U14) and the experimental boot ROM (U15–U17) of THT v2.1 are included, and optional as there.

> **Status: draft for placement review. The schematic is done; the board has every part placed and labelled but is
> NOT routed. Not built, not tested.**
> Schematic: ERC gives the same messages as THT v2.1, and its netlist is identical to THT v2.1's (201 nets, 589 pins,
> 102 parts; only footprints, values and part numbers differ). Board: 0 courtyard overlaps, 0 schematic parity
> issues, 418 connections still to route. The BOM has part numbers but no prices: see [BOM](#bom).
> The board is not in the Hardware workflow yet, because an unrouted board fails its DRC.

![Placement, front](docs/renders/PicoGUS-SMD-v2.1.0-placement-red-front.jpg)

| File | What |
|---|---|
| `PicoGUS-SMD.kicad_pro/.kicad_sch/.kicad_pcb/.kicad_dru` | KiCad **10** project. The board is the THT v2.1 board (outline, stackup, plane zones, logo, edge connector, connectors and modules in place) with the SMD parts placed and no tracks |
| `audio.kicad_sch`, `mixer.kicad_sch`, `bootrom.kicad_sch` | The DAC, line-out mixer and boot ROM sheets of THT v2.1 with SMD footprints |
| `PicoGUS-SMD-schematic.pdf` | Schematic PDF |
| `docs/BOM-SMD.csv` | BOM with part numbers, no prices yet: see [BOM](#bom) |
| `docs/renders/` | Render of the placement |

## What changed from THT v2.1

| Part | THT v2.1 | SMD v2.1 |
|---|---|---|
| U3 74AHC14, U4/U5 74AHC00, U6 74AHCT126 | DIP-14 | SOIC-14 |
| U7 74AHC244 | DIP-20 | SOIC-20 wide |
| U11 NE5532 | DIP-8 | SOIC-8 |
| U9 MCP1826S 3.3 V regulator | TO-220, lying | SOT-223 |
| U16 74HCT138, U17 74HCT153 (boot ROM) | TSSOP-16 on the back | SOIC-16 on the front |
| Resistors | axial; four 0805 on the back | 0805 on the front; 0.1 % thin film in the audio path (R10, R11, R21–R26), 1 % thick film elsewhere |
| Capacitors up to 2.2 µF | radial ceramic | 0805: X7R, and C0G for the DAC filter (C21, C22) and the mixer feedback (C24, C25) |
| 10 µF (C11–C13, C17, C47) | radial ceramic | 1206 |
| 47 µF and 100 µF (C1, C28, C29, C38) | radial electrolytic | SMD can, 6.3 mm |
| C39–C42, 2.2 µF Waveblaster coupling | film | bipolar SMD electrolytic, 4 mm can (as `hw-smd` does for the same job) |
| FB1–FB4 | axial ferrite | 0805 ferrite bead |
| D2, D3 | 1N5819, DO-41 | B5819W, SOD-123 |
| D1 | 3 mm LED | 0805 LED |
| F1 | radial PTC | 1812 PTC |
| SW1, SW2 | 6 mm through-hole button | 6 mm SMD button |

Unchanged: U1/U2 (SN74CB3T3257) and U10 (PCM5100A) exist only as TSSOP and keep THT v2.1's footprints with
hand-solder toes and a test pad per pin; U13 (M62429L) is a SOIC-8 already. All connectors and headers stay through
hole, as do the module sockets and the PLCC-32 socket of the boot ROM. The radio module keeps its hand-solder lands.

## Placement

The blocks are where THT v2.1 has them, so the two boards read the same. Inside each block the parts sit as an SMD
layout wants them:

* **ISA logic column** (U6, U4, U5, U3, U7): the SOICs on the DIPs' centres, pin 1 at the same corner, each with its
  0.1 µF capacitor directly over its supply pin. The pull-ups and the pull-down (R18, R5, R1) sit beside the chip
  they belong to.
* **Boot ROM**: the address select U17 is right of U7 and the decoder U16 right of the flash socket, both on the front.
* **Power**: the regulator U9 with its input and output capacitors against its pins, then the two supply diodes in a
  row that ends at the module's supply pin.
* **DAC**: the analogue capacitors and the output filter in a row above U10, the digital ones below. The AVDD bulk
  capacitor C38 moved next to the bead that feeds AVDD (it was beside the mixer).
* **Mixer**: the feedback resistor and capacitor of each channel beside the op-amp pins they connect, the input
  resistors below them, the ±12 V filters above and below.
* **Waveblaster volume**: U13 is turned a quarter so both channels face the mixer, and all four coupling capacitors
  are at the chip. On THT v2.1 the two input capacitors are more than 30 mm away, near the header, which leaves a
  long high-impedance track. The input pull-downs R31/R32 moved here with them.
* The analogue parts stay inside THT v2.1's AGND and AVDD plane islands, which are unchanged.

Neighbouring parts have at least 0.6 mm between their pads; the 0805 rows at the DAC and the mixer have 0.9 mm.
Every part has its reference on the silkscreen: 0.8 mm text beside the part, 1.0 mm for the chips. Five labels are
inside their part instead, where the part covers them once it is soldered: U13 and U16 between their pad rows, and
C23, R31 and R32 between their two pads. Those three are 0.45 mm text, the largest that fits the 0.8 mm between
0805 pads, and their footprints lose the two small body marks to make room; DRC lists the small text and the changed
footprints as six warnings. The mounting holes and J2 (named `SWD`) have no label. The silkscreen check finds no
new overlap: what it reports, the divider line at J3 and U12's corner mark on its pin 1, is on THT v2.1 as well.

## BOM

`docs/BOM-SMD.csv` lists every part with manufacturer part number and DigiKey number. The column **Part number**
says where each line comes from:

* `as on THT v2.1` (18 lines): the same part as on the through-hole board, taken from its BOM.
* `hw-smd BOM (2026-09-29)` (21 lines): the part and its DigiKey number come from the checked BOM of `hw-smd`.
* `checked 2026-11-24` (12 lines): the parts that are new here, looked up on DigiKey's product page and, where the
  land pattern or the pin-out depends on it, in the manufacturer's datasheet. The Notes column says what was read.

Stock and prices were not looked at for any line, and there are no cart upload files yet: they come when the board
is routed.

Three things from the check that are easy to get wrong when ordering:

* **C38** is EEE-FK1E101**X**P (6.3 mm can). EEE-FK1E101P without the X is 8 mm and does not fit.
* **C39–C42** are UWP1**V**2R2MCL1GB (35 V, 4 mm can). The 50 V UWP1H2R2MCL1GB is 5 mm and does not fit. They are
  bipolar and fit either way round; the + mark on the board comes with KiCad's footprint and means nothing here.
* **U9**: the SOT-223 MCP1826S has the same pin order as the TO-220 of THT v2.1 (VIN, GND, VOUT, tab on GND).
  DigiKey sells MCP1826S-3302E/DB only in tubes; the BOM names the taped MCP1826ST-3302E/DB.

## Next

1. Review of the placement.
2. Routing (4 layers, 0.2 mm tracks and clearances; the boot ROM corner should not need THT v2.1's 0.15 mm any more),
   DRC, then the Hardware workflow entry and JLCPCB files.
3. Prices, DigiKey and LCSC upload files.
