# PicoGUS THT v2.1 – PicoGUS 2.0 in through hole, on a Waveshare Core2350B (RP2350B)

Board revision **v2.1**: the [PicoGUS 2.0](../hw-chipdown) circuit (chip-down RP2040, SMD) rebuilt for hand assembly
with through-hole parts. The RP2040, its flash, crystal and core regulator are replaced by a
**Waveshare Core2350B2** module (RP2350B, 16 MB flash, **8 MB PSRAM**) on four 2x8 sockets, and the module's PSRAM is
the GUS sample RAM, so v2.0's separate APS6404L is gone. Everything else is v2.0's circuit with the same GPIO map:
ISA bus logic, PCM5100A DAC, M62429 Waveblaster volume, MIDI out, USB-A host port and IRQ/DMA jumpers. The card is
the [THT v1.3.2](../hw-tht-v1.3/README.md) outline (102.0 × 99.95 mm), with v1.3.2's bracket-side connectors (USB-A,
line out, MIDI out) and bracket tab holes in v1.3.2's places, so the [v1.3.2 bracket](../hw-tht-v1.3/README.md#bracket) fits.
v2.0's passive line-out mix (four 1k resistors) is replaced by the **op-amp mixer of the THT v1.3 boards** (NE5532 on
ISA ±12 V, Waveblaster +6 dB), with film coupling capacitors and a DC-coupled DAC path.

The parts that only exist as SMD are soldered on the card: U1/U2 (SN74CB3T3257, TSSOP-16) and U10 (PCM5100A,
TSSOP-20) on THT v1.3.2's test-pad TSSOP footprints (hand-solder toes and a 0.7 mm test pad per pin), U13 (M62429L)
on v2.0's SOIC-8. Everything else is through hole; see
[Parts with no through-hole version](#parts-with-no-through-hole-version).

> **Status: schematic done; board placed and routed (4 layers, DRC clean, matches the schematic), for review. Not
> built or tested. Firmware: ported and built (`picogus-v21.uf2`), not tested on hardware** (see
> [Firmware](#firmware) and [Layout status](#layout-status)). JLCPCB files: `hw-common/tools/jlcpcb.sh
> hw-tht-v2.1/PicoGUS-THT.kicad_pcb` with `ALLOW_DRC=courtyards_overlap` (the 11 accepted overlaps in the logic
> column); the Hardware workflow builds them on every change.

Files:

| File | What |
|---|---|
| `PicoGUS-THT.kicad_pro/.kicad_sch/.kicad_pcb/.kicad_dru` | KiCad **10** project. Root sheet: v2.0's ISA logic, power, USB, Waveblaster and module |
| `audio.kicad_sch` | PCM5100A DAC sheet (v2.0's, footprints only) |
| `mixer.kicad_sch` | Line-out mixer: NE5532 inverting summing amplifier (from THT v1.3), ±12 V filters, Waveblaster input pull-downs |
| `PicoGUS-THT-schematic.pdf` | Schematic PDF |
| `docs/BOM-THT.csv` | BOM with a **THT status** column (THT / THT but a different part than v2.0 / no THT version exists / module), the v2.0 part each line replaces, manufacturer, MPN, DigiKey and LCSC numbers, a digikey.no link, alternatives and digikey.no prices; then the kit items (module sockets and headers, jumpers), the optional IC sockets and the v2.0 parts that are gone |
| `docs/BOM-THT-digikey.csv` (+ `-optional`: the IC sockets) | DigiKey cart/BOM uploads (see [Cost](#cost)). Customer Reference `PicoGUS-THT-2.1.x: <references>` on every line, so the bags of a mixed order say which board they are for; at most 48 characters (DigiKey prints "Character Limit Exceeded" instead of a longer one), so the 15 × 0.1 µF line reads `C2-7,14-16,20,…` |
| `docs/BOM-THT-sockets.csv` | Optional: every IC socketed, with socket and part heights against the Waveblaster daughterboard. The same parts are the optional socket lines of `docs/BOM-THT.csv`, ordered with the `-optional` upload files |
| `docs/BOM-THT-lcsc-order.csv`, `docs/BOM-THT-lcsc.csv` (+ `-optional`) | The whole card from LCSC (see [Cost](#cost)): `-order` has every BOM line with the LCSC part, order quantity (LCSC minimums), stock and price on the day and what to buy elsewhere; the others are the uploads for LCSC's BOM tool, same customer references as the DigiKey ones |
| `tools/digikey_upload.py` | Writes both DigiKey upload files from `docs/BOM-THT.csv` (`python3 tools/digikey_upload.py [cards]`; more than one card gives `-<n>x` files) |
| `tools/lcsc_bom.py` | Writes the three LCSC files from `docs/BOM-THT.csv` with LCSC's live stock and prices (`python3 tools/lcsc_bom.py [cards]`) |
| `tools/gen_core2350b.py` | Generates the Core2350B footprint and 3D model in `hw-common` and checks the pin map |
| `docs/renders/` | 3D renders of the routed card: red from the front and the back, green, purple, white and black from the front (ENIG; white silkscreen, black on white). Made with `hw-common/tools/render.sh` |
| `docs/figures/routing.png` | The routed board: tracks per layer, vias and the AGND/AVDD islands |

The shared `PiGUS library` footprints are linked from `hw-common/`. **New in the shared library for v2.1:**
`Waveshare_Core2350B` (with a generated 3D model, `hw-common/parts/Waveshare_Core2350B.wrl`),
`USB_C_Receptacle_GCT_USB4085`, `TSSOP-16_4.4x5mm_P0.65mm_HandSolder_TestPads` and
`TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder_TestPads` (THT v1.3.2's: KiCad's TSSOP with the toes to ±3.9 mm and one
0.7 mm round test pad per pin in two staggered columns per side, at x ±4.45 / ±5.1 mm),
`USB_A_Molex_67643_Horizontal_ShieldPin5` (KiCad's Molex 67643 with the shield pads
numbered 5, as the `USB_A` symbol's shield pin) and `C_Film_P5.00mm_7.8x7.8mm_or_CP_D5.0mm_P2.00mm` (C39–C42).

## What changed compared to v2.0

The schematic is v2.0's (`hw-chipdown`), upgraded to KiCad 10 and edited. Its netlist was compared with v2.0's
connection by connection: apart from the changes in this table the two are identical (119 multi-pin nets each).
Reference designators are v2.0's; new ones are D3, R19, R20 and C46.

| v2.0 | v2.1 | Why |
|---|---|---|
| U12 RP2040 (QFN-56), U11 W25Q128 flash, Y1 12 MHz crystal, R12/R14/R15/R16, C24–C37 (the RP2040 sheet) | **U12 Waveshare Core2350B** module on four 2x8 sockets (or soldered on its headers) | No chip-down RP2040 by hand. The module has the RP2350B, 16 MB flash, crystal, its own 3.3 V LDO and the 27 Ω USB resistors |
| U9 AMS1117-3.3 (SOT-223) | **U9 MCP1826S-3302E/AB** (TO-220) | Through hole, and 250 mV dropout: VSYS is now one Schottky drop below +5V (see [Power](#power)) |
| Q1 AO3401A ideal-diode switch (+5V → VSYS), R17 | **D3 1N5819** | No good TO-92 P-MOSFET; a second Schottky as for USB (D2) |
| J6 micro-USB (SMD) | **J6 GCT USB4085** through-hole USB-C + **R19/R20** 5.1 kΩ CC pull-downs | Through hole; device only, never sources VBUS |
| U6 74LVC2G06 (SOT-363), open-drain MIDI and I/O CH RDY drivers | **U6 74AHCT126** (DIP-14) on +5V + **C46** | No DIP open-drain inverter that runs on 3.3 V; the v1.3 THT boards' solution (OE = the old input, A tied low, so OE high pulls the line low). Spare gates tied low |
| U4/U5 74LVC00, U3 74AHC14, U7 SN74CB3T3245 (TSSOP) | 74AHC00 ×2, 74AHC14, **74AHC244** (DIP) | AHC: 3.3 V supply with 5 V tolerant inputs (as on v1.2/v1.3) |
| U1/U2 SN74CB3T3257, U10 PCM5100 (TSSOP) | same chips, **SMD on the card** (TSSOP-16/-20 on v1.3.2's hand-solder footprints with a test pad per pin) | No through-hole version exists |
| U8 APS6404L PSRAM (SOIC-8, GPIO0–3, PIO SPI) | **removed**: the Core2350B2's 8 MB PSRAM on the RP2350B's QMI (CS1 = GPIO47) | No through-hole PSRAM exists; the module's is memory mapped and faster. Needs the firmware port. GPIO0–3 are free |
| U13 M62429L (SOP-8) | same chip, **SMD on the card** (SOIC-8, v2.0's footprint) | The DIP-8 M62429P is not buyable |
| R6–R9 (1k passive mix), C43/C44 | **U11 NE5532 mixer** (sheet `mixer.kicad_sch`): R21–R32, C24–C29 | As THT v1.3: Waveblaster ×2 (+6 dB) and PCM ×1 summed, 100 Ω outputs, buffered line out |
| C39–C42 2.2 µF electrolytic (SMD) | **2.2 µF polyester film**, dual footprint (film on 5 mm, or a bipolar electrolytic on 2 mm) | Audio coupling: non-polar, low distortion |
| 0402/0603/0805/1206 R/C, CP_Elec_4x5.4, SOD-323, 0603 LED, 1206 PTC, 0805 beads, TL3305 switch, JST-SH debug | axial resistors and beads, radial MLCC discs and electrolytics, DO-41, 3 mm LED, radial PTC, 6 mm tactile switch, 1x3 header | Through hole |
| C11 47 µF (+3V3) | C11 10 µF | MCP1826S: at most 22 µF on its output |
| C38 100 µF ceramic | C38 100 µF electrolytic | Through hole |
| Card outline 102.87 × 96.74 mm, v2.0 bracket | **THT v1.3.2's outline** (102.0 × 99.95 mm) and bracket: J7 (MIDI out, top), J9 (line out) and J5 (USB-A, now v1.3.2's **Molex 67643**), H1/H2 and the ISA fingers at v1.3.2's positions | Reuses the v1.3.2 bracket; the 102 mm card is v1.3.2's cheaper JLCPCB size. The bracket's volume-pot hole is unused (v2.1's Waveblaster volume is digital) |
| 4 layers: In1 GND + AGND, In2 +3V3 + AVDD | 4 layers: In1.Cu GND plane + AGND island, In2.Cu +3V3 plane + AVDD island, under the audio section | As v2.0 |
| `+3.3V` power symbols | `+3V3` | The RP2040 sheet joined the two names; without it they would have split |

Unchanged references: J1 (ISA), J3 (IRQ/DMA), J7 (MIDI out), J8 (Waveblaster), J9 (line out), H1/H2, and every
GPIO assignment except GPIO0–3 (PSRAM, now free). Board positions: see [Layout status](#layout-status).

## Parts with no through-hole version

These are the v2.0 parts that cannot be replaced by a through-hole part. Everything else on v2.0 has a DIP, TO-220,
axial or radial equivalent (`THT status` column of `docs/BOM-THT.csv`).

| v2.0 part | Package | Why there is no THT replacement | v2.1 |
|---|---|---|---|
| **RP2040** (+ W25Q128 flash, 12 MHz crystal) | QFN-56, SOIC-8, 3225 | No THT microcontroller of this class | **Waveshare Core2350B2 module** (RP2350B QFN-80 + 16 MB flash + crystal + 8 MB PSRAM) on four 2x8 headers |
| **APS6404L** 8 MB PSRAM | SOIC-8 | No THT PSRAM; the largest DIP SRAM (23LC1024) is 128 KB, a GUS needs 1 MB | **On the module**: the Core2350B2's PSRAM (firmware port needed) |
| **SN74CB3T3257** ×2 (5 V tolerant bus switches) | TSSOP-16 (no DIP/SOIC) | TI makes it in TSSOP/SSOP/TVSOP/VQFN only; the ISA interface needs it | Same chip, **SMD on the card** (TSSOP-16, 0.65 mm pitch) |
| **PCM5100A** DAC | TSSOP-20 only | The whole PCM510x family is TSSOP-20 | Same chip, **SMD on the card** (TSSOP-20, 0.65 mm pitch) |
| **M62429** volume control | SOP-8 (DIP-8 M62429P discontinued) | No DIP part in stock at DigiKey or LCSC; DIP clones (FM62429, U2429) only at hobby shops | Same chip, **SMD on the card** (SOIC-8, 1.27 mm pitch) |
| micro-USB receptacle | SMD | – | **GCT USB4085** through-hole USB-C: through hole, but 0.85 mm contact pitch (the hardest joint on the card) |

Replaced by a different through-hole part (circuit change): 74LVC2G06 → **74AHCT126** (U6), AO3401A → **1N5819**
(D3), AMS1117 → **MCP1826S** (U9), 74LVC00 → 74AHC00, SN74CB3T3245/74LVC244 → 74AHC244.

## Cost

digikey.no prices of 2026-10-30, for one board, in NOK; shipping not included. Per-line prices are in `docs/BOM-THT.csv`.

| | NOK excl. MVA | NOK incl. 25 % MVA |
|---|---|---|
| DigiKey order, `docs/BOM-THT-digikey.csv` (43 lines) | 580.00 | 725.00 |
| Optional IC sockets, `docs/BOM-THT-digikey-optional.csv` (U3–U7, U11) | 58.98 | 73.72 |
| Both | 638.98 | 798.73 |

* **Not in the DigiKey order**: the Core2350B2 (waveshare.com, US$6.99) and the M62429L (LCSC C52260216, minimum 5),
  and the PCB.
* J5 is now v1.3.2's Molex 67643 (DK WM4993-ND, 16.58 NOK, price of 2026-10-31), so it is in the DigiKey order.
* **C39–C42** (2.2 µF film, 61 NOK) are on backorder at DigiKey until about April 2027: see the LCSC film part or the
  bipolar electrolytic in [Audio](#audio).

**LCSC instead** (lcsc.com prices and stock of 2026-11-06, US$, shipping not included; `docs/BOM-THT-lcsc-order.csv` has every
line):

| | US$ |
|---|---|
| LCSC order, `docs/BOM-THT-lcsc.csv` (40 lines, quantities raised to LCSC's minimums) | 50.93 |
| Optional IC sockets, `docs/BOM-THT-lcsc-optional.csv` | 7.24 |

* **Not at LCSC**: U3 SN74AHC14N, U6 SN74AHCT126N and U9 MCP1826S-3302E/AB (none in stock, and no LCSC part fits
  without a design change: the 74HC14 does not take 5 V inputs at 3.3 V, the 125 has the opposite enable, the
  LM2940 clones want a larger output capacitor with ESR): buy them at DigiKey (296-4550-5-ND, 296-4659-5-ND,
  MCP1826S-3302E/AB-ND, about 30 NOK). The Core2350B2 comes from Waveshare.
* **Different parts from the BOM** (all in the `Note` column): U7 is TI's SN74LVC244AN (same pinout, 5 V-tolerant
  inputs at 3.3 V like the AHC244, stronger outputs), J7/J9 the XKB clones of the CUI jacks (as on THT v1.3.2),
  C21/C22 Dersonic C0G discs, R27/R28 and R31/R32 the taped Yageo MFR-25, and the generic headers, sockets,
  ferrites, film and electrolytic capacitors named in `docs/BOM-THT.csv`.
* U4/U5 (SN74AHC00N): 60 in stock at LCSC on the day, at US$1.46 each.

## Audio

* **DAC**: PCM5100A (U10) as v2.0, output RC filter R10/R11 470 Ω + **C0G** C21/C22 2.2 nF (polypropylene film
  WIMA FKP2 is the alternative). Charge pump and decoupling MLCCs per the TI datasheet (X7R).
* **Waveblaster volume**: M62429L (U13) on AVDD as v2.0, inputs pulled to AGND by 100k (R31/R32, as THT v1.3),
  **film** coupling capacitors in and out (C39–C42).
* **Mixer** (`mixer.kicad_sch`, THT v1.3's): U11 NE5532 inverting summing amplifier on ISA ±12 V through 10 Ω / 47 µF
  (R29/R30, C28/C29). Waveblaster through 10k (×2, +6 dB), PCM through 20k (×1), feedback 20k ‖ 47 pF C0G (pole about
  170 kHz), 100 Ω output resistors to the jack. The PCM5100A's output is ground centred, so the PCM path is DC coupled:
  no electrolytic anywhere in the PCM path. U11 can be socketed for an OPA2134PA.
* **Capacitor choices** (`Alternatives` column of the BOM): 2.2 µF film at 5 mm pitch is scarce: DigiKey had none in
  stock on 2026-10-29 (TDK B32529D0225J000 on backorder), LCSC has Faratronic C241J225J2SC000. The footprint also
  takes a 5 mm bipolar electrolytic (Rubycon 50NXA2R2MEFC5X11, in stock) on its inner holes. Nichicon's Muse ES/BP, FG
  and FW are discontinued in 5 mm cans, so AVDD bulk C38 is a Panasonic FR (EEU-FR1C101).

* **Ground and supply planes**: as on v2.0, AGND (In1.Cu) and AVDD (In2.Cu) are copper islands cut out of the GND and
  +3V3 planes under the audio section (x 140–209.6, y 47.5–81.7, see `docs/figures/routing.png`): the DAC's analog
  pin row and output filter, the volume control, the film capacitors, the mixer and the line-out jack. They meet GND
  and +3V3 only at FB3 and FB4, side by side across the island's lower left edge. The Waveblaster header, the MIDI
  jack and the DAC's digital row stay outside them. Digital tracks cross the islands only where they must: the volume
  control's clock and data (about 18 and 14 mm, to U13 on the island), MIDI (about 45 mm of UART_TX to the
  Waveblaster and the MIDI resistors, and 16 mm from R2 to J7: both are beyond the island), 13 mm of RI/OCHRDY and 4
  mm of DIN. The ±12 V supplies of the mixer and the +5 V of the USB-A port also run over them.

## Core2350B module

The Core2350B is 25.4 × 25.4 mm with 64 plated holes on a 2.54 mm grid: a 10×10 outer ring and an 8×8 inner
ring. Waveshare's schematic draws them as four 2x8 headers, P1–P4, arranged as a pinwheel, so **four standard 2x8
female headers** take the module (BOM lines `(U12 socket)` and `(U12 headers)`). The footprint numbers the pads
16·(header − 1) + pin. Seen from the top, pin 1 of each header is a GND corner; the module's chamfered corner
(pad 1, P1.1) is at the top left:

```
  G   0  VB  3E RUN UDP UDM  BS  47   G      VB  VBUS (module LDO input)      3E  3V3_EN (100k up to VBUS)
  2   1 3V3   G  AV  SC   G  SD  46  45      3V3 module LDO output            AV  ADC_VREF
  4   3                          43  44      UDP/UDM  USB D+/D- (27R inside)  SC/SD  SWCLK/SWDIO
  6   5                          41  42      BS  BOOTSEL                      G   GND
  8   7                          39  40      numbers: GPIO
 10   9                          37  38
 12  11                          35  36      P1 = left two columns (GPIO0-14)
 14  13                          33  34      P2 = top two rows (USB, SWD, RUN, BOOTSEL, power, GPIO45-47)
 15  16  18  20  22  24  26  28  31  32      P3 = bottom two rows (GPIO15-29)
  G  17  19  21  23  25  27  29  30   G      P4 = right two columns (GPIO30-44)
```

Source: Waveshare's [Core2350B schematic](https://files.waveshare.com/wiki/Core2350B0/Core2350B.pdf) (P1–P4) and
[pinout drawing](https://www.waveshare.com/wiki/Core2350B0); `tools/gen_core2350b.py` checks that every GPIO
appears once and that the cells form the two rings. On the module: the user LED is on **GPIO39**, and on the
Core2350B1/B2 the PSRAM chip select (QMI CS1) is **GPIO47** (through a 0 Ω link to the edge pin).

* **U12 is placed at 0°**: GPIO0–14 (P1) face the logic on the left, GPIO15–29 (P3) face the ISA fingers,
  USB/SWD/RUN/BOOTSEL/power (P2) face up and the unused GPIO30–44 (P4) face the bracket.
* **Variants**: Core2350B0 (no PSRAM, $4.99), B1 (2 MB, $5.99), **B2 (8 MB, $6.99, recommended)**; the `-Kit`
  versions add Waveshare's USB-C FPC adapter. Not sold by DigiKey: buy from waveshare.com (SKU 31690 for the B2) or a
  Waveshare reseller.
* **Heights**: on 8.5 mm female headers the module's top is at about 12 mm. It sits right of the Waveblaster
  daughterboard area, so that is fine. Soldered straight into the card on its male headers, the module sits at about
  3.5 mm.
* **Low-profile socket (alternative)**: four **Mill-Max 803-43-016-10-002000** (2x8 double-row low-profile socket,
  4.2 mm tall and 5.08 mm wide per Mill-Max's series 800–803 drawing; 0.56 mm tails; the contacts take 0.64 mm square
  pins, so the same male headers fit) put the module top at about 7.7 mm. On LCSC: XFCN PM254V-12-08-H355-U (C55595101,
  3.55 mm, 5.0 mm wide; about 7 mm) or Hong Cheng HC-PM254-5.0H-2x8PZ (C27985236, 5 mm, gold). The footprint takes
  them as is (1.0 mm holes, 2x8 blocks; a part wider than 5.08 mm would not fit, as the two rings are side by side).
  Fit the male headers short end down into a low-profile socket so their plastic sits on it, long end up through the
  module. The same idea as PicoPew's low-profile headers (hackaday.io project 167369, log 170776). The `(U12 socket)`
  line of `docs/BOM-THT.csv` lists them as alternatives; the upload files keep the Sullins part.

## GPIO map (= v2.0)

| GPIO | Net | GPIO | Net |
|---|---|---|---|
| 0–3 | free (v2.0: U8 PSRAM; the GUS RAM is the module's PSRAM now) | 16 | DIN (I2S to U10) |
| | | 17 | BCK |
| | | 18 | LRCK |
| | | 19 | ~RDACK |
| 4 | ~RIOW | 20 | RTC |
| 5 | ~RIOR | 21 | RIRQ |
| 6–13 | AD0–AD7 (U1/U2 bus switches) | 22 | RDRQ |
| 14 | RA8 | 23 | LED (D1) |
| 15 | RA9 | 24, 25 | RV_DATA, RV_CLK (U13 M62429) |
| 26 | RI/OCHRDY (U6) | 27 | ADS (U1/U2 select, U3) |
| 28 | UART_TX (MIDI, J4, Waveblaster) | 29 | GND (board ID, as v2.0) |
| 30–46 | not connected (39: module LED) | 47 | module PSRAM CS (QMI CS1), on the module |

## Power

* **VSYS** = +5V (ISA) through **D3** or USB VBUS (J6) through **D2**, both 1N5819 (as the Pico's VSYS). This
  replaces v2.0's AO3401A ideal diode: VSYS is about 0.3–0.4 V below +5V.
* **U9 MCP1826S-3302E/AB** (TO-220, 1 A, 250 mV dropout, **lying down, tab down**: `TO-220-3_Horizontal_TabDown`,
  so it stays well under the Waveblaster daughterboard) makes the card's +3V3 from VSYS: logic, bus switches,
  DAC (AVDD through FB4) and M62429. With an AMS1117-class regulator (about 1.1 V dropout) a low ISA +5V would
  not leave enough headroom after the diode. Its pinout is VIN–GND–VOUT, tab GND (**not** the 1117 pinout, so an
  LD1117V33 does not fit). C11/C13 (10 µF) + C10 (1 µF) on its output: Microchip asks for at least 1 µF and at most
  22 µF. C12 (10 µF, input) is about 10 mm from the VIN pin, joined by a 0.4 mm track.
* **The module powers itself**: VSYS goes to the module's **VBUS** pin, and its own LDO (ME6217C33, 800 mA, 3V3_EN
  pulled up on the module) makes the module's 3.3 V. The module's **3V3 pin is not connected**: the ME6217 must not
  see VOUT above VIN + 0.3 V, so it cannot be back-fed from U9. Both rails are 3.3 V ± 2 %; the RP2350's GPIO0–39 are
  5 V tolerant while it is powered and 3.3 V failsafe when it is not.
* The module's own FPC USB port (the `-Kit` adapter) also works for programming: its VBUS powers the module, and
  through the module's VBUS pin also VSYS and U9 (D2/D3 keep it off the ISA +5V and J6).
* **Reset (RUN)**: as on v2.0, ISA RESET DRV reaches the module's RUN pin through U5 (pin 8), so a PC reset also
  resets the RP2350, and RUN goes back into U5 (pin 5) so that a reset also clears BUSOE. U5 pin 8 now drives RUN
  **through R4 (1 kΩ in series)** instead of v2.0's pull-up, so the **RUN button SW2** (6 × 6 mm, next to BOOTSEL SW1)
  can pull RUN to GND without shorting the gate output (3.3 mA while pressed). C45 (0.1 µF) on RUN filters it. To
  enter the bootloader: hold BOOTSEL, press and release RUN, release BOOTSEL.

## USB

As on v2.0, the RP2350's one USB port goes to both **J5** (USB-A host: joystick, mouse, keyboard, storage, hub; VBUS
from +5V through F1) and **J6** (USB-C, for programming: BOOTSEL + drag-and-drop, and powering the card from USB
alone). Use one at a time. J6 has 5.1 kΩ Rd on CC1/CC2, so any USB-C charger or host supplies 5 V; it never sources
VBUS.

**J6 is a GCT USB4085-GF-A**, the through-hole USB-C 2.0 receptacle: 16 contacts on a 0.85 mm pitch in two rows (0.4
mm holes, 0.15 mm between pads), plus four shell legs. It is the hardest part to solder on the card: use a fine tip,
thin solder and flux, and check for bridges with a magnifier. The footprint has a 0.13 mm local pad clearance and
`PicoGUS-THT.kicad_dru` allows 0.13 mm / 0.4 mm hole-to-hole between J6's own pads (the board minimum clearance is
0.13 mm only for that; every netclass keeps 0.2 mm). The contacts' holes are 0.45 mm apart (GCT's land pattern):
check that the PCB fab accepts it.

## Firmware

**The firmware for v2.1 is on this branch, built but not tested on hardware**: `picogus-v21.uf2` (the multi-mode
bundle, like `picogus.uf2`) and single-mode `pg-*-v21.uf2` images. Build them with

```
sw/scripts/build_variants.sh -p picogus21 all
```

or `cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=picogus21`. Details are in [sw/docs/rp2350.md](../sw/docs/rp2350.md#picogus-v21-tht-core2350b2-rp2350b):

* Built on the RP2350 port (Pico 2) merged from branch `claude/sweet-goodall-x5r6j6`, plus a board header
  (`sw/boards/picogus21.h`: RP2350B, 16 MB flash, PSRAM CS on GPIO47).
* **Board type at compile time**: v2.0 is recognised by GPIO29 on the ADC, which the RP2350B cannot do (its ADC is
  GPIO40–47), so the v2.1 build is a PicoGUS 2.0 by definition: LED on GPIO23, M62429 on GPIO24/25, same ISA, I2S
  and MIDI pins.
* **GUS sample RAM in the module's PSRAM** (QMI window 1, memory mapped). The GUS code's `psram_*` calls become memory
  accesses; at boot the PSRAM is identified, reset, switched to QPI and set up for quad reads/writes at 92.5 MHz
  (370 MHz / 4), with the read sample delay calibrated on a test pattern. The setup is re-applied after every clock
  change and settings save, and a settings save holds core 1 (which plays from the PSRAM) while the flash is written.
* Use the `-v21` files on this board: the bootloader and `pgusinit /flash` accept any RP2350 image and cannot tell
  a Pico 2 image from a v2.1 one.
* A Core2350B0 (no PSRAM) runs every mode except GUS, which stops at the PSRAM check.

## Mechanical

* **Outline**: THT v1.3.2's (101.90 × 99.85 mm at the line centres, 102.0 × 99.95 mm over the lines), without its
  Pico micro-USB relief slots.
* **Bracket end as on THT v1.3.2's final layout (commit 258c8f8)**, top to bottom: J7 MIDI OUT at (217.00, 49.00),
  J9 LINE OUT at (217.00, 75.50) and J5 USB-A (Molex 67643) at (208.64, 92.50), with the bracket tab holes H1/H2 at
  x 214.70 and the ISA fingers in v1.3.2's places, so the [v1.3.2 bracket](../hw-tht-v1.3/README.md#bracket) fits
  (its drawing is on User.Drawings). Its WT VOL hole between the two jacks stays unused: v2.1's Waveblaster volume is
  digital, and U13 with its output capacitors sits behind that hole, where v1.3.2 has RV1.
* **J6 (USB-C) at the bracket end**, centred at y 108.89, where v1.3.2 has the Pico's micro-USB. It faces the
  bracket with the footprint's PCB-edge line on the board edge, so its front is 2.5 mm past the edge (x 220.80), 0.9
  mm behind the inner face of the bracket drawing's plate. As on v1.3.2, the bracket has no hole there (v1.3.2's
  README: a USB plug cannot enter the slot opening at that height), so J6 is for programming and powering the card on
  the bench with the bracket off. In the PC, update the firmware with `pgusinit`.
* **J8 (Waveblaster)** with pin 1 where v1.3.2 has it, (158.89, 39.63), and like v1.3.2 without daughterboard
  standoff holes (`Wavetable_NoMountingHoles`; v2.0 had two, 6.35 mm right of pin 1): the daughterboard is held by
  its header. **J3 (IRQ/DMA)** at v1.3.2's position, pin 1 (136.03, 124.91).
* **Under the Waveblaster daughterboard** (a full-size board's outline ends at x 170.32 and y 121.9; its underside
  is about 11 mm above the card): the logic DIPs, the SMD chips (U1, U2, U10), the DAC block, FB3/FB4, R31/R32, the
  disc capacitors, U9 lying down and D3. C38 (AVDD bulk, 5 × 11 mm) was under it and has moved out (see [Layout
  status](#layout-status)). **C39** (2.2 µF film, about 11 mm tall) was 3 mm inside the outline; C39 and C40 moved
  2.19 mm right together, as far as the copper logo then allowed (C40's outer pad keeps the audio net class's 0.35 mm
  from the "P"). C39's 7.2 mm film body now ends at x 170.09, 0.23 mm short of the outline (its 7.8 mm footprint envelope
  0.5 mm): a full-size daughterboard sitting low could just touch its top corner. Small daughterboards (DreamBlaster
  S2, X2) are nowhere near it.

## Layout status

**Placed and routed (2026-11-07), for review.** The placement and the silkscreen labels are the user's (the DAC
block turned and moved under the daughterboard; regulator, module, USB-C parts and headers moved), with the changes
listed below. **DRC with schematic parity: 0 unconnected items, no clearance, hole or parity errors.** What is left:
the 11 accepted courtyard overlaps of the logic column (U3–U7 on v1.3.2's 12.05 mm pitch with their decoupling discs
and R1/R5/R18 between them) and 16 warnings for connector silkscreen past the bracket edge (J5, J6, J7 and J9 hang
over it, as on v1.3.2).

![The routed board: F.Cu red, B.Cu blue, In2.Cu green, AGND/AVDD islands shaded](docs/figures/routing.png)

* **Stack-up**: F.Cu and B.Cu signals; **In1.Cu GND** with the **AGND island**, **In2.Cu +3V3** with the **AVDD
  island** (see [Audio](#audio)); FB3 (GND–AGND) and FB4 (+3V3–AVDD) are the only ties. 0.2 mm signal tracks, 0.4 mm
  for +5V, VSYS, the USB VBUS nets and ±12 V; the plane nets reach their planes through short stubs and vias. 0.6/0.3
  mm vias. About 2.5 m of track on each outer layer and 139 vias.
* **In2.Cu carries eight short signal hops** (52 mm in all, the longest 12 mm: D5, ADS, D4, D7, GPIO23, D2, AD3, AD1)
  where the band above the ISA fingers is full on both outer layers. They cut slots in the +3V3 plane, not in GND.
* **Routing rules**: no copper between the gold fingers (rule areas between them; tracks leave each finger straight
  up) and no vias over the fingers (rule area from y 128.3); digital tracks kept off the analog islands where possible
  (see [Audio](#audio) for what crosses). No track or via runs on F.Cu under U9, whose metal back (GND) lies on the
  board.
* **TSSOP escape**: U1, U2 and U10 are routed through their 0.7 mm test pads, with vias in most test pads, so their
  pins reach both outer layers without fanning out past the toes.
* **Changes to the user's placement**: R4 in series on RUN and the RUN button SW2 next to BOOTSEL SW1 (J10's 1×2
  header is gone; see [Power](#power)); U9 lying down, tab down; `v2.1.0` at (212.84, 40.58) with the copper logo
  (see below); H1/H2 without silkscreen; on the back, the title, copyright, licence, project URL and thanks in one
  framed group behind U1/U2. **C38 moved out from under the Waveblaster daughterboard** (its 5 × 11 mm can was
  taller than the daughterboard's 11 mm clearance) to (184.75, 77.0), between the mixer and RUN, still on the
  AVDD/AGND islands: its pads go straight into the In2 AVDD and In1 AGND islands, so it needs no tracks. For the
  audio this is the same capacitor: it is the AVDD reservoir behind FB4, and at audio frequencies the island planes
  join it to the DAC with milliohms, while C17 (10 µF) and C15/C16 (0.1 µF) stay at U10's pins for the local
  decoupling. C38 is now in the `Mixer + line out` group; the C29 label moved above C29, as C28's is. **C39 and C40
  moved 2.19 mm right together** (to x 171.19 / 179.69, pad 1), to get C39 out from under the daughterboard; their
  four track ends were stretched onto the moved pads. **The PicoGUS logo and `v2.1.0` then moved 1.02 mm right as
  one group**, so they sit centred between C40 and the board edge: 0.77 mm from C40's silkscreen outline to the "P"
  and from the end of `v2.1.0` to the edge (letters x 186.92–212.69, `v2.1.0` at (212.84, 40.58)).
* **Labels to look at** (visible references over a neighbour's courtyard): C23 under J9's body and U13 under C41
  (both hidden once the parts are fitted), J6 over D2, F1 over SW1, D1 over R13, C16 over C14.
* **PicoGUS logo in copper, as on v1.3.2**: the eight solder-mask openings (the letters) with copper letters 0.15 mm
  larger under them, and a rule area that keeps tracks and vias off them. `v2.1.0` stands right of it (Jost 1.2 mm).
* **Silkscreen labels** (Jost, embedded in the board file as on v1.3.2): `WAVEBLASTER`, `MIXER`, `DAC`, `WT VOL`,
  `MIDI OUT`, `LINE OUT`, `USB HOST`, `USB-C FLASH`, `RUN`, `BOOTSEL`, `UART`, `SWD`, and `IRQ`/`DMA` over J3.
* **Groups, as on v1.3.2**, so a block moves as one: `74AHCT126 (U6)`, `74AHC00 (U4)`, `74AHC00 (U5)`,
  `74AHC14 (U3)` (with the column resistors), `74AHC244 (U7)`, `ISA bus switches (U1, U2)`,
  `Waveblaster header (J8)`, `DAC (U10)`, `Mixer + line out (U11, J9)`, `WT volume (U13)`, `MIDI out (J7)`,
  `USB host (J5)`, `USB-C (J6)`, `Core2350B + power (U12)`, `IRQ/DMA jumpers (J3)`, `PicoGUS logo`,
  `Copyright and attribution (back)` and the bracket drawing.
* **R21–R32 (mixer) are on 10.16 mm pitch** like every other resistor on the card.
