#!/usr/bin/env bash
# Product renders of a board for its README, with kicad-cli (KiCad 10) and ImageMagick.
#
#   hw-common/tools/render.sh hw-tht-v1.3/PicoGUS-THT.kicad_pcb OUT_DIR NAME
#
# The rule: red from the front and the back; green, purple, white and black from the front only.
# Writes OUT_DIR/NAME-<colour>-<front|back>.jpg (1600 px wide, JPEG quality 92). RENDERS="red:front ..." overrides
# the list.
#
# Each colour is rendered from a temporary copy of the board's folder (so ${KIPRJMOD} 3D models resolve) with the
# stackup recoloured: solder mask on both sides, ENIG, white silkscreen (black on a white mask, as fabs print it).
# The board file itself is not changed. Each render takes a few minutes (2600 x 2500, high quality).
set -euo pipefail

pcb=$(realpath "$1"); out=$(realpath -m "$2"); name=$3
RENDERS=${RENDERS:-"red:front red:back green:front purple:front white:front black:front"}
dir=$(dirname "$pcb"); base=$(basename "$pcb")
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out"

for spec in $RENDERS; do
    colour=${spec%%:*}; view=${spec##*:}
    case $view in front) side=top ;; back) side=bottom ;; *) echo "bad view: $spec" >&2; exit 1 ;; esac
    work="$tmp/$colour"
    if [ ! -d "$work" ]; then
        mkdir -p "$work"
        # the project folder without build/ (exports there can change while this runs)
        tar -C "$dir" --exclude=./build -cf - . | tar -C "$work" -xf -
        # relative links such as parts -> ../hw-common/parts (the 3D models) would point outside the copy
        for l in "$work"/*; do
            if [ -L "$l" ]; then ln -sfn "$(realpath "$dir/$(basename "$l")")" "$l"; fi
        done
        python3 -I - "$work/$base" "$colour" <<'EOF'
import re, sys
path, colour = sys.argv[1], sys.argv[2].capitalize()
silk = 'Black' if colour == 'White' else 'White'
t = open(path, encoding='utf-8').read()
i = t.index('(stackup'); j = t.index('(copper_finish', i)
s = t[i:j]
s, n = re.subn(r'(\(type "(?:Top|Bottom) Solder Mask"\)\s*)(?:\(color "[^"]*"\)\s*)?', r'\1(color "%s")\n\t\t\t\t' % colour, s)
s = re.sub(r'(\(type "(?:Top|Bottom) Silk Screen"\))(\s*\(color "[^"]*"\))?', r'\1 (color "%s")' % silk, s)
rest = re.sub(r'\(copper_finish "[^"]*"\)', '(copper_finish "ENIG")', t[j:], count=1)
assert n == 2, 'solder mask layers not found in the stackup'
open(path, 'w', encoding='utf-8').write(t[:i] + s + rest)
EOF
    fi
    png="$tmp/$colour-$view.png"
    echo "== $colour $view"
    (cd "$work" && kicad-cli pcb render --quality high --use-board-stackup-colors --background opaque --floor \
        --side "$side" --width 2600 --height 2500 -o "$png" "$base" >/dev/null)
    convert "$png" -resize 1600x -quality 92 "$out/$name-$colour-$view.jpg"
done
echo "== Done: $out"
