# PicoGUS THT (through-hole) v1.3.2 – based on PicoGUS v1.2

Board revision **v1.3.2**: the v1.3.1 (rev B) THT board on a 102 mm outline, with
the fine-pitch chips soldered straight to the card and a test pad on every one of their pins.

* **U3/U4** (SN74CB3T3257, TSSOP-16) and **U9** (PCM5102A, TSSOP-20; the PCM5100A/5101A fit too) only exist as TSSOPs. They are soldered to
  the card on hand-solder footprints, and **every pin has its own 0.7 mm round test pad** in line with it, for a
  probe, a meter or a scope (see [U3/U4 and U9 with a test pad per pin](#u3u4-and-u9-with-a-test-pad-per-pin)).
  The TSSOP-to-DIP adapter boards of the earlier v1.3.2 layout are gone, and so are their sockets.
* **The GY-PCM5102 module option (J10) is gone.** The on-board DAC is always fitted.
* **New placement** of the DAC block, the mixer and the jacks (2026-10-31 / 2026-11-01): the DAC sits above U3/U4
  with its analog side facing east toward the output filter and the mixer above it, MIDI OUT at the top of the
  bracket end and LINE OUT below WT VOL (see [Placement](#placement)).
* **C36** (U9 charge-pump decoupling) is a through-hole 0.1 µF disc, and all eight DAC decoupling discs sit on the
  side of U9 where their pins are (see [DAC decoupling](#dac-decoupling)).
* **C8/C10** (U8 PSRAM decoupling) are the v1.3.1 through-hole discs, next to U8.
* **1.84 mm shorter**: 102.0 mm long over the outline (v1.3.1: 103.8 mm), trimmed at the bracket end. The bracket tab
  holes H1/H2 are 1.70 mm further from the bracket plate, so the card needs a bracket with longer tabs (see
  [Bracket](#bracket)).
* **No artwork on the back silkscreen**: the princess artwork is to be UV printed instead. The back texts stay.

The main board is through-hole apart from **U3, U4, U9** (TSSOP, 0.65 mm pitch), **U8** (APS6404L PSRAM, SOIC-8,
1.27 mm pitch) and the optional **U12** (USBLC6-2SC6, SOT-23-6), all on the front. The rest of the circuit is
v1.3.1's: USB-A host port, Waveblaster header, op-amp mixer.

> **Status: v1.3.2, ready to order (tag `hw-tht-v1.3.2`); not built or tested yet.** The placement of 2026-11-01 is
> routed on two layers (2026-11-01).
> The schematic's ERC is the same as v1.3.1's (280 warnings, 0 errors). DRC with schematic parity finds 0
> unconnected items and, as errors, only the 12 accepted courtyard overlaps in the logic column, so
> `hw-common/tools/jlcpcb.sh` exports with `ALLOW_DRC=courtyards_overlap`; `build/jlcpcb/` holds that export (not in git: the repository
> ignores `build/`). The first routed TSSOP layout (commit 298e993) was reviewed independently; this placement
> shortens the DAC outputs and the mixer's summing nodes that review pointed at, and its route applies the review's
> rules from the start. Compared with the routed DIP-adapter layout (commit d5c4a85) it is better on I2S, the
> DAC-to-mixer path and the vias at silkscreen text, and worse on the ISA data-bus length, which the test pads set:
> see [Layout status](#layout-status).

Files:

| File | What |
|---|---|
| `PicoGUS-THT.kicad_pro/.kicad_sch/.kicad_pcb` | KiCad **10** project (the root sheet is the v1.2 logic). The board is routed |
| `audio.kicad_sch` | PCM510x DAC sheet: U9 a TSSOP-20 with test pads, C36 a THT disc, no J10 |
| `wavetable.kicad_sch` | Waveblaster header, WT volume, op-amp mixer, line-out jack (as v1.3.1) |
| `usb.kicad_sch` | USB host port (J11 USB-A, F1, C34/C35, U12) (as v1.3.1) |
| `PicoGUS-THT-schematic.pdf` | Schematic PDF |
| `docs/BOM-THT.csv` | BOM with DigiKey **and** LCSC part numbers, stock (2026-11-07) and a digikey.no link per line |
| `docs/BOM-THT-digikey.csv`, `docs/BOM-THT-lcsc.csv` (+ `-optional`) | DigiKey and LCSC upload files for one board, made from `BOM-THT.csv` (see [Ordering](#ordering)); optional: sockets (and U12 for LCSC; the DigiKey file has U12, marked "(optional)") |
| `docs/BOM-THT-sockets.csv` (+ `-digikey-optional`, `-lcsc-optional`) | Socket build: every DIP IC and the Pico socketed, with heights (see [Socket build](#socket-build)) |
| `docs/renders/` | 3D renders of the routed card: red from the front and the back, green, purple, white and black from the front (ENIG; white silkscreen, black on white). Made with `hw-common/tools/render.sh` |
| `docs/figures/` | Figures for the README's exceptions and design choices (e.g. the test pads, the placement options, the accepted courtyard overlaps), with the scripts that make them in `docs/figures/src/` |

The shared `PiGUS library` symbols and footprints are linked from `hw-common/`, like the other boards. **New in the
shared library for v1.3.2:** `TSSOP-16_4.4x5mm_P0.65mm_HandSolder_TestPads` (U3, U4),
`TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder_TestPads` (U9) and `RPi_Pico_SMD_TH_USB_NoSWD` (U1, see
[102 mm board](#102-mm-board)), in `hw-common/PiGUS library.pretty`; DRC's library check finds the board's U1, U3,
U4 and U9 identical to them. The adapter footprints and adapter 3D models of the earlier v1.3.2 layout are removed;
the older `DIP-16_W7.62mm_TSSOP-16_Nested` stays unused.

Every part has a 3D model. The ones KiCad 10's library does not have are in `hw-common/parts`, reached through the
project folder's `parts` link: the CUI SJ1-3535NG jacks (J8 green; J5 a black copy of the same CUI model), the Alps
RK09712 pot (RV1) and a simple generated Bourns MF-R050 (F1). U3/U4/U9 use KiCad's TSSOP models, U1 KiCad's own
`RaspberryPi_Pico.step`, and J9's header model path is fixed. The library footprints carry the same models as the
board.

## What changed compared to v1.3.1

### U3/U4 and U9 with a test pad per pin

| | U3, U4 (SN74CB3T3257, TSSOP-16) | U9 (PCM5102A, TSSOP-20) |
|---|---|---|
| Footprint | `PiGUS library:TSSOP-16_4.4x5mm_P0.65mm_HandSolder_TestPads` | `PiGUS library:TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder_TestPads` |
| Courtyard | 11.4 × 5.75 mm | 11.4 × 7.05 mm |
| Position (centre) | U3 (150.22, 110.10), U4 (161.44, 110.12), both 90° (pin 1 at the bottom left) | (156.10, 84.53), 180° (pin 1 at the bottom right) |

* **Land pattern**: the TSSOP hand-solder pads (1.775 × 0.40 mm at ±3.01 mm from the centre, toes out to ±3.9 mm),
  long enough to put an iron on each pin.
* **One test pad per pin**: a 0.7 mm round pad (bare copper, `testpoint` property) straight out from each pin,
  joined to it by a 0.2 mm track. The pads alternate between two columns, 4.45 and 5.10 mm from the centre, so each
  sits on its own pin's line and 1.3 mm from the next pad in its column; the track to an outer pad passes between two
  inner pads with 0.2 mm on each side, the board's normal clearance. The footprint has the pin numbers at the column
  ends in 0.8 mm silk. The pads make every pin reachable for a probe, a meter or a scope clip, and a solder bridge
  between two pins shows up as a short between their pads.
* **Routing**: an outer-column pin is routed from its test pad. An inner-column pin that is used gets a 0.2 mm
  escape stub straight on from its test pad, centred between the two outer pads, to 5.80 mm from the centre, and is
  routed from the stub end (26 stubs; the two left unused are removed after routing, see
  [Layout status](#layout-status)). The router cannot squeeze a track between two outer pads (0.6 mm
  gap) any other way, and the stubs keep the 0.2 mm clearance exactly as the pin-to-pad tracks do.
* **Pin numbers stay the schematic's**: U9 is 1 CPVDD, 2 CAPP, 3 CPGND, 4 CAPM, 5 VNEG, 6 OUTL, 7 OUTR, 8 AVDD,
  9 AGND, 10 DEMP, 11 FLT, 12 SCK, 13 BCK, 14 DIN, 15 LRCK, 16 FMT, 17 XSMT, 18 LDOO, 19 DGND, 20 DVDD.
* The symbols' `Adapter` fields and the adapter wording in the descriptions and sheet notes are gone; the root sheet
  says "SMD: U3/U4 TSSOP-16, U9 TSSOP-20 (with test pads), U8 SOIC-8, U12 opt.".

![Test pads on U9 (and U3/U4): the TSSOP-20 land pattern to scale with its 20 test pads in two staggered columns
(inner 4.45 mm, outer 5.10 mm from the centre, 1.3 mm between two pads of a column), the 0.2 mm track from each pin
to its pad passing between two pads with 0.2 mm clearance on each side, and the escape stubs that carry an
inner-column pin out between two outer pads to the routing](docs/figures/test-pads.png)

### No GY-PCM5102 module option

J10 (the GY-PCM5102 module, soldered on male pins over the DAC area as the alternative to the on-board DAC since
v1.3.0) is gone from the schematic and the board. Its dashed silkscreen outline and the "DAC -OR- GY-PCM5102"
heading went with it; the heading now just says "DAC". Its nets (BCK, DIN, LRCK, +5V, GND, AUDIO_L/R) stay,
because U9's circuit uses them. U9, C12–C20, C36, FB3, R7 and R8 are always fitted. The BOM's J10 line and its
"DAC AREA: leave out ..." notes are gone.

### DAC decoupling

C36 (U9 CPVDD decoupling) is the board's standard 0.1 µF 50 V X7R radial disc (Murata RDER71H104K0M1H03A, the 0.1 µF
line of the BOM) instead of v1.3.1's 0603. U9 stands at 180° with its analog pins 1–10 facing east, toward the output
filter and the mixer, and its digital pins 11–20 facing west. Each disc sits on the side of the pins it serves:

* **East, beside pins 1–10**, in rows 3.0 mm apart: C15 (AVDD, 10 µF) and C13 (AVDD) at the top, the output
  resistors R8/R7 level with pins 7/6, then C16 (CAPP/CAPM) and C17 (VNEG); the filter discs C20/C19 end the C13 and
  C16 rows on the east.
* **South, under pins 1–3**: C36 (CPVDD/CPGND), with C14 (+3V3) below it.
* **West, beside pins 11–20**: C18 (LDOO) level with pins 17–19 and C12 (DVDD) below pin 20. FB3 (the AVDD bead)
  runs along the top of the block from the +3V3 side in the west to its AVDD pad above C15.

Distances from the chip pin to the nearest pad of the part, straight line:

| Part | Pins | DIP-adapter layout (d5c4a85) | First routed TSSOP layout (298e993) | Now |
|---|---|---|---|---|
| C36 | CPVDD (1) / CPGND (3) | 2.7 / 9.2 mm | 9.7 / 5.6 mm | 3.6 / 5.4 mm |
| C16 | CAPP (2) / CAPM (4) | 3.2 / 3.2 mm | 9.5 / 4.6 mm | 9.7 / 5.5 mm |
| C17 | VNEG (5) | 6.6 mm | 9.6 mm | 8.1 mm |
| C13 | AVDD (8) / AGND (9) | 4.7 / 6.7 mm | 5.1 / 9.6 mm | 5.6 / 9.9 mm |
| C15 | AVDD (8) | 7.3 mm | 12.4 mm | 7.8 mm |
| C18 | LDOO (18) / DGND (19) | 3.5 / 9.1 mm | 4.5 / 9.5 mm | 4.0 / 9.0 mm |
| C12 | DVDD (20) / DGND (19) | 4.5 / 8.4 mm | 2.6 / 6.4 mm | 5.4 / 3.8 mm |
| R7 / R8 | OUTL (6) / OUTR (7) | 3.0 / 2.8 mm | 6.8 / 8.2 mm | 4.7 / 4.6 mm |
| FB3 | AVDD (8) | 2.8 mm | 14.1 mm | 6.3 mm |

* **C36 is the nearest disc to its pin** (3.6 mm from CPVDD), as the PCM510x datasheet asks for the charge-pump
  supply. C16 (the flying capacitor between CAPP and CAPM) stays 9.7 mm from CAPP: it shares the east column with
  the output resistors, which sit level with their pins.
* C16 and C17 were 0.02 mm off the 3.0 mm row grid in the uploaded placement (y 88.60 / 91.60), so C16's courtyard
  touched R7's by 0.015 mm; they are now at y 88.62 / 91.62, in line with C19.
### C8/C10: through-hole discs next to U8

| Ref | Part | DigiKey | LCSC |
|---|---|---|---|
| C8 | 0.1 µF 50 V X7R radial disc, 5.0 mm pitch, Murata RDER71H104K0M1H03A (the BOM's 0.1 µF line) | 490-9144-1-ND | C527620 |
| C10 | 1 µF 50 V X7R radial disc, 5.0 mm pitch, Murata RDER71H105K2M1H03A | 490-9146-1-ND | C1620132 |

These are the v1.3.1 parts again (the routed layout of commit 8dbb6c6 had 0805s on the back under U8). U8 (SOIC-8,
front) stands at 90° at (190.00, 92.47): pins 1–4 along the bottom (y 94.95, pin 1 at the left), pins 5–8 along the
top (y 90.00), pin 8 (+3V3) at (188.09, 90.00), pins 3/4/7 GND. C8 (0.1 µF) stands vertically left of U8, its +3V3
pad 2.50 mm straight left of pin 8 at (185.59, 90.00) and its GND pad at (185.59, 95.00); C10 (1 µF) lies above U8,
+3V3 pad at (187.09, 87.00), GND pad at (192.09, 87.00). With them, every SMD part (U3, U4, U8, U9, U12) is on the
front.

### 102 mm board

The bracket end of the outline is trimmed 1.84 mm (x 220.13 → 218.29). The board is 101.90 × 99.85 mm at the
outline's line centres, 102.00 × 99.95 mm over its 0.1 mm lines (v1.3.1 and the routed v1.3.2 layout: 103.74 ×
99.85 / 103.84 × 99.95 mm). JLCPCB quoted USD 24.50 for 10 black ENIG 2-layer boards at 102.0 mm (and 101.9 mm), USD 33 at
102.7 mm and 33.53 at 103.74 mm (2026-10-28). The ISA fingers, the other three edges and the bracket plate's place in the case do not change.
To keep copper at least 0.35 mm from the new edge, these parts moved left, away from the bracket:

| Part | Moved | Now | Nearest copper to the new edge |
|---|---|---|---|
| H1 / H2 (bracket tab holes) | 1.70 mm | x 214.70 | 0.39 mm (their 6.4 mm pads) |
| J5 (MIDI out) | 1.10 mm | x 217.00 | 0.39 mm (sleeve pad) |
| J8 (line out) | 0.90 mm | x 217.00 | 0.39 mm (sleeve pad) |
| U1 (Pico), with its five test-pad rule areas and its two micro-USB relief slots | 1.35 mm | centre x 192.88, pin 1 (217.01, 100.00) | 0.36 mm (TP2/TP3); the slots (centre x 216.88) end 0.41 mm from the edge |
| U4 / U3 | – | TSSOP centres x 161.44 / 150.22 (re-placed for the soldered TSSOPs, see [Placement](#placement)) | – |

J11 and RV1 stay where they were. The distances are to the outline's line centre. Between these parts and the edge
there is no room for a track or a via: 0.3 mm copper-to-edge, a 0.2 mm track and 0.2 mm clearance need 0.7 mm. What
the trim means for the bracket is under [Bracket](#bracket): the tab holes need a bracket with 1.70 mm longer tabs.

![The bracket end trimmed from 103.74 to 101.90 mm: old (dashed) and new outline with the trimmed 1.84 mm strip, the
bracket plate and tabs from User.Drawings, the tab holes H1/H2 1.70 mm further in (7.03 mm from the plate instead of
5.33), J5, RV1, J8, J11 and the Pico's USB end with their moves and nearest copper to the new edge, and a table of the
x positions](docs/figures/bracket-trim.png)

* **U1 uses the new footprint `PiGUS library:RPi_Pico_SMD_TH_USB_NoSWD`**: `RPi_Pico_SMD_TH_USB` without the
  three SMD castellation pads at the SWD end (pads 41–43: SWCLK and SWDIO, both not connected, and GND; their
  through-holes stay) and with the courtyard 0.2 mm past the module there instead of 0.5 mm. With the old
  courtyard, U4's would overlap the Pico's by 0.25 mm.
* The Pico's USB end (x 218.38) is now flush with the edge: 0.09 mm past the outline's line centre (0.40 mm inside
  it before).
* The Pico's far end is at x 167.38, 2.9 mm under a full-size Waveblaster daughterboard's outline (x 170.32; routed
  v1.3.2 layout 1.6 mm, v1.3.1 1.3 mm). That matters for a socketed Pico (see [Socket build](#socket-build)).

### Placement

The board holds a hand arrangement on the 102 mm outline: the logic column and the Pico from 2026-10-28, the jack
arrangement for the soldered TSSOPs from 2026-10-31, and the DAC block and mixer as rearranged on 2026-11-01 after a
first route, routed afterwards (see [Layout status](#layout-status)).

* **Logic column at the left edge** (as on the earlier v1.3.2 layout). U10, U7, U6 and U5 (DIP-14, 90°, pin 1 at the
  left as on v1.3.1) are stacked in line with U2 at x 120.07, 12.05 mm apart (pin 1 at y 65.62 / 77.67 / 89.71 /
  101.76; U2: 113.81). Each chip's decoupling cap lies horizontally (0°) in the gap above it, pad 1 2.21 mm straight
  above the supply pin (pin 14; U2: pin 20): C21 (U10, +5V), C6 (U7), C9 (U6), C7 (U5) and C4 (U2), pads at x
  120.07 / 125.07, y 55.79 / 67.83 / 79.88 / 91.93 / 103.97. R18, R5, R6 and R17 stand vertically (−90°) in one
  column at x 138.60, at the chips' right-hand ends (pad 1 at y 56.73 / 68.78 / 80.82 / 92.87). The column is
  packed tighter than the courtyards allow: 12 overlaps, accepted (see [Layout status](#layout-status)). The five
  references stand vertically at the board edge (x 117.50), centred on each chip.
* **U2** (DIP-20, 90°) at the lower left, pin 1 (120.07, 113.81). **J1** (the IRQ/DMA jumper block) at pin 1
  (136.03, 124.91), 90°, with its `IRQ`/`DMA` labels and numbers above it.
* **U3/U4** (TSSOP-16 with test pads, 90°): U3 (D0–D3, A0–A3) at (150.22, 110.10) and U4 (D4–D7, A4–A7) at
  (161.44, 110.12), pin 1 at the bottom left. Their decoupling discs C3/C5 lie above them at y 102.29 (C3's +3V3 pad
  at x 147.88, 4.8 mm from U3's pin 16), R2 at (160.97, 99.01).
* **DAC block, above U3/U4 and the Pico's far end**: U9 (TSSOP-20 with test pads) at (156.10, 84.53), 180°: pins
  11–20 (I2S, LDOO, DGND, DVDD) on its west side, pins 1–10 (charge pump, VNEG, outputs, AVDD, AGND) on its east side,
  facing the output filter and the mixer. East of it, in rows 3.0 mm apart (see [DAC decoupling](#dac-decoupling)):
  C15 (163.60, 76.53), C13 (168.60, 79.53), R8 (163.60, 82.60), R7 (163.62, 85.62), C16 (168.60, 88.62) and C17
  (163.60, 91.62), with the filter discs C20 (170.70, 79.53) and C19 (170.70, 88.62) at the east end. C36 (161.00,
  90.50) and C14 (156.00, 93.50) lie under U9's pins 1–3, C18 (149.10, 86.03) and C12 (153.60, 90.53) on its west
  side, and FB3 (the AVDD bead) along the top at (148.60, 77.03). `DAC` stands at (146.11, 92.10).
* **Mixer, above the DAC block**: U11 (DIP-8, 0°) at (173.00, 61.00), its feedback discs C30/C31 standing (90°) at
  its two ends, (170.12, 65.08) and (183.50, 67.00); C28 (+12VA) above it at (180.62, 54.08) and C29 (−12VA) below at
  (175.62, 75.08); R23/R24 (±12V 10 Ω) at (170.42, 57.50) / (170.42, 72.00); C32/C33 (47 µF) standing at x 184.62
  (y 57.12 / 70.08). The mixer resistors stay in two columns: R13, R11, R22, R10, R21 at x 189.43 (y 54.50 / 58.00 /
  65.00 / 72.50 / 79.50) and R20, R12, R19 at x 199.58 (y 61.50 / 69.00 / 76.00). `MIXER` at (171.12, 55.04).
* **C22/C23** (the 10 µF bipolar Waveblaster coupling caps) stand at the top next to J9, (174.00, 40.00) and
  (181.50, 40.00), −90°, with their references above them at y 38.00. C22's can is 0.48 mm outside a full-size
  daughterboard's outline (x 170.32) at nominal size.
* **Bracket end, top to bottom: MIDI OUT, WT VOL, LINE OUT.** J5 (MIDI out) at (217.00, 49.00) with its parts
  R3 (176.64, 46.50), R4 (186.80, 50.50), FB1 (189.50, 46.50) and FB2 (189.50, 50.50) along the top; RV1 (WT VOL)
  at (214.00, 61.40); J8 (line out) at (217.00, 75.50). The labels
  `MIDI OUT`, `WT VOL` and `LINE OUT` are left-aligned at x 204.0 (y 43.5 / 65.0 / 70.0).
* **USB host corner**: U8 (SOIC-8) at (190.00, 92.47), 90°, with C8/C10 next to it (see
  [C8/C10](#c8c10-through-hole-discs-next-to-u8)); U12 at (197.14, 91.45), F1 vertical at (204.00, 90.00), C35 at
  (200.00, 95.00), C34 at (202.30, 83.50).
* **Waveblaster side**: R16 at (150.00, 46.00), 0°; C24, C25 and C26 in a row at y 45.50 (x 146.90 / 143.00 /
  139.00).
* **Pico (U1)** at (192.88, 108.89), pin 1 (217.01, 100.00), and the bracket end (H1/H2, J11) as on the earlier
  v1.3.2 layout (see [102 mm board](#102-mm-board)).

**Why this arrangement.** Four placements of the DAC, mixer and jacks were compared before the first route, each
with the DAC discs on their pins and the logic-column labels as above: A (the first hand placement, DAC block high in
the middle), A2 (A with a DAC block turned 90°), C (that block in the middle with the mixer beside it) and D. In A2
and C, U9's I2S pins face away from the Pico, and a trial route of the three I2S nets alone (with the DAC outputs kept
clear) needed 232 and 238 mm; in A 171 mm; in D 122 mm. D had the shortest DAC-to-mixer path and the shortest
ratsnest overall and was routed first (commit 298e993). That route left the mixer's summing nodes 51 / 53 mm long
and the DAC outputs 19 / 20 mm, because the output filter sat above U9 and the mixer inputs far from U11. The
present arrangement keeps D's jacks and moves the DAC block 12.9 mm west with the output filter east of U9, between
it and the mixer, and turns U11 so that its feedback discs sit on its pins. Against D, in ratsnest length: summing
nodes 95.7 → 61.9 mm, DAC outputs 18.5 → 9.5 mm, mixer outputs 64.3 → 41.5 mm, AVDD and LDOO 26.5 → 15.6 mm; I2S
113.6 → 123.5 mm and the Waveblaster path behind C22/C23 56.0 → 75.4 mm are longer; all nets but GND 4229 → 4169 mm.

![The comparison before the first route (D became commit 298e993; the present arrangement grew out of it). Placements A, A2, C and D side by side: board outlines with the DAC area and the mixer area shaded, the I2S
ratsnest, the audio-path ratsnest and the digital ratsnest that crosses the DAC area, and a table of the trial I2S
route, I2S ratsnest, distances from the DAC area to the Pico and U8, digital ratsnest through the DAC and mixer areas,
DAC-to-mixer and Waveblaster paths, mixer summing nodes, line out, ±12V, SPI and total ratsnest](docs/figures/placement-options.png)

| Part | Routed DIP-adapter layout (d5c4a85) | Now |
|---|---|---|
| U3 / U4 | DIP-16 adapter pin 1 (146.53, 106.42) / (157.74, 106.44), 0° | TSSOP-16 centre (150.22, 110.10) / (161.44, 110.12), 90° |
| U9 | DIP-20 adapter pin 1 (146.00, 78.30), 90° | TSSOP-20 centre (156.10, 84.53), 180° |
| U11 | (173.02, 51.10), 90° | (173.00, 61.00), 0° |
| J5 (MIDI) / J8 (line out) | (217.00, 75.50) / (217.00, 49.10) | (217.00, 49.00) / (217.00, 75.50) |
| U8 | (189.00, 93.90), 0° | (190.00, 92.47), 90° |
| C8 / C10 | (191.50, 88.40) / (184.00, 88.40), 180° | (185.59, 90.00), −90° / (187.09, 87.00), 0° |
| J1 | (120.00, 123.70) | (136.03, 124.91) |
| FB3 | (164.10, 93.73), 90° | (148.60, 77.03), 0° |
| C3 / C5 | (153.50, 103.28) / (164.69, 103.34), 180° | (147.88, 102.29) / (158.88, 102.29), 0° |

Positions are footprint origins (pin 1 for the DIPs, the centre for the TSSOPs), in mm. 51 of the 88 footprints moved
against the DIP-adapter layout (29 against the first routed TSSOP layout, 298e993); the logic column, U1, U2, the Waveblaster header and the bracket-end parts other
than J5/J8 did not.

### Silkscreen and back artwork

* Front `v1.3.2`, back `PicoGUS THT v1.3.2` (THT added on 2026-11-07, so the back names the board).
* **Label font: Jost, embedded in the board file.** The 21 board labels (`WAVEBLASTER`, `MIXER`, `DAC`, `v1.3.2`,
  `LINE OUT`, `WT VOL`, `MIDI OUT`, `UART`, `USB PWR`, `IRQ`/`DMA` and their numbers, `< REAR`) use Jost Medium and
  Bold ([indestructible-type/Jost](https://github.com/indestructible-type/Jost), SIL Open Font License 1.1; the
  copyright and licence are in the font files' metadata), embedded with the board's embedded-fonts option. KiCad
  therefore shows and plots them the same on every machine, with or without the font installed. They were in
  Futura, inherited from v1.2, which was not embedded: each machine plotted its own substitute (DejaVu Sans or Inter
  in the cloud sessions that made the earlier gerbers). Futura cannot be embedded: Futura PT is marked
  "Preview & Print" only, so KiCad refuses it, and its licence forbids sharing the files. Jost is a free
  Futura-style face and looks almost the same at this size. Embedding adds about 120 KB to the board file. The
  v1.3.1 and v1.3.0 boards still use un-embedded Futura.
* **No princess artwork on the board.** It is removed from the back silkscreen and is to be UV printed on the back
  instead (source: `hw-common/art/princess.svg`; the routed v1.3.2 layout had it at 52.8 × 27.0 mm, v1.3.1 at
  71.6 × 37.0 mm). The back text block (version, copyright, licence, URL, thanks, dedication) and `< REAR` stay.
* **Front copper logo moved.** The exposed-copper PicoGUS logo (F.Mask openings over the front GND pour, with a rule
  area that keeps tracks and vias out) is turned horizontal and sits along the top edge above `LINE OUT`, x
  185.9–211.6, y 37.8–40.6, with `v1.3.2` right of it at (214.1, 40.0). Before, the logo stood vertically at the left
  edge (x 118.3–121.1, y 43.5–69.3) and `v1.3.2` at (120.5, 40.5). The pour under it is filled, so the logo shows
  copper.
* Labels placed by hand (2026-10-31; `MIXER`, `DAC` and C22/C23 on 2026-11-01). `MIXER` (171.12, 55.04) and `DAC`
  (146.11, 92.10) are centred, 1.2 mm high
  like the bracket-side labels (`DAC` was vertical at (186.3, 74.5) on v1.3.1). `WAVEBLASTER` is 1.2 mm (was 0.8),
  centred at (143.65, 37.5). `MIDI OUT`, `WT VOL` and `LINE OUT` are left-aligned at x 204.0 (y 43.5 / 65.0 / 70.0),
  `UART` and `USB PWR` at x 208.2 (y 121.5 / 126.0), so `UART` no longer touches J6's outline. The references of the
  logic column (U10, U7, U6, U5, U2) stand vertically at the board edge (x 117.50), centred on each chip; the
  references of the discs sit in the middle of each disc. The references of C22, C23, C32, C33, C34, U8, U12 and J11
  were placed by hand (C22's and C23's above their cans at y 38.00); U8's stands at (185.00, 88.00), nearer C10 than U8, and U12's is 0.08 mm from neighbouring silk.
  `< REAR` moved 1.70 mm left with H1/H2, its arrow tip still in line with them.
* **No via hole in any label**: the router counted every front and back silk text as a no-via area, so no via hole
  is within 0.10 mm of any text (the earlier routed layout had 13).
* v1.3.1's U12 pin-1 dot, left behind at (201.5, 89.7) next to F1 when U12 moved, is removed.
* Silkscreen DRC (with silk_over_copper/silk_overlap enabled): 45 silk_over_copper, all from the outlines of the
  decoupling discs in the logic column crossing the neighbouring DIP pads (`jlcpcb.sh` plots the silkscreen with the
  mask openings subtracted, so none of it prints on a pad); 2 silk_overlap (the IRQ/DMA divider line over J1 twice,
  as on v1.3.1); 19 silk_edge_clearance (J8, J5, RV1, J11 and the Pico's outline at
  the bracket end).

### Other

* Title blocks say v1.3.2 (root sheet and board 2026-10-28, the other sheets 2026-10-23). The schematic notes about
  the GY module and the SMD parts are updated, and `PicoGUS-THT-schematic.pdf` is plotted from the current schematic.
* No reference designator changed and none is new; J10 is the only one removed. The design rules and net classes
  are v1.3.1's (the `.kicad_pro` is unchanged).
* The schematic fields of C8/C10 name the Murata discs again (as on v1.3.1), and U1's footprint field is
  `PiGUS library:RPi_Pico_SMD_TH_USB_NoSWD`.
* 32 footprints (the wavetable sheet set, FB3, RV1, U11) still carried symbol links (KIID paths) from before the
  v1.3.0 schematic rebuild; DRC parity had only matched them by reference. They are re-linked to their symbols, so
  "Update PCB from schematic" keeps them.
* The U3/U4/U9 symbols point at the new TSSOP footprints with test pads; their `Adapter` fields are removed, and the
  sheet notes say "soldered to the card" (C36's note: "close to U9 pins 1 (CPVDD) and 3 (CPGND)").

## Layout status

The placement of 2026-11-01 is routed on two layers (2026-11-01). It replaces a first routed TSSOP layout (commit
298e993) whose route was reviewed independently from the analog/power and the digital/manufacturing side; the rules
that review led to are applied to this route from the start (see [Review](#review)). The comparison is with that
layout and with the routed DIP-adapter layout of commit d5c4a85 (same netlist apart from the U3/U4/U9 footprints),
measured with the same scripts; d5c4a85's README (`git show d5c4a85:hw-tht-v1.3.2/README.md`) compares it with
8dbb6c6 and v1.3.1.

### Routing

* **Test-pad fan-out first**: the 52 pin-to-test-pad tracks and 26 escape stubs (see
  [U3/U4 and U9 with a test pad per pin](#u3u4-and-u9-with-a-test-pad-per-pin)) are fixed copper; the router lands
  on the outer test pads and the stub ends. The stubs of U9's CPGND (3) and AGND (9) ended up unused and were
  removed (24 remain); both pins reach ground through the front pour around U9.
* **U9's east side by hand.** On U9's analog side the pins come out OUTR, OUTL, VNEG, CAPM, CPGND, CAPP from the
  top, while their parts stand in a column 2.4 mm east of the test pads (R8, R7, C16, C17) with room for one track
  between the two. On one layer DAC_OUTL, VNEG, CAPM and CAPP have to cross, and the grid router could not place a
  via that close to the test pads. These four nets are routed by hand: DAC_OUTL on the front to R7; VNEG through a
  via just past its stub and on the back under the test pads to C17; CAPM on the front between R7 and a CAPP via to
  C16; CAPP through that via and on the back to C16's far pad. All at least 0.207 mm from other copper.
* **Grid router** (negotiated-congestion rip-up and re-route, as for the earlier layouts) for everything else, then
  polished, a GND pass (pours on both layers, stitching vias) and clean-up steps, each kept only if DRC stayed clean:
  redundant vias, GND finger tracks, stitching along the copper logo, thermal stubs, power widened up to 0.8 mm where
  it fits, vias moved off other-net pads, acute pad entries squared, power entries necked beside the gold fingers.
* **DAC block kept clear.** Digital, I2S and USB nets pay a cost within 1 mm of the audio copper, the DAC outputs and
  the DAC's analog nets (AVDD, VNEG, CAPP/CAPM, LDOO), and may not enter the block's analog half (east of U9, up to
  the output-filter discs) or the area under U9. −12V, +12V, ~RI/OCHRDY and +5V may not enter the DAC block at all,
  +5V also not the mixer. No track but GND runs on the front under U9, so the front pour under the chip is one piece;
  4 GND vias (3 under the body) tie it to the back pour.
* **I2S may change layers** (via cost lowered): DIN and BCK leave U9 in the opposite order to the Pico's pins, so one
  of them has to cross the other.
* **Labels are no-via areas**: no via hole is within 0.10 mm of any silk text, front or back.
* Design rules unchanged (the `.kicad_pro` is v1.3.1's): 0.2 mm clearance, 0.3 mm copper to edge, 0.25 mm default
  track and 0.7/0.35 mm vias, `pwr` 0.4 mm and `GND` 0.5 mm with 0.8/0.4 mm vias, `audio` with 0.6 mm clearance.
  The 0.2 mm test-pad tracks and stubs, the centred 0.2 mm necks of +5V, +3V3 and ±12V between Pico and U2 pins and
  short jogs at pads are narrower than their class.
* 5494 mm of track and 422 vias: 205 GND, 26 power, 191 signal (756 plated holes with the THT pads).
* DRC with schematic parity: 0 unconnected items, and as errors only the 12 accepted courtyard overlaps below, so
  `jlcpcb.sh` runs with `ALLOW_DRC=courtyards_overlap`. Warnings: 3 `lib_footprint_mismatch` (J3, J11, RV1; expected, see
  [Kept from v1.3.1](#kept-from-v131)), 19 `silk_edge_clearance`, 8 `starved_thermal` and 83 schematic-parity field
  warnings (footprints without the symbols' Manufacturer/MPN fields, J3's BOM flag). The starved thermals are J9's
  GND pins 7, 11, 15 and 25, U7 pin 10 and U2 pin 10 (GND) and the Pico's GND pin 33 on both its pads: each has one
  spoke on the side where signal tracks pass close, and joins the pours with two or three spokes on the other side or
  through 0.5 mm GND tracks.

  ![The 8 starved thermal reliefs: J9 pins 7, 11, 15 and 25, U7 pin 10, U2 pin 10 and Pico pin 33
  (also on its castellation pad), each with one spoke where signal tracks pass close, and how each joins the GND
  pours on the other side](docs/figures/starved-thermals.png)

* **Courtyard overlaps (accepted), all in the logic column**: each decoupling disc's courtyard overlaps those of the
  DIPs above and below it by 0.33–0.34 mm (C21/U10, C6/U10, C6/U7, C9/U7, C9/U6, C7/U6, C7/U5, C4/U5, C4/U2), and
  the vertical resistors' overlap end to end by 0.20 mm (R18/R5, R5/R6, R6/R17). The disc bodies (F.Fab) stay about
  1.6 mm from the DIP bodies and 0.16 mm (seen from above) from the DIP pads; the discs' pads are 0.61 mm from the
  DIP pads. The resistors' pads are 0.29 mm apart end to end (GND/RESET, GND/+5V, DACK/+3V3): solder those with
  care.

  ![The 12 courtyard overlaps: 3D render, courtyard map with each overlap's depth, and close-ups of a disc between two
  DIPs and of two resistors end to end](docs/figures/courtyard-overlaps.png)

### Review

The first routed TSSOP layout (298e993) was reviewed independently; its findings shaped this route's rules:

* **U9's front ground**: AVDD, I2S, DAC_OUTL and ~RDACK ran under the chip there. Here nothing but GND runs on the
  front under U9, and 4 GND vias (3 under the body) tie that pour to the back. A 0.8 mm +3V3 track (DVDD pin 20 to
  CPVDD pin 1 and C36) runs along the chip's south edge, 0.12 mm outside its body.
* **Supplies through the analog area**: +5V is kept out of the DAC block and the mixer, −12V and +12V out of the DAC
  block (0 mm² of +5V and +12V there; −12V touches the block's edge with 0.017 mm²).
* **Digital next to the DAC's analog nets**: none within 1 mm of AVDD, VNEG, CAPP/CAPM or LDOO; the nearest is LRCK
  1.33 mm from LDOO on its way to U9's west pins. The nearest digital copper to U9's pins 1–10 is an RA9 via 3.6 mm
  away. No digital track or via is within 1 mm of any audio net.
* **Gold fingers**: no power copper within 0.30 mm of an other-net finger (the nearest power entries are 0.40 mm or
  more away), and no copper sticks out of a finger into the opening between the fingers.

  ![Solder side above J3's B-row fingers on 8dbb6c6 and on v1.3.2: an overview of B1-B31 and the no-fill strip,
  then close-ups of B5-B11 (±12V) and B28-B31 (+5V). Each finger is its own mask opening and only mask lies between
  a finger top (y 128.97) and the opening between the fingers (y 129.60)](docs/figures/gold-fingers.png)

  ![The GND tracks from the ground fingers B1, B10 and B31 on v1.3.2: B10's track 1.0 mm, then 0.5 mm
  for 2.6 mm past C1's +3V3 pad (0.22 mm) to the back pour, 6.6 mm in all; B1 and B31 1.0 mm straight into the pour;
  B10 on 8dbb6c6 for comparison](docs/figures/b10-ground-finger.png)

* **Mixer summing nodes and DAC outputs**: long on 298e993 (51 / 53 mm and 19 / 20 mm), set by that placement. The
  present placement shortens them (see [Placement](#placement)); routed they are 44.4 / 28.6 mm and 5.8 / 4.9 mm.

### Compared with the earlier routed layouts

| | DIP adapters (d5c4a85) | first TSSOP layout (298e993) | v1.3.2 now |
|---|---|---|---|
| DRC errors / unconnected (schematic parity) | 12 / 0 | 12 / 0 | 12 / 0 (the accepted courtyard overlaps) |
| starved_thermal warnings | 6 | 7 | 8 |
| Track / vias (GND / power / signal) | 5504 mm / 445 (194 / 32 / 219) | 5587 mm / 450 (214 / 29 / 207) | 5494 mm / 422 (205 / 26 / 191) |
| ISA D0–D7 length / vias | 283 mm / 21 | 407 mm / 26 | 350 mm / 18 |
| I2S length / vias | 261 mm / 6 | 187 mm / 16 | 186 mm / 5 |
| USB D+ / D− length | 51.3 / 43.8 mm | 48.1 / 45.6 mm | 46.3 / 46.2 mm |
| +12V / −12V length (vias) | 125 (0) / 159 (5) mm | 146 (6) / 139 (4) mm | 153 (5) / 142 (4) mm |
| DAC_OUTL / DAC_OUTR (U9 to R7/R8) | 3.3 / 3.0 mm | 18.9 / 19.9 mm | 5.8 / 4.9 mm |
| AUDIO_L / AUDIO_R (filter to mixer) | 44.0 / 59.2 mm | 11.4 / 43.7 mm | 24.8 / 34.2 mm |
| Mixer summing nodes U11.2 / U11.6 | 14.6 / 36.1 mm | 51.3 / 52.5 mm | 44.4 / 28.6 mm |
| Digital tracks and vias within 1 mm of audio copper | none | none | none |
| Other-net track within 1 mm of audio copper, same layer | 11.4 mm | 21.5 mm | 22.3 mm (17 mm of it U9's own nets at its pins) |
| Supply copper in the DAC block: +5V / −12V / +12V | – | 0 / 0 / 2.9 mm² | 0 / 0.02 / 0 mm² |
| Line-out ground error per ampere of loop current from J8's sleeve to the fingers, worst channel: no Waveblaster / Waveblaster at full volume | 285 / 466 µV/A | 1019 / 292 µV/A | 868 / 334 µV/A |
| same, loop current to the bracket (H1/H2) | 313 / 223 µV/A | 466 / 253 µV/A | 437 / 280 µV/A |
| Line-out ground error at 500 mA of USB-host load, worst channel: no Waveblaster / at full volume | 128 / 133 µV | 269 / 258 µV | 211 / 26 µV |
| Power copper within 0.30 mm of an other-net gold finger | none | none | none |
| Vias < 0.5 / < 1.0 mm from a gold finger | 0 / 0 | 0 / 1 | 0 / 1 |
| Vias < 0.30 / < 0.25 mm from an other-net pad | 0 / 0 | 3 / 1 | 2 / 1 |
| Via holes < 0.10 mm from silk text (inside the ink) | 7 (4) | 0 (0) | 0 (0) |

All columns come from the same scripts. The line-out rows come from the DC copper model described in d5c4a85's README
(refilled pours, 0.1 mm grid, the Pico's GND pins tied; `e = (Vref − Vsleeve) + 0.977·(Vref − V(U9.9)) + 2·(Vref − Vwt)`).
The figures compare v1.3.2 with 8dbb6c6, the 103.7 mm layout that README compared against.

**Better** than 298e993: the DAC outputs, the summing nodes and AUDIO_R, I2S vias, the ISA data bus, fewer vias
overall, +12V kept out of the DAC block, the line-out ground error without a Waveblaster (868 against 1019 µV/A for a loop to the
fingers) and from USB load (211 against 269 µV). **Worse**: AUDIO_L (the filter sits east of U9, the mixer input to
the north-east), +12V length, the line-out ground error with a Waveblaster at full volume (334 against 292 µV/A to the
fingers, 280 against 253 to the bracket). Against d5c4a85 the ISA data bus is still longer, the DAC outputs and summing
nodes slightly longer, as the placement sets them, and so is the line-out ground error from a loop through the
line-out cable (868 against 285 µV/A without a Waveblaster).

### Known weaker points (accepted)

* **Hand-routed corner east of U9**: the VNEG via is 0.216 mm from R7's DAC_OUTL pad (DRC minimum 0.20 mm), and
  CAPM passes R7's pad at 0.207 mm. Inspect that corner after soldering R7.
* **Audio beside other copper** (22.3 mm within 1 mm, same layer): U9's own AVDD, CAPM, VNEG and CAPP beside
  DAC_OUTL/R where they leave the neighbouring pins (17 mm, from 0.10 mm at the test pads), −12V along R24 (its own
  feed resistor into the mixer; 2.7 mm at 0.68 mm) and at the top left (2.3 mm, 0.62 mm, as on d5c4a85).
* **ISA data bus**: D0–D7 are 350 mm with 18 vias (d5c4a85 283 mm / 21; D2 69 mm with 6 vias), as U3/U4 moved to
  make room for their test pads. Irrelevant at ISA speed.
* **Digital in the DAC block's digital half**: besides I2S, ~RIOW, ~RIOR, I/OCHRDY and RA9 cross the block west and
  south of U9 (the side of its digital pins and of C12/C14/C18), at least 3.6 mm from pins 1–10.
* **Vias near other-net pads**: besides the VNEG via above, an RA9 via is 0.266 mm from the Pico's pin 18 (GND).
* **U9's ground pins** (CPGND, AGND, DEMP, FLT, SCK, FMT, DGND) reach a GND via 0.8–4.5 mm away (CPGND the farthest),
  through the front pour that surrounds the chip.
* **Line-out ground loop**: per ampere of loop current from J8's sleeve to the fingers, the ground error at the
  jack is 868 µV (left channel, no Waveblaster, RV1 turned down; d5c4a85 285 µV, 298e993 1019 µV), and 437 µV to
  the bracket. With 10 mA of loop current that is 8.7 µV through the fingers and 4.4 µV through the bracket: below
  −100 dBV, under the DAC's noise floor (about 21 µV) and 100–300× less than the drop along the cable shield itself.
  The bracket path only exists with a metal bracket screwed to the chassis; a 3D-printed bracket carries no loop
  current. 500 mA of USB-host load adds 211 µV at the worst channel (d5c4a85 128 µV).

  ![Line-out ground loop: ground-potential maps of the board for 1 A from J8's sleeve to the bracket tab holes H1/H2
  and to the ISA GND fingers, with U11.3/U11.5 and U9.9 marked; the worst-channel line-out ground error against
  8dbb6c6 (fingers 868 / 334 against 410 / 715 µV/A, bracket 437 / 280 against 185 / 166 µV/A, 500 mA USB load
  211 / 26 against 226 / 314 µV); and at 10 mA of loop current, 4.4-8.7 µV next to the DAC's 21 µV noise floor and
  1-3 mV across a cable shield](docs/figures/groundloop.png)

* **B10's GND track**: 1.0 mm for 4.0 mm, then 0.5 mm for 2.6 mm (past C1's +3V3 pad at 0.22 mm) before it reaches
  the back pour, 6.6 mm in all; B1 and B31, the other two ground fingers, run 1.0 mm straight into the pour.
* **Silk**: no via hole touches any text. U8's reference stands nearer C10 than U8 and U12's is 0.08 mm from
  neighbouring silk (both placed by hand, left as placed).

  ![Via holes near silkscreen text: none is within 0.10 mm of any text on either side; the six nearest
  are 0.15 to 0.17 mm away (U10, `< REAR`, U11, U9's pin-number text, `DMA`, F1)](docs/figures/silk-vias.png)

* **I2S around the DAC**, for reference:

  ![BCK, DIN and LRCK from the Pico to the DAC U9 on v1.3.2 and on 8dbb6c6, both copper layers, with
  U9's analog pins 1-10, the CAPP/CAPM/VNEG/AVDD copper and the audio-class copper inside a 1 mm keep-away. v1.3.2:
  185.7 mm and 5 vias to U9's west pins, 5.04 mm from pins 1-10, no digital copper inside the keep-away; close-ups
  of U9 and of the hand-routed corner east of it. 8dbb6c6: 235.5 mm and 1 via; LRCK passes U9 pin 1 at 0.33 mm](docs/figures/i2s-around-dac.png)

### Before ordering and building

* **Bracket**: get or print a bracket whose tabs are 1.70 mm longer than a Keystone 9202's (see [Bracket](#bracket)).
* In JLCPCB's placement preview, check pin 1 of U3, U4, U8, U9 and U12.
* **Solder the TSSOPs first** and check them on their test pads (see [Ordering](#ordering)).
* **Solder J11 before the Pico** and check the gap between its shield pad and Pico pin 4 (SPI_SCK, 0.80 mm) for a
  bridge (see [Kept from v1.3.1](#kept-from-v131)).

## Socket build

`docs/BOM-THT-sockets.csv` (with its DigiKey and LCSC upload files) lists everything needed to build the
card with every DIP IC and the Pico socketed. The same parts are the optional socket lines of `docs/BOM-THT.csv`. U3, U4
and U9 are soldered TSSOPs and U8/U12 SMD parts: they are never socketed.

| Socket for | Part (DigiKey) | Height above the board | Top of the part |
|---|---|---|---|
| U2 (DIP-20) | On Shore SA203000 (ED3020-ND) | 4.6 mm | ≤ 9.8 mm, **under the daughterboard** |
| U5, U6, U7, U10 (DIP-14 ×4) | Adam Tech ICM-314-1-GT-HT (2057-ICM-314-1-GT-HT-ND) | 4.6 mm | ≤ 9.8 mm, **under the daughterboard** |
| U11 (DIP-8) | On Shore SA083000 (ED3038-ND) | 4.6 mm | ≤ 9.8 mm, outside |
| U1 Pico | Adafruit 5585 short sockets (1528-5585-ND) + 5584 short plug headers (1528-5584-ND) | 5.0 mm | Pico board at ~7.5 mm |

* All are machined (round-pin) sockets with 0.5 mm tails that fit the 0.8 mm DIP holes. LCSC equivalents
  (CONNFLY DS1001-01) are in the CSV.
* **Daughterboard clearance**: a Waveblaster daughterboard sits ~11 mm above the card. U2, U5, U6, U7 and U10 are
  under it, and a socketed DIP reaches ~9.8 mm (about 1.2 mm worst-case margin, 2 mm typical). That is fine for a
  daughterboard with a flat underside; if yours has parts underneath, solder those ICs directly, or use the
  low-profile Mill-Max 115-43-3xx-41-003000 sockets (2.41 mm; they need 1.0 mm holes, so the DIP footprints would
  need a change).
* **A socketed Pico has no USB host port** (J11) unless three short wires connect its TP1–TP3 to the holes under
  it. The Adafruit short sockets keep the Pico at ~7.5 mm; the standard 8.5 mm Sullins sockets at ~11–12 mm. The
  Pico's far end (x 167.38) is 2.9 mm under a full-size daughterboard's outline (x 170.32), so with a daughterboard
  fitted use the short sockets.

## Kept from v1.3.1

* **RV1** stays in the middle of the bracket end (at the top, its M7 bushing would be 0.4 mm above the PC's slot
  opening); v1.3.2 puts J5 (MIDI out) above it and J8 (line out) below it. **J11 at x 208.64** (at x 210 its flange
  stood 1.26 mm into the bracket plate). **C32/C33** (47 µF, 11 mm tall) stay out from under the daughterboard, now at
  x 184.62, and so do C22/C23 (10 µF, 11 mm) at the top next to J9 (C22's can 0.48 mm outside its outline). C1's can
  is about 0.4 mm from a full-size daughterboard's maximum outline at nominal size, 0.16 mm at maximum can size
  (v1.3.1: 0.68 / 0.43 mm; C1 moved 0.27 mm with the Pico row).
* No copper inside the gold-finger area: a no-fill rule area covers the strip above the fingers, and **J3's mask
  opening between the fingers starts 0.63 mm lower (y 129.6) than the finger tops**. Each finger also has its own
  mask opening, the size of the finger (from y 128.97, no mask expansion), so a track beside a finger top is
  covered only by the mask between them. Keep power copper at least 0.30 mm from the finger copper (this layout:
  0.42 mm or more, −12V next to B6 (DRQ2); signal tracks pass the finger tops at 0.24 mm). The ISA GND fingers B1,
  B10 and B31 each have a back-side track into the pour: B1's and B31's are 1.0 mm straight into it, B10's is 1.0 mm
  for 4.0 mm and then 0.5 mm for 2.6 mm (see [Known weaker points](#known-weaker-points-accepted)).
* **Don't run "Update footprint from library"** on J3 (the mask edit), J11 (the shield padstack below) or RV1 (the
  library footprint has a stray Edge.Cuts line that breaks the outline). DRC lists these three as
  `lib_footprint_mismatch` warnings; that is expected. J5, J8, RV1 and F1 also carry 3D models from `hw-common/parts`
  in place of stock model paths that KiCad 10's library does not have: updating them from the library puts the
  missing paths back. The two Pico micro-USB shell relief slots (1.3 × 2.0 mm) are
  drawn on Edge.Cuts as locked board items (JLCPCB wants non-plated slots on the outline layer), and five rule
  areas keep vias and the front pour away from the Pico's test pads. On v1.3.2 they moved with U1 (1.62 mm left in
  all: 0.27 mm, then 1.35 mm for the 102 mm trim); **move them with U1 again if U1 ever moves**.
* J11's shield pad next to the Pico's top pin row has a 2.6 mm pad on the front (3.0 mm on the back). With the Pico
  1.35 mm further in, it faces Pico pin 3 (GND): 0.48 mm on the front (2.08 mm to the pin's THT pad on the back), so
  a bridge there is harmless. Pin 4 (SPI_SCK), which faced it at 0.48 mm on v1.3.1 and the routed layout, is now
  0.80 mm away. **Solder J11 before the Pico and check the gap to pin 4 for a bridge.** H2's mask opening is 0.3 mm
  smaller (it sits 0.38 mm from Pico pin 39).
* Rule areas keep copper tracks out of the front exposed-copper logo, and vias and the front GND pour out from under
  the flat Pico's test pads (TP4–TP6, Pico 2 TP7) and its TP1–TP3 USB area.
* U12's GND (pin 2) has a GND via 1.30 mm from the pin (the DIP-adapter layout: 1.26 mm); keep one there.
* A `JLCJLCJLCJLC` order-number placeholder sits on B.Fab next to the Pico (not plotted). No via hole touches its
  box (the nearest, a GND via, is 0.45 mm below it). If an order ever uses "Specify a location", check the box for
  via holes again after any routing change.

## Unchanged from v1.3.1

### USB host port (J11, sheet 4 "USB host port")

A USB-A host port at the bracket for a mouse, joystick, keyboard, flash drive (CD-ROM images) or a hub, as on the
PicoGUS NE2000 card and the PicoMEM 2.

* **Data**: straight from the Pico's own USB D+/D− test points **TP3 / TP2** (behind the Pico's 27 Ω series
  resistors). U1 uses `RPi_Pico_SMD_TH_USB_NoSWD`: the v1.3.0 Pico footprint plus three plated holes under TP1
  (GND), TP2 (D−) and TP3 (D+), without the SMD pads at the SWD end (v1.3.1: `RPi_Pico_SMD_TH_USB`, with them). The
  Pico must be **soldered flat** (castellated edge or through-hole pins, no header) so
  the three pads can be soldered from the back of the card. The positions are the same on Pico, Pico 2, Pico W and
  Pico 2 W. On headers or in a socket, solder three short wires (26–30 AWG, under 3 cm) from TP1–TP3 into the holes,
  or use the micro-USB OTG route of v1.3.0. The Pico's micro-USB stays in parallel with J11: **use one or the other.**
  TP2 = D− and TP3 = D+ were checked against Raspberry Pi's Pico R3 schematic and KiCad's own Pico footprint.
* **J11**: Molex 67643 (0676430910, DigiKey WM4993-ND / LCSC C2798029), stock footprint
  `Connector_USB:USB_A_Molex_67643_Horizontal`. Its front lip (14.4 × 7.16 mm, the frontmost part, 12.99 mm in
  front of the pin row) sits 0.1 mm behind the inside of the bracket plate. It is larger than the 13.6 × 6.2 mm
  cut-out, so it is the panel stop: nothing of J11 enters the cut-out, only the USB plug comes in through it. The
  receptacle face is 0.9 mm behind the outside of the bracket (PicoGUS 2.0: 1.3 mm), see [Bracket](#bracket).
* **VBUS**: ISA +5 V through **F1** (Bourns MF-R050, 0.5 A hold) with C34 (220 µF low-impedance; USB 2.0 asks for
  ≥ 120 µF) and C35 (0.1 µF), always on, like the PicoMEM 2's USB-A port. The Pico's VBUS pin (J7) is not involved.
* **U12** (USBLC6-2SC6, SOT-23-6): optional ESD protection on D+/D−, routed flow-through as on v1.3.1: D+ J11 →
  pin 6 → under the body → pin 1, D− pin 4 → pin 3, on to the Pico, so there are no stubs and the port works without
  U12. On v1.3.2 it sits next to U8, about 10 mm west of J11's pins (F1 between), with C35 (0.1 µF) just below it.
  Pin 1 is marked by the footprint's silk triangle just above it (v1.3.1's hand-placed dot is removed). **If you fit
  U12, solder it before C35**, 0.36 mm below it.
* USB-C was considered and left out: the PicoMEM 2 also uses USB-A for its host port and USB-C only for
  programming (5.1k Rd, never sourcing VBUS). A dual USB-A/USB-C footprint is in the shared library, unused.

### Waveblaster header without standoff holes

J9 uses `PiGUS library:Wavetable_NoMountingHoles`: the v1.3.0 `Wavetable` footprint without the two 3.2 mm
daughterboard standoff holes (the far one landed in U4 in the rev B placement). The daughterboard is held by the
2x13 header only, so there is no M3 standoff and no screws in the BOM. A full-size daughterboard overhangs the
card's far end; support it by hand when plugging/unplugging cables.

### Bracket

The bracket parts are where v1.3.1 had them, not where v1.3.0 (and v1.2) had them. **The tab holes H1/H2 are
1.70 mm further from the plate than on a Keystone 9202**: their centres are 7.03 mm behind the plate's inner face
instead of 5.33 mm (measured on the board's bracket drawing on User.Drawings), because the card is 1.84 mm shorter
at the bracket end (see [102 mm board](#102-mm-board)). A stock Keystone 9202, a PicoGUS 2.0 bracket and the brackets
for v1.2, v1.3.0, v1.3.1 and the routed v1.3.2 layout therefore do not fit: the card needs a bracket with tabs
1.70 mm longer, e.g. a 3D-printed one (the bracket drawing on User.Drawings has them). The plate itself sits where
it always does in the case, so the openings do not change. Hole positions (distances measured down from the
bracket's top surface, height = centre above the card's component side):

| Part | Hole | Down from the bracket top | Height |
|---|---|---|---|
| J5 MIDI out | Ø6.3–6.5 mm | 14.9 mm | 7.0 mm |
| RV1 WT volume (M7 bushing) | Ø7.2–7.5 mm | 23.9 mm | 6.5 mm |
| J8 line out | Ø6.3–6.5 mm | 41.4 mm | 7.0 mm |
| J11 USB-A | 13.6 × 6.2 mm rectangle, corners R ≤ 0.5 | 54.0 mm (centre; the cut-out runs from 47.2 to 60.8 mm) | 1.0–7.2 mm (centre 4.1) |

* All four are inside the PC/AT slot opening (it starts 10.95 mm below the bracket top). A jack is on top because its
  Ø6 nose fits there; RV1's M7 nut needs the room further down. MIDI OUT (J5) is on top and LINE OUT (J8) below
  RV1 (the DIP-adapter layout had them the other way round).
* No hole for the Pico's micro-USB: its plug can't enter the slot opening. Flash the Pico before soldering it,
  or with `pgusinit`.
* The webs between the holes are about 2.1 mm (J5–RV1), 10.6 mm (RV1–J8) and 2.5 mm (J8–J11).
* J11's x position was checked against the PicoGUS 2.0, whose USB-A port is proven in production. Its bracket is a
  Keystone 9202 derivative with 1.0 mm longer tabs (its tab holes sit 0.94 mm closer to the edge connector), so
  both brackets' plates end up within 0.06 mm of each other in a case. The 2.0's USB-A face is 1.31 mm behind the
  outside of its plate; J11's is 0.89 mm behind a Keystone 9202's, so J11 (x 208.64) is right. Moving it out would
  put its front lip into the plate. A PicoGUS 2.0 bracket does not fit this card: its cut-outs are elsewhere, and
  its tab holes are 0.94 mm, not 1.70 mm, further in.
* **J5 and J8 moved in** with the trim (1.10 / 0.90 mm), so their noses now end 0.32 / 0.31 mm behind the plate's outer
  face (on the bracket drawing's 0.78 mm plate, x 221.73–222.51; v1.3.1: 0.79 / 0.58 mm past it). A
  plug whose body is wider than the hole then stops on the plate about 0.3 mm short of the jack. A counterbore of
  about 0.4–0.5 mm around those two holes on the outside of the plate lets it seat fully; on a thicker (3D-printed)
  plate the noses end further behind its outer face, so size the counterbore for that plate.
* J5 and RV1 are 9.0 mm apart (v1.3.0: 9.7 mm between J5 and RV1): with a Ø8 mm plug in J5, use a knob of
  Ø9.5 mm or less on RV1.


# v1.3.0 reference (unchanged in rev B and v1.3.2 unless noted above)

## What changed in v1.3.0 compared to v1.2

### Through-hole parts

| Part | v1.2 | THT board |
|---|---|---|
| Resistors | 0805 | 1/4 W axial, 10.16 mm hole pitch |
| Ceramic caps | 0805 / 1206 | radial MLCC / disc, **5.0 mm** lead pitch |
| Electrolytics | 1206 / SMD | 5 mm radial electrolytics |
| D1 | B5819W SOD-123 | 1N5819 DO-41 |
| U2 | 74LVC244 TSSOP-20 | **74AHC244 DIP-20** |
| U5 | 74AHC14 SOIC-14 | **74AHC14 DIP-14** |
| U6 | 74LVC00 SOIC-14 | **74AHC00 DIP-14** |
| U7 | 74LVC1G00 SOT-23-5 | **one gate of a 74AHC00 DIP-14**, 3 spare gates tied off |
| U10 | 74LVC2G06 SOT-23-6 (open drain, 3V3) | **74AHCT126 DIP-14 at +5 V**, used as a "pseudo open-drain" |
| FB1/FB2 | 0805 beads | axial ferrite beads (bead on leads), 12.7 mm hole pitch |

Only the parts that exist in SMD packages alone are not plain through-hole parts:

* **U3, U4** SN74CB3T3257: FET bus-switch mux with 5 V→3.3 V clamping. It only comes in TSSOP-16/TVSOP. No DIP
  part does this job without redesigning the ISA bus interface (and the PIO timing). **v1.3.2: soldered to the
  card, with a test pad per pin** (v1.3.0/v1.3.1: soldered to the card, no test pads).
* **U9** PCM5100A/PCM5101A/PCM5102A DAC: TSSOP-20. **v1.3.2: soldered to the card, with a test pad per pin.**
* **U8** APS6404L PSRAM: SOIC-8 (1.27 mm pitch), soldered to the card.
* **U12** (rev B, optional) USBLC6-2SC6 ESD array: SOT-23-6, 0.95 mm pitch.

All of them can be hand-soldered. (Rev B also had C36 as an 0603; v1.3.2 makes it a THT disc.)

The 3.3 V logic needs 5 V-tolerant inputs, so it is 74**AHC** in DIP (TI still makes these and DigiKey
stocks them). **LCSC has almost none of these DIPs in stock** (0–74 pcs on 2026-09-28): order the logic
ICs from DigiKey.

### U10: 74AHCT126 instead of 74LVC2G06

The open-drain 74LVC2G06 has no DIP equivalent that takes a 3.3 V input and has a 5 V
open-drain output. HC/LS open-drain parts either need 5 V input levels or float "high"
while the Pico boots, which would hang the ISA bus through IOCHRDY. The AHCT126 runs from +5 V and
accepts the 3.3 V signals (TTL thresholds). It is wired as a pseudo open-drain driver:
input **A is tied to GND**, and the signal that drove the old inverter input now drives **OE**. OE=1 pulls the line low;
OE=0 releases it. The polarity is the same as on v1.2, so **no firmware change** is needed.

* Gate A: IOCHRDY (as v1.2). **R18** (100 k pull-down on `~RI/OCHRDY`) keeps IOCHRDY released
  while +5 V is up but the Pico's 3.3 V rail isn't yet, or while the Pico is in reset.
* Gate B: MIDI-out current loop (as v1.2).
* Gate C: Waveblaster `/RESET`. Gate D: Waveblaster MIDI (see below).
* **Only use 74AHCT126 here** (SN74AHCT126N). A 74**HCT**126 has
  input clamp diodes to VCC: with the Pico powered from USB and the PC off, the Pico's
  outputs would back-power the ISA +5 V rail through it. A 74**HC**126 at 5 V doesn't recognise 3.3 V
  highs. An **AHC**126 would work electrically, but its 5 V thresholds leave no margin.

**R17** (10 k pull-up on `UART_TX`) holds the MIDI and Waveblaster MIDI lines idle while the Pico boots
(the RP2040 pads default to pull-down, which would otherwise send a MIDI "break").

### Waveblaster header (new)

`J9` is the standard 26-pin Waveblaster header (2x13 male, 2.54 mm; the daughterboard has the
female socket). The pinout is as on PicoGUS 2.0: DGND 1,3,5,7,9,11 · AGND 15–25 odd · +5 V 6,10,14 ·
+12 V 18 · −12 V 22 · MIDI to DB 4 · audio R 20 · audio L 24 · /RESET 26 · 2,8,12,13,16 n.c.

* **MIDI** (pin 4): the MPU-401 UART (`UART_TX`, the same data as the MIDI-out jack) through U10D,
  a pseudo open-drain driver with a **10 k pull-up to +3.3 V** (R16). The line swings 0/3.3 V: that is
  a valid TTL high for 5 V daughterboards, and it is safe for modern 3.3 V daughterboards
  (Dreamblaster etc.) that are not 5 V tolerant.
* **/RESET** (pin 26): ISA RESET through U10C, also pseudo open-drain with a 10 k pull-up to +3.3 V (R9).
* **±12 V** come straight from the ISA bus. They are now connected on this board, with 0.1 µF decoupling at J9.
* v1.3.0: the footprint includes the standard support hole at the far end of the daughterboard (M3 × 11 mm
  standoff). **Rev B and v1.3.2 have no standoff holes** (see above).

### Analogue WT volume + op-amp mixer (new)

```
                    v1.2 DAC filter
PCM510x OUTL --- 470R ---+--- 20k (R10) ---+-------+---- 47pF (C30) ----+
                         |                 |       +---- 20k (R19) -----+
                       2.2nF               |       |                    |
                         |                 |       +--|- \              |
                        GND                |          |NE5532 >---------+--- 100R (R21) --> LINE OUT L (J8)
WT L (J9.24) --+-- C22 ---- RV1A ---- R12 -+       +--|+ /
               |                                   |
          100k (R14)                              GND
               |
              GND                    (right channel: R11, R13, R20, C31, R22, RV1B, C23, R15)
```
C22 = 10 µF bipolar · RV1A = 10k log (CW end from C22, CCW end to GND, wiper to R12) · R12 = 10k · R14 = 100k

* The two sources are summed by a **NE5532 inverting summer** (U11, DIP-8, powered from ISA ±12 V).
  **PCM has (almost) unity gain** (R19/(R7+R10) = 20k/20.47k, −0.2 dB), so the PCM line out has the same
  level as on v1.2 (2.1 Vrms full scale). **The WT input has a gain of 2 (+6 dB) at RV1 max** (R19/R12 = 20k/10k):
  Waveblaster daughterboards put out only about 1–1.2 Vrms full scale (a DreamBlaster X2 uses an AKM AK4388
  DAC, 1.13 Vrms), so at unity gain MIDI ended up 5–6 dB quieter than PCM. RV1 trims it down. C30/C31
  (47 pF) put the feedback corner at ~170 kHz (10k ∥ 100 pF was ~160 kHz). The output is phase-inverted, which you can't hear.
* **Changed 2026-10-09** (BOM only, no board change): R10, R11, R19, R20 were 10k and C30/C31 were 100 pF
  (about unity gain for both sources; PCM was −0.4 dB). On a board built that way, swap R10/R11/R19/R20 to
  20k (and C30/C31 to 47 pF; leaving 100 pF costs only ~0.2 dB more at 20 kHz) to get the extra WT gain. Raising the feedback instead
  of lowering R12/R13 keeps the daughterboard's load as before: about 4.8 kΩ at RV1 max (RV1 ∥ R12 ∥ R14).
* **RV1** (dual-gang 10 k audio taper, right angle, 15 mm flat shaft: Bourns PTD902-2015F-A103, or the Alps RK097 the footprint was drawn for) sets the Waveblaster
  level. Its shaft goes through the ISA bracket between the MIDI and line-out jacks. The signal is on the
  CW-end terminals (3/6) and GND is on the CCW end (1/4), so the level rises clockwise. The 10 k summing
  resistor loads the wiper only a little, because a log pot's wiper is near the GND end for most of its travel.
* C22/C23 (10 µF **bipolar** electrolytics, Panasonic SU) block any DC offset from the daughterboard.
  The DC across them depends on the daughterboard (some outputs are AC-coupled, some carry an offset), so a
  polarised cap could end up reverse-biased; a bipolar one doesn't care. R14/R15 (100 k) give the
  capacitors a defined DC level when no daughterboard is fitted. Without a daughterboard, RV1's setting doesn't matter.
* The PCM510x sees a ~20.5 kΩ load (R7 + R10; its datasheet minimum is 1 kΩ). The output is DC-coupled, which is fine because the
  DAC's outputs are ground-centred; expect a few mV of offset at the jack.
* R21/R22 (100 Ω) isolate the op-amp from cable capacitance. The NE5532 drives 600 Ω headphones
  easily, but this is still a line output.
* **Op-amp supply filter.** The ISA ±12 V rails carry PSU ripple and noise from other cards. They reach U11
  through R23/R24 (10 Ω) and C32/C33 (47 µF), an RC low-pass at about 340 Hz, with C28/C29 (0.1 µF) at the
  op-amp pins. The NE5532's supply rejection falls at high frequencies, which is where this filter works.
  The daughterboard still gets the plain ±12 V (decoupled at J9); it has its own regulators and filtering.
* **DAC analogue supply.** On v1.2 the PCM510x AVDD (pin 8) shares the Pico's switch-mode 3.3 V with all the logic.
  Here AVDD has its own rail behind ferrite bead FB3 (the same part as FB1/FB2), with C13 (0.1 µF) and C15 (10 µF) at
  the pin. DVDD and CPVDD stay on the plain 3.3 V, as on v1.2/v1.3.0. Since rev B the charge-pump supply CPVDD (pin 1)
  has its own 0.1 µF, C36, so its current loop pin 1 → C36 → pin 3 (CPGND) is short: 1.9 + 0.6 mm on rev B; on v1.3.2,
  a THT disc right under pins 1–3 (its +3V3 pad 3.6 mm from pin 1 and its GND pad 5.4 mm from pin 3 in a
  straight line; see [DAC decoupling](#dac-decoupling)) (before rev B it was
  about 24 mm of track to C4 and back through the pour). TI's layout guide and PicoGUS 2.0 put this cap right at the
  pin. CPVDD stays on +3V3, as on v1.2/v1.3.0, and TI's 10 µF bulk cap at CPVDD is not fitted locally (the nearest
  bulk is C1). The bead drops only millivolts at the DAC's ~10 mA.
* **Capacitor types in the signal path:** C19/C20 (DAC output filter) and C30/C31 (op-amp feedback) are
  **C0G/NP0**, which has no voltage or temperature coefficient and so adds no distortion. X7R is used only for
  supply decoupling.

v1.3.0 and v1.3.1 also had a footprint for a GY-PCM5102 DAC module (J10) as an alternative to the on-board DAC;
**v1.3.2 has no module option** (see above).

### Back silkscreen

The back carries the princess artwork (hw-common/art/princess.svg: a big and a small princess, both crowned, with
sparkles and a castle, over the PicoGUS logo): on rev B 71.6 × 37.0 mm, its lower edge 3 mm above the Pico's pin
holes, 6 mm off the card's centre toward the bracket. The back is full of through-hole pads, so the artwork is cut
out 0.3 mm around every pad and hole (silkscreen never lands on copper). **v1.3.2 has no artwork on the board**: it
is to be UV printed on the back instead (the routed v1.3.2 layout, commit 8dbb6c6, had it re-placed at 52.8 ×
27.0 mm; see [Silkscreen and back artwork](#silkscreen-and-back-artwork)).

The version and licence block sits between the Pico's pin rows, one text per line, starting with `PicoGUS THT v1.3.2`. Its link line reads
`https://github.com/waagbo/picogus-tht`, the home of this board, as agreed with polpo (v1.3.1 linked
github.com/polpo/picogus); its letters are 0.78 mm wide, like the thanks line, so it clears the via holes. The
block ends with the thanks and the dedication: *Thanks to polpo and all the PicoGUS contributors. This revision is dedicated to my wonderful family, Mila
and Alisa.* On v1.3.1 each line was nudged (at most 0.9 mm) so that no via hole lands in a letter; on the routed
v1.3.2 layout the lines were re-spaced so that no via hole was within 0.10 mm of a letter. This board keeps those
positions, and its via holes are at least 0.14 mm from every letter of the text block and 0.13 mm from `< REAR`.
"< REAR" points at the bracket, its arrow tip in line
with the bracket tab holes H1/H2. All are mirrored on B.SilkS so they read correctly from the back.

### Other changes

* The board is **103.7 × 99.9 mm** (v1.3.2: 101.9 mm long, see [102 mm board](#102-mm-board)). It has the same length, ISA edge connector and bracket tab holes (H1/H2) as
  v1.2 (on v1.3.0 the MIDI and line-out jacks are also in the v1.2 places; rev B and v1.3.2 move them, see
  [Bracket](#bracket); v1.3.2 is also 1.84 mm shorter, with H1/H2 1.70 mm further in, see
  [102 mm board](#102-mm-board)), but it is taller, like PicoGUS 2.0, to fit the Waveblaster header along the top edge.
  1.6 mm FR4 (the v1.2 and v1.3.0 board files' stackup says 0.57 mm; rev B's and v1.3.2's say 1.6 mm, so their 3D
  and STEP exports have the right thickness).
* Design rules are relaxed for THT/DIY: 0.2 mm clearance, 0.25 mm signal / 0.4 mm power tracks
  (+5 V, +3.3 V, VSYS), 0.7/0.35 mm vias (rev B and v1.3.2: the same, plus a 0.6 mm-clearance `audio` class).
  GND is poured on both layers (v1.3.2 too, with stitching vias; see [Layout status](#layout-status)).
* **J2 (the 2x31 ISA debug/probe header of v1.2, never fitted) is removed.** On a through-hole board
  its pads formed a wall right above the gold fingers that every ISA signal had to cross.
* The `+3.3V` power symbols are renamed `+3V3`. KiCad 8+ names power nets after the symbol value, so the
  upgraded v1.2 sheets would otherwise have split the 3.3 V rail into two nets.
* The symbol fields of U3/U4 now name the CB3T3257 that is actually used (the v1.2 fields still said 74FST3257DR2G).
* **Layout is grouped by circuit block.** Each chip and its decoupling caps and resistors form a KiCad group,
  so a block can be selected and moved as one: DAC (U9), Mixer + line out (U11, J8), WT volume (RV1),
  Waveblaster header (J9), MIDI out (J5), Pico + power (U1), ISA bus switches (U3, U4), ISA buffer (U2),
  74AHC14 (U5), 74AHC00 (U6, U7), 74AHCT126 (U10), PSRAM (U8, C8, C10), IRQ/DMA jumpers (J1).
  On v1.3.2, U2, U6 and U7 (with C4, C6, C9) are not grouped.
* **Layout for audio quality:** every decoupling cap sits next to its chip's supply pin. FB3 sits next to C13/C15.
  The ±12 V filter (R23/R24, C32/C33) sits above U11. The DAC is turned so that its outputs, the output RC filter
  (R7/R8, C19/C20) and the mixer op-amp form one short path to the line-out jack, away from the Pico and the ISA
  logic (v1.3.2: U9 at 180° above U3/U4, its pins 1–10 on the east side, facing the output filter and the
  mixer).

## Card dimensions (ISA 8-bit)

The card is **101.9 mm long × 99.85 mm tall** (bottom of the fingers to the top edge, at the outline's line
centres; 102.0 × 99.95 mm over its 0.1 mm lines). v1.3.1 and the routed v1.3.2 layout were 103.7 mm long: v1.3.2 is
trimmed 1.84 mm at the bracket end (see [102 mm board](#102-mm-board)). The IBM PC/XT full-height limit is 4.2 in
(106.7 mm), so the card is 6.8 mm under it; the AT limit is 4.8 in (121.9 mm). The edge connector (62 contacts,
0.1 in pitch) and its position are unchanged from v1.2; the bracket tab holes H1/H2 are 1.70 mm further in than on
v1.2 (see [Bracket](#bracket)). The board is 1.6 mm, the standard ISA card thickness. A Waveblaster daughterboard
sits about 11 mm above the card, inside the 0.8 in (20.3 mm) AT slot pitch. Clip the THT leads short (≤ 2 mm) on the
solder side, where the next card sits. There are no SMD parts on the back.

The **SMD** version of this board is in [`hw-smd/`](../hw-smd/README.md).

## Ordering

`docs/BOM-THT.csv` lists, for every line, the DigiKey part (with DigiKey stock) and the LCSC part (with LCSC stock),
plus alternatives. Stock was re-read on 2026-11-07: LCSC from its product API, DigiKey from digikey.no (a `*` marks
the few DigiKey figures that could not be re-read and are from the earlier checks of 2026-09-28 to 2026-10-23).

**Upload files.** `docs/BOM-THT-digikey.csv` (DigiKey cart upload) and `docs/BOM-THT-lcsc.csv` (LCSC BOM tool) hold
one board; `-optional` adds the sockets (and U12 for LCSC). Each line's Customer Reference is the board and its
positions, e.g. `PicoGUS-THT-1.3.2: C30,C31`, so every bag arrives labelled for sorting. The DigiKey file includes
the optional U12 as `PicoGUS-THT-1.3.2: U12 (optional)`, so it comes with the order (`--digikey-main U12`). The quantities are for one board: set
the number of boards in the shop's BOM tool. LCSC raises lines to its minimum order quantities (50 for some
resistors). After changing `BOM-THT.csv`, regenerate them (from this folder):

```
python3 ../hw-common/tools/bom_uploads.py docs/BOM-THT.csv docs/BOM-THT --prefix 'PicoGUS-THT-1.3.2: ' \
    --label '(jumpers)=J1,J7 shunts' --label '(U1-socket)=U1 sockets' --label '(DIP14-socket)=U5,U6,U7,U10 sockets' \
    --label '(DIP20-socket)=U2 socket' --label '(DIP8-socket)=U11 socket' --digikey-main U12
python3 ../hw-common/tools/bom_uploads.py docs/BOM-THT-sockets.csv docs/BOM-THT-sockets \
    --prefix 'PicoGUS-THT-1.3.2: ' --all-optional
```

* **Logic ICs (U2, U5, U6, U7, U10): order from DigiKey.** LCSC lists the DIPs, but had 0 of U2, U5 and U10 and
  60 of U6/U7 on 2026-11-07. **The Pico (U1)** is not sold in the LCSC shop, so the LCSC file leaves it out.
* **U9 is the PCM5102A** (112 dB). DigiKey's line is the automotive PCM5102AQPWRQ1, in stock when the PCM5102APWR
  was not; LCSC's line is the PCM5102APWR. The PCM5100A (100 dB) is the budget choice: with the firmware's 16-bit
  output the grades sound the same. All three are pin-compatible.
* **The pin headers are gold-plated** (30 µin): Amphenol 77313-118-26LF for J9, 77313-118-18LF for J1 and
  77311-118-02LF for J6/J7, with the same pin lengths as the parts they replace (the tin Sullins PEC13DAAN for J9,
  the Amphenol 10129381-918001BLF for J1, which DigiKey lists with only 1 µin gold, and a tin Sullins 1x40 strip cut
  into 1x2s for J6/J7). LCSC's "gold 2x13 header" C9040 is a shrouded box header: don't use it.
* **C15/C24**: DigiKey now sells Murata's direct replacement RCER71H106MWK1H03B; LCSC still has the
  RCER71H106MWM1H03A. **C22/C23** (Panasonic ECE-A1HN100U): DigiKey had none on 2026-11-07 (200 due 2026-11-16,
  orderable as a backorder); LCSC had stock. **R14/R15/R18, R21/R22**: LCSC's Yageo lines were out of stock, so
  the LCSC column uses UNI-ROYAL MFR0W4F metal film (as R1 and R7/R8 already did).
* **SMD parts on the card**: U3, U4 (TSSOP-16), U9 (TSSOP-20), U8 (SOIC-8) and the optional U12 (SOT-23-6), all on
  the front. Solder them before the through-hole parts, the TSSOPs first; then check each pin against its neighbours
  on the test pads (a bridge shows as a short between two pads) and ring each test pad through to the part it
  connects to.
* **DigiKey BOM cost**: about 563 kr excl. MVA for one board at digikey.no's single-piece prices of 2026-11-07
  (parts only, no PCB, no shipping; price breaks lower it a little). The PCM5102A-Q1 is 60 kr of that: with the
  PCM5100A the board is about 528 kr. The optional machined socket set was ~80 kr on rev B, plus ~21 kr for the
  Pico sockets.
* **Ceramic capacitors must have 5.0 mm (or 5.08 mm) lead pitch.** 2.5 mm parts don't fit (e.g. TDK FG1x, Vishay K…L2).

Mistakes found in the v1.2 BOM/JLCPCB data (don't copy these from the v1.2 files):
C139966 is a Vilsion part, not the APS6404L. C2910528 is a 3.5 mm jack with a different pinout.
C2905423 is a female header, not the Pico. C2935925 is DEALON, not Amphenol. C6942 is the SOIC SN74AHC14DR.

## JLCPCB files

```
CPL=all ALLOW_DRC=courtyards_overlap hw-common/tools/jlcpcb.sh hw-tht-v1.3/PicoGUS-THT.kicad_pcb   # -> hw-tht-v1.3/build/jlcpcb/
```

The script stops if DRC (including the schematic-parity check) finds errors. On this board DRC finds only the 12
accepted courtyard overlaps (see [Layout status](#layout-status)), which are errors in this project's rules.
`ALLOW_DRC=courtyards_overlap` lets exactly that type through and still stops on any other error. `build/jlcpcb/` holds the export of the
routed board made that way (2026-11-07, with the waagbo/picogus-tht link on the back; not in git: the repository ignores `build/` and `*.zip`, so a clone has to
run the script): outline to x 218.29 (102.0 mm), 422 vias, 756 plated holes. It writes:

* `PicoGUS-THT-gerbers.zip`: upload this to jlcpcb.com. It holds the Gerbers (Protel extensions, no X2)
  and the Excellon drill files (PTH and NPTH separate, mm).
* `PicoGUS-THT-bom-jlc.csv` and `PicoGUS-THT-cpl-jlc.csv`: the BOM and placement files for JLCPCB assembly, with
  **every part** (39 BOM lines, 85 placements), so JLCPCB can solder the through-hole parts too; for an SMD-only
  order, deselect the through-hole lines in JLCPCB's BOM step. The SMD parts are all on Top: **U3 and U4 (90°),
  U8 (90°), U9 (180°) and U12 (0°)**. The rotations are KiCad's, with no JLCPCB offset table; check each part in
  JLCPCB's placement preview (a correction worked out for v1.3.1's U8, −90°, does not carry over). The TSSOPs' test
  pads are part of their footprints, so JLCPCB can place U3/U4/U9 too.
  The BOM takes the LCSC part from each part's LCSC field (any note after the number dropped; the MPN column then
  names the part LCSC sells). JLCPCB fits no jumper
  shunts: add the four by hand. On 2026-11-07 LCSC had none of U2, U5 and U10, and the Pico (C7203002) is only in
  JLCPCB's assembly library.
* `PicoGUS-THT-drc.rpt`: the DRC report.

The **Hardware (JLCPCB files)** GitHub Action runs the same script for this board (with
`ALLOW_DRC=courtyards_overlap` and `CPL=all`) and for `hw-smd`.

Order options: 2 layers, **1.6 mm**, **Surface finish: ENIG** and **Gold fingers: Yes** with the **45° finger
chamfer** (the ISA edge connector). ENIG is required: JLCPCB only gold-plates the fingers on ENIG boards; with HASL
the fingers are tinned, and tin wears and oxidises in an ISA slot. The fingers start 0.58–0.60 mm above the card
edge, as on v1.2, so JLCPCB's bevel (about 0.74 mm) takes about 0.15 mm off their tips. Check the rotations in
JLCPCB's placement preview before paying for assembly (pin 1 of U3, U4, U8, U9 and U12). JLCPCB no
longer prints an order number by default.
The `JLCJLCJLCJLC` placeholder sits on B.Fab next to the Pico (not plotted): if an order ever needs "Specify a
location", move it to B.Silkscreen first, or JLCPCB prints the placeholder text itself.

## Caveats

1. **Not built or tested.** v1.3.2 is routed and reviewed (see [Layout status](#layout-status)). The circuit is the
   proven v1.2 one with the v1.3.x additions, but no v1.3.x THT board has been fabricated or run in a PC yet.
2. **Audio needs −12 V.** The mixer op-amp runs from ISA ±12 V, so without −12 V there is
   **no line output at all** (not even PCM). Every AT/ATX PC power supply provides −12 V. Some unusual
   backplanes or small DC-DC "pico" supplies may not, so check yours.
3. **Slower logic in the ISA path.** 74AHC00 (U6/U7) is a few ns slower than the v1.2 74LVC00,
   and IOCHRDY now goes through an AHCT126 instead of an LVC2G06. At ISA speeds this should be
   harmless, but it hasn't been tested on fast (10–12 MHz) buses.
4. **AHC input thresholds.** At 3.3 V, 74AHC needs ~2.3 V for a logic high (74LVC: 2.0 V). The ISA
   signals that reach U5/U7 directly (AEN, RESET, DACK) are fine from any normal chipset or
   74LS/F/ACT driver (VOH ≈ 3.4 V or more). A marginal old bus that only gives the TTL minimum of 2.4 V
   has little margin.
5. **Daughterboard clearance.** The Waveblaster daughterboard sits ~11 mm above the card, and a
   full-size (Creative-spec) board covers most of the card's left half. Only low parts are placed under it (DIP ICs,
   the TSSOPs, axial resistors, radial MLCCs, the J1 jumper block as on PicoGUS 2.0). Electrolytics and the pot are
   outside that area; the Pico's far end is about 2.9 mm under it (1.3 mm on v1.3.1), so with a daughterboard fitted
   solder the Pico flat or use the short Adafruit sockets. IC sockets add 4.6–5.1 mm: solder the ICs directly or
   check the clearance if the daughterboard has parts on its underside (see [Socket build](#socket-build)).
6. **Fine-pitch soldering.** U3, U4 and U9 are 0.65 mm-pitch TSSOPs soldered to the card. The hand-solder land
   pattern leaves room for an iron, and every pin has a test pad, but it needs flux, a fine tip and a magnifier.
   Check for bridges on the test pads before fitting the through-hole parts around them.
7. **RV1.** The pot has a **15 mm flat (D) shaft**, so one knob fits both BOM lines: DigiKey's Bourns
   PTD902-2015F-A103 (27.42 kr) and LCSC's Alps RK09712200MC (C444934, $1.69; DigiKey does not sell it). The
   Bourns has the same 2.5 mm pin grid and M7 bushing as the Alps RK097 the footprint was drawn for; test-fit
   one with its knob in the bracket before a large order. The 20 mm versions (Bourns PTD902-2020K-A103, knurled,
   the earlier BOM part; Alps RK09712200HA, flat, 44.66 kr at DigiKey) also fit if a knob needs more shaft. Alps
   North America has published a discontinuation notice that covers parts of the RK097 range.
8. **ISA bracket.** No stock bracket fits: the tabs must be 1.70 mm longer than a Keystone 9202's, and no bracket
   made for v1.2, v1.3.0, v1.3.1 or the routed v1.3.2 layout fits. Cut the four openings in the
   [Bracket](#bracket) table (and counterbore the J5/J8 holes).
9. **Mixed LCSC stand-ins.** LCSC's 3.5 mm jacks are XKB PJ-325C5 equivalents. Their unused
   TN pin sits ~0.4 mm further out, so clip it if it doesn't fit. Don't use C2910528
   (different pinout; it is in the v1.2 JLCPCB project). The Pico is not sold in the LCSC shop
   (only in the JLCPCB assembly library). DigiKey doesn't stock the APS6404L PSRAM; Adafruit 4677 (the bare chip) is
   the DigiKey source.
10. **Ferrite beads.** FB1/FB2 are axial beads on leads (DigiKey Fair-Rite 2743002112, LCSC FH
    RH3.5x0.8x9P52E). A wire link also works (MIDI out works without the beads, with slightly worse EMI).
11. The v1.2 symbols are cached in the schematic, so KiCad reports "symbol doesn't match library"
    warnings. That is expected; don't "update symbols from library" blindly.
12. Waveblaster pin 8 (MIDI out from the daughterboard) and the daughterboard audio inputs (pins 12/16)
    are not used. PicoGUS has no MIDI in.
