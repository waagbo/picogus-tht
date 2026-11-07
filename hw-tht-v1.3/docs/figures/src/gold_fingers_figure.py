# Usage: python3 gold_fingers_figure.py NOW.json OLD.json OUT.png [OLD_NAME]
#   NOW.json    copper_dump.py output of the routed v1.3.2 board (PicoGUS-THT.kicad_pcb)
#   OLD.json    copper_dump.py output of the layout to compare with, by default the 103.7 mm layout
#               (git show 8dbb6c6:hw-tht-v1.3.2/PicoGUS-THT.kicad_{pcb,pro})
#   OLD_NAME    its name in the figure (default 8dbb6c6)
# Draws gold-fingers.png: the solder side (B.Cu) above J3's fingers, the per-finger mask openings, the opening
# between the fingers, the measured gaps from +12V / -12V / +5V copper to the finger copper and copper that sticks
# out of a finger into the opening between the fingers, on the old layout and now.
# Every number printed is measured here from the two dumps (the 0.30 mm rule is the README's).
# Needs Pillow, numpy, shapely; figlib.py next to this script.
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from figlib import *
from shapely.ops import polygonize

NOW, BEF, OUT = sys.argv[1:4]
OLDN = sys.argv[4] if len(sys.argv) > 4 else '8dbb6c6'
POWER = ('+12V', '-12V', '+5V', '+3V3', '/VSYS')
RULE = 0.30   # README: keep power copper at least 0.30 mm from the finger copper

def prep(path):
    b = Board(path)
    d = b.d
    b.fing = {p['num']: p for p in d['pads'] if p['ref'] == 'J3' and 'B.Cu' in p['g']}
    b.fingers = unary_union([p['g']['B.Cu'] for p in b.fing.values()])
    g = next(x for x in d['fps']['J3']['graphics'] if x['layer'] == 'B.Mask')
    b.common_open = unary_union([Polygon(c) for c in g['geo'][1]]).buffer(g['w'] / 2, join_style=2)
    b.outline = unary_union(list(polygonize(unary_union(b.edges))))
    b.finger_top = min(p['g']['B.Cu'].bounds[1] for p in b.fing.values())
    b.open_top = b.common_open.bounds[1]
    # power copper (B.Cu) to every finger of another net
    b.gaps = {}
    for net in POWER:
        cu = unary_union([t['g'] for t in d['tracks'] if t['layer'] == 'B.Cu' and t['net'] == net] +
                         [v['g'] for v in d['vias'] if v['net'] == net])
        if cu.is_empty:
            continue
        for n, p in b.fing.items():
            if p['net'] == net:
                continue
            dd, a, c = gap(cu, p['g']['B.Cu'])
            if dd < 0.6:
                b.gaps[(net, n)] = (dd, a, c)
    # track/via copper outside the fingers but inside the common mask opening (exposed)
    other = unary_union([t['g'] for t in d['tracks'] if t['layer'] == 'B.Cu'] + [v['g'] for v in d['vias']])
    ex = other.difference(b.fingers.buffer(0.002)).intersection(b.common_open)
    b.exposed = [q for q in polys(ex) if q.area > 2e-4]
    b.min_power = min(v[0] for v in b.gaps.values())
    return b

now, bef = prep(NOW), prep(BEF)
for name, b in (('before', bef), ('now', now)):
    print(name, 'finger top y %.3f, common opening from y %.3f' % (b.finger_top, b.open_top))
    for k, v in sorted(b.gaps.items(), key=lambda kv: kv[1][0]):
        print('   %-5s -> J3.%-2s (%s) %.3f mm' % (k[0], k[1], b.fing[k[1]]['net'], v[0]))
    print('   exposed slivers:', [(round(q.area, 4), [round(c, 2) for c in q.bounds]) for q in b.exposed])

def draw_bcu(b, x0, y0, x1, y1, S):
    p = Panel(x0, y0, x1, y1, S, SS=3)
    d = b.d
    win = p.win
    p.fill(win.difference(b.outline), OUTSIDE)
    p.fill(b.common_open.intersection(b.outline).difference(b.fingers), MASK_OPEN)
    if ('GND', 'B.Cu') in b.fill:
        p.fill(b.fill[('GND', 'B.Cu')], POUR)
    for r in d['rules']:
        if r['no_fill'] and 'B.Cu' in r['layers'] and not r['name']:
            g = poly_of(r['outline'])
            p.hatch(g, (205, 205, 205), step=12, width=1.2)
            p.outline(g, (150, 150, 150), width=1.5, dash=(9, 6))
    for t in d['tracks']:
        if t['layer'] != 'B.Cu':
            continue
        col = NET_COL.get(t['net'], SIG)
        p.fill(t['g'], col)
    for v in d['vias']:
        p.fill(v['g'], NET_COL.get(v['net'], SIG))
    for q in d['pads']:
        if 'B.Cu' in q['g']:
            p.fill(q['g']['B.Cu'], PAD)
    p.holes([(v['pos'][0], v['pos'][1], v['drill']) for v in d['vias']], col=(255, 255, 255))
    p.holes([(q['pos'][0], q['pos'][1], q['hole'][0]) for q in d['pads'] if q['hole']], col=(255, 255, 255))
    # outline of the common mask opening
    p.outline(b.common_open.intersection(b.outline).difference(b.fingers.buffer(0.001)), (190, 160, 100), width=1.2)
    p.outline(b.outline, (60, 60, 60), width=2)
    return p

def entry_points(b, box_):
    """for every finger in the window: the widest same-net B.Cu segment touching it, as (finger, net, width, its point just above the finger top)"""
    out = []
    for n, q in b.fing.items():
        bb = q['g']['B.Cu'].bounds
        if not (box_[0] < (bb[0] + bb[2]) / 2 < box_[2]):
            continue
        ts = [t for t in b.d['tracks'] if t['layer'] == 'B.Cu' and t['net'] == q['net'] and t['g'].intersects(q['g']['B.Cu'])]
        if not ts:
            continue
        wmax = max(t['w'] for t in ts)
        t = min((t for t in ts if t['w'] == wmax), key=lambda t: min(t['a'][1], t['b'][1]))
        (xa, ya), (xb, yb) = t['a'], t['b']
        yl = bb[1] - 0.15                      # just above the finger top
        if min(ya, yb) < yl < max(ya, yb):
            top = (xa + (xb - xa) * (yl - ya) / (yb - ya), yl)
        else:
            top = min((t['a'], t['b']), key=lambda p: p[1])
        out.append((n, q['net'], wmax, top, ((bb[0] + bb[2]) / 2, bb[1])))
    return out

# ---------------- layout
pad = 36; gap_ = 40; lg = 118          # lg: left gutter of the zoom rows (mask-band labels)
SA = 78
A = (168.3, 124.7, 184.5, 131.3)    # +-12V at B5..B9, GND at B10, B11
B = (117.0, 124.7, 127.9, 131.3)    # +5V at B28..B31
wA = round((A[2] - A[0]) * SA); wB = round((B[2] - B[0]) * SA); hZ = round((A[3] - A[1]) * SA)
W = lg + wA + gap_ + wB + pad
OV = (116.0, 121.3, 197.8, 136.4)
SO = (W - lg - pad) / (OV[2] - OV[0])
hO = round((OV[3] - OV[1]) * SO)

head = 156
y_ov = head + 40
y_r1 = y_ov + hO + 76
y_r2 = y_r1 + hZ + 76
H = y_r2 + hZ + 260
can = Image.new('RGB', (W, H), (255, 255, 255))
dr = ImageDraw.Draw(can)
dr.text((pad, 26), 'PicoGUS THT v1.3.2: power copper next to the gold fingers', font=font(40, 'Bold'), fill=INK)
dr.text((pad, 82), 'Solder side (B.Cu) above J3\'s B-row fingers, seen through the board from the component side (not mirrored), on %s and on v1.3.2.' % OLDN,
        font=font(24), fill=GREY)
dr.text((pad, 114), 'Each finger has its own mask opening the size of the finger, so a track beside a finger top is separated from it only by solder mask.',
        font=font(24), fill=GREY)

# overview
xo = lg
po = draw_bcu(now, *OV, SO)
im = po.done(); can.paste(im, (xo, y_ov)); frame(dr, xo, y_ov, im.size[0], im.size[1])
heading(dr, (xo, y_ov - 8), 'Overview: J3 fingers B1-B31 and the strip above them (v1.3.2)')
ao = Annot(can, po, xo, y_ov)
for n, q in now.fing.items():
    cx = (q['g']['B.Cu'].bounds[0] + q['g']['B.Cu'].bounds[2]) / 2
    ao.text((cx, 133.3), 'B%s' % n, size=15, col=(70, 55, 15), w='Bold', halo=False)
for n in ('3', '7', '9', '10', '29', '31', '1'):
    q = now.fing[n]; cx = (q['g']['B.Cu'].bounds[0] + q['g']['B.Cu'].bounds[2]) / 2
    ao.text((cx, 134.9), q['net'].replace('-12V', '−12V'), size=14, col=NET_COL[q['net']], w='Bold', halo=True)
for (x0, y0, x1, y1), lab in ((A, 'zoom 1'), (B, 'zoom 2')):
    P0 = ao.P((x0, y0)); P1 = ao.P((x1, y1))
    dr.rectangle([P0[0], P0[1], P1[0], P1[1]], outline=INK, width=2)
    dr.text((P0[0] + 5, P0[1] + 3), lab, font=font(17, 'Bold'), fill=INK, anchor='lt', stroke_width=3, stroke_fill=(255, 255, 255))
ao.text((147.5, 124.0), 'no-fill strip (rule area, both layers): no pour above the fingers', size=17, col=(70, 70, 70), w='SemiBold')
dr.text((xo - 10, ao.P((OV[0], 132.0))[1]), 'J3', font=font(24, 'Bold'), fill=INK, anchor='rm')
dr.text((xo - 10, ao.P((OV[0], 132.0))[1] + 26), 'B side', font=font(17), fill=GREY, anchor='rm')

FIN_NAME = {'unconnected-(J3-UNUSED-Pad8)': '(0WS)', 'unconnected-(J3-DRQ2-Pad6)': '(DRQ2)',
            'unconnected-(J3-OSC-Pad30)': '(OSC)', 'unconnected-(J3--5V-Pad5)': '(−5V)',
            'unconnected-(J3-~{SMEMW}-Pad11)': '(SMEMW)', 'unconnected-(J3-ALE-Pad28)': '(ALE)'}

def zoom(b, box_, x, y, title, before):
    p = draw_bcu(b, *box_, SA)
    im = p.done(); can.paste(im, (x, y)); frame(dr, x, y, im.size[0], im.size[1])
    heading(dr, (x, y - 8), title, size=24)
    a = Annot(can, p, x, y)
    # finger names on white tags
    for n, q in b.fing.items():
        bb = q['g']['B.Cu'].bounds
        cx = (bb[0] + bb[2]) / 2
        if box_[0] < cx < box_[2] - 0.5:
            net = q['net']
            nm = 'B%s %s' % (n, FIN_NAME.get(net, net.replace('-12V', '−12V')))
            col = NET_COL.get(net, (90, 70, 20))
            a.box_label(a.P((cx, 130.7)), nm, size=17, col=col, w='Bold', fill=(255, 255, 255))
    # width of the track entering each finger, written on the finger
    for n, net, w, top, (fx, fy) in entry_points(b, box_):
        col = tuple(int(c * 0.72) for c in NET_COL.get(net, SIG))
        P = a.P(top); X = a.P((fx, fy + 1.05))
        dr.line([P, (X[0], X[1] - 12)], fill=col, width=2)
        dr.ellipse([P[0] - 3, P[1] - 3, P[0] + 3, P[1] + 3], fill=col)
        a.box_label(X, '%.1f mm' % w, size=17, col=col, w='Bold', fill=(255, 255, 255))
    for (net, n), (dd, pa, pb) in b.gaps.items():
        if not (box_[0] < pa[0] < box_[2]) or dd > 0.45:
            continue
        col = RED if dd < RULE else (20, 120, 60)
        a.measure(pa, pb, '%.2f mm' % dd, at=LABEL_OFF.get((net, n), ('px', 0, -26)), col=col, size=18)
    for q in b.exposed:
        c = q.centroid; C = a.P((c.x, c.y))
        if box_[0] < c.x < box_[2]:
            dr.ellipse([C[0] - 17, C[1] - 17, C[0] + 17, C[1] + 17], outline=RED, width=3)
    return a

def gutter(a, x, y):
    """bracket in the left gutter: the mask-only band between the finger tops and the common opening"""
    Y0 = a.P((A[0], now.finger_top))[1]; Y1 = a.P((A[0], now.open_top))[1]
    X = x - 10
    col = (120, 95, 40)
    for Y in (Y0, Y1):
        dr.line([(X - 6, Y), (x + 12, Y)], fill=col, width=2)
    dr.line([(X, Y0), (X, Y1)], fill=col, width=2)
    f = font(16, 'SemiBold')
    dr.text((X - 2, Y0 - 5), 'y %.2f' % now.finger_top, font=f, fill=col, anchor='rb')
    dr.text((X - 2, Y1 + 5), 'y %.2f' % now.open_top, font=f, fill=col, anchor='rt')
    dr.text((X - 10, (Y0 + Y1) / 2 - 1), 'mask', font=font(17, 'Bold'), fill=col, anchor='rb')
    dr.text((X - 10, (Y0 + Y1) / 2 + 1), 'only', font=font(17, 'Bold'), fill=col, anchor='rt')

# label offsets (px from the middle of the gap) for the gap boxes; width-label offsets per (before, net)
LABEL_OFF = {('+12V', '8'): ('px', -76, -16), ('+12V', '7'): ('px', 46, -78), ('-12V', '6'): ('px', 66, -4),
             ('+5V', '30'): ('px', -66, -4), ('+12V', '10'): ('px', -10, -70), ('+5V', '28'): ('px', 60, -40)}
xA, xB = lg, lg + wA + gap_
a1 = zoom(bef, A, xA, y_r1, 'Zoom 1, %s: ±12V and GND at B5-B11' % OLDN, True)
zoom(bef, B, xB, y_r1, 'Zoom 2, %s: +5V at B28-B31' % OLDN, True)
a2 = zoom(now, A, xA, y_r2, 'Zoom 1, v1.3.2: ±12V and GND at B5-B11', False)
zoom(now, B, xB, y_r2, 'Zoom 2, v1.3.2: +5V at B28-B31', False)
gutter(a1, xA, y_r1); gutter(a2, xA, y_r2)

# legend
ly = y_r2 + hZ + 26
x = pad
fs = font(21)
x = swatch(dr, x, ly, 'gold finger = its own mask opening', fill=PAD, fs=fs)
x = swatch(dr, x, ly, 'opening between the fingers (bare laminate)', fill=MASK_OPEN, outline=(190, 160, 100), fs=fs)
x = swatch(dr, x, ly, 'GND pour', fill=POUR, fs=fs)
x = swatch(dr, x, ly, 'no-fill strip', fill=(246, 246, 246), outline=(150, 150, 150), fs=fs, hatch=(190, 190, 190))
dr.rounded_rectangle([x, ly - 1, x + 66, ly + 23], radius=4, fill=(255, 255, 255), outline=(110, 110, 110), width=1)
dr.text((x + 33, ly + 11), '0.6 mm', font=font(16, 'Bold'), fill=(110, 110, 110), anchor='mm')
dr.text((x + 76, ly + 11), 'width of the track entering the finger', font=fs, fill=INK, anchor='lm')
ly2 = ly + 36; x = pad
for net in ('+12V', '-12V', '+5V', '+3V3', 'GND'):
    x = swatch(dr, x, ly2, net.replace('-12V', '−12V'), fill=NET_COL[net], kind='line', fs=fs) - 10
x = swatch(dr, x, ly2, 'other nets', fill=SIG, kind='line', fs=fs)
for txt, col, lab in (('0.22', RED, 'below 0.30 mm'), ('0.36', (20, 120, 60), '0.30 mm or more')):
    dr.rounded_rectangle([x, ly2 - 1, x + 60, ly2 + 23], radius=4, fill=(255, 255, 255), outline=col, width=1)
    dr.text((x + 30, ly2 + 11), txt, font=font(16, 'Bold'), fill=col, anchor='mm')
    dr.text((x + 70, ly2 + 11), lab, font=fs, fill=INK, anchor='lm'); x += 70 + dr.textlength(lab, font=fs) + 30
dr.ellipse([x, ly2, x + 22, ly2 + 22], outline=RED, width=3)
dr.text((x + 32, ly2 + 11), 'copper sticking out of a finger into the opening', font=fs, fill=INK, anchor='lm')

fz = font(21); vy = ly2 + 52
def gl(b):
    """the power-to-finger gaps under 0.45 mm, as text"""
    xs_ = sorted(b.gaps.items(), key=lambda kv: kv[1][0])
    return ', '.join('%s %.2f mm from B%s%s' % (net.replace('-12V', '−12V'), v[0], n,
                     ' ' + FIN_NAME.get(b.fing[n]['net'], '(%s)' % b.fing[n]['net'])) for (net, n), v in xs_ if v[0] < 0.45) or 'none'
def ex(b):
    out = []
    for q in b.exposed:
        c = q.centroid
        f = sorted(b.fing.items(), key=lambda kv: kv[1]['g']['B.Cu'].distance(q))[:2]
        nets = {t['net'] for t in b.d['tracks'] if t['layer'] == 'B.Cu' and t['g'].intersects(q)}
        if q.area >= 0.01:
            out.append('%.2f mm² of %s copper between B%s and B%s' % (q.area, '/'.join(sorted(nets)), f[0][0], f[1][0]))
    return out
nb = sum(1 for v in now.gaps.values() if v[0] < RULE); ob = sum(1 for v in bef.gaps.values() if v[0] < RULE)
lines = (
    'Why: between a finger top (y %.2f) and the opening between the fingers (y %.2f) only solder mask separates a track from the bare finger next to it.' % (now.finger_top, now.open_top),
    'Rule: power copper at least 0.30 mm from finger copper. Power copper within 0.45 mm of another net\'s finger on %s: %s;' % (OLDN, gl(bef)),
    'on v1.3.2: %s (%d below 0.30 mm on v1.3.2, %d on %s).' % (gl(now), nb, ob, OLDN),
    ('Copper sticking out of a finger into the opening (bare): ' + ('; '.join(ex(now)) or 'none') + ' on v1.3.2' + (', none on %s.' % OLDN if not ex(bef) else ', %s on %s.' % ('; '.join(ex(bef)), OLDN))),
)
for ln in lines:
    dr.text((pad, vy), ln, font=fz, fill=SUB); vy += 30
    if dr.textlength(ln, font=fz) > W - 2 * pad: print('WARNING: footer line too long:', ln)
can = can.crop((0, 0, W, vy + 22))
can.save(OUT)
print(can.size)
