---
name: sorting-sheets
description: >-
  Make printable component sorting sheets (PDF, A4 or A4-folding-to-A5) from a DigiKey order export plus
  one or more board BOMs: one taping square per BOM line with all reference designators, quantity per board,
  value/package, the ordered part as printed on the bag, and the DigiKey order line number (IDX) at the
  bottom, with shared bags, shortages, back-orders and parts missing from the order flagged. Use this
  whenever the user wants to sort, kit, bag, tape or organise the parts from a parts order for building
  boards (PicoGUS THT/SMD/NE2K or any other PCB project), asks for a sorting list, sorting sheet, kitting
  sheet or parts map, wants to know which order line goes to which reference, or uploads a DigiKey order
  CSV (DK_PRODUCTS_*.csv, Norwegian or English headers) together with a BOM - even if they only say
  "sorting list" or "print squares for the components".
---

# Component sorting sheets

A DigiKey order arrives as a heap of bags labelled with DigiKey part numbers, often mixed with parts for
other projects. These sheets turn the board BOM(s) into one square per BOM line: the builder finds the bag
by its order line number (IDX) and MPN, tapes the parts into the square, and then builds the board by
reference designator. Everything the builder needs while sorting must be on the square itself, which
is why the scripts print the refs, the quantity for ONE board, the value, the ordered MPN and DigiKey's
description (that's what the bag label says), and put the IDX at the bottom.

Three bundled scripts in this skill's `scripts/` folder do the work. `${CLAUDE_SKILL_DIR}` below is the
folder that contains this SKILL.md (Claude Code fills it in; on other surfaces use the skill's base
directory). Call the scripts by that full path: they live with the skill, not in the user's project. Use them rather than writing a new generator: the layout rules
(no text in the fold band, every ref printed, wide squares for long ref lists, taping zones that stay
usable) took several rounds of print testing to get right.

| Script | What it does |
|---|---|
| `${CLAUDE_SKILL_DIR}/scripts/build_mapping.py CONFIG.json mapping.json` | Reads the order and the BOMs, matches every BOM line to an order line, prints a report |
| `${CLAUDE_SKILL_DIR}/scripts/render.py mapping.json OUT.pdf --layout ...` | Draws the sheets (reportlab) |
| `${CLAUDE_SKILL_DIR}/scripts/check_sheets.py mapping.json OUT.pdf --layout ... --png-dir DIR` | Verifies the PDF against the mapping and writes PNG previews |

## Setup

- Python 3 with **reportlab** for render.py (build_mapping.py needs only the standard library). The
  claude.ai / API code-execution sandboxes have it. Elsewhere, if `import reportlab` fails, make a venv in
  a scratch or temp directory (`python3 -m venv <tmp>/venv-rl && <tmp>/venv-rl/bin/pip install reportlab`)
  and run render.py with that interpreter.
- A DejaVu Sans TTF for µ, Ω and × (`fonts-dejavu-core` on Debian/Ubuntu; matplotlib's copy also works).
- For check_sheets.py: poppler-utils (`pdftotext`, `pdftoppm`), or the Python package pdfplumber, which it
  falls back to automatically (preinstalled in the claude.ai / API sandboxes).
- The order CSV is the user's own purchase data and was downloaded from elsewhere: copy it into its own
  empty directory in the scratchpad, run Python with `-I`, and don't commit it to the repo.

## Workflow

### 1. Gather the inputs
- **Order CSV**: DigiKey's order or cart export (`DK_PRODUCTS_<order>.csv`). Column names are found by
  pattern, so English, Norwegian (Indeks, DigiKey-delenummer, Antall, Kundereferanse, Restordre), German and
  other locales work. A summary/total line without a part number is skipped.
- **BOM CSV per board**: needs a references column; Qty, Value, Type/Package, Manufacturer, MPN and
  DigiKey PN are used when present. This repo's `hw-*/BOM-*.csv` files are the reference format. Lines whose
  Type says "not fitted", "optional" or "DNP", or whose Qty is 0, are left out.
- **How many of each board** the user builds (`builds`); the quantity check uses it.

### 2. Write a config and build the mapping
Start from `${CLAUDE_SKILL_DIR}/assets/picogus-v1.3.example.json` (PicoGUS THT + SMD, with the substitutions and presentation
tweaks used for the first v1.3 build) or from the minimal form:

```json
{"order_csv": "order/DK_PRODUCTS_123.csv",
 "boards": [{"key": "THT", "title": "PicoGUS THT v1.3", "bom": "../hw-tht-v1.3/docs/BOM-THT.csv", "builds": 1}]}
```

Relative paths are relative to the config file. All options are described in `${CLAUDE_SKILL_DIR}/references/config.md`.
Run `python3 -I ${CLAUDE_SKILL_DIR}/scripts/build_mapping.py config.json mapping.json` and read the report:

- **NOT IN ORDER**: no order line has the BOM's DigiKey PN or MPN (DigiKey packaging variants such as
  -1-ND/-2-ND/CT-ND count as the same part). Up to three unused order lines that share value words are
  shown as candidates. Decide with the user, line by line:
  - The user ordered an equivalent part on purpose (another manufacturer, gold plating, an alternative from
    the BOM): add a `substitutions` entry pointing at that order line's MPN or DigiKey PN, with a note that
    names the BOM part. Check that it really is equivalent (value, voltage, dielectric/tolerance, package
    or pin pitch, pinout) and say so; a wrong substitution means a wrong part on the board.
  - Otherwise leave it: the square is printed dashed with "NOT IN THIS ORDER" and the BOM part number, so
    the builder knows what still has to be bought (the Pico module, hardware, and so on).
- **AMBIGUOUS**: the part is on several order lines; the first is used and the others are named in the note.
- **SHORT**: an order line has fewer parts than all boards together need (qty × builds). Tell the user
  how many to order; the square shows "SHORT: order N more" in red.
- **BACKORDER**: DigiKey has not shipped everything yet; shown in red on the square.
- **Unused order lines**: often other projects in the same order. Mention them in the summary; if a line
  that the boards use is also meant for another project (its customer reference says so), add an
  `order_notes` entry so the square says the bag must be shared.

Re-run build_mapping.py after each config change.

### 3. Choose a layout and render
| Layout | Squares | Use when |
|---|---|---|
| `a5fold` (default) | 3 × 2 per A5 half, about 63 × 61 mm | always, unless the user asks for something else |
| `a5fold-compact` | 5 × 2 per A5 half, about 37 × 61 mm | the user wants fewer pages / smaller squares |
| `a4` | 4 × 5 per flat A4 page, about 47 × 53 mm | the user wants flat A4 sheets |

The default is the format the builder settled on after printing the others: A4 sheets that fold (or are cut)
to A5, with squares about 50 % larger than the compact version, because jacks, pots, DIP ICs, the Pico
and 1x20 header strips did not fit the smaller squares. In this layout a part and its companion (U1 and
its pin strip) get a square each. Use it whenever the request just says "sorting sheets" or "A5"; switch
only when the user asks for compact/smaller squares or flat A4.

`python3 ${CLAUDE_SKILL_DIR}/scripts/render.py mapping.json sheets.pdf` (default layout a5fold), using the venv's
python if you made one. Add `--split` for one PDF per board, e.g. `sheets-{board}.pdf`. Lines with 18 or more parts (9 in the compact and A4
layouts) get a double-width square so every reference stays readable and the parts fit; change that with `--wide-min`.
render.py prints the smallest taping zones: below about 12 mm of height a square is cramped, so shorten that
square's notes/values in the config (or go back to the default a5fold layout if a compact one was used).

### 4. Check before delivering
Run `python3 -I ${CLAUDE_SKILL_DIR}/scripts/check_sheets.py mapping.json sheets.pdf --layout <same> --png-dir <dir>`
(with `--boards` when the PDF holds only some boards). It must say OK. Then look at the PNGs yourself:
text that collides with the Qty or the border, awkward wraps, empty or crowded squares. The checker
only proves that everything is printed, not that it reads well.

### 5. Deliver
Send the PDF(s) and summarise in a few lines:
- layout, number of pages/squares per board;
- substitutions you applied (BOM part → ordered part);
- parts not in the order (still to buy) and shortages ("RV1: 1 ordered for 2 boards, order 1 more");
- order lines not used by these boards;
- printing: 100 % / actual size, single-sided. For the A5 layouts: fold with the printed side out and
  flip over the fold, or cut along the line; folding printed side in makes the taped parts collide.

## What the squares mean (so you can explain them)
- **Qty**: parts for ONE board, even when several boards are built.
- **IDX n**: the line number on the DigiKey order (the "Index"/"Indeks" column); "ordered N" is the line's
  quantity. **also on X**: board X uses the same bag, so split it.
- **Dashed border, NOT IN THIS ORDER**: still to be bought; the BOM part number is printed.
- **Red SHORT / BACKORDER**: not enough parts in the bag.
- **Dotted area**: where the parts are taped. The header reminds the builder to bend axial leads into a U and
  to cut long tapes into rows.
- **A5 layouts**: each half is a complete A5 page with its own header; the band around the fold stays empty.
