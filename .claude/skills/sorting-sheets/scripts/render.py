"""Render printable component sorting sheets (PDF) from build_mapping.py's mapping.json.

usage: python3 render.py MAPPING.json OUT.pdf [--layout a5fold|a5fold-compact|a4] [--boards A,B] [--split]
                         [--wide-min N]

a5fold          (default) A4 portrait that folds across the middle to A5: each half is a complete A5 page
                with its own header and 3 x 2 large squares (about 63 x 61 mm); nothing is printed in a band
                along the fold. A part and its companion (U1 + header pins) get a square each.
a5fold-compact  as a5fold with 5 x 2 smaller squares per half (about 37 x 61 mm): fewer pages
a4              flat A4 portrait page with 4 x 5 squares (about 47 x 53 mm)
(a5fold-large is accepted as the old name of a5fold)
--split       one PDF per board: OUT may contain {board} (default: OUT with -<board> before .pdf)
--wide-min N  lines with at least N parts get a double-width square (default 18 for a5fold, 9 otherwise)

Needs reportlab and a DejaVu Sans TTF (for µ, Ω, ×).
"""
import argparse, glob, json, os, re, sys
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

ap = argparse.ArgumentParser()
ap.add_argument('mapping'); ap.add_argument('out')
ap.add_argument('--layout', default='a5fold', choices=['a5fold', 'a5fold-compact', 'a4', 'a5fold-large'])
ap.add_argument('--boards', default='')
ap.add_argument('--split', action='store_true')
ap.add_argument('--wide-min', type=int, default=0)
A = ap.parse_args()
LAYOUT = 'a5fold' if A.layout == 'a5fold-large' else A.layout
LARGE = LAYOUT == 'a5fold'                  # the large-square A5 layout
COLS_A5 = 3 if LARGE else 5
WIDE_MIN = A.wide_min or (18 if LARGE else 9)
D = json.load(open(A.mapping, encoding='utf-8'))
META = D.get('meta', {})
BOARDS = A.boards.split(',') if A.boards else [b['key'] for b in META.get('boards', [])] or list(D['squares'])
BOARD_NAME = {b['key']: b.get('title', b['key']) for b in META.get('boards', [])}
BUILDS = {b['key']: b.get('builds', 1) for b in META.get('boards', [])}


def find_font(name):
    for pat in (f'/usr/share/fonts/**/{name}', f'/usr/local/share/fonts/**/{name}', f'/Library/Fonts/**/{name}',
                os.path.expanduser(f'~/.fonts/**/{name}'), f'C:/Windows/Fonts/{name}',
                os.path.join(os.path.dirname(__file__), '..', 'assets', name)):
        hits = glob.glob(pat, recursive=True)
        if hits:
            return hits[0]
    try:                                    # matplotlib ships DejaVu
        import matplotlib
        p = os.path.join(os.path.dirname(matplotlib.__file__), 'mpl-data', 'fonts', 'ttf', name)
        if os.path.exists(p):
            return p
    except ImportError:
        pass
    sys.exit(f'font {name} not found: install fonts-dejavu-core (or pip install matplotlib)')


pdfmetrics.registerFont(TTFont('Sans', find_font('DejaVuSans.ttf')))
pdfmetrics.registerFont(TTFont('SansB', find_font('DejaVuSans-Bold.ttf')))
W, H = A4
PAD = 2.2 * mm
GUT = 1.6 * mm                      # gap between squares, so every square has its own border
GREY = (0.3, 0.3, 0.3)
RED = (0.8, 0, 0)
VSIZE = 8.2                         # value line; the IDX uses the same size
MFR = {'Adafruit Industries LLC': 'Adafruit', 'Alps Alpine': 'Alps', 'Amphenol ICC (FCI)': 'Amphenol',
       'Diodes Incorporated': 'Diodes', 'Fair-Rite Products Corp.': 'Fair-Rite', 'Harwin Inc.': 'Harwin',
       'Murata Electronics': 'Murata', 'Panasonic Industry': 'Panasonic', 'Same Sky (Formerly CUI Devices)': 'Same Sky',
       'Samsung Electro-Mechanics': 'Samsung', 'Shenzhen Slkormicro Semicon Co., Ltd.': 'Slkor',
       'Stackpole Electronics Inc': 'Stackpole', 'Sullins Connector Solutions': 'Sullins',
       'Texas Instruments': 'TI', 'Vishay Beyschlag/Draloric/BC Components': 'Vishay'}
SIDE = 8 * mm
NB = '\u00a0'


def short_mfr(m):
    """'Texas Instruments' -> 'TI', 'Murata Electronics' -> 'Murata', 'Amphenol ICC (FCI)' -> 'Amphenol'"""
    if m in MFR:
        return MFR[m]
    m = re.sub(r'\(.*?\)', '', m)
    m = re.sub(r'[,.]?\s+(Inc|Incorporated|Corp|Corporation|Co|Ltd|LLC|GmbH|AG|Electronics|Electro-Mechanics|Industries|'
               r'Industry|Products|Semiconductor|Components|Connector Solutions|Devices|Technology|International)\b\.?', '', m)
    m = m.split('/')[0].strip(' ,.')
    return m if len(m) <= 14 else m.split(' ')[0]


def frames():
    """the grid frames on one physical page: dict(top = page y of the frame's top edge, grid_top, cols, rows,
    cw, ch). The header sits between top and grid_top."""
    if LAYOUT == 'a4':
        top, bot = 0, 7 * mm
        return [dict(top=H, title_y=H - 9.5 * mm, grid_top=H - 19 * mm, cols=4, rows=5,
                     cw=(W - 2 * SIDE) / 4, ch=(H - 19 * mm - bot) / 5)]
    hh = H / 2
    edge, head = 6 * mm, 12 * mm        # same margins on every side of an A5 half, fold side included
    out = []
    for k in range(2):
        top = H - k * hh
        gt = top - edge - head
        out.append(dict(top=top, title_y=top - edge - 3.6 * mm, grid_top=gt, cols=COLS_A5, rows=2,
                        cw=(W - 2 * SIDE) / COLS_A5, ch=(gt - (top - hh + edge)) / 2))
    return out


def sw(t, f, s):
    return pdfmetrics.stringWidth(t, f, s)


def wrap(text, font, size, width, sep=' '):
    """greedy wrap on `sep` (keeps the separator at the end of a line for comma lists)"""
    words = text.split(sep)
    lines, cur = [], ''
    for i, w in enumerate(words):
        piece = w + (sep if i < len(words) - 1 else '')
        trial = cur + piece
        if cur and sw(trial.rstrip(), font, size) > width:
            lines.append(cur.rstrip())
            cur = piece.lstrip()
        else:
            cur = trial
    if cur.strip():
        lines.append(cur.rstrip())
    return lines


def fit(text, font, sizes, width, maxlines, sep=' '):
    for s in sizes:
        ls = wrap(text, font, s, width, sep)
        if len(ls) <= maxlines and all(sw(l, font, s) <= width for l in ls):
            return s, ls
    s = sizes[-1]
    ls = wrap(text, font, s, width, sep)[:maxlines]
    return s, ls


def ellipsize(text, font, size, width):
    if sw(text, font, size) <= width:
        return text
    while text and sw(text + '…', font, size) > width:
        text = text[:-1]
    return text + '…'


def type_label(sq):
    t = sq['type']
    if t == 'SMD only':
        return 'SMD part'
    if t.startswith('THT module'):
        return 'module'
    return t


def text_lines(c, x, ty, lines, font, size, lead=1.2, color=None):
    if color:
        c.setFillColorRGB(*color)
    c.setFont(font, size)
    for l in lines:
        c.drawString(x, ty, l)
        ty -= size * lead
    if color:
        c.setFillColorRGB(0, 0, 0)
    return ty


def body(c, x, ty, w, sq, title=None, inline_missing=False):
    """value/package, ordered part, notes and SHORT warning of one item, from baseline ty down; returns ty.
    title replaces the value line (used for the second item of a combined square)."""
    missing = not sq.get('idx')
    if title:
        tsize, tl = fit(title, 'SansB', [VSIZE, 7.6], w, 2)
        ty = text_lines(c, x, ty, tl, 'SansB', tsize, 1.22)
        if sq.get('sub'):
            ssize, sl = fit(sq['sub'], 'Sans', [7.0, 6.6], w, 2)
            ty = text_lines(c, x, ty, sl, 'Sans', ssize)
    else:
        # value + package: one line at full size if it fits, else the value (wrapped) and the package below
        val, typ = sq['value'], type_label(sq)
        one = f"{val} · {typ}" if typ else val
        if sw(one, 'SansB', VSIZE) <= w:
            vsize, vl = VSIZE, [one]
        else:
            vsize, vl = fit(val, 'SansB', [VSIZE, 7.6, 7.0], w, 3, sep=', ' if ', ' in val and not typ else ' ')
            vl = vl + ([typ] if typ else [])
        ty = text_lines(c, x, ty, vl, 'SansB', vsize, 1.22)
    ty -= 0.4
    if not missing:
        # ordered part: manufacturer + MPN (what the bag label shows), then DigiKey description
        mfr = short_mfr(sq['o_mfr'])
        mt = f"{mfr} {sq['o_mpn']}"
        msize = next((s for s in (6.8, 6.4) if sw(mt, 'Sans', s) <= w), None)
        ml = [mt] if msize else [mfr, ellipsize(sq['o_mpn'], 'Sans', 6.8, w)]
        ty = text_lines(c, x, ty, ml, 'Sans', msize or 6.8, 1.22)
        dsize, dl = fit(sq['o_desc'], 'Sans', [6.3, 6.0], w, 3)
        ty = text_lines(c, x, ty, dl, 'Sans', dsize, 1.2, GREY)
    note = sq.get('note', '')
    if missing:
        if inline_missing:
            ty = text_lines(c, x, ty, ['NOT IN THIS ORDER'], 'SansB', 7.2, 1.25)
        note = ''
        if sq['bom_dkpn']:
            head, _, last = sq['bom_mpn'].rpartition(' ')
            pair = f"{last}{NB}({sq['bom_dkpn']})"
            if sw(pair if head else f"BOM: {pair}", 'Sans', 6.5) > w:   # an unbreakable run must fit the width
                pair = pair.replace(NB, ' ')
            note = f"BOM:{NB}" + (f"{head} " if head else '') + pair
    extra = [t for t in (note, sq.get('alt', '')) if t]
    if extra:
        nsize, nl = fit('  ·  '.join(extra), 'Sans', [6.5, 6.2], w, 4)
        ty = text_lines(c, x, ty, nl, 'Sans', nsize)
    if sq.get('short'):
        more = sq['need_total'] - sq['o_qty']
        ssize, sl = fit(f"SHORT: order {more} more", 'SansB', [7.5, 7.0], w, 2)
        ty = text_lines(c, x, ty - 0.4 * mm, sl, 'SansB', ssize, 1.2, RED)
    if sq.get('o_back'):
        bsize, bl = fit(f"BACKORDER: {sq['o_back']} not delivered yet", 'SansB', [7.0, 6.6], w, 2)
        ty = text_lines(c, x, ty - 0.4 * mm, bl, 'SansB', bsize, 1.2, RED)
    return ty


def draw_square(c, x, y, sq, cell_w, cell_h, span=1):
    """(x, y) = bottom-left corner of the cell; span = number of cells wide. A square with a 'companion'
    (U1 + its header pins) shows the two items side by side and the companion's order line at the bottom."""
    x, y = x + GUT / 2, y + GUT / 2
    cw, ch = span * cell_w - GUT, cell_h - GUT
    comp = sq.get('companion')
    bottom_sq = comp or sq                  # whose IDX / order facts go on the bottom row
    missing = not bottom_sq.get('idx')
    c.setLineWidth(0.7)
    c.setStrokeColorRGB(0, 0, 0)
    if missing:
        c.setDash(3, 2)
    c.rect(x, y, cw, ch)
    c.setDash()
    inner_w = cw - 2 * PAD
    top = y + ch - PAD

    # --- quantity (top right)
    unit = sq.get('qty_unit')
    qtxt = f"Qty: {sq.get('qty_text', sq['qty'])}" + (f" {unit}" if unit else '')
    qsize = 9.5 if cell_w - GUT - 2 * PAD > 38 * mm else 8.5
    qw = sw(qtxt, 'SansB', qsize)
    c.setFont('SansB', qsize)
    c.drawRightString(x + cw - PAD, top - qsize * 0.82, qtxt)

    # --- references (top left, wrapped on commas, all listed); first line must clear the Qty text
    label = sq['refs'][0].startswith('(')
    reftxt = sq.get('label', sq['refs'][0]) if label else ', '.join(sq['refs'])
    sep = ' ' if label else ', '
    rw_first = inner_w - qw - 2.5 * mm
    size, lines = None, None
    for s_, maxl in ((11, 1), (10.5, 1), (10, 2), (9.5, 2), (9, 3)):
        if label and maxl > 1:
            break
        first = wrap(reftxt, 'SansB', s_, rw_first, sep)[0]
        if sw(first, 'SansB', s_) > rw_first:
            continue
        rest = reftxt[len(first):].lstrip(', ').strip()
        more = wrap(rest, 'SansB', s_, inner_w, sep) if rest else []
        if 1 + len(more) <= maxl and all(sw(l, 'SansB', s_) <= inner_w for l in more):
            size, lines = s_, [first] + more
            break
    ty0 = None
    if size is None:                    # long list: full-width lines under the Qty row
        ty0 = top - qsize * 0.82 - qsize * 1.25
        size, lines = fit(reftxt, 'SansB', [11, 10.5, 10] if label else [9, 8.5, 8], inner_w, 2 if label else 5, sep)
    ty = text_lines(c, x + PAD, ty0 if ty0 is not None else top - size * 0.82, lines, 'SansB', size, 1.12)
    if sq.get('sub') and not comp:
        ssize, sl = fit(sq['sub'], 'Sans', [7.5, 7.0], inner_w, 2)
        ty = text_lines(c, x + PAD, ty - 0.5, sl, 'Sans', ssize) + 0.5
    ty -= 0.6 * mm

    if comp:
        gap = 3 * mm
        colw = (inner_w - gap) / 2
        col_top = ty
        t1 = body(c, x + PAD, ty, colw, sq, inline_missing=True)
        t2 = body(c, x + PAD + colw + gap, ty, colw, comp, title=f"+ {short_label(comp)}: {comp['value']}")
        ty = min(t1, t2)
        c.setLineWidth(0.4)
        c.setStrokeColorRGB(0.6, 0.6, 0.6)
        c.line(x + PAD + colw + gap / 2, col_top + VSIZE, x + PAD + colw + gap / 2, ty + 4)
        c.setStrokeColorRGB(0, 0, 0)
    else:
        ty = body(c, x + PAD, ty, inner_w, sq)

    # --- bottom: IDX (left, same size as the value line) + order facts (right)
    base = y + PAD
    if missing:
        nt = 'NOT IN THIS ORDER'
        nsz = next((s for s in (VSIZE, 7.6, 7.2, 6.8) if sw(nt, 'SansB', s) <= inner_w), 6.8)
        c.setFont('SansB', nsz)
        c.drawString(x + PAD, base + 0.5, nt)
        bottom_used = VSIZE
    else:
        itxt = f"IDX {bottom_sq['idx']}" + (f" ({short_label(comp)})" if comp else '')
        c.setFont('SansB', VSIZE)
        c.drawString(x + PAD, base + 0.5, itxt)
        facts = [(f"ordered {bottom_sq['o_qty']}", 'Sans', 6.4)]
        if bottom_sq.get('shared_with'):
            facts.append((f"also on {'/'.join(bottom_sq['shared_with'])}", 'SansB', 6.8))
        room = inner_w - sw(itxt, 'SansB', VSIZE) - 1.5 * mm
        if sum(sw(t, f, s) for t, f, s in facts) + 2 * mm * (len(facts) - 1) <= room:
            fx = x + cw - PAD
            for t, f, s in facts:       # one line, right to left: "also on SMD   ordered 10"
                c.setFont(f, s)
                c.drawRightString(fx, base + 0.5, t)
                fx -= sw(t, f, s) + 2 * mm
            bottom_used = VSIZE
        else:                           # stack: "ordered N" on the IDX line, "also on ..." above it
            fy = base + 0.5
            for t, f, s in facts:
                c.setFont(f, s)
                c.drawRightString(x + cw - PAD, fy, t)
                fy += s * 1.22
            bottom_used = max(VSIZE, fy - base - 6.8 * 0.22)
    # --- taping zone between the text block and the bottom row
    zt = ty + 2.2 * mm - 1.6 * mm
    zb = base + bottom_used + 1.6 * mm
    if zt - zb > 6 * mm:
        c.setLineWidth(0.6)
        c.setStrokeColorRGB(0.45, 0.45, 0.45)
        c.setDash(1.5, 1.5)
        c.roundRect(x + PAD, zb, inner_w, zt - zb, 1.5 * mm)
        c.setDash()
        c.setStrokeColorRGB(0, 0, 0)
    return (zt - zb, inner_w)


def short_label(sq):
    """'U1 header pins' -> 'header pins' when it starts with the main part's reference"""
    lab = sq.get('label') or ', '.join(sq['refs'])
    return re.sub(r'^[A-Z]+\d+\s+', '', lab)


def header(c, fr, board, page, pages):
    c.setFont('SansB', 13 if LAYOUT == 'a4' else 11.5)
    title = f"{BOARD_NAME.get(board, board)} — component sorting sheet"
    while sw(title, 'SansB', 13 if LAYOUT == 'a4' else 11.5) > W - 2 * SIDE - 32 * mm and ' ' in title:
        title = title.rsplit(' ', 1)[0]
    c.drawString(SIDE, fr['title_y'], title)
    c.setFont('Sans', 8.5)
    c.drawRightString(W - SIDE, fr['title_y'], f"page {page}/{pages}" + (' (A5)' if LAYOUT != 'a4' else ''))
    c.setFont('Sans', 6.5)
    c.setFillColorRGB(*GREY)
    l1 = fr['title_y'] - 3.7 * mm
    n = BUILDS.get(board, 1)
    others = len(BOARDS) > 1 or len(META.get('boards', [])) > 1
    c.drawString(SIDE, l1, f"{META.get('order_label', 'DigiKey order')}.   IDX = line number on the order.   "
                           f"Qty = parts for ONE board{f' (building {n})' if n > 1 else ''}."
                           + ("   'also on X' = board X uses the same bag: split it." if others else ''))
    c.drawString(SIDE, l1 - 2.7 * mm, "Tape the parts in the dotted area (bend axial leads into a U first, cut long "
                                      "tapes into rows).   Dashed square = not in this order.")
    c.setFillColorRGB(0, 0, 0)


def fold_line(c):
    """dashed line on the crease; the instruction sits centred just above it, inside the empty fold band"""
    y = H / 2
    c.setLineWidth(0.35)
    c.setStrokeColorRGB(0.55, 0.55, 0.55)
    c.setDash(5, 3)
    c.line(SIDE, y, W - SIDE, y)
    c.setDash()
    c.setFillColorRGB(0.4, 0.4, 0.4)
    c.setFont('Sans', 5.5)
    c.drawCentredString(W / 2, y + 0.9 * mm, 'fold here to A5, printed side out (flip over the fold to see the other half)'
                                             '  ·  or cut along the line  ·  print at 100 % (actual size)')
    c.setFillColorRGB(0, 0, 0)
    c.setStrokeColorRGB(0, 0, 0)


def pack(sqs, cols, rows, fill=True):
    """place squares in reading order, frame by frame; a wide square takes two cells in one row. If a wide
    square would start in the last column, that cell stays empty, or with fill=True the next normal square
    fills it first (keeps the page count down, bends the reading order)."""
    slots, pending, frame, row, col = [], list(sqs), 0, 0, 0
    while pending:
        i = 0
        if pending[0].get('wide') and col == cols - 1:
            i = next((k for k, s in enumerate(pending) if not s.get('wide')), None) if fill else None
            if i is None:
                i, col, row = 0, 0, row + 1
                if row == rows:
                    frame, row = frame + 1, 0
        sq = pending.pop(i)
        span = 2 if sq.get('wide') else 1
        slots.append((frame, row, col, span, sq))
        col += span
        if col >= cols:
            col, row = 0, row + 1
            if row == rows:
                frame, row = frame + 1, 0
    return slots, frame + (1 if (row or col) else 0)


FR = frames()
zones = []


def layout_squares(sqs):
    """lines with many parts get a double-width square; a main part + companion square (e.g. U1 + its header
    pins) is double width, or two ordinary squares again in the large layout"""
    out = []
    for sq in sqs:
        sq = dict(sq)
        comp = sq.get('companion')
        if comp and LARGE:
            sq.pop('companion'); sq.pop('qty_text', None)
            out += [dict(sq, wide=sq['qty'] >= WIDE_MIN and not sq['refs'][0].startswith('(')), dict(comp, wide=False)]
            continue
        sq['wide'] = bool(comp) or (sq['qty'] >= WIDE_MIN and not sq['refs'][0].startswith('('))
        out.append(sq)
    return out


def new_canvas(path, boards):
    c = canvas.Canvas(path, pagesize=A4)
    c.setTitle(', '.join(BOARD_NAME.get(b, b) for b in boards) + ' - component sorting sheets'
               + (' (A4 folds to A5)' if LAYOUT != 'a4' else ''))
    return c


outputs = []
if A.split:
    targets = [(A.out.format(board=b) if '{board}' in A.out else re.sub(r'(\.pdf)?$', f'-{b}.pdf', A.out, count=1), [b])
               for b in BOARDS]
else:
    targets = [(A.out, BOARDS)]
for out_path, out_boards in targets:
    c = new_canvas(out_path, out_boards)
    for board in out_boards:
        per_page = len(FR)
        sqs = layout_squares(D['squares'][board])
        slots, nframes = pack(sqs, FR[0]['cols'], FR[0]['rows'], fill=False)
        filled = pack(sqs, FR[0]['cols'], FR[0]['rows'], fill=True)
        if -(-filled[1] // per_page) < -(-nframes // per_page):     # gaps would cost a sheet: fill them
            slots, nframes = filled
        npages = (nframes + per_page - 1) // per_page
        for p in range(npages):
            for k, fr in enumerate(FR):
                fi = p * per_page + k
                if fi >= nframes:
                    continue
                header(c, fr, board, fi + 1, nframes)
                for frame, row, col, span, sq in slots:
                    if frame != fi:
                        continue
                    x = SIDE + col * fr['cw']
                    y = fr['grid_top'] - (row + 1) * fr['ch']
                    z, iw = draw_square(c, x, y, sq, fr['cw'], fr['ch'], span)
                    zones.append((board, ','.join(sq['refs'])[:20], round(z / mm, 1), round(iw / mm, 1)))
            if LAYOUT != 'a4':
                fold_line(c)
            c.showPage()
        print(board, npages, 'A4 pages,', nframes, 'frames,', len(slots), 'squares')
    c.save()
    outputs.append(out_path)
    print('saved', out_path)
print('smallest taping zones (h mm, w mm):', sorted(zones, key=lambda z: z[2])[:6])
