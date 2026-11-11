#!/bin/bash
# Usage: misc_figures.sh BOARD.kicad_pcb OLD_8dbb6c6.kicad_pcb V131.kicad_pcb OUTDIR WORKDIR
#   BOARD.kicad_pcb        hw-tht-v1.3/PicoGUS-THT.kicad_pcb (its directory needs ../hw-common for the libraries)
#   OLD_8dbb6c6.kicad_pcb  the routed 103.74 mm layout: git show 8dbb6c6:hw-tht-v1.3.2/PicoGUS-THT.kicad_pcb > file
#   V131.kicad_pcb         git show 9e6007f:hw-tht-v1.3.1/PicoGUS-THT.kicad_pcb > file
#   OUTDIR                 where starved-thermals.png, silk-vias.png and bracket-trim.png go (docs/figures)
#   WORKDIR                scratch folder for the board copy, the DRC report and the JSON dumps (kept afterwards)
# The DRC runs on a copy of the board's directory in WORKDIR, because kicad-cli rewrites the .kicad_pro next to the
# board it checks; the boards themselves are only read. Needs kicad-cli and kipython (KiCad 10 with shapely).
set -e
B=$(readlink -f "$1"); OLD=$(readlink -f "$2"); V131=$(readlink -f "$3"); OUT=$(readlink -f "$4"); W=$5
SRC=$(dirname "$(readlink -f "$0")")
mkdir -p "$OUT" "$W"; W=$(readlink -f "$W")
BD=$(dirname "$B"); NAME=$(basename "$BD")
mkdir -p "$W/board"
cp -a "$BD" "$W/board/"
[ -e "$W/board/hw-common" ] || ln -s "$(readlink -f "$BD/../hw-common")" "$W/board/hw-common"
COPY="$W/board/$NAME/$(basename "$B")"
kicad-cli pcb drc --severity-all --refill-zones --schematic-parity --format json -o "$W/drc.json" "$COPY" | grep -v property.h || true
kipython "$SRC/misc_dump.py" "$B" "$W/new.json" --refill 2>&1 | grep -v property.h || true
kipython "$SRC/misc_dump.py" "$OLD" "$W/old_8dbb6c6.json" --no-zones 2>&1 | grep -v -e property.h -e Futura || true
kipython "$SRC/misc_dump.py" "$V131" "$W/old_v131.json" --no-zones 2>&1 | grep -v -e property.h -e Futura || true
kipython "$SRC/starved_thermals_figure.py" "$W/new.json" "$W/drc.json" "$OUT/starved-thermals.png" 2>&1 | grep -v property.h
kipython "$SRC/silk_vias_figure.py" "$W/new.json" "$OUT/silk-vias.png" 2>&1 | grep -v property.h
kipython "$SRC/bracket_trim_figure.py" "$W/new.json" "$W/old_8dbb6c6.json" "$W/old_v131.json" "$OUT/bracket-trim.png" 2>&1 | grep -v property.h
