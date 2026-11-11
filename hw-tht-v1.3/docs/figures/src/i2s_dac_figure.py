# Usage: python3 i2s_dac_figure.py NOW.json NOW.kicad_pro OLD.json OLD.kicad_pro OUT.png
#   NOW.json / NOW.kicad_pro  copper_dump.py output and project file of the routed v1.3.2 board
#   OLD.json / OLD.kicad_pro  the same for the 103.7 mm layout (git show 8dbb6c6:hw-tht-v1.3.2/PicoGUS-THT.kicad_{pcb,pro})
#   (the .kicad_pro supplies the net-class patterns that define the audio class)
# Draws i2s-around-dac.png: the I2S nets (BCK, DIN, LRCK) from the Pico (U1) to the DAC (U9, PCM5100A; a TSSOP-20
# with test pads on v1.3.2, on a DIP-20 adapter on 8dbb6c6), the audio-class and DAC-output copper, U9's analog pins
# 1-10 (and their test pads) and its CAPP/CAPM/VNEG/AVDD copper with a 1 mm keep-away, on v1.3.2 and on 8dbb6c6, every
# digital track and via inside the keep-away (red), and two close-ups of v1.3.2: U9 with its I2S pins, and the place
# where digital copper comes nearest the AVDD copper. Every number printed is measured here.
# (Rewritten for the re-routed board of 2026-11-01: U9 turned 180 degrees above U3/U4, I2S on its west pins;
#  windows moved for the final placement of 2026-11-01.)
# Needs Pillow, numpy, shapely; figlib.py next to this script.
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from figlib import *

NOWJ, NOWP, OLDJ, OLDP, OUT = sys.argv[1:6]
I2S = ('BCK', 'DIN', 'LRCK')
ANA = {'Net-(U9-CAPP)', 'Net-(U9-CAPM)', 'Net-(U9-VNEG)', '/PCM5100 audio/AVDD'}
NOT_DIGITAL = {'', 'GND', '+3V3', '+5V', '/VSYS', '/USB host port/VBUS_USB', '+12V', '-12V', '/Wavetable & line out/+12VA',
               '/Wavetable & line out/-12VA', '/Wavetable & line out/LINE_L', '/Wavetable & line out/LINE_R', 'Net-(U9-LDOO)'}
KEEP = 1.0

def prep(jp, pro):
    b = Board(jp, pro)
    d = b.d
    b.aud = {n for n in d['nets'] if b.netclass(n) == 'audio'} | {'/PCM5100 audio/DAC_OUTL', '/PCM5100 audio/DAC_OUTR'}
    b.u9a = [p for p in d['pads'] if p['ref'] == 'U9' and int(p['num']) <= 10]
    b.sens = {}          # per layer: audio, analog, U9 pins 1-10
    for L in ('F.Cu', 'B.Cu'):
        b.sens[L] = {'audio': b.copper(L, b.aud, pours=False), 'analog': b.copper(L, ANA, pours=False),
                     'u9': unary_union([p['g'][L] for p in b.u9a if L in p['g']])}
    b.keep = unary_union([g.buffer(KEEP, quad_segs=16) for L in b.sens for g in b.sens[L].values()])
    # I2S totals
    b.i2s_len = sum(t['cl'].length for t in d['tracks'] if t['net'] in I2S)
    b.i2s_vias = [v for v in d['vias'] if v['net'] in I2S]
    # nearest I2S copper (same layer; vias on both) to each sensitive set
    b.near = {}
    for key in ('audio', 'analog', 'u9'):
        best = None
        for L in ('F.Cu', 'B.Cu'):
            G = b.sens[L][key]
            if G.is_empty:
                continue
            items = [(t['net'], t['g']) for t in d['tracks'] if t['net'] in I2S and t['layer'] == L]
            items += [('via ' + v['net'], v['g']) for v in b.i2s_vias]
            for n, g in items:
                dd, pa, pb = gap(g, G)
                if best is None or dd < best[0]:
                    best = (dd, pa, pb, n, L)
        b.near[key] = best
    # digital copper (any net that is not power, GND, audio, DAC analog or line out) within 1 mm, same layer
    b.dig_close = []
    b.dig_min = None
    from shapely.prepared import prep as prepared
    for L in ('F.Cu', 'B.Cu'):
        S_ = unary_union(list(b.sens[L].values()))
        near3 = prepared(S_.buffer(3.0, quad_segs=8))          # only items within 3 mm need an exact distance
        cand = [(t['net'], t['g'], t) for t in d['tracks'] if t['layer'] == L]
        cand += [('via ' + v['net'], v['g'], v) for v in d['vias']]
        for n, g, it in cand:
            net = n.replace('via ', '')
            if net in NOT_DIGITAL or net in b.aud or net in ANA or not near3.intersects(g):
                continue
            dd = g.distance(S_)
            if b.dig_min is None or dd < b.dig_min[0]:
                b.dig_min = (dd, n)
            if dd < KEEP:
                ln = it['cl'].intersection(S_.buffer(KEEP + it['w'] / 2, quad_segs=16)).length if 'cl' in it else 0.0
                b.dig_close.append((n, L, dd, ln, it))
    # I2S tracks crossing audio copper on the other layer
    b.cross = []
    for t in d['tracks']:
        if t['net'] in I2S:
            O = 'B.Cu' if t['layer'] == 'F.Cu' else 'F.Cu'
            if t['g'].intersects(b.sens[O]['audio']):
                b.cross.append(t)
    # LRCK (any I2S) length within 1 mm of U9 pins 1-10
    b.i2s_u9_len = 0.0
    b.i2s_u9_geo = []
    for t in d['tracks']:
        if t['net'] in I2S:
            K = b.sens[t['layer']]['u9'].buffer(KEEP + t['w'] / 2, quad_segs=16)
            x = t['cl'].intersection(K)
            if x.length > 0:
                b.i2s_u9_len += x.length
                b.i2s_u9_geo.append((t['net'], x.buffer(t['w'] / 2, cap_style=2).intersection(t['g'])))
    return b

now, old = prep(NOWJ, NOWP), prep(OLDJ, OLDP)
for name, b in (('v1.3.2', now), ('8dbb6c6', old)):
    print(name, 'I2S %.1f mm, %d vias' % (b.i2s_len, len(b.i2s_vias)))
    for k, v in b.near.items():
        print('   nearest I2S to %-6s %.3f mm (%s, %s)' % (k, v[0], v[3], v[4]))
    print('   digital within 1 mm:', [(n, L, round(dd, 3), round(ln, 2)) for n, L, dd, ln, _ in b.dig_close],
          'nearest digital %.3f (%s)' % b.dig_min)
    print('   I2S crossing audio on the other layer:', len(b.cross), ' I2S length within 1 mm of U9 pins 1-10: %.2f mm' % b.i2s_u9_len)

# ---------------- drawing
CTX_F = (226, 211, 211); CTX_B = (208, 214, 228); CTX_PAD = (232, 214, 160)
AUD_F = (196, 32, 120); AUD_B = (232, 128, 186)
ANA_C = (0, 150, 150)
I2S_F = (24, 88, 208); I2S_B = (110, 168, 245); I2S_EDGE = (16, 60, 150)
KEEP_C = (247, 196, 218)
U9_C = PAD

def draw(b, box_, S, SS=3, lw=1.0):
    p = Panel(*box_, S, SS=SS)
    d = b.d
    p.fill(b.keep, KEEP_C)
    for L, col in (('B.Cu', CTX_B), ('F.Cu', CTX_F)):
        for t in d['tracks']:
            if t['layer'] == L and t['net'] not in I2S and t['net'] not in b.aud and t['net'] not in ANA:
                p.fill(t['g'], col)
    for v in d['vias']:
        if v['net'] not in I2S:
            p.fill(v['g'], (190, 190, 196))
    for q in d['pads']:
        if q['ref'] == 'U9' and int(q['num']) <= 10:
            continue
        for L, g in q['g'].items():
            p.fill(g, CTX_PAD)
    for L, col in (('B.Cu', AUD_B), ('F.Cu', AUD_F)):
        for t in d['tracks']:
            if t['layer'] == L and t['net'] in b.aud:
                p.fill(t['g'], col)
            if t['layer'] == L and t['net'] in ANA:
                p.fill(t['g'], ANA_C if L == 'F.Cu' else (110, 200, 200))
    for q in d['pads']:
        if q['net'] in b.aud:
            for g in q['g'].values():
                p.fill(g, AUD_F)
        elif q['net'] in ANA:
            for g in q['g'].values():
                p.fill(g, ANA_C)
    for q in b.u9a:
        for g in q['g'].values():
            p.fill(g, U9_C)
            p.outline(g, (150, 60, 20), width=1.6 * lw)
    for L, col in (('B.Cu', I2S_B), ('F.Cu', I2S_F)):
        for t in d['tracks']:
            if t['layer'] == L and t['net'] in I2S:
                if L == 'B.Cu':
                    p.fill(t['g'].buffer(0.05), I2S_EDGE)
                p.fill(t['g'], col)
    for v in b.i2s_vias:
        p.fill(Point(v['pos']).buffer(v['dia'] / 2 + 0.12), I2S_EDGE)
        p.fill(v['g'], I2S_F)
    p.outline(b.keep, (205, 90, 150), width=1.3 * lw, dash=(7, 5))
    holes = [(v['pos'][0], v['pos'][1], v['drill']) for v in d['vias']]
    holes += [(q['pos'][0], q['pos'][1], q['hole'][0]) for q in d['pads'] if q['hole']]
    p.holes(holes, col=(255, 255, 255))
    return p

# ---------------- digital copper inside the keep-away, per layout
NAMES = {'~{RI{slash}OCHRDY}': '~RI/OCHRDY', '~{RIOW}': '~RIOW', '~{RDACK}': '~RDACK'}


def nn(n):
    n = n.replace('via ', '')
    n = NAMES.get(n, n)
    return n.split('/')[-1] if n.startswith('/') else n


def sens_class(b, g, L):
    """which sensitive copper a digital item is nearest to"""
    best = None
    for key, G in (('U9 pins 1-10', b.sens[L]['u9']), ('audio', b.sens[L]['audio'])):
        if not G.is_empty:
            dd = g.distance(G)
            if best is None or dd < best[0]:
                best = (dd, key)
    for net in sorted(ANA):
        G = b.copper(L, {net}, pours=False)
        if not G.is_empty:
            dd = g.distance(G)
            if best is None or dd < best[0]:
                best = (dd, net.split('/')[-1].replace('Net-(U9-', '').replace(')', ''))
    return best


for b in (now, old):
    keepL = {L: unary_union(list(b.sens[L].values())).buffer(KEEP, quad_segs=16) for L in ('F.Cu', 'B.Cu')}
    b.red = []           # (net, layer, geometry inside the keep-away)
    b.per = {}           # net -> (min gap, nearest class, length inside)
    for n, L, dd, ln, it in b.dig_close:
        g = it['g'].intersection(keepL[L])
        b.red.append((n, L, g))
        k = nn(n)
        cls = sens_class(b, it['g'], L)[1]
        p = b.per.get(k)
        b.per[k] = (min(dd, p[0]) if p else dd, cls if (p is None or dd < p[0]) else p[1], (p[2] if p else 0) + ln)
    print('keep-away intruders:', {k: (round(v[0], 3), v[1], round(v[2], 2)) for k, v in sorted(b.per.items(), key=lambda kv: kv[1][0])})
red_len_now = sum(v[2] for v in now.per.values())
red_len_old = sum(v[2] for v in old.per.values())

# ---------------- layout
pad = 36; gp = 32; S = 18.5
Y0, Y1 = 67.0, 121.0
NB_ = (138.0, Y0, 190.0, Y1)
OB_ = (148.5, Y0, 194.0, Y1)
wN = round((NB_[2] - NB_[0]) * S); wO = round((OB_[2] - OB_[0]) * S); hP = round((Y1 - Y0) * S)
Z1 = (147.0, 78.7, 163.4, 90.5); SZ1 = 27.5          # v1.3.2: U9
Z2 = (158.6, 82.0, 169.6, 92.0); SZ2 = 41.0          # v1.3.2: U9's analog pins (east), hand-routed corner
wZ = max(round((Z1[2] - Z1[0]) * SZ1), round((Z2[2] - Z2[0]) * SZ2))
hZ1 = round((Z1[3] - Z1[1]) * SZ1); hZ2 = round((Z2[3] - Z2[1]) * SZ2)
W = pad + wN + gp + wO + gp + wZ + pad
head = 160
y1 = head + 44
stat_h = 150
H = y1 + hP + stat_h + 300
can = Image.new('RGB', (W, H), (255, 255, 255))
dr = ImageDraw.Draw(can)
dr.text((pad, 26), 'PicoGUS THT v1.3.2: I2S from the Pico to the DAC', font=font(40, 'Bold'), fill=INK)
dr.text((pad, 82), 'BCK, DIN and LRCK from the Pico (U1) to the PCM5100A (U9). U9\'s pins 1-10 are its analog side (charge pump, outputs, AVDD); with the',
        font=font(24), fill=GREY)
dr.text((pad, 114), 'CAPP/CAPM/VNEG/AVDD and audio-class copper they get a 1 mm keep-away (pink). Red: digital copper inside it. Both layers, seen from the front.',
        font=font(24), fill=GREY)


def panel(b, box_, x, w, title):
    p = draw(b, box_, S)
    can.paste(p.done(), (x, y1)); frame(dr, x, y1, p.W, p.H)
    heading(dr, (x, y1 - 8), title, size=26)
    return Annot(can, p, x, y1)


def overlay_red(a, b):
    win = box(a.p.x0, a.p.y0, a.p.x1, a.p.y1)
    for net, L, g in b.red:
        for q in polys(g.intersection(win)):
            dr.polygon([a.P(c) for c in q.exterior.coords], fill=RED)


xN = pad; xO = xN + wN + gp; xZ = xO + wO + gp
aN = panel(now, NB_, xN, wN, 'v1.3.2 (102 mm), U9 above U3/U4')
aO = panel(old, OB_, xO, wO, '8dbb6c6 (103.7 mm)')
overlay_red(aN, now)
overlay_red(aO, old)


def labels(a, b, box_):
    u9 = b.d['fps']['U9']['pos']; rot = round(b.d['fps']['U9']['rot']) % 360
    turned = rot in (0, 180)          # pins in two columns (TSSOP turned) rather than two rows (DIP on its side)
    i2s_pads = {}
    for q in b.d['pads']:
        if q['ref'] == 'U9' and q['net'] in I2S:
            i2s_pads.setdefault(q['net'], []).append(q)
    if turned:
        a.box_label(a.P((u9[0], u9[1] - 4.7)), 'U9 PCM5100A', size=18, col=INK, w='Bold')
        xs_ = [q['pos'][0] for qs in i2s_pads.values() for q in qs]
        ys_ = [q['pos'][1] for qs in i2s_pads.values() for q in qs]
        nums = sorted({int(q['num']) for qs in i2s_pads.values() for q in qs})
        a.box_label(a.P((min(xs_) - 1.6, sum(ys_) / len(ys_))), 'I2S, pins %d-%d' % (nums[0], nums[-1]), size=14, col=I2S_EDGE, w='Bold', anchor='rm')
        a1 = [p for p in b.u9a if p['num'] == '1']; a10 = [p for p in b.u9a if p['num'] == '10']
        xr = max(p['pos'][0] for p in b.u9a) + 0.8
        a.text((xr, max(p['pos'][1] for p in a1)), '1', size=15, col=(150, 60, 20), w='Bold', anchor='lm')
        a.text((xr, max(p['pos'][1] for p in a10)), '10', size=15, col=(150, 60, 20), w='Bold', anchor='lm')
    else:
        a.box_label(a.P((u9[0] + 0.6, u9[1] - 9.7)), 'U9 PCM5100A (pins 11-20: digital)', size=18, col=INK, w='Bold')
        for net, qs in i2s_pads.items():
            q = qs[0]
            a.box_label(a.P((q['pos'][0], q['pos'][1] - 1.75)), net, size=15, col=I2S_EDGE, w='Bold')
        p1 = next(p for p in b.u9a if p['num'] == '1'); p10 = next(p for p in b.u9a if p['num'] == '10')
        a.text((p1['pos'][0], p1['pos'][1] + 1.75), '1', size=17, col=(150, 60, 20), w='Bold')
        a.text((p10['pos'][0], p10['pos'][1] + 1.75), '10', size=17, col=(150, 60, 20), w='Bold')
    u1p = [p for p in b.d['pads'] if p['ref'] == 'U1' and p['net'] in I2S and p['pos'][1] > 110]
    ux = sum(p['pos'][0] for p in u1p) / len(u1p)
    a.box_label(a.P((ux, 119.6)), 'U1 (Pico) GP pins', size=18, col=INK, w='Bold')
    if turned:
        for net in I2S:
            q = min((p for p in u1p if p['net'] == net), key=lambda p: p['pos'][1])
            a.box_label(a.P((q['pos'][0], q['pos'][1] - 2.3)), net, size=14, col=I2S_EDGE, w='Bold')


labels(aN, now, NB_)
labels(aO, old, OB_)
aO.box_label(aO.P((159.7, 87.0)), 'LRCK', size=17, col=I2S_EDGE, w='Bold')
# FB3 (the AVDD filter) on the new board
fb3 = [p for p in now.d['pads'] if p['ref'] == 'FB3']
if fb3:
    pa = next((p for p in fb3 if p['net'] in ANA), fb3[0])
    aN.leader_text(pa['pos'], (34, -30), 'FB3 AVDD pad', size=16, col=(0, 110, 110), anchor='lm')


def stats(x, y, lines):
    for i, (t, bold, col) in enumerate(lines):
        dr.text((x, y + i * 28), t, font=font(20, 'Bold' if bold else 'Medium'), fill=col)


ys = y1 + hP + 14
nn_ = now.near
order_ = sorted(now.per.items(), key=lambda kv: kv[1][0])
classes = sorted({v[1] for v in now.per.values()})
lst = ', '.join('%s %.2f' % (k, v[0]) for k, v in order_)
def wrap(text, f, width):
    out = []; cur = ''
    for w_ in text.split(' '):
        t = (cur + ' ' + w_).strip()
        if dr.textlength(t, font=f) <= width:
            cur = t
        else:
            out.append(cur); cur = w_
    out.append(cur)
    return out


an_net = sens_class(now, next(t['g'] for t in now.d['tracks'] if t['net'] == nn_['analog'][3] and t['layer'] == nn_['analog'][4]
                              and t['g'].distance(Point(nn_['analog'][1])) < 1e-3), nn_['analog'][4])[1]
txt = ('Nearest I2S copper: %.2f mm to U9 pins 1-10, %.2f mm to audio copper, %.2f mm to the DAC\'s analog copper (%s to %s).'
       % (nn_['u9'][0], nn_['audio'][0], nn_['analog'][0], nn_['analog'][3], an_net))
red_txt = ('Digital copper inside the keep-away, all of it at %s: %s mm.' % (' / '.join(classes), lst)) if now.per else 'No digital copper inside the keep-away.'
L_ = [('I2S: %.1f mm of track, %d vias' % (now.i2s_len, len(now.i2s_vias)), True, INK)]
L_ += [(ln, False, SUB) for ln in wrap(txt, font(20), wN - 4)]
L_ += [(ln, False, RED) for ln in wrap(red_txt, font(20), wN - 4)]
stats(xN, ys, L_)
lr = [x for x in old.dig_close if x[0] == 'LRCK']
stats(xO, ys, [('I2S: %.1f mm of track, %d via' % (old.i2s_len, len(old.i2s_vias)), True, INK),
               ('LRCK runs past U9 pin 1 at %.2f mm: %.1f mm of it lies within' % (old.near['u9'][0], old.i2s_u9_len), False, SUB),
               ('1 mm of the analog pins (red). Nearest I2S to the analog copper', False, SUB),
               ('%.2f mm, to audio copper %.2f mm.' % (old.near['analog'][0], old.near['audio'][0]), False, SUB)])


# close-ups of v1.3.2
def zoom(box_, SZ, y, title):
    p = draw(now, box_, SZ, lw=1.3)
    can.paste(p.done(), (xZ, y)); frame(dr, xZ, y, p.W, p.H)
    heading(dr, (xZ, y - 8), title, size=23)
    a = Annot(can, p, xZ, y)
    overlay_red(a, now)
    return a


zy1 = y1
a1 = zoom(Z1, SZ1, zy1, 'v1.3.2: U9, I2S on the west pins')
n = now.near['u9']
a1.measure(n[1], n[2], '%.2f mm' % n[0], at=('px', 0, -26), size=17)
u9 = now.d['fps']['U9']['pos']
for i, net in enumerate(sorted(I2S, key=lambda n_: min(q['pos'][1] for q in now.d['pads'] if q['ref'] == 'U9' and q['net'] == n_))):
    qs = [q for q in now.d['pads'] if q['ref'] == 'U9' and q['net'] == net]
    q = min(qs, key=lambda q: q['pos'][0])
    a1.box_label(a1.P((q['pos'][0] - (1.0 if i % 2 == 0 else 2.7), q['pos'][1])), net, size=13, col=I2S_EDGE, w='Bold', anchor='rm')
xr = max(p['pos'][0] for p in now.u9a)
a1.text((xr + 0.7, max(p['pos'][1] for p in now.u9a if p['num'] == '1')), 'pin 1', size=15, col=(150, 60, 20), w='Bold', anchor='lm')
a1.text((xr + 0.7, max(p['pos'][1] for p in now.u9a if p['num'] == '10')), 'pin 10', size=15, col=(150, 60, 20), w='Bold', anchor='lm')
yy_ = zy1 + hZ1 + 12
for ln in wrap('BCK, DIN and LRCK reach pins 13-15 from the left; the nearest I2S copper is %.2f mm from pins 1-10 (%s).' % (n[0], n[3]), font(19), wZ):
    dr.text((xZ, yy_), ln, font=font(19), fill=SUB); yy_ += 26
# ~RI/OCHRDY under U9 (B.Cu), next to the AVDD via there
for k, v in order_:
    if k == '~RI/OCHRDY':
        G = now.copper('B.Cu', {'/PCM5100 audio/AVDD'}, pours=False)
        best = None
        for nm_, L, dd, ln, it in now.dig_close:
            if nn(nm_) == k and L == 'B.Cu':
                d_, pa_, pb_ = gap(it['g'], G)
                if best is None or d_ < best[0]:
                    best = (d_, pa_, pb_)
        if best and box(*Z1).contains(Point(best[1])):
            a1.measure(best[1], best[2], '%s %.2f (B.Cu)' % (k, best[0]), at=('px', 10, 64), size=15)
zy2 = zy1 + hZ1 + 150
a2 = zoom(Z2, SZ2, zy2, 'v1.3.2: digital copper at the AVDD copper' if red_len_now > 0 else 'v1.3.2: U9 pins 1-10 and their discs')
win2 = box(*Z2)
OFFZ = {'RESET': ('px', 96, -6), '~RDACK': ('px', 86, -50), 'BCK': ('px', -76, -34), 'LRCK': ('px', -84, 34), '~RIOW': ('px', 70, 52)}
shown = []
for k, v in order_:
    if v[1] != 'AVDD':
        continue
    # the nearest point pair of this net to AVDD inside the window
    G = {L: now.copper(L, {'/PCM5100 audio/AVDD'}, pours=False) for L in ('F.Cu', 'B.Cu')}
    best = None
    for nm_, L, dd, ln, it in now.dig_close:
        if nn(nm_) != k or not it['g'].intersects(win2):
            continue
        d_, pa_, pb_ = gap(it['g'], G[L])
        if best is None or d_ < best[0]:
            best = (d_, pa_, pb_)
    if best and win2.contains(Point(best[1])):
        shown.append((k, best))
for i, (k, (d_, pa_, pb_)) in enumerate(shown):
    print('  close-up 2: %s %.3f at %s -> %s' % (k, d_, [round(c, 2) for c in pa_], [round(c, 2) for c in pb_]))
    a2.measure(pa_, pb_, '%s %.2f' % (k, d_), at=OFFZ.get(k, ('px', 0, -40)), size=15)
yy_ = zy2 + hZ2 + 12
cap2 = ('At FB3\'s AVDD pad and the AVDD track from it: %s. AVDD is the DAC\'s filtered analog supply.' % ', '.join('%s %.2f mm' % (k, b_[0]) for k, b_ in shown)
        if shown else 'East of U9: DAC_OUTL, VNEG, CAPM and CAPP to R7, C17 and C16 (routed by hand, two vias). No digital copper is inside the keep-away anywhere on v1.3.2.')
for ln in wrap(cap2, font(19), wZ):
    dr.text((xZ, yy_), ln, font=font(19), fill=SUB); yy_ += 26

# legend
ly = y1 + hP + stat_h + 22
x = pad; fs = font(21)
x = swatch(dr, x, ly, 'I2S on F.Cu', fill=I2S_F, kind='line', fs=fs) - 8
x = swatch(dr, x, ly, 'I2S on B.Cu', fill=I2S_B, kind='line', fs=fs) - 8
dr.ellipse([x + 6, ly, x + 28, ly + 22], fill=I2S_EDGE); dr.ellipse([x + 9, ly + 3, x + 25, ly + 19], fill=I2S_F)
dr.ellipse([x + 13, ly + 7, x + 21, ly + 15], fill=(255, 255, 255))
dr.text((x + 44, ly + 11), 'I2S via', font=fs, fill=INK, anchor='lm'); x += 44 + dr.textlength('I2S via', font=fs) + 28
x = swatch(dr, x, ly, 'audio class + DAC outputs (F / B)', fill=AUD_F, kind='line', fs=fs) - 8
x = swatch(dr, x, ly, 'CAPP / CAPM / VNEG / AVDD', fill=ANA_C, kind='line', fs=fs) - 8
ly2 = ly + 36; x = pad
x = swatch(dr, x, ly2, 'U9 pins 1-10 and their test pads (analog side)', fill=U9_C, outline=(150, 60, 20), fs=fs) - 8
x = swatch(dr, x, ly2, '1 mm keep-away', fill=KEEP_C, outline=(205, 90, 150), fs=fs) - 8
x = swatch(dr, x, ly2, 'digital copper inside it', fill=RED, fs=fs) - 8
ly3 = ly2 + 36; x = pad
x = swatch(dr, x, ly3, 'other copper F.Cu', fill=CTX_F, kind='line', fs=fs) - 8
x = swatch(dr, x, ly3, 'other copper B.Cu', fill=CTX_B, kind='line', fs=fs) - 8
x = swatch(dr, x, ly3, 'other pads', fill=CTX_PAD, fs=fs)

vy = ly3 + 52; fz = font(21)
lines = (
    'With U9 at 180° above U3/U4 (I2S on its west pins), I2S is %.1f mm with %d vias (8dbb6c6: %.1f mm, %d via) and stays %.2f mm or more from pins 1-10 and %.2f mm'
    % (now.i2s_len, len(now.i2s_vias), old.i2s_len, len(old.i2s_vias), now.near['u9'][0], now.near['audio'][0]),
    'or more from audio copper; %s. %s'
    % ('no I2S track crosses an audio net on the other layer' if not now.cross else '%d I2S tracks cross an audio net on the other layer' % len(now.cross),
       ('Inside the keep-away there is only digital copper next to AVDD: %.1f mm of' % red_len_now) if red_len_now > 0 else 'No digital copper is inside the keep-away:'),
    ('track (8dbb6c6: %s). The GND pours are not drawn.' if red_len_now > 0 else '8dbb6c6 had %s. The GND pours are not drawn.') % ', '.join('%.2f mm of %s next to %s' % (v[2], k, 'U9 pin 1' if v[1] == 'U9 pins 1-10' else v[1] + ' copper')
                                                                    for k, v in sorted(old.per.items(), key=lambda kv: -kv[1][2])),
)
for ln in lines:
    dr.text((pad, vy), ln, font=fz, fill=SUB); vy += 30
    if dr.textlength(ln, font=fz) > W - 2 * pad: print('WARNING: footer line too long:', ln)
can = can.crop((0, 0, W, vy + 22))
can.save(OUT)
print(can.size)
