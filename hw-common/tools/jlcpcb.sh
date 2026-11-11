#!/usr/bin/env bash
# JLCPCB fabrication (and assembly) files for a KiCad board, with kicad-cli (KiCad 10).
#
#   hw-common/tools/jlcpcb.sh hw-tht-v1.3/PicoGUS-THT.kicad_pcb [OUT_DIR]
#
# Output (default OUT_DIR: <board dir>/build/jlcpcb):
#   <name>-gerbers.zip     upload this to jlcpcb.com (gerbers + Excellon drill files)
#   <name>-drc.rpt         DRC report (errors only, incl. schematic parity)
#   <name>-bom-jlc.csv     JLCPCB assembly BOM  (Comment, Designator, Footprint, LCSC Part #, MPN), every part
#   <name>-cpl-jlc.csv     JLCPCB placement list (Designator, Mid X, Mid Y, Layer, Rotation): SMD parts only (plus
#                          SMD parts on nested dual footprints, *_Nested); CPL=all lists every part in the BOM, so
#                          JLCPCB can also solder the through-hole parts (deselect them there for an SMD-only order)
#
# The BOM's LCSC column is the schematic's LCSC field with any note dropped: "C3690252 (RCER71H106MWM1H03A)" gives
# LCSC Part # C3690252 and MPN RCER71H106MWM1H03A (the part LCSC sells). <name>-jlcpcb-parts.csv next to the board
# (Designator, LCSC Part #, MPN) overrides single parts for assembly, where JLCPCB needs a different part than the
# hand build.
#
# The script stops when DRC finds errors. ALLOW_DRC=type1,type2 (DRC report types such as
# courtyards_overlap) lets those types through but still stops on any other; FORCE=1 exports anyway.
set -euo pipefail

pcb=$(realpath "$1")
name=$(basename "$pcb" .kicad_pcb)
dir=$(dirname "$pcb")
sch="$dir/$name.kicad_sch"
out=$(realpath -m "${2:-$dir/build/jlcpcb}")
rm -rf "$out"
mkdir -p "$out/gerbers"

echo "== DRC ($name)"
if ! kicad-cli pcb drc --severity-error --schematic-parity --exit-code-violations \
        -o "$out/$name-drc.rpt" "$pcb" >/dev/null; then
    grep -E '^\*\* Found' "$out/$name-drc.rpt" || true
    # violation types in the report, e.g. courtyards_overlap, unconnected_items
    found=$(grep -oE '^\[[a-z_]+\]' "$out/$name-drc.rpt" | tr -d '[]' | sort -u | tr '\n' ' ')
    other=""
    for t in $found; do
        case ",${ALLOW_DRC:-}," in *",$t,"*) ;; *) other="$other $t" ;; esac
    done
    if [ -n "${ALLOW_DRC:-}" ] && [ -z "$other" ]; then
        echo "DRC errors are all of the accepted types ($ALLOW_DRC)"
    elif [ "${FORCE:-0}" != 1 ]; then
        echo "DRC errors:${other:- $found} - see $out/$name-drc.rpt (ALLOW_DRC=<types> or FORCE=1 to export anyway)" >&2
        exit 1
    fi
fi
grep -E '^\*\* Found' "$out/$name-drc.rpt" || true

echo "== Gerbers"
# JLCPCB: Protel extensions (default), no X2 attributes, silkscreen clipped by the mask openings
kicad-cli pcb export gerbers -o "$out/gerbers" \
    --layers F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts \
    --exclude-value --subtract-soldermask --no-x2 --no-netlist --check-zones "$pcb" >/dev/null

echo "== Drill"
kicad-cli pcb export drill -o "$out/gerbers/" --format excellon --excellon-units mm \
    --excellon-zeros-format decimal --excellon-oval-format alternate --excellon-separate-th \
    --drill-origin absolute "$pcb" >/dev/null

python3 - "$out/gerbers" "$out/$name-gerbers.zip" <<'EOF'
import os, sys, zipfile
src, dst = sys.argv[1:]
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(src)):
        z.write(os.path.join(src, f), f)
print('   ', os.path.basename(dst), '-', len(os.listdir(src)), 'files')
EOF

if [ -f "$sch" ]; then
    echo "== Assembly BOM + CPL"
    kicad-cli sch export bom -o "$out/$name-bom-jlc.csv" \
        --fields 'Value,Reference,Footprint,LCSC,MPN,${QUANTITY}' \
        --labels 'Comment,Designator,Footprint,LCSC Part #,MPN,Qty' \
        --group-by 'Value,Footprint,LCSC,MPN' --ref-range-delimiter '' --exclude-dnp "$sch" >/dev/null
    python3 - "$out/$name-bom-jlc.csv" "$dir/$name-jlcpcb-parts.csv" <<'EOF'
import csv, os, re, sys
bom, ovr = sys.argv[1:]
rows = list(csv.DictReader(open(bom, newline='', encoding='utf-8')))
over = {}
if os.path.exists(ovr):
    over = {r['Designator'].strip(): r for r in csv.DictReader(open(ovr, newline='', encoding='utf-8'))}
def clean(lcsc, mpn):
    m = re.match(r'\s*(C\d+)\s*(?:\((.*)\))?\s*$', lcsc or '')
    if not m:
        return '', mpn  # no LCSC number (or a note only)
    note = re.sub(r'\s*\(.*\)\s*$', '', m[2] or '').strip()  # "Dersonic CD1H222JC9PECHD000 (5.08 mm pitch)"
    if note:  # the note names the part LCSC sells, maybe after its maker: "UNI-ROYAL MFR0W4F1003A50"
        first, _, rest = note.partition(' ')
        mpn = rest if rest and not re.search(r'\d', first) else note
    return m[1], mpn
groups = {}
for r in rows:
    for ref in [x.strip() for x in r['Designator'].split(',') if x.strip()]:
        lcsc, mpn = clean(r['LCSC Part #'], r['MPN'])
        if ref in over:
            lcsc, mpn = over[ref]['LCSC Part #'].strip(), over[ref]['MPN'].strip()
        groups.setdefault((r['Comment'], r['Footprint'], lcsc, mpn), []).append(ref)
with open(bom, 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f, quoting=csv.QUOTE_ALL)
    w.writerow(['Comment', 'Designator', 'Footprint', 'LCSC Part #', 'MPN', 'Qty'])
    for (comment, fp, lcsc, mpn), refs in groups.items():
        w.writerow([comment, ','.join(refs), fp, lcsc, mpn, len(refs)])
print('   ', len(groups), 'BOM lines' + (' (overrides: %s)' % ', '.join(over) if over else ''))
EOF
    kicad-cli pcb export pos -o "$out/$name-pos.csv" --format csv --units mm --side both \
        --smd-only --exclude-dnp "$pcb" >/dev/null
    # dual footprints with an SMD part nested in through-hole pads (e.g. DIP-16_W7.62mm_TSSOP-16_Nested) are
    # typed through-hole, so --smd-only drops them; their origin and rotation are the SMD part's, add them back
    kicad-cli pcb export pos -o "$out/$name-pos-all.csv" --format csv --units mm --side both \
        --exclude-dnp "$pcb" >/dev/null
    CPL=${CPL:-smd} python3 - "$out/$name-pos.csv" "$out/$name-pos-all.csv" "$out/$name-cpl-jlc.csv" \
        "$out/$name-bom-jlc.csv" <<'EOF'
import csv, os, sys
src, src_all, dst, bom = sys.argv[1:]
rows = list(csv.DictReader(open(src)))
smd = {r['Ref'] for r in rows}
n_smd = len(rows)
if os.environ['CPL'] == 'all':  # every part in the BOM (not holes or edge connectors)
    in_bom = {x for r in csv.DictReader(open(bom)) for x in r['Designator'].split(',')}
    tht = [r for r in csv.DictReader(open(src_all)) if r['Ref'] in in_bom and r['Ref'] not in smd]
    print('   ', len(tht), 'through-hole placements')
    rows += tht; smd = {r['Ref'] for r in rows}
nested = [r for r in csv.DictReader(open(src_all)) if r['Package'].endswith('_Nested') and r['Ref'] not in smd]
with open(dst, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['Designator', 'Mid X', 'Mid Y', 'Layer', 'Rotation'])
    for r in rows + nested:
        w.writerow([r['Ref'], r['PosX'] + 'mm', r['PosY'] + 'mm',
                    'Top' if r['Side'] == 'top' else 'Bottom', r['Rot']])
print('   ', n_smd, 'SMD placements' + (f" + {len(nested)} nested ({', '.join(r['Ref'] for r in nested)})" if nested else ''))
EOF
    rm -f "$out/$name-pos.csv" "$out/$name-pos-all.csv"
fi
rm -rf "$out/gerbers"
echo "== Done: $out"
ls -1 "$out"
