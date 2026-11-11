#!/bin/bash
# usage: groundloop.sh <board>.kicad_pcb OUT.png [WORKDIR]
#   e.g. (from hw-tht-v1.3/):  bash docs/figures/src/groundloop.sh PicoGUS-THT.kicad_pcb docs/figures/groundloop.png /tmp/gl
# Makes the line-out ground-loop figure: extracts the GND copper with refilled pours (the board's .kicad_pro must sit
# next to it; the board file is only read), solves the DC model (about 2 minutes) and draws the PNG.
# Intermediate files (geom.pkl, gl.npz, gl.json with the node potentials and line-out errors) go to WORKDIR
# (default: a new temporary directory).
# needs: kipython (Python with KiCad 10's pcbnew, numpy, scipy, shapely, Pillow) and pyamg (pip install pyamg, or put
# it on PYTHONPATH); Inter fonts in /usr/share/fonts/opentype/inter/ for the look of the other figures.
set -e
B=$(realpath "$1"); OUT=$(realpath -m "$2"); W=${3:-$(mktemp -d)}
D=$(dirname "$(realpath "$0")")
mkdir -p "$W"
REFILL=1 kipython "$D/gnd_extract.py" "$B" "$W/geom.pkl" GND 2>&1 | grep -v -E "assert|Debug:" || true
kipython "$D/groundloop_solve.py" "$W/geom.pkl" "$W/gl.npz" 2>&1 | grep -v -E "assert|Debug:" || true
[ -s "$W/gl.npz" ] || { echo "solver failed (pyamg missing?)" >&2; exit 1; }
kipython "$D/groundloop_figure.py" "$W/gl.json" "$W/gl.npz" "$OUT"
echo "$OUT (intermediate files in $W)"
