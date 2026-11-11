# Usage: kipython bracket_trim_figure.py NEW.json OLD.json OLD131.json OUT.png
#   NEW.json     dump of the 102 mm board (v1.3.2)
#   OLD.json     dump of the routed 103.74 mm layout (git show 8dbb6c6:hw-tht-v1.3.2/PicoGUS-THT.kicad_pcb)
#   OLD131.json  dump of git show 9e6007f:hw-tht-v1.3.1/PicoGUS-THT.kicad_pcb (same outline as 8dbb6c6; only the Pico's own move differs)
#   each from 'kipython misc_dump.py BOARD.kicad_pcb X.json --no-zones'
# Needs misc_figlib.py next to this script.
#
# Draws bracket-trim.png: the bracket end of the old (dashed) and new (solid) Edge.Cuts outline, the strip that was
# trimmed, the bracket drawing from User.Drawings (plate and tabs, old tabs dashed), the bracket tab holes H1/H2 and
# the bracket-side parts (J8, RV1, J5, J11, the Pico U1's USB end) old and new, with every move and each part's
# nearest copper to the new edge measured from the dumps. Views are turned 90 degrees: the bracket end is at the top.
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from misc_figlib import *
from shapely import affinity
from shapely.ops import polygonize
from shapely.geometry import MultiPoint

new = load(sys.argv[1]); old = load(sys.argv[2]); o131 = load(sys.argv[3]); OUT = sys.argv[4]


def rot(g):
    """board (x, y) -> view (y, -x): bracket end (large x) at the top"""
    return affinity.affine_transform(g, [0, 1, -1, 0, 0, 0])


def R(p):
    return (p[1], -p[0])


def outline(d, minw=0.09):
    segs = [LineString(pl) for e in d['edge'] if e['w'] >= minw for pl in e['pl']]
    return max(polygonize(unary_union(segs)), key=lambda p: p.area)


def slots(d):
    """the thin Edge.Cuts cut-outs (the Pico's micro-USB relief slots): convex stadiums, so the hull of each cluster"""
    segs = [LineString(pl) for e in d['edge'] if e['w'] < 0.09 for pl in e['pl']]
    if not segs:
        return []
    groups = unary_union([g.buffer(0.05) for g in segs])
    return [MultiPoint([p for g in segs if g.intersects(grp) for p in g.coords]).convex_hull for grp in getattr(groups, 'geoms', [groups])]


def main_lw(d):
    return max(e['w'] for e in d['edge'])


def copper(d, ref):
    gs = []
    for p in d['fps'][ref]['pads']:
        for L, pl in p['cu'].items():
            gs.append((p['num'], polys_to_geom(pl)))
    return gs


def fab(d, ref):
    return [LineString(pl) for g in d['fps'][ref]['fab'] if g['side'] == 'F' for pl in g['pl'] if len(pl) > 1]


# ---------------------------------------------------------------- measure
BN, BO = outline(new), outline(old)
EN, EO = BN.bounds[2], BO.bounds[2]
LW = main_lw(new)
WN, WO = BN.bounds[2] - BN.bounds[0], BO.bounds[2] - BO.bounds[0]
HN = BN.bounds[3] - BN.bounds[1]
assert abs(outline(o131).bounds[2] - EO) < 1e-6, 'v1.3.1 and 8dbb6c6 outlines differ'
print('outline x %.2f -> %.2f: trimmed %.2f mm; %.2f x %.2f mm at line centres (was %.2f), %.2f x %.2f over the %.2f mm lines (was %.2f)'
      % (EO, EN, EO - EN, WN, HN, WO, WN + LW, HN + LW, LW, WO + LW))


def edge_dist(d, ref, E):
    best = None
    for num, g in copper(d, ref):
        dd = E - g.bounds[2]
        if best is None or dd < best[0]:
            best = (dd, num)
    return best


PARTS = ['H1', 'H2', 'J8', 'J5', 'U1', 'RV1', 'J11']
JACKS = ('J5', 'J8')
# the two 3.5 mm jacks swapped places on v1.3.2 (J5 MIDI out now above RV1, J8 line out below H1): each jack is
# compared with the jack that sat in the same place on the old boards (its "slot"), not with its own old position
SLOT = {r: r for r in PARTS}
for r in JACKS:
    SLOT[r] = min(JACKS, key=lambda o: abs(old['fps'][o]['pos'][1] - new['fps'][r]['pos'][1]))
SWAPPED = any(SLOT[r] != r for r in JACKS)
M = {}
for r in PARTS:
    o = SLOT[r]
    xn, xo, x1 = new['fps'][r]['pos'][0], old['fps'][o]['pos'][0], o131['fps'][o]['pos'][0]
    dn = edge_dist(new, r, EN)
    fx = [p[0] for g in new['fps'][r]['fab'] if g['side'] == 'F' for pl in g['pl'] for p in pl]
    fxo = [p[0] for g in old['fps'][o]['fab'] if g['side'] == 'F' for pl in g['pl'] for p in pl]
    M[r] = {'xn': xn, 'xo': xo, 'x131': x1, 'mv': xo - xn, 'mv131': x1 - xn, 'cu': dn[0], 'cu_pad': dn[1],
            'front': max(fx) if fx else None, 'front_old': max(fxo) if fxo else None, 'y': new['fps'][r]['pos'][1],
            'yo': old['fps'][o]['pos'][1], 'slot': o}
    print('%-4s x %.3f -> %.3f (moved %.2f; vs v1.3.1 %.2f)  nearest copper to the new edge %.2f mm (pad %s)  body front x %s%s'
          % (r, xo, xn, xo - xn, x1 - xn, dn[0], dn[1], '%.2f' % max(fx) if fx else '-',
             '  [in the place of %s on 8dbb6c6: y %.2f -> %.2f]' % (o, old['fps'][o]['pos'][1], new['fps'][r]['pos'][1]) if o != r else ''))

# bracket drawing: plate faces (long vertical lines) and tab ends
vert = sorted({round(pl[0][0], 3) for e in new['drawings'] for pl in e['pl']
               if len(pl) == 2 and abs(pl[0][0] - pl[1][0]) < 1e-3 and abs(pl[0][1] - pl[1][1]) > 5 and pl[0][0] > EN})
PIN, POUT = vert[0], vert[-1]


def tab_end(d, ycen):
    xs = [p[0] for e in d['drawings'] for pl in e['pl'] for p in pl if abs(p[1] - ycen) < 3.5 and p[0] < PIN]
    return min(xs)


TN, TO = tab_end(new, M['H1']['y']), tab_end(old, M['H1']['y'])
print('plate %.3f..%.3f (%.2f mm); H1/H2 centre %.2f mm from the plate inner face (was %.2f); tab end x %.2f (was %.2f): %.2f mm longer'
      % (PIN, POUT, POUT - PIN, PIN - M['H1']['xn'], PIN - M['H1']['xo'], TN, TO, TO - TN))
SN, SO = slots(new), slots(old)
slot_gap = EN - max(s.bounds[2] for s in SN)
slot_gap_o = EO - max(s.bounds[2] for s in SO)
usb = M['U1']['front']; usb_o = M['U1']['front_old']
print('Pico USB end x %.2f: %.2f mm past the edge line centre (was %.2f mm inside); slots end %.2f mm from the edge (was %.2f)'
      % (usb, usb - EN, EO - usb_o, slot_gap, slot_gap_o))
noseJ8, noseJ5 = M['J8']['front'], M['J5']['front']
print('J8 / J5 nose %.3f / %.3f mm behind the plate outer face (the jack in the same place was %.3f / %.3f past it); J11 front %.2f mm behind the plate inner face'
      % (POUT - noseJ8, POUT - noseJ5, M['J8']['front_old'] - POUT, M['J5']['front_old'] - POUT, PIN - M['J11']['front']))
# the upper and the lower jack (smaller / larger y)
JU, JL = sorted(JACKS, key=lambda r: M[r]['y'])

# ---------------------------------------------------------------- drawing helpers
OLDC = (215, 70, 60)       # old outline / old positions
BRK = (40, 110, 200)       # bracket drawing
BRKF = (214, 228, 246)
TRIM = (229, 57, 53)
BOARD = (232, 238, 226)
FABC = (90, 90, 90)


def draw_scene(v, S_, detail=1.0, parts=PARTS, labels_old=True):
    """everything in rotated coordinates on View v"""
    # board and trimmed strip
    v.fill(rot(BN), BOARD)
    strip = BO.difference(BN)
    v.fill(rot(strip), (250, 222, 220))
    v.hatch(rot(strip), TRIM, 9, 1.4)
    # bracket: plate and tabs from the new drawing, old tabs dashed
    plate = box(PIN, BN.bounds[1] - 3, POUT, BN.bounds[3] + 3)
    v.fill(rot(plate), BRKF)
    newd = {tuple(map(lambda c: round(c, 3), (pl[0] + pl[-1]))) for e in new['drawings'] for pl in e['pl'] if len(pl) > 1}
    for e in new['drawings']:
        for pl in e['pl']:
            if len(pl) > 1:
                v.outline(rot(LineString(pl)), BRK, 1.6 * detail)
    for e in old['drawings']:
        for pl in e['pl']:
            if len(pl) > 1 and tuple(map(lambda c: round(c, 3), (pl[0] + pl[-1]))) not in newd:
                v.outline(rot(LineString(pl)), (235, 150, 40), 1.6 * detail, dash=(6, 4))
    # slots
    for s in SN:
        v.fill(rot(s), WHITE)
        v.outline(rot(s), INK, 1.4 * detail)
    for s in SO:
        v.outline(rot(s), OLDC, 1.4 * detail, dash=(5, 4))
    # parts: body (F.Fab) and copper; old copper dashed
    for r in parts:
        for ln in fab(new, r):
            v.outline(rot(ln), FABC, 1.3 * detail)
        if abs(M[r]['mv']) > 1e-3 and labels_old:
            for num, g in copper(old, M[r]['slot']):
                v.outline(rot(g), OLDC, 1.3 * detail, dash=(5, 4))
        for num, g in copper(new, r):
            v.fill(rot(g), PAD)
        for p in new['fps'][r]['pads']:
            h = hole_geom(p)
            if h is not None:
                v.fill(rot(h), WHITE)
    # outlines
    v.outline(rot(BO.exterior), OLDC, 2.2 * detail, dash=(10, 6))
    v.outline(rot(BN.exterior), INK, 2.4 * detail)


# ---------------------------------------------------------------- main strip
S1 = 21.6
Y0, Y1 = BN.bounds[1] - 1.2, BN.bounds[3] - 6.0        # board y range shown (left -> right)
X0, X1 = 199.0, POUT + 2.2                                 # board x range shown (bottom -> top)
v1 = View(Y0, -X1, Y1, -X0, S1, bg=WHITE)
v1.grid(5, (236, 236, 232))
draw_scene(v1, S1)
strip_im = v1.finish()
d1 = ImageDraw.Draw(strip_im)
fls = font(18, 'SemiBold'); flb = font(19, 'Bold'); fsm = font(21); fz = font(20)
# edge annotations inside the strip panel
T1 = lambda p: v1.T(R(p))
x_e = T1((EN, 0))[1]; x_o = T1((EO, 0))[1]
tag(d1, (T1((0, 41.5))[0], (x_e + x_o) / 2), 'trimmed %.2f mm' % (EO - EN), flb, TRIM, anchor='mm')
tag(d1, (T1((0, 84.0))[0], T1((POUT, 0))[1] - 6), 'bracket plate (User.Drawings), %.2f mm' % (POUT - PIN), fls, BRK, anchor='mb')

# ---------------------------------------------------------------- zooms
def zoom(xa, xb, ya, yb, S_):
    v = View(ya, -xb, yb, -xa, S_, bg=WHITE)
    v.grid(1, (236, 236, 232))
    draw_scene(v, S_, 1.2)
    im = v.finish()
    return im, v


def T(v, p):
    return v.T(R(p))


hy = M['H1']['y']
zH, vH = zoom(209.4, 223.4, hy - 8.0, hy + 8.0, 30)
jy = M[JU]['y']
zJ, vJ = zoom(211.4, 223.6, jy - 6.9, jy + 7.1, 34)
zU, vU = zoom(212.9, 223.5, 102.5, 115.3, 37)

# H1: hole centres measured from the plate's inner face, old on the left, new on the right
dH = ImageDraw.Draw(zH)
xl, xr = T(vH, (0, hy - 4.6))[0], T(vH, (0, hy + 4.6))[0]
yp = T(vH, (PIN, 0))[1]; yn = T(vH, (M['H1']['xn'], 0))[1]; yo = T(vH, (M['H1']['xo'], 0))[1]
cxp = T(vH, (0, hy))[0]
for yy_, col in ((yn, INK), (yo, OLDC)):
    dH.line([(xl - 10, yy_), (xr + 10, yy_)], fill=col, width=1)
dH.line([(xl - 10, yp), (xr + 10, yp)], fill=BRK, width=1)
dim(dH, (xl, yp), (xl, yo), '%.2f' % (PIN - M['H1']['xo']), fls, OLDC, side='left', off=8)
dim(dH, (xr, yp), (xr, yn), '%.2f' % (PIN - M['H1']['xn']), fls, INK, side='right', off=8)
xm = T(vH, (0, hy - 6.6))[0]
dim(dH, (xm, yo), (xm, yn), '%.2f' % M['H1']['mv'], fls, RED, side='right', off=6)
dH.line([(xm - 8, yo), (xl - 10, yo)], fill=OLDC, width=1); dH.line([(xm - 8, yn), (xl - 10, yn)], fill=INK, width=1)

CAP = {
    'H': ['H1 (H2 the same): centre %.2f mm from the plate\'s inner' % (PIN - M['H1']['xn']),
          'face, was %.2f; moved %.2f mm. The bracket tab (blue) must' % (PIN - M['H1']['xo'], M['H1']['mv']),
          'reach %.2f mm further in than the old one (orange).' % (TO - TN),
          'Its %.1f mm pad is %.2f mm from the new edge.' % (2 * max(abs(g.bounds[2] - M['H1']['xn']) for n_, g in copper(new, 'H1')), M['H1']['cu'])],
    'J': (['%s now sits where %s was, %.2f mm further in (%s, below, %.2f mm further in' % (JU, M[JU]['slot'], M[JU]['mv'], JL, M[JL]['mv']),
           'than %s was there): the sleeve pads are %.2f / %.2f mm from the new edge.' % (M[JL]['slot'], M[JU]['cu'], M[JL]['cu'])]
          if SWAPPED else
          ['%s moved %.2f mm in (%s %.2f mm): the sleeve pads are %.2f mm' % (JU, M[JU]['mv'], JL, M[JL]['mv'], M[JU]['cu']),
           'from the new edge.']) +
         ['The jack noses (%s / %s) end %.2f / %.2f mm behind the plate\'s outer face' % (JU, JL, POUT - M[JU]['front'], POUT - M[JL]['front']),
          '(the old ones %.2f / %.2f mm past it).' % (M[JU]['front_old'] - POUT, M[JL]['front_old'] - POUT)],
    'U': ['The Pico moved %.2f mm in for the trim (%.2f mm from v1.3.1).' % (M['U1']['mv'], M['U1']['mv131']),
          'Its USB end is %.2f mm past the edge line (was %.2f mm' % (usb - EN, EO - usb_o),
          'inside); the relief slots end %.2f mm from the edge and' % slot_gap,
          'TP2/TP3 copper is %.2f mm from it.' % M['U1']['cu']],
}

WHY = ['Why: JLCPCB quoted USD 24.50 for 10 black ENIG 2-layer boards at 102.0 mm and USD 33.53 at 103.74 mm (2026-10-28). The ISA fingers, the other',
       'three edges and the plate\'s place in the case do not change. The tab holes are %.2f mm further from the plate, so a stock Keystone 9202 or an'
       % M['H1']['mv'],
       'older PicoGUS bracket does not fit: use one with %.2f mm longer tabs, e.g. 3D-printed (the bracket drawing on User.Drawings has them).' % (TO - TN)]

# ---------------------------------------------------------------- compose
pad = 40; gap = 34
W = max(strip_im.size[0] + 2 * pad + 0, 2150)
can = Image.new('RGB', (W, 2400), WHITE); dr = ImageDraw.Draw(can)
ft = font(40, 'Bold'); fs = font(24); fh = font(26, 'SemiBold')
dr.text((pad, 28), 'PicoGUS THT v1.3.2: %.1f mm board, trimmed %.2f mm at the bracket end' % (WN + LW, EO - EN), font=ft, fill=INK)
dr.text((pad, 84), '%.2f × %.2f mm at the outline\'s line centres (8dbb6c6 and v1.3.1: %.2f), %.2f × %.2f mm over its %.1f mm lines. '
        'The bracket tab holes H1/H2 moved' % (WN, HN, WO, WN + LW, HN + LW, LW), font=fs, fill=GREY)
dr.text((pad, 116), '%.2f mm away from the plate, so the bracket needs %.2f mm longer tabs. Views turned 90°: bracket at the top, ISA fingers to the right.'
        % (M['H1']['mv'], TO - TN), font=fs, fill=GREY)
y = 170
dr.text((pad, y), 'Bracket end: old outline dashed, new solid; parts and holes where they are now (old copper dashed red)', font=fh, fill=INK)
y += 44
sx = pad
can.paste(strip_im, (sx, y)); frame(dr, sx, y, strip_im)

# part labels under the strip, with leader lines to the part
LBL = {
    'J8': ['J8 line out' + (' (was J5\'s place)' if SLOT['J8'] != 'J8' else ''), '%.2f mm in, sleeve pad %.2f mm' % (M['J8']['mv'], M['J8']['cu'])],
    'RV1': ['RV1 WT volume', 'stays, copper %.2f mm' % M['RV1']['cu']],
    'H1': ['H1 tab hole', '%.2f mm in, pad %.2f mm' % (M['H1']['mv'], M['H1']['cu'])],
    'J5': ['J5 MIDI out' + (' (was J8\'s place)' if SLOT['J5'] != 'J5' else ''), '%.2f mm in, sleeve pad %.2f mm' % (M['J5']['mv'], M['J5']['cu'])],
    'J11': ['J11 USB-A', 'stays, copper %.2f mm' % M['J11']['cu']],
    'U1': ['U1 Pico, USB end', '%.2f mm in (v1.3.1: %.2f), TP2/3 %.2f mm' % (M['U1']['mv'], M['U1']['mv131'], M['U1']['cu'])],
    'H2': ['H2 tab hole', '%.2f mm in, pad %.2f mm' % (M['H2']['mv'], M['H2']['cu'])],
}
ROW = {JU: 0, 'RV1': 1, 'H1': 0, JL: 1, 'J11': 0, 'U1': 1, 'H2': 0}
ANCH = {'J8': (M['J8']['y'], 213.0), 'RV1': (61.4, 210.5), 'H1': (M['H1']['y'], 212.0), 'J5': (M['J5']['y'], 213.0), 'J11': (92.5, 207.0),
        'U1': (108.9, 214.0), 'H2': (M['H2']['y'], 212.0)}
ly0 = y + strip_im.size[1] + 18
for r in PARTS:
    px, py = v1.T(R((ANCH[r][1], ANCH[r][0])))
    bx = sx + px
    by = ly0 + ROW[r] * 62
    l1, l2 = LBL[r]
    w = max(dr.textlength(l1, font=flb), dr.textlength(l2, font=fls))
    bx0 = min(max(bx - w / 2, sx + 4), sx + strip_im.size[0] - w - 4)
    dr.line([(bx, y + py), (bx, by - 2)], fill=(150, 150, 150), width=1)
    dr.ellipse([bx - 4, y + py - 4, bx + 4, y + py + 4], fill=(150, 150, 150))
    dr.rectangle([bx0 - 6, by, bx0 + w + 6, by + 54], fill=WHITE, outline=(190, 190, 190))
    dr.text((bx0, by + 4), l1, font=flb, fill=INK)
    dr.text((bx0, by + 29), l2, font=fls, fill=RED)
y = ly0 + 2 * 62 + 34

# zoom row + captions
capend = 0
fzc = font(20)
xz = [pad, pad + zH.size[0] + gap, pad + zH.size[0] + gap + zJ.size[0] + gap]
for xx_, ttl, im_, k in ((xz[0], 'Zoom: H1 and its bracket tab', zH, 'H'), (xz[1], 'Zoom: %s at the bracket plate' % JU, zJ, 'J'),
                          (xz[2], 'Zoom: the Pico\'s USB end', zU, 'U')):
    dr.text((xx_, y), ttl, font=fh, fill=INK)
    can.paste(im_, (xx_, y + 42)); frame(dr, xx_, y + 42, im_)
    yy_ = y + 42 + im_.size[1] + 10
    words = ' '.join(CAP[k]).split(' '); cur = ''
    lines_ = []
    for w_ in words:
        t_ = (cur + ' ' + w_).strip()
        if dr.textlength(t_, font=fzc) <= im_.size[0]:
            cur = t_
        else:
            lines_.append(cur); cur = w_
    lines_.append(cur)
    for ln in lines_:
        dr.text((xx_, yy_), ln, font=fzc, fill=TXT2); yy_ += 27
    capend = max(capend, yy_)
yz = capend
# table of measured moves, right of the zooms if it fits, else below
xt = xz[2] + zU.size[0] + gap
yt = y
if xt + 500 > W - pad:
    xt, yt = pad, yz + 20
cols = [('', 0), ('v1.3.1', 165), ('8dbb6c6', 250), ('v1.3.2', 345), ('moved', 430)]
fT = font(19); fTb = font(19, 'Bold')
dr.text((xt, yt), 'x positions (mm)', font=fh, fill=INK)
yt += 42
for name, cx in cols:
    dr.text((xt + cx, yt), name, font=fTb, fill=INK)
yt += 30
TNAME = {'U1': 'U1 Pico (centre)', JU: '%s (upper jack)' % JU, JL: '%s (lower jack)' % JL}
rows = [('Outline end', outline(o131).bounds[2], EO, EN, EO - EN)] + [
    (TNAME.get(r, r), M[r]['x131'], M[r]['xo'], M[r]['xn'], M[r]['mv']) for r in ['H1', 'H2', JU, JL, 'U1', 'RV1', 'J11']]
for nm_, x1_, xo, xn, mv in rows:
    dr.line([(xt, yt - 4), (xt + 500, yt - 4)], fill=(225, 225, 225))
    dr.text((xt, yt), nm_, font=fT, fill=INK)
    dr.text((xt + 165, yt), '%.2f' % x1_, font=fT, fill=TXT2)
    dr.text((xt + 250, yt), '%.2f' % xo, font=fT, fill=TXT2)
    dr.text((xt + 345, yt), '%.2f' % xn, font=fT, fill=TXT2)
    dr.text((xt + 430, yt), '%.2f' % mv if abs(mv) > 1e-3 else '–', font=fTb if abs(mv) > 1e-3 else fT, fill=RED if abs(mv) > 1e-3 else TXT2)
    yt += 28
yt += 4
for ln in (['moved = 8dbb6c6 to v1.3.2; U1 moved %.2f mm' % M['U1']['mv131'], 'from v1.3.1. Positions are footprint origins.']
           + (['%s and %s swapped places: the old columns give the' % (JU, JL), 'jack that was in the same place (%s above, %s below).' % (M[JU]['slot'], M[JL]['slot'])]
              if SWAPPED else [])):
    dr.text((xt, yt), ln, font=font(18), fill=TXT2); yt += 25
ymax = max(yz, yt)

ly = ymax + 26
xx = pad
dr.line([xx, ly + 11, xx + 34, ly + 11], fill=INK, width=3); dr.text((xx + 44, ly + 11), 'new outline', font=fsm, fill=INK, anchor='lm')
xx += 44 + dr.textlength('new outline', font=fsm) + 26
for k in range(0, 34, 12):
    dr.line([xx + k, ly + 11, xx + min(k + 8, 34), ly + 11], fill=OLDC, width=3)
dr.text((xx + 44, ly + 11), 'old outline, old copper', font=fsm, fill=INK, anchor='lm')
xx += 44 + dr.textlength('old outline, old copper', font=fsm) + 26
xx = swatch(dr, xx, ly, (250, 222, 220), 'trimmed strip', fsm, edge=TRIM) + 26
xx = swatch(dr, xx, ly, BRKF, 'bracket (drawing)', fsm, edge=BRK) + 26
for k in range(0, 34, 10):
    dr.line([xx + k, ly + 11, xx + min(k + 6, 34), ly + 11], fill=(235, 150, 40), width=3)
dr.text((xx + 44, ly + 11), 'old tabs', font=fsm, fill=INK, anchor='lm')
xx += 44 + dr.textlength('old tabs', font=fsm) + 26
xx = swatch(dr, xx, ly, PAD, 'pad', fsm) + 26
dr.line([xx, ly + 11, xx + 34, ly + 11], fill=FABC, width=2); dr.text((xx + 44, ly + 11), 'part body (F.Fab)', font=fsm, fill=INK, anchor='lm')
vy = ly + 50
for ln in WHY:
    dr.text((pad, vy), ln, font=fsm, fill=TXT2); vy += 30
can = can.crop((0, 0, W, vy + 22))
can.save(OUT)
print('saved', OUT, can.size)
