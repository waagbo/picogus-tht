# Lessons learned: PicoGUS THT v1.3.x, SMD v1.3.0 and NE2K

The branch that made the through-hole board (v1.3.0 → v1.3.1 rev B → v1.3.2), the SMD v1.3.0 board and the
NE2000 card ran from 2026-09-28 to 2026-11-01 (98 commits). It was squash-merged, so its commit messages are
gone. This file keeps what they recorded that the board READMEs don't: what was tried and dropped and why,
the layout rules the reviews produced, and the tool and sourcing pitfalls. The details of each board are in
its own README (`hw-tht-v1.3/README.md`, `hw-smd/README.md`, `hw-ne2k/README.md`).

v1.3.2 had not been built or tested when this was written. Add what the first build teaches.

## What was tried and dropped

| Tried | Dropped in | Why |
|---|---|---|
| Hybrid footprints: axial resistor or 0805 between the holes, 5 mm disc or 0805, DIP-14/20 or TSSOP | v1.3.0 | Two BOMs in one board, larger courtyards, more to check. The THT board went through-hole only, apart from the parts with no THT version (U3/U4 CB3T3257, U8 PSRAM, U9 PCM510x). |
| J2, the 2x31 ISA probe header carried over from v1.2 | v1.3.0 | Never fitted, and its 62 pads made a wall just above the fingers that every ISA signal had to cross; the autorouter got stuck on it. Removing it freed the strip where U3/U4 now sit, right above the data fingers. |
| A second IRQ (J10/J11) on U2's spare buffer channel | v1.3.0 | All 26 Pico GPIOs are used. The only free signal was the PSRAM's SPI_TX, so it only worked with firmware that leaves the PSRAM idle. |
| GY-PCM5102 module (J10) as an either/or alternative to the on-board DAC | v1.3.2 | The DAC area had to fit both. The module also carries the 470 Ω / 2.2 nF output filter, so the either/or outline had to include R7/R8 and C19/C20. The on-board DAC is now always fitted. |
| U3/U4 on a nested DIP-16 + TSSOP-16 footprint | v1.3.1 rev B | Little gain for the extra footprint, BOM lines and assembly notes. Back to the plain TSSOP-16. |
| U3/U4/U9 on TSSOP-to-DIP adapter boards in DIP sockets | v1.3.2 (298e993) | A socketed adapter reaches about 10 mm, close to the Waveblaster daughterboard (about 11 mm above the card), and adds two more PCBs to order. The TSSOPs are now soldered to the card on hand-solder footprints with a 0.7 mm test pad per pin. |
| Dual USB-A/USB-C footprint for J11 | v1.3.1 rev B, on hold | The USB-C pins needed tighter rules (0.45 mm hole-to-hole, 0.12 mm annular) and an always-on VBUS warning. USB-A only. The dual footprint stays in the library. |
| Waveblaster daughterboard standoff holes | v1.3.1 rev B | The far hole landed in U4. The daughterboard is held by the J9 header only. |
| Futura for the board labels | v1.3.2 | Futura PT is licensed for preview and print only: KiCad can't embed it, and it can't be shared. Each machine plotted its own substitute (DejaVu Sans or Inter in the cloud sessions), so earlier gerbers differ. The labels now use Jost (SIL OFL), embedded in the board file. |
| Back-side princess artwork in silkscreen | v1.3.2 | Removed, to be UV printed instead. |

Changes that stayed:

- **+6 dB Waveblaster gain** (R10/R11/R19/R20 20 k, C30/C31 47 pF). Daughterboards put out about 1–1.2 Vrms full
  scale (DreamBlaster X2: 1.13 Vrms) against the PCM5100A's 2.1 Vrms. At unity gain, MIDI was 5–6 dB quieter than
  PCM at full volume. The 47 pF keeps the feedback pole at about 170 kHz.
- **102.0 mm card** (v1.3.2). JLCPCB prices boards of 102 mm or less lower, so the bracket end was trimmed 1.84 mm
  and the edge parts moved just enough to keep copper 0.35 mm or more from the edge. The bracket tab holes then sit
  1.70 mm further in than on a stock bracket, so the card needs a bracket with longer tabs (e.g. 3D-printed).
- **USB-A host port with protection**: polyfuse F1 and 220 µF bulk C34 (USB 2.0 asks for 120 µF or more per host
  port). The optional USBLC6-2SC6 ESD array U12 is wired flow-through, with no stubs.
- **CPVDD decoupling at U9**: C36 sits next to pins 1–3. Before it, the nearest +3V3 cap was 23.6 mm of track
  away.
- **DAC AVDD on its own rail** behind ferrite bead FB3, with its own +3V3 feed. The NE5532 gets its ±12 V
  through 10 Ω + 47 µF RC filters.

## Layout rules that came out of the reviews

Every route was followed by independent review rounds: analog/power, then digital/ISA/USB/manufacturing, then
silkscreen. Each round found real faults. Apply these from the start on the next layout:

- **DAC block**: only GND on the front under U9. Keep +5V, ±12V, digital, I2S and USB out of the block's analog
  half, and 1 mm or more from all audio copper.
- **Audio net class**: 0.6 mm clearance. No other net within 0.5 mm of an audio net.
- **Gold fingers**:
  - Power entries keep 0.30 mm or more from other-net fingers.
  - No vias in the finger area.
  - Vias just above the fingers are tented. The finger mask opening is trimmed on the board, not in the shared
    footprint.
  - Gold fingers need ENIG.
- **±12V**:
  - 0.30 mm or more from non-GND through-hole pads and from the bare fingers.
  - Neck to 0.2 mm where it crosses the Pico pin rows, so it stays 0.32 mm or more from the pads.
  - Never loop around a Pico pin.
- **GND finger B10**: give it a wide strap into the logic pour. The narrow link was 12.6 mΩ; the strap brought it
  to 3.5 mΩ.
- **The line-out sleeve** should tie to the op-amp through the back pour. With a metal bracket, the sleeve's return
  can run through the bracket tabs (see the ground-loop figure in the v1.3.2 README).
- **Vias**:
  - 0.30 mm or more from other-net pads.
  - Never in a silkscreen letter: a via once cut the "b" in Waagbo.
  - Back artwork is cut 0.3 mm around pads, holes and vias.
- **Pico**:
  - No front GND pour under the flat-soldered Pico's test pads or the TP1–TP3 USB area.
  - USB data comes from TP2 = D− and TP3 = D+, checked against the Pico R3 schematic.
  - The NoSWD footprint drops the unused SWD pads.
  - Remove the library footprint's pin-name and SWD silkscreen.
- **Pico W (NE2K)**: the antenna keep-out applies only between the pin rows (Pico W datasheet 2.2.1), so pins
  17–24 stay routable. The strip from the module end to the bracket is a board-level RF keep-out.
- **Decoupling sits at the pin it serves.** Chips and their caps/resistors form one KiCad group per circuit
  block, which makes hand placement and moving blocks safe.

## Mechanical

- **Bracket**:
  - Check every bracket part against the PC's slot opening and the bracket plate: RV1's bushing once sat above
    the slot opening, and J11's flange once poked through the plate.
  - J11's position was checked against the PicoGUS 2.0, which is proven in production.
  - The hole positions are in the Bracket section of the v1.3.2 README.
- **Waveblaster daughterboard**:
  - About 11 mm above the card.
  - Only low parts under it: DIPs, axial resistors, radial MLCCs, J1.
  - Electrolytics, the pot and the USB parts stay outside.
  - Machined IC sockets add 4.6–5.1 mm.
  - The Pico's far end is under it, so solder the Pico flat or use short sockets.
- **3D/STEP stackup**: v1.2's project file had a 0.57 mm board. It is now 1.6 mm. This only affected 3D exports.

## KiCad and tool pitfalls

- **Keep the `.kicad_pro` next to the board.**
  - Without it, pcbnew (including `pcbnew.LoadBoard` in scripts) falls back to KiCad's defaults: Default net
    classes only, and pours that break the audio 0.6 mm clearance.
  - Saving a board without its project file wrote the default rules over ours once (2e34b45).
  - Some tools rewrite the project file, so diff it before committing.
- **The 3D models of the jacks, the pot and F1 live in `hw-common/parts`**, which the board folder links as `parts`
  (`${KIPRJMOD}/parts/...`). A copy of the board folder (for a render or a review) must keep that link pointing at
  the real folder, or those parts silently vanish from the 3D view; `hw-common/tools/render.sh` re-points it.
- **Copying sheets between projects**: give the new items their own UUIDs. Reused UUIDs mixed up ERC markers.
- **KiCad 10 draws an empty pin name `~` literally.** Use `""` instead.
- **Check `Edge.Cuts` after every hand-edited upload.** The 6.35 mm step next to the ISA fingers (x 197.41) was
  dragged off the board twice.
- **JLCPCB**:
  - NPTH slots (the Pico's USB-shell relief) must also be drawn on `Edge.Cuts`.
  - JLCPCB no longer prints an order number by default, so the placeholder text lives on B.Fab.
  - `hw-common/tools/jlcpcb.sh` gates on DRC with schematic parity. The 12 accepted courtyard overlaps in the
    logic column pass with `ALLOW_DRC=courtyards_overlap`, which still stops on any other error type.
- **Routing**:
  - Lock the critical pre-routes first: the finger corridor (IRQ2, D5), the USB pair between the Pico's shell
    holes, and the DAC GND/VNEG/XSMT stubs.
  - Then autoroute (Freerouting, with GND routed as a net), then fill and stitch the pours.
  - The tightest spots, such as U9's east test pads, are faster by hand.
  - After a rip-up, compare each net's track length with the old board, power nets first. DRC only knows connected
    or not: on THT v2.1 (radio module, USB-C move) an autorouter pass rejoined +5V by a long detour on the far
    side of the card instead of the short run that had been ripped up, and the report was clean.
- **Starved thermals**: a few pads get one spoke. The v1.3.2 README lists where each one joins GND.
- **Cloud environment**: `.claude/cloud-env-setup.sh` installs KiCad 10, the libraries, a `kipython` wrapper,
  Freerouting, gerbv and ngspice. `kipython -I` drops the path it needs, so use `kipython -P -s` and run it
  from your own directory.

## Sourcing (DigiKey and LCSC)

- **DigiKey cart upload**:
  - Columns: `Quantity, Digi-Key Part Number, Manufacturer Part Number, Customer Reference`.
  - Put the board and references in the customer reference so each bag is labelled, e.g.
    `PicoGUS-THT-1.3.2: C30,C31`. Keep it within the limit the upload accepts (the files use 48 characters);
    shorten ranges like `C3-9,11-14`.
  - Parts sold under a purchase limit are rejected by the upload, so pick an equivalent.
  - Use cut-tape (`-1-ND`) numbers. The `-2-ND` number is a full reel.
- **Order exports** (`DK_PRODUCTS_*.csv`, sometimes with Norwegian headers) are personal data: keep them out of
  the repo. The `sorting-sheets` skill turns one plus the BOMs into printable kitting sheets.
- **PSRAM**:
  - DigiKey doesn't stock the APS6404L; Adafruit 4677 is the DigiKey source.
  - On LCSC it is C5333729 (APS6404L-3SQR-SN).
  - The v1.2 BOM's LCSC C139966 is a Vilsion VTI7064MSME, not an APS6404L.
  - Lyontek LY68L6400 is gone from LCSC.
- **LCSC gaps**:
  - Almost no DIP logic.
  - The Pico is only in JLCPCB's assembly library.
  - C2910528 is a 3.5 mm jack with a different pinout (it is in the v1.2 JLCPCB project): don't use it.
- **DAC**:
  - PCM5100A, PCM5101A, PCM5102A and the automotive PCM5102A-Q1 are pin-compatible.
  - PCM5102APWR was out of stock at DigiKey until 2027, but PCM5102AQPWRQ1 was in stock.
  - The difference is not audible with the firmware's 16-bit output, so v1.3.2 went back to the cheapest grade,
    the PCM5100APWR (24.69 kr at DigiKey against 59.83 kr for the PCM5102AQPWRQ1).
- **J9 in gold**: Amphenol 77313-118-26LF has 30 µin gold and the same dimensions as the tin Sullins PEC13DAAN.
- **Check a part's drawing, not its listing title.** LCSC C9040 is listed as a gold 2x13 pin header but is a shrouded
  box header with latches, which a Waveblaster daughterboard cannot plug into.
- **Generate the upload files from the BOM** (`hw-common/tools/bom_uploads.py`) instead of editing them by hand, and
  re-read stock before an order: between September and November the PCM5102APWR, Panasonic ECE-A1HN100U and
  LCSC's Yageo 100 k / 100 Ω lines ran out, and Murata replaced the RCER71H106MWM1H03A.
- **RV1**:
  - 15 mm flat shaft on both lines, so one knob fits: Bourns PTD902-2015F-A103 at DigiKey (the same pin grid and
    bushing as the Alps, at about half DigiKey's Alps price), Alps RK09712200MC (C444934) at LCSC, which DigiKey
    does not sell. Check the shaft type, not only the length: the earlier lines mixed a knurled Bourns with a flat
    Alps.
  - Alps has issued a discontinuation notice for parts of the RK097 range.

## Audio

- The signal path already uses C0G caps (C19/C20, C30/C31) and a bipolar electrolytic for the Waveblaster coupling
  (C22/C23, about 1.6–3.2 Hz corner). The DAC output needs no coupling cap. Film capacitors would not improve any
  position on this board.
- What limits the sound is the 16-bit source, the daughterboard and noise from the PC, not the DAC grade or the
  capacitors. Spend effort on keeping digital noise out of the analog area.

## Process

- **Review every route independently**, and check the README against the board the same way. One check
  (a2e1d6a) found README sections still describing the earlier blank board, and measurements that had drifted.
- **Explain each exception or design choice in the README with a figure next to the text.** The scripts that
  make the figures are in `hw-tht-v1.3/docs/figures/src/`, so the figures can be regenerated when the board
  changes.
- **One folder per board, at the version to build.** During the branch each revision had its own folder (`hw-tht`
  for v1.3.0, `hw-tht-v1.3.1`, `hw-tht-v1.3.2`); at the merge only v1.3.2 was kept, as `hw-tht-v1.3`. The older
  revisions are in the history of the `hw1.3-tht` branch (last at 9e6007f).
