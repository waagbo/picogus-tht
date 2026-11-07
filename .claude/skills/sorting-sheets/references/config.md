# build_mapping.py config reference

The config is one JSON file. Relative paths are resolved from the config file's directory. Only
`order_csv` and `boards` are required; everything else tunes the result for a particular order.
Keys starting with `_` (e.g. `_comment`) are ignored.

## Required

| Key | Example | Meaning |
|---|---|---|
| `order_csv` | `"order/DK_PRODUCTS_123456789.csv"` | DigiKey order or cart export |
| `boards` | see below | one entry per board type |

```json
"boards": [
  {"key": "THT", "title": "PicoGUS THT v1.3 (through-hole)", "bom": "../hw-tht-v1.3/docs/BOM-THT.csv", "builds": 2},
  {"key": "SMD", "title": "PicoGUS SMD v1.3", "bom": "../hw-smd/BOM-SMD.csv"}
]
```
- `key`: short name, printed as "also on THT" on shared lines and used in `--boards` and `{board}` names.
- `title`: sheet heading (shortened if it does not fit).
- `builds` (default 1): how many of this board are built. Quantities on the squares stay per board;
  the shortage check multiplies by `builds`.

## Optional

| Key | Example | Meaning |
|---|---|---|
| `order_label` | `"DigiKey order 123456789"` | header text; default is taken from the digits in the order file name |
| `skip_types` | `["not fitted", "optional", "dnp"]` | BOM lines whose Type contains one of these are left out |
| `substitutions` | see below | use a different order line than the BOM's part number |
| `values` | `{"J9": "2x13 pin header", "THT:J5": "3.5 mm jack"}` | display value, keyed by the line's first ref; prefix `BOARD:` for one board only |
| `labels` | `{"(jumpers)": {"label": "Jumpers", "sub": "J1 ×3, J7 ×1"}}` | heading and sub-line for BOM lines whose reference is not a designator (`(jumpers)`, `(DB-screws)`) |
| `alt` | `{"(jumpers)": "alt. IDX 53 (gold), 63 (red)"}` | extra line on a square (alternatives in the same order) |
| `order_notes` | `{"35YXJ47M5X11": "bag also holds C14, C43 of the other card"}` | note on every square that uses that order line (MPN or DigiKey PN), e.g. a bag shared with another project |
| `extra_squares` | see below | parts that are not BOM lines (a header strip cut into pieces, spare fuses) |
| `combine` | see below | merge several lines into one square (all on the same order line, or all not ordered) |
| `companions` | see below | show a part and its accessory in one double-width square |
| `category_order` | `["U", "Q", "D", "C", "R", "L", "J"]` | order of the squares by reference prefix; unknown prefixes follow, non-designator lines come last in BOM order |

### substitutions
```json
"substitutions": [
  {"ref": "RV1", "use": "RK09712200HA", "note": "BOM: Bourns PTD902-2020K-A103"},
  {"boards": ["SMD"], "ref": "C10", "use": "445-8044-1-ND", "note": "same value/size as BOM C2012X7R1H105K125AB"},
  {"ref": "J6", "use": "idx:112", "note": "gold"}
]
```
- `ref`: the first reference of the BOM line (as printed in the BOM).
- `use`: the ordered part's MPN, DigiKey PN, or `idx:N` for a specific order line. Prefer MPN/DigiKey PN
  so the config still works for the next order.
- `boards`: limit to some boards (default: all).
- `note`: printed on the square. Name the BOM part so the builder knows what was replaced.

### extra_squares
```json
"extra_squares": [
  {"boards": ["THT", "SMD"], "refs": ["(pins)"], "label": "U1 header pins", "sub": "cut 2× 1x20 from one strip",
   "qty": 1, "qty_unit": "strip", "value": "1x40 strip", "type": "THT", "use": "PREC040SAAN-RC",
   "bom_mpn": "PREC040SAAN-RC", "bom_dkpn": "S1012EC-40-ND", "note": "Not needed with a Pico H."}
]
```
`refs` in parentheses mark a non-designator square (its `label` is the heading). `use` finds the order
line; if it is not in the order the square is printed as not ordered, with `bom_mpn`/`bom_dkpn` as the
part to buy.

### combine
```json
"combine": [{"refs": ["(DB-standoff)", "(DB-screws)"], "label": "DB hardware", "sub": "Waveblaster mounting",
             "value": "M3×11 F-F standoff ×1, M3×6 screw ×2"}]
```
Merges the listed lines (by first ref) into the first one: Qty becomes "1 + 2". Lines on different
order lines are not merged (their IDX would be lost).

### companions
```json
"companions": [{"main": "U1", "companion": "(pins)", "qty_text": "1 + 1 pin strip"}]
```
Prints the companion's order data next to the main part in one double-width square (both items are too
long for a normal square: a Pico and its 1x20 pin strips). The default large-square a5fold layout prints them as two normal
squares again.

## Output (mapping.json)
`meta` (order label, boards, builds), `squares` (per board, in print order), `order` (all order lines by
IDX) and `report` (unmatched with candidates, ambiguous, short, backorder, unused). render.py and
check_sheets.py read this file; nothing else depends on its format.
