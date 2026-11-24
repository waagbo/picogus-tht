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
> issues, 418 connections still to route. The BOM is a draft: see [BOM](#bom).
> The board is not in the Hardware workflow yet, because an unrouted board fails its DRC.

![Placement, front](docs/renders/PicoGUS-SMD-v2.1.0-placement-red-front.jpg)

| File | What |
|---|---|
| `PicoGUS-SMD.kicad_pro/.kicad_sch/.kicad_pcb/.kicad_dru` | KiCad **10** project. The board is the THT v2.1 board (outline, stackup, plane zones, logo, edge connector, connectors and modules in place) with the SMD parts placed and no tracks |
| `audio.kicad_sch`, `mixer.kicad_sch`, `bootrom.kicad_sch` | The DAC, line-out mixer and boot ROM sheets of THT v2.1 with SMD footprints |
| `PicoGUS-SMD-schematic.pdf` | Schematic PDF |
| `docs/BOM-SMD.csv` | Draft BOM: see [BOM](#bom) |
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

Every part has its reference on the silkscreen beside it (0.8 mm text; chips 1.0 mm). The silkscreen check finds no
new overlap; the two overlaps and two clipped items it reports are on THT v2.1 as well.

## BOM

`docs/BOM-SMD.csv` lists every part with manufacturer part number and DigiKey number where one is known. The column
**Part number** says how far each line can be trusted:

* `as on THT v2.1`: the same part as on the through-hole board, taken from its BOM.
* `hw-smd BOM (2026-09-29)`: the part and DigiKey number come from the checked BOM of `hw-smd`. Stock and price were
  not looked at again.
* `checked 2026-11-24`: looked up for this board; the note says against what.
* `NOT CHECKED`: a candidate part number that has not been verified. Check it before ordering.

There are no prices and no cart upload files yet: they come when the board is routed.

## Next

1. Review of the placement.
2. Routing (4 layers, 0.2 mm tracks and clearances; the boot ROM corner should not need THT v2.1's 0.15 mm any more),
   DRC, then the Hardware workflow entry and JLCPCB files.
3. Prices, DigiKey and LCSC upload files.
