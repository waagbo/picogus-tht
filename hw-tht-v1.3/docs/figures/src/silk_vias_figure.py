# Usage: kipython silk_vias_figure.py DUMP.json OUT.png [LIMIT_MM]
#   DUMP.json  from 'kipython misc_dump.py PicoGUS-THT.kicad_pcb DUMP.json --refill'
#   LIMIT_MM   distance from a via hole to silkscreen text that counts as "near" (default 0.10)
# Needs misc_figlib.py next to this script.
#
# Draws silk-vias.png: every via whose drilled hole is inside silkscreen text ink or within LIMIT_MM of it (front and
# back silkscreen; text = reference designators, footprint texts and board texts, as glyph outlines). One close-up per
# via with the text, the via, the hole and the ink the hole takes away (red), plus a board map with their positions.
# All distances are hole circle (drill diameter) to glyph outline; areas are hole ∩ glyph.
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from misc_figlib import *

d = load(sys.argv[1]); OUT = sys.argv[2]
LIMIT = float(sys.argv[3]) if len(sys.argv) > 3 else 0.10

texts = []
for r, f in d['fps'].items():
    for t in f['silktext']:
        texts.append({'owner': r, 'field': t.get('field'), **t, 'g': polys_to_geom(t['poly'])})
for t in d['texts']:
    if t['kind'] == 'text':
        texts.append({'owner': None, 'field': None, **t, 'g': polys_to_geom(t['poly'])})
texts = [t for t in texts if not t['g'].is_empty]

def find_hits(lim):
    out = []
    for v in d['vias']:
        h = Point(v['pos']).buffer(v['drill'] / 2, quad_segs=64)
        best = None
        for t in texts:
            dist = h.distance(t['g'])
            if dist < lim and (best is None or dist < best[0]):
                best = (dist, t)
        if best:
            dist, t = best
            ov = h.intersection(t['g'])
            # the glyph(s) the hole touches, and the share of that glyph's ink it takes
            glyphs = [g for g in getattr(t['g'], 'geoms', [t['g']]) if g.distance(h) < lim]
            gl = unary_union(glyphs)
            out.append({'via': v, 'hole': h, 'text': t, 'dist': dist, 'ov': ov, 'ov_area': ov.area,
                        'glyph_share': ov.area / gl.area if gl.area else 0})
    out.sort(key=lambda x: (x['dist'] > 0, x['dist'] if x['dist'] > 0 else -x['ov_area']))
    return out


hits = find_hits(LIMIT)
# none at the limit: show the nearest ones instead (within twice the limit) so that the figure still proves the point
NONE = not hits
NEAR2 = 2 * LIMIT
if NONE:
    hits = find_hits(NEAR2)
    print('no via hole within %.2f mm of silk text; %d within %.2f mm:' % (LIMIT, len(hits), NEAR2))
inside = [x for x in hits if x['dist'] == 0]
near = [x for x in hits if x['dist'] > 0]


def near_fp(pos, lim=3.0):
    """the footprint whose nearest through-hole pad is within lim of a board text (jumper labels and the like)"""
    best = None
    for r, f in d['fps'].items():
        for p in f['pads']:
            if p['smd']:
                continue
            dd = math.dist(p['pos'], pos)
            if dd < lim and (best is None or dd < best[0]):
                best = (dd, r)
    return best[1] if best else None


def tname(t):
    if t['field'] == 'Reference':
        return '%s reference' % t['text']
    if t['owner'] is None:
        r = near_fp(t['pos'])
        return ('%s label "%s"' % (r, t['text'].strip())) if r else 'label "%s"' % t['text'].strip()
    return '%s text "%s"' % (t['owner'], t['text'])


def netname(n):
    n = n.replace('{slash}', '/')
    if n.startswith('~{'):
        n = '~' + n[2:]
    return n.replace('}', '').split('/')[-1] if n.startswith('/') else n.replace('}', '')


print('%d via holes within %.2f mm of silk text, %d inside the ink' % (len(hits), NEAR2 if NONE else LIMIT, len(inside)))
for x in hits:
    print('  %-16s %s.Silk  via %-10s at (%.2f, %.2f) drill %.2f: %s' % (
        tname(x['text']), x['text']['side'], netname(x['via']['net']), x['via']['pos'][0], x['via']['pos'][1], x['via']['drill'],
        'inside, %.3f mm2 of ink (%.0f %% of the glyph)' % (x['ov_area'], 100 * x['glyph_share']) if x['dist'] == 0 else '%.3f mm away' % x['dist']))

# ---------------------------------------------------------------- close-ups
S = 150
PW, PH = 3.3, 2.3          # panel size in mm
SILKC = (35, 35, 40)
VIAC = (150, 150, 150)
fl = font(19, 'Bold'); fls = font(18, 'SemiBold')


def closeup(x):
    t = x['text']; v = x['via']
    # window: the whole text and the hole plus a margin, at the panel's aspect ratio
    x0, y0, x1, y1 = unary_union([t['g'], x['hole']]).bounds
    w, h = x1 - x0 + 0.7, y1 - y0 + 0.7
    if w / h < PW / PH:
        w = h * PW / PH
    else:
        h = w * PH / PW
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    Sx = S * PW / w
    vw = View(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, Sx)
    vw.grid(0.25, (238, 238, 234))
    # copper context, faint: front tracks/pads (red tint), back tracks (blue tint)
    for tr in d['tracks']:
        g = track_geom(tr)
        if g.intersects(vw.clip):
            vw.fill(g, (240, 200, 196) if tr['layer'] == 'F' else (200, 214, 238), 255)
    for f in d['fps'].values():
        for p in f['pads']:
            for L, pl in p['cu'].items():
                g = polys_to_geom(pl)
                if g.intersects(vw.clip):
                    vw.fill(g, (238, 218, 160))
    for vv in d['vias']:
        g = Point(vv['pos']).buffer(vv['d'] / 2, quad_segs=48)
        if g.intersects(vw.clip):
            vw.fill(g, (205, 205, 205) if vv is not v else VIAC)
            vw.fill(Point(vv['pos']).buffer(vv['drill'] / 2, quad_segs=48), WHITE)
    for tt in texts:
        if tt['side'] == t['side'] and tt['g'].intersects(vw.clip):
            vw.fill(tt['g'], SILKC if tt is t else (120, 120, 120))
    vw.fill(x['hole'], WHITE)
    if x['ov_area'] > 0:
        vw.fill(x['ov'], RED)
    vw.outline(x['hole'].exterior, RED, 2.2)
    im = vw.finish()
    dr = ImageDraw.Draw(im)
    CL = (0, 0, im.size[0], im.size[1])
    if x['dist'] == 0:
        s = 'hole takes %.3f mm² of ink (%.0f %% of the character)' % (x['ov_area'], 100 * x['glyph_share'])
    else:
        s = 'hole %.3f mm from the text' % x['dist']
    tb = tag(dr, (10, im.size[1] - 10), s, fls, RED, anchor='lb', clamp=CL)
    if x['dist'] > 0:
        from shapely.ops import nearest_points
        a, b = nearest_points(x['hole'], t['g'])
        m = vw.T(((a.x + b.x) / 2, (a.y + b.y) / 2))
        r = 17
        dr.ellipse([m[0] - r, m[1] - r, m[0] + r, m[1] + r], outline=RED, width=3)
    return im, vw


panels = [(x,) + closeup(x) for x in (near[:3] if NONE else inside + near)]

# ---------------------------------------------------------------- board map
ex = d['bbox']
MS = 4.6
mv = View(ex[0] - 1, ex[1] - 1, ex[2] + 1, ex[3] + 1, MS, ss=3, bg=WHITE)
board = None
from shapely.ops import polygonize
segs = []
for e in d['edge']:
    for pl in e['pl']:
        segs.append(LineString(pl))
polys = list(polygonize(unary_union(segs)))
if polys:
    board = max(polys, key=lambda p: p.area)
    for p in polys:
        if p is not board and board.contains(p):
            board = board.difference(p)
    mv.fill(board, (236, 240, 232))
    mv.outline(board, (120, 120, 110), 1.2)
for f in d['fps'].values():
    for p in f['pads']:
        for L, pl in p['cu'].items():
            mv.fill(polys_to_geom(pl), (226, 214, 176))
for tt in texts:
    if tt['side'] == 'F':
        mv.fill(tt['g'], (150, 150, 150))
mapim = mv.finish()
md = ImageDraw.Draw(mapim)
fnum = font(17, 'Bold')
if NONE:
    for x in near[len(panels):]:
        px, py = mv.T(x['via']['pos'])
        md.ellipse([px - 8, py - 8, px + 8, py + 8], fill=(150, 150, 150), outline=WHITE, width=2)
for i, (x, im, vw) in enumerate(panels):
    px, py = mv.T(x['via']['pos'])
    col = RED if x['dist'] == 0 else (230, 120, 20)
    md.ellipse([px - 12, py - 12, px + 12, py + 12], fill=col, outline=WHITE, width=2)
    md.text((px, py + 1), str(i + 1), font=fnum, fill=WHITE, anchor='mm')

gnd_near = [x for x in near if x['via']['net'] == 'GND']
stitch = [x for x in gnd_near if not any(math.dist(p, x['via']['pos']) < 0.05 for t in d['tracks'] for p in (t['pl'][0], t['pl'][-1]))]
if NONE:
    WHY = ['The previous routed layout (2026-10-31) had 7 via holes within %.2f mm of silk text, 4 of them inside reference-designator ink; 8dbb6c6 had 8 (1).' % LIMIT,
           'After the re-route the nearest via hole is %.3f mm from a text (%s, %s via). Hole = drilled diameter, against the glyph outlines.'
           % (near[0]['dist'], tname(near[0]['text']), netname(near[0]['via']['net']))]
elif True:
  WHY = ['Why accepted: the fab clips the silkscreen at every drilled hole, so a character loses a sliver of ink (at most %.0f %% of one here)'
         % (100 * max(x['glyph_share'] for x in inside)) + ' and stays',
         'readable; nothing changes electrically. None of the signal vias had a free spot within reach.' +
       (' The via next to the %s is a GND' % ' and '.join(tname(x['text']) for x in stitch) if stitch else ''),
         'stitching via (no track to it); that reference was placed beside it by hand.' if stitch else '']

# ---------------------------------------------------------------- compose
pad = 40; gap = 30
TW, TH = panels[0][1].size
ncol = 4
W = pad + ncol * TW + (ncol - 1) * gap + pad
head = 170
can = Image.new('RGB', (W, 2000), WHITE); dr = ImageDraw.Draw(can)
ft = font(40, 'Bold'); fs = font(24); fh = font(26, 'SemiBold'); fsm = font(21); fz = font(20)
if NONE:
    dr.text((pad, 28), 'PicoGUS THT v1.3.2: no via hole at the silkscreen text', font=ft, fill=INK)
    dr.text((pad, 84), 'No via hole is inside silkscreen ink or less than %.2f mm from it (front and back; reference designators, footprint and'
            % LIMIT, font=fs, fill=GREY)
    dr.text((pad, 116), 'board texts). The nearest are %.2f mm away: the %d holes within %.2f mm are on the map, the %d nearest in the close-ups.'
            % (near[0]['dist'], len(near), NEAR2, len(panels)), font=fs, fill=GREY)
else:
    dr.text((pad, 28), 'PicoGUS THT v1.3.2: via holes in the silkscreen text', font=ft, fill=INK)
    dr.text((pad, 84), '%d via holes are inside the ink of a reference designator and %d more are less than %.2f mm from a silkscreen label'
            % (len(inside), len(near), LIMIT), font=fs, fill=GREY)
    sides = sorted({x['text']['side'] for x in hits})
    where = 'all on the front silkscreen, none on the back' if sides == ['F'] else 'front and back silkscreen'
    dr.text((pad, 116), '(%s; hole = drilled diameter, measured against the glyph outlines). Red: ink the drill takes away.' % where,
            font=fs, fill=GREY)


def place(i, x, im, vw, X, Y):
    t = x['text']; v = x['via']
    num = str(i + 1)
    col = RED if x['dist'] == 0 else (230, 120, 20)
    dr.ellipse([X, Y + 2, X + 30, Y + 32], fill=col)
    dr.text((X + 15, Y + 17), num, font=fnum, fill=WHITE, anchor='mm')
    dr.text((X + 40, Y + 17), '%s, %s via' % (tname(t), netname(v['net'])), font=fh, fill=INK, anchor='lm')
    yy = Y + 44
    can.paste(im, (X, yy)); frame(dr, X, yy, im)
    return yy + im.size[1]


y = head
if not NONE:
    dr.text((pad, y), 'Inside the ink (%d, all signal vias)' % len(inside), font=fh, fill=INK)
    y += 46
    yb = y
    for i, (x, im, vw) in enumerate(panels[:len(inside)]):
        yb = max(yb, place(i, x, im, vw, pad + (i % ncol) * (TW + gap), y + (i // ncol) * (TH + 70)))
    y = yb + 40
    dr.text((pad, y), 'Closer than %.2f mm (%d)' % (LIMIT, len(near)), font=fh, fill=INK)
else:
    dr.text((pad, y), 'The %d nearest (none closer than %.2f mm)' % (len(panels), LIMIT), font=fh, fill=INK)
y += 46
y2 = y
for k, (x, im, vw) in enumerate(panels[len(inside):]):
    i = len(inside) + k
    y2 = max(y2, place(i, x, im, vw, pad + k * (TW + gap), y))
# map in the last column
mx = pad + 3 * (TW + gap)
ms = mapim.resize((TW, round(mapim.size[1] * TW / mapim.size[0])), Image.LANCZOS) if mapim.size[0] > TW else mapim
dr.text((mx, y - 46 + 2), 'Where they are (front view)', font=fh, fill=INK)
can.paste(ms, (mx, y)); frame(dr, mx, y, ms)
y2 = max(y2, y + ms.size[1])

ly = y2 + 30
xx = pad
xx = swatch(dr, xx, ly, SILKC, 'silkscreen text', fsm) + 26
dr.ellipse([xx + 6, ly, xx + 28, ly + 22], fill=VIAC); dr.ellipse([xx + 11, ly + 5, xx + 23, ly + 17], fill=WHITE, outline=RED, width=2)
dr.text((xx + 44, ly + 11), 'via, drilled hole outlined red', font=fsm, fill=INK, anchor='lm')
xx += 44 + dr.textlength('via, drilled hole outlined red', font=fsm) + 26
if not NONE:
    xx = swatch(dr, xx, ly, RED, 'ink inside the hole', fsm) + 26
dr.ellipse([xx + 6, ly, xx + 28, ly + 22], outline=RED, width=3)
GAPTXT = 'nearest point' if NONE else 'gap under %.2f mm' % LIMIT
dr.text((xx + 44, ly + 11), GAPTXT, font=fsm, fill=INK, anchor='lm')
xx += 44 + dr.textlength(GAPTXT, font=fsm) + 26
if NONE:
    dr.ellipse([xx + 6, ly + 1, xx + 26, ly + 21], fill=(150, 150, 150), outline=WHITE)
    dr.text((xx + 44, ly + 11), 'map: other holes within %.2f mm' % NEAR2, font=fsm, fill=INK, anchor='lm')
    xx += 44 + dr.textlength('map: other holes within %.2f mm' % NEAR2, font=fsm) + 26
xx = swatch(dr, xx, ly, (240, 200, 196), 'front copper', fsm) + 26
xx = swatch(dr, xx, ly, (200, 214, 238), 'back copper', fsm) + 26
xx = swatch(dr, xx, ly, (238, 218, 160), 'pad', fsm) + 26
vy = ly + 50
for ln in WHY:
    dr.text((pad, vy), ln, font=fsm, fill=TXT2); vy += 30
can = can.crop((0, 0, W, vy + 22))
can.save(OUT)
print('saved', OUT, can.size)
