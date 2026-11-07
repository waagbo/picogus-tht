# Usage: python3 b10_ground_figure.py NOW.json OLD.json OUT.png
#   NOW.json  copper_dump.py output of the routed v1.3.2 board (PicoGUS-THT.kicad_pcb)
#   OLD.json  copper_dump.py output of the 103.7 mm layout (git show 8dbb6c6:hw-tht-v1.3.2/PicoGUS-THT.kicad_pcb)
# Draws b10-ground-finger.png: the GND tracks from the ISA ground fingers B1, B10 and B31 (J3.1/.10/.31) to the
# pours, their widths and lengths, the neighbours that keep B10's track narrow (with the clearance a 1.0 mm track
# would leave), what blocks a straight route up from B10, and B10 on 8dbb6c6 for comparison. Every number printed is
# measured here from the two dumps. (Rewritten for the re-routed board of 2026-11-01: B10's track is all on B.Cu.)
# Needs Pillow, numpy, shapely; figlib.py next to this script.
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from figlib import *
from shapely.ops import polygonize, substring

NOW, OLD, OUT = sys.argv[1:4]

def prep(path):
    b = Board(path)
    b.outline = unary_union(list(polygonize(unary_union(b.edges))))
    b.fing = {p['num']: p for p in b.d['pads'] if p['ref'] == 'J3' and 'B.Cu' in p['g']}
    g = next(x for x in b.d['fps']['J3']['graphics'] if x['layer'] == 'B.Mask')
    b.common_open = unary_union([Polygon(c) for c in g['geo'][1]]).buffer(g['w'] / 2, join_style=2)
    b.rule = unary_union([poly_of(r['outline']) for r in b.d['rules'] if r['no_fill'] and not r['name']])
    return b

def near(a, c, e=0.03):
    return math.dist(a, c) < e

def trace(b, num):
    """GND segments from finger J3.num to the first segment whose copper touches a GND pour (same layer)"""
    pad = b.fing[num]['g']['B.Cu']
    gt = [t for t in b.d['tracks'] if t['net'] == 'GND']
    gv = [v for v in b.d['vias'] if v['net'] == 'GND']
    start = [t for t in gt if t['layer'] == 'B.Cu' and (pad.buffer(0.01).contains(Point(t['a'])) or pad.buffer(0.01).contains(Point(t['b'])))]
    # orient: walk from the finger outwards
    path, seen, vias = [], set(), []
    cur = max(start, key=lambda t: t['w'])
    a, c = (cur['a'], cur['b']) if pad.contains(Point(cur['a'])) else (cur['b'], cur['a'])
    if pad.contains(Point(cur['a'])) and pad.contains(Point(cur['b'])):
        a, c = (cur['a'], cur['b']) if cur['a'][1] > cur['b'][1] else (cur['b'], cur['a'])
    layer = cur['layer']
    while True:
        path.append({'t': cur, 'a': a, 'b': c, 'layer': layer})
        seen.add(id(cur))
        fill = b.fill.get(('GND', layer))
        if fill is not None and cur['g'].intersects(fill):
            break
        nxt = [t for t in gt if id(t) not in seen and t['layer'] == layer and (near(t['a'], c) or near(t['b'], c))]
        if not nxt:
            v = next((v for v in gv if near(v['pos'], c, 0.05)), None)
            if v is None:
                break
            vias.append(v)
            layer = 'F.Cu' if layer == 'B.Cu' else 'B.Cu'
            nxt = [t for t in gt if id(t) not in seen and t['layer'] == layer and (near(t['a'], c, 0.05) or near(t['b'], c, 0.05))]
            if not nxt:
                break
        cur = nxt[0]
        a, c = (cur['a'], cur['b']) if near(cur['a'], c, 0.05) else (cur['b'], cur['a'])
    return path, vias

def touch_len(b, seg_list, w, layer):
    """length along a chain of points until a disc of diameter w first touches the GND pour on that layer"""
    F = b.fill[('GND', layer)]
    s = 0
    for a, c in seg_list:
        L = math.dist(a, c); n = max(2, int(L / 0.005))
        for i in range(n + 1):
            p = (a[0] + (c[0] - a[0]) * i / n, a[1] + (c[1] - a[1]) * i / n)
            if Point(p).buffer(w / 2, quad_segs=16).intersects(F):
                return s + L * i / n, p
        s += L
    return s, c

def runs(path):
    """group consecutive segments by (layer, width)"""
    out = []
    for p in path:
        k = (p['layer'], p['t']['w'])
        if out and out[-1]['k'] == k and near(out[-1]['pts'][-1], p['a'], 0.05):
            out[-1]['pts'].append(p['b']); out[-1]['segs'].append((p['a'], p['b']))
        else:
            out.append({'k': k, 'pts': [p['a'], p['b']], 'segs': [(p['a'], p['b'])]})
    for r in out:
        r['len'] = sum(math.dist(a, c) for a, c in r['segs'])
    return out

def neighbours(b, segs, w, layer, exclude=('GND',), lim=0.5):
    """min clearance from a chain (at width w) to every other-net item on the layer, as {label: (gap, pa, pb)}"""
    g = unary_union([LineString(s).buffer(w / 2, quad_segs=24) for s in segs])
    out = {}
    def put(key, geo):
        dd = g.distance(geo)
        if dd < lim and (key not in out or dd < out[key][0]):
            _, pa, pb = gap(g, geo)
            out[key] = (dd, pa, pb)
    for t in b.d['tracks']:
        if t['layer'] == layer and t['net'] not in exclude:
            put(('trk', t['net']), t['g'])
    for v in b.d['vias']:
        if v['net'] not in exclude:
            put(('via', v['net']), v['g'])
    for p in b.d['pads']:
        if layer in p['g'] and p['net'] not in exclude:
            put(('pad', p['ref'] + '.' + p['num'], p['net']), p['g'][layer])
    return out

now, old = prep(NOW), prep(OLD)
P = {n: trace(now, n) for n in ('1', '10', '31')}
P_old = {n: trace(old, n) for n in ('1', '10', '31')}
R = {n: runs(P[n][0]) for n in P}
R_old = {n: runs(P_old[n][0]) for n in P_old}
for tag_, RR in (('v1.3.2', R), ('8dbb6c6', R_old)):
    for n, rr in RR.items():
        print(tag_, 'B%s:' % n, ' -> '.join('%s %.1f mm x %.2f mm' % (r['k'][0], r['k'][1], r['len']) for r in rr))

# B10 (now): the runs from the finger to the pour, their neighbours, and what a 1.0 mm track on the same path would leave
r10 = R['10']
lay10 = sorted({r['k'][0] for r in r10})
in_finger = r10[0]['len'] if r10[0]['k'][1] == 1.0 else 0.0
narrow = [r for r in r10 if r['k'][1] < 1.0 - 1e-6]
NB = [(r, neighbours(now, r['segs'], r['k'][1], r['k'][0]), neighbours(now, r['segs'], 1.0, r['k'][0], lim=0.35)) for r in r10]
for r, nb, nb1 in NB:
    print('  B10 run %s %.1f mm x %.2f mm: nearest %s; at 1.0 mm: %s' % (r['k'][0], r['k'][1], r['len'],
          sorted((round(v[0], 3), k) for k, v in nb.items())[:3], sorted((round(v[0], 3), k) for k, v in nb1.items())[:3]))
last = r10[-1]
touch_d, touch_pt = touch_len(now, last['segs'], last['k'][1], last['k'][0])
total = sum(r['len'] for r in r10[:-1]) + touch_d
# the neighbour that the narrow runs pass closest to
close = min(((v[0], k, v) for r, nb, nb1 in NB if r['k'][1] < 1.0 for k, v in nb.items()), key=lambda x: x[0])
close1 = min((v[0] for r, nb, nb1 in NB if r['k'][1] < 1.0 for k, v in nb1.items() if k == close[1]), default=None)
# copper sticking out of B10 into the opening between the fingers
ex10 = [t for t in now.d['tracks'] if t['layer'] == 'B.Cu' and t['net'] == 'GND']
exg = unary_union([t['g'] for t in ex10]).difference(unary_union([p['g']['B.Cu'] for p in now.fing.values()]).buffer(0.002)).intersection(now.common_open)
exg = [q for q in polys(exg) if q.area > 2e-4]
# +12V next to B10 (its entry into B9)
p12 = unary_union([t['g'] for t in now.d['tracks'] if t['layer'] == 'B.Cu' and t['net'] == '+12V'])
g12 = gap(p12, now.fing['10']['g']['B.Cu'])
# straight up from the finger centre: what is in the way of a 1.0 mm track
f10 = now.fing['10']['g']['B.Cu'].bounds
xc = (f10[0] + f10[2]) / 2
y = f10[1]
while y > f10[1] - 12 and not Point(xc, y).buffer(0.5).intersects(now.fill[('GND', 'B.Cu')]):
    y -= 0.05
straight = LineString([(xc, f10[1] + 0.8), (xc, y)]).buffer(0.5)
block = sorted({('via ' if 'drill' in it else '') + it['net'] for it in [t for t in now.d['tracks'] if t['layer'] == 'B.Cu'] + now.d['vias']
                if it['net'] != 'GND' and it['g'].intersects(straight)})
print('B10 to the pour %.2f mm (%s); nearest to the narrow runs: %s %.3f (a 1.0 mm track: %s); +12V %.3f mm from B10; exposed %s; '
      'straight up from the finger centre (to y %.1f) blocked by %s' % (total, '+'.join(lay10), close[1], close[0], close1, g12[0],
                                                                       [round(q.area, 3) for q in exg], y, block))
r31 = R['31']; r1 = R['1']


GD = (20, 110, 65)       # dark green for GND labels
NB_C = (60, 62, 70)
def draw_layer(b, L, box_, S, path, extra_layer=None):
    p = Panel(*box_, S, SS=3)
    d = b.d
    p.fill(p.win.difference(b.outline), OUTSIDE)
    p.fill(b.common_open.intersection(b.outline), MASK_OPEN)
    if ('GND', L) in b.fill:
        p.fill(b.fill[('GND', L)], POUR)
    p.hatch(b.rule, (205, 205, 205), step=12, width=1.2)
    p.outline(b.rule, (150, 150, 150), width=1.5, dash=(9, 6))
    pid = {id(x['t']) for x in path}
    for t in d['tracks']:
        if t['layer'] != L or id(t) in pid:
            continue
        col = NET_COL.get(t['net'], SIG)
        if t['net'] == 'GND':
            col = (120, 185, 145)
        p.fill(t['g'], col)
    for v in d['vias']:
        p.fill(v['g'], NET_COL.get(v['net'], SIG) if v['net'] != 'GND' else (120, 185, 145))
    for q in d['pads']:
        if L in q['g']:
            p.fill(q['g'][L], PAD)
    for x in path:
        if x['layer'] == L:
            p.fill(x['t']['g'], NET_COL['GND'])
        elif extra_layer:
            p.fill(x['t']['g'], (150, 205, 170))
            p.outline(x['t']['g'], NET_COL['GND'], width=2, dash=(7, 5))
    p.holes([(v['pos'][0], v['pos'][1], v['drill']) for v in d['vias']], col=(255, 255, 255))
    p.holes([(q['pos'][0], q['pos'][1], q['hole'][0]) for q in d['pads'] if q['hole']], col=(255, 255, 255))
    p.outline(b.outline, (60, 60, 60), width=2)
    return p


def wlabel(a, pt, txt, dx, dy, col=GD, size=18):
    P0 = a.P(pt); X = (P0[0] + dx, P0[1] + dy)
    a.dr.line([P0, X], fill=col, width=2)
    a.dr.ellipse([P0[0] - 3, P0[1] - 3, P0[0] + 3, P0[1] + 3], fill=col)
    a.box_label(X, txt, size=size, col=col, w='Bold', anchor='mm')

def mid(segs):
    L = sum(math.dist(a, c) for a, c in segs); s = 0
    for a, c in segs:
        l = math.dist(a, c)
        if s + l >= L / 2:
            t = (L / 2 - s) / l
            return (a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t)
        s += l

def pick(nb, kind, net):
    return next(v for k, v in nb.items() if k[0] == kind and (k[-1] == net or k[1] == net))


# ---------------- layout
pad = 36; gp = 36; S = 66
H_MM = 10.4
BX = (167.4, 120.6, 177.4, 131.0)       # B10 now
ZX = (168.4, 126.4, 173.6, 130.75)       # B10 now, close-up of the narrow runs at B11
_at11 = any(k[0] == 'pad' and k[1] == 'J3.11' for r, nb, nb1 in NB if r['k'][1] < 1.0 for k in nb)
if not _at11:                            # the narrow run is elsewhere: centre the close-up on its nearest approach
    _c = close[2][1]; ZX = (_c[0] - 2.6, _c[1] - 2.175, _c[0] + 2.6, _c[1] + 2.175)
BO = (169.5, 120.6, 175.3, 131.0)        # B10 on 8dbb6c6
SZ = round((BX[3] - BX[1]) * S) / (ZX[3] - ZX[1])
B1X = (193.1, 123.6, 197.5, 131.0)
B31X = (116.3, 123.6, 121.4, 131.0)
wB = round((BX[2] - BX[0]) * S); hB = round((BX[3] - BX[1]) * S); wZ = round((ZX[2] - ZX[0]) * SZ); wO = round((BO[2] - BO[0]) * S)
w1 = round((B1X[2] - B1X[0]) * S); w31 = round((B31X[2] - B31X[0]) * S); h2 = round((B1X[3] - B1X[1]) * S)
W = pad + wB + gp + wZ + gp + wO + pad
head = 160
y1 = head + 40
cap_h = 150
y2 = y1 + hB + cap_h + 70
H = y2 + h2 + 300
can = Image.new('RGB', (W, H), (255, 255, 255))
dr = ImageDraw.Draw(can)
dr.text((pad, 26), 'PicoGUS THT v1.3.2: the GND tracks from the ground fingers B1, B10 and B31', font=font(40, 'Bold'), fill=INK)
dr.text((pad, 82), 'Each ISA ground finger has a track into a GND pour. No pour may come down to the fingers (no-fill strip), so each track crosses',
        font=font(24), fill=GREY)
bn = {}
for b_ in block:
    bn.setdefault(b_.replace('via ', ''), []).append('via' if b_.startswith('via ') else 'track')
btxt = ', '.join('%s%s' % (n_, ' (track and via)' if len(k_) > 1 else '') for n_, k_ in sorted(bn.items(), key=lambda kv: (-len(kv[1]), kv[0])))
dr.text((pad, 114), 'the strip. Straight above B10, %s are in the way, so B10\'s track goes round them.' % btxt,
        font=font(24), fill=GREY)


def tag(a, pt, txt, col, size=17):
    """net name on a white tag placed on its own track"""
    a.box_label(a.P(pt), txt, size=size, col=col, w='Bold', fill=(255, 255, 255))


def caption(x, y, lines, width):
    for i, (txt, bold, col) in enumerate(lines):
        dr.text((x, y + i * 27), txt, font=font(19, 'Bold' if bold else 'Medium'), fill=col)


def finger_tags(a, b, lay, names, yy=130.45):
    for num, txt in names:
        q = next(p for p in b.d['pads'] if p['ref'] == 'J3' and p['num'] == num)
        bb = q['g'][lay].bounds
        col = GD if q['net'] == 'GND' else NET_COL.get(q['net'], (90, 70, 20))
        if col == NET_COL.get('+12V'):
            col = (175, 85, 10)
        tag(a, ((bb[0] + bb[2]) / 2, yy), txt, col, size=17)


# --- row 1: B10 now (B.Cu), its narrow runs at B11, B10 on 8dbb6c6
xB_ = pad; xZ_ = xB_ + wB + gp; xO_ = xZ_ + wZ + gp
pB = draw_layer(now, 'B.Cu', BX, S, P['10'][0], extra_layer=True); can.paste(pB.done(), (xB_, y1)); frame(dr, xB_, y1, wB, hB)
heading(dr, (xB_, y1 - 8), 'B10, v1.3.2: solder side (B.Cu), front runs dashed', size=25)
aB = Annot(can, pB, xB_, y1)
pZ = draw_layer(now, 'B.Cu', ZX, SZ, P['10'][0]); can.paste(pZ.done(), (xZ_, y1)); frame(dr, xZ_, y1, wZ, hB)
heading(dr, (xZ_, y1 - 8), 'Close-up: round the top of B11' if _at11 else 'Close-up: the narrow run', size=25)
aZ = Annot(can, pZ, xZ_, y1)
pO = draw_layer(old, 'B.Cu', BO, S, P_old['10'][0], extra_layer=True); can.paste(pO.done(), (xO_, y1)); frame(dr, xO_, y1, wO, hB)
heading(dr, (xO_, y1 - 8), 'B10 on 8dbb6c6', size=25)
aO = Annot(can, pO, xO_, y1)

finger_tags(aB, now, 'B.Cu', (('11', 'B11'), ('10', 'B10 GND'), ('9', 'B9 +12V'), ('8', 'B8')))
OFF = {0: (-70, 30), 1: (-70, 22), 2: (-120, 4), 3: (-90, -40), 4: (-80, 0), 5: (-80, -36)}
for i, r in enumerate(r10):
    if i == 0:
        continue
    m = mid(r['segs'])
    dx, dy = OFF.get(i, (60, 0))
    wlabel(aB, m, '%.1f mm' % r['k'][1] if r['len'] < 2 else '%.1f mm × %.1f mm' % (r['k'][1], r['len']), dx, dy, col=GD if r['k'][1] >= 1.0 else RED)
TP = aB.P(touch_pt)
dr.ellipse([TP[0] - 13, TP[1] - 13, TP[0] + 13, TP[1] + 13], outline=GD, width=3)
X = (TP[0] + 60, TP[1] - 4)
dr.line([(TP[0] + 13, TP[1] - 2), (X[0] - 4, X[1])], fill=GD, width=2)
for i, t in enumerate(('touches the', 'back pour')):
    dr.text((X[0], X[1] + (i - 0.5) * 22), t, font=font(18, 'Bold'), fill=GD, anchor='lm', stroke_width=3, stroke_fill=(255, 255, 255))
for net, pt in (('+12V', (173.6, 128.6)), ('IRQ2', (175.6, 126.2)), ('+3V3', (174.6, 124.45))):
    tag(aB, pt, net, (175, 85, 10) if net == '+12V' else NET_COL.get(net, NB_C))
yc = y1 + hB + 14
_rt = ['%s %.1f × %.1f mm' % ('back' if r['k'][0] == 'B.Cu' else 'front', r['k'][1], r['len'] if r is not last else touch_d) for r in r10]
_nchg = sum(1 for a_, b_ in zip(r10, r10[1:]) if a_['k'][0] != b_['k'][0])
_cl = [' → '.join(_rt[i:i + 2]) + (' →' if i + 2 < len(_rt) else '') for i in range(0, len(_rt), 2)]
_tail = '%.1f mm from the finger to the pour, %s.' % (total, 'all on the solder side' if _nchg == 0 else '%d layer change%s' % (_nchg, 's' if _nchg > 1 else ''))
caption(xB_, yc, [(t_, False, SUB) for t_ in (['Width × length:'] + _cl + [_tail])], wB)

# close-up: the gaps to B11 and to +12V, the copper out of the finger
finger_tags(aZ, now, 'B.Cu', (('11', 'B11 (SMEMW)'), ('10', 'B10 GND'), ('9', 'B9 +12V')), yy=130.35)
for r, nb, nb1 in NB:
    if r['k'][1] < 1.0 and ('pad', 'J3.11', now.fing['11']['net']) in nb:
        v = nb[('pad', 'J3.11', now.fing['11']['net'])]
        aZ.measure(v[1], v[2], '%.2f' % v[0], at=('px', -40 if r['k'][1] < 0.5 else -62, -46 if r['k'][1] < 0.5 else 30), size=18)
aZ.measure(g12[1], g12[2], '%.2f' % g12[0], at=('px', 50, -36), size=18)
for q in exg:
    c = aZ.P((q.centroid.x, q.centroid.y))
    dr.ellipse([c[0] - 24, c[1] - 24, c[0] + 24, c[1] + 24], outline=RED, width=3)
_g11 = min(((v[0], r['k'][1]) for r, nb, nb1 in NB for k, v in nb.items() if k[0] == 'pad' and k[1] == 'J3.11'), default=(None, None))
_cn = close[1][1] if close[1][0] in ('trk', 'via') else close[1][1].replace('J3.', 'finger ')
_exa = sum(q.area for q in exg)
_l1 = [('The %.1f mm run passes %.2f mm from B11 (not connected on' % (_g11[1], _g11[0]), False, SUB),
       ('this card); the narrow runs pass %s at %.2f mm,' % (_cn, close[0]), False, SUB)] if _g11[0] is not None else \
      [('The narrow runs pass %s at %.2f mm,' % (_cn, close[0]), False, SUB)]
caption(xZ_, yc, _l1 + [
                  (('where a 1.0 mm track would overlap it.' if close1 == 0 else 'where a 1.0 mm track would leave %.2f mm.' % close1), True, RED),
                  ('+12V passes %.2f mm from B10. ' % g12[0] + ('The entry stays inside B10\'s outline.' if _exa < 0.005 else
                   'The entry sticks out of B10 (%.2f mm², red ring).' % _exa), False, SUB)], wZ)

# 8dbb6c6
ro = R_old['10']
for r, (dx, dy) in zip(ro, ((56, 34), (70, 0), (64, 0))):
    txt = '%.1f mm%s' % (r['k'][1], ' (F.Cu)' if r['k'][0] == 'F.Cu' else '')
    wlabel(aO, mid(r['segs']), txt, dx, dy, size=17)
finger_tags(aO, old, 'B.Cu', (('10', 'B10 GND'), ('9', 'B9')))
caption(xO_, yc, [('1.0 mm all the way: B.Cu, a', False, SUB), ('1.0 mm F.Cu jumper between', False, SUB),
                  ('two vias, B.Cu into the pour.', False, SUB)], wO)

# --- row 2: B1 and B31 now, and the summary
x = pad
p1 = draw_layer(now, 'B.Cu', B1X, S, P['1'][0]); can.paste(p1.done(), (x, y2)); frame(dr, x, y2, w1, h2)
heading(dr, (x, y2 - 8), 'B1, v1.3.2 (B.Cu)', size=25)
a1 = Annot(can, p1, x, y2)
f1 = now.fing['1']['g']['B.Cu'].bounds
a1.text(((f1[0] + f1[2]) / 2, 130.4), 'B1 GND', size=19, col=GD, w='Bold')
wlabel(a1, mid(r1[0]['segs']), '%.1f mm' % r1[0]['k'][1], -70, -40)
x += w1 + gp
p31 = draw_layer(now, 'B.Cu', B31X, S, P['31'][0]); can.paste(p31.done(), (x, y2)); frame(dr, x, y2, w31, h2)
heading(dr, (x, y2 - 8), 'B31, v1.3.2 (B.Cu)', size=25)
a31 = Annot(can, p31, x, y2)
f31 = now.fing['31']['g']['B.Cu'].bounds
a31.text(((f31[0] + f31[2]) / 2, 130.4), 'B31 GND', size=19, col=GD, w='Bold')
for i, r in enumerate(r31):
    wlabel(a31, mid(r['segs']), '%.1f mm' % r['k'][1], 70, 26 - 36 * i)

# summary table
xs = x + w31 + 60
ty = y2 + 4
dr.text((xs, ty), 'GND track from each finger to the pour', font=font(25, 'SemiBold'), fill=INK); ty += 46
cols = (xs, xs + 120, xs + 640)
f = font(20); fb = font(20, 'Bold')
dr.text((cols[0], ty), 'finger', font=fb, fill=GREY); dr.text((cols[1], ty), 'v1.3.2', font=fb, fill=GREY)
dr.text((cols[2], ty), '8dbb6c6', font=fb, fill=GREY); ty += 34
dr.line([xs, ty - 6, W - pad, ty - 6], fill=FRAME, width=1)


def desc(n):
    rr = R[n]
    lay = ' + '.join(sorted({r['k'][0] for r in rr}))
    if all(r['k'][1] == 1.0 for r in rr):
        return ['1.0 mm all the way (%s)' % lay]
    nchg = sum(1 for a_, b_ in zip(rr, rr[1:]) if a_['k'][0] != b_['k'][0])
    return ['%s mm (%s),' % (' / '.join('%.1f' % r['k'][1] for r in rr), lay),
            '%d layer change%s' % (nchg, '' if nchg == 1 else 's'),
            'over %.1f mm to the pour; narrowest %.1f mm' % (total if n == '10' else sum(r['len'] for r in rr), min(r['k'][1] for r in rr))]


def desc_old(n):
    ws = sorted({r['k'][1] for r in R_old[n][:3]})
    lay = ' + '.join(sorted({r['k'][0] for r in R_old[n]}))
    return ['%s mm to the pour (%s)' % ('/'.join('%.1f' % w for w in ws), lay)]


for n in ('1', '10', '31'):
    a_ = desc(n); b_ = desc_old(n)
    dr.text((cols[0], ty), 'B%s' % n, font=fb, fill=GD)
    for i, ln in enumerate(a_):
        dr.text((cols[1], ty + i * 28), ln, font=f, fill=INK)
    for i, ln in enumerate(b_):
        dr.text((cols[2], ty + i * 28), ln, font=f, fill=INK)
    ty += 28 * max(len(a_), len(b_)) + 16
    dr.line([xs, ty - 8, W - pad, ty - 8], fill=FRAME, width=1)

# legend
ly = y2 + h2 + 30
x = pad; fs = font(21)
x = swatch(dr, x, ly, 'GND path finger → pour', fill=NET_COL['GND'], kind='line', fs=fs)
x = swatch(dr, x, ly, 'path on the other layer', fill=(150, 205, 170), outline=NET_COL['GND'], fs=fs)
x = swatch(dr, x, ly, 'GND pour', fill=POUR, fs=fs)
x = swatch(dr, x, ly, 'no-fill strip', fill=(246, 246, 246), outline=(150, 150, 150), fs=fs, hatch=(190, 190, 190))
x = swatch(dr, x, ly, 'pad / finger', fill=PAD, fs=fs)
x = swatch(dr, x, ly, 'opening between the fingers', fill=MASK_OPEN, fs=fs)
ly2 = ly + 36; x = pad
for net in ('+12V', '+5V', '+3V3'):
    x = swatch(dr, x, ly2, net, fill=NET_COL[net], kind='line', fs=fs) - 10
x = swatch(dr, x, ly2, 'other nets', fill=SIG, kind='line', fs=fs)
dr.rounded_rectangle([x, ly2 - 1, x + 56, ly2 + 23], radius=4, fill=(255, 255, 255), outline=RED, width=1)
dr.text((x + 28, ly2 + 11), '0.21', font=font(16, 'Bold'), fill=RED, anchor='mm')
dr.text((x + 66, ly2 + 11), 'clearance in mm', font=fs, fill=INK, anchor='lm')
x += 66 + dr.textlength('clearance in mm', font=fs) + 36
dr.ellipse([x, ly2, x + 22, ly2 + 22], outline=RED, width=3)
dr.text((x + 32, ly2 + 11), 'copper out of the finger', font=fs, fill=INK, anchor='lm')

vy = ly2 + 54; fz = font(21)
lines = (
    'B10\'s track is %.1f mm at its narrowest; it carries a share of the card\'s ground return with B1 (%s) and B31 (%s).'
    % (min(r['k'][1] for r in narrow), desc('1')[0].split(' (')[0], desc('31')[0].split(' (')[0]),
    '8dbb6c6 had 1.0 mm from all three, B10 with an F.Cu jumper between two vias. The +12V entry into B9 and the B10 copper in the opening are in the',
    'gold-finger figure as well.',
)
for ln in lines:
    dr.text((pad, vy), ln, font=fz, fill=SUB); vy += 30
    if dr.textlength(ln, font=fz) > W - 2 * pad: print('WARNING: footer line too long:', ln)
can = can.crop((0, 0, W, vy + 22))
can.save(OUT)
print(can.size)
