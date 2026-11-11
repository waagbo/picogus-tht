# python3 courtyard_figure.py crt.json render_top.png courtyard-overlaps.png
# crt.json from courtyard_dump.py; render_top.png from 'kicad-cli pcb render --side top --width 2600 --height 2500'
# of a red-mask copy of the board (the crop is found from the red board area); needs Pillow and numpy
# Draws the figure that explains the 12 accepted courtyard overlaps.
import sys, json, math
from PIL import Image, ImageDraw, ImageFont

d = json.load(open(sys.argv[1]))
ren = Image.open(sys.argv[2]).convert('RGB')
OUT = sys.argv[3]
F = '/usr/share/fonts/opentype/inter/'
def font(sz, w='Medium'):
    try: return ImageFont.truetype(F + 'Inter-%s.otf' % w, sz)
    except OSError: return ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', sz)

ov = d['overlaps']
refs = sorted({o['a'] for o in ov} | {o['b'] for o in ov})
fps = d['fps']
xs = [p[0] for r in refs for c in fps[r]['crt'] for p in c]; ys = [p[1] for r in refs for c in fps[r]['crt'] for p in c]
M = 1.2
ML = 2.0   # left margin: the column's reference labels stand vertically at x 117.5, left of the courtyards
X0, X1, Y0, Y1 = min(xs) - ML, max(xs) + M, min(ys) - M, max(ys) + M

BG = (250, 250, 248); INK = (30, 32, 36); PAD = (214, 168, 40); FAB = (140, 140, 140)
CRT = (30, 110, 220); RED = (229, 57, 53)

def panel(x0, x1, y0, y1, S, crt_w=2, labels=True, depth=True):
    W, H = round((x1 - x0) * S), round((y1 - y0) * S)
    im = Image.new('RGB', (W, H), BG)
    ov_l = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im); do = ImageDraw.Draw(ov_l)
    T = lambda p: ((p[0] - x0) * S, (p[1] - y0) * S)
    for r, f in fps.items():
        bx = [p[0] for c in f['crt'] for p in c] or [f['pos'][0]]; by = [p[1] for c in f['crt'] for p in c] or [f['pos'][1]]
        if max(bx) < x0 - 5 or min(bx) > x1 + 5 or max(by) < y0 - 5 or min(by) > y1 + 5:
            continue
        for kind, g in f['fab']:
            w = max(1, round(S * 0.06))
            if kind == 'line': dr.line([T(g[0]), T(g[1])], fill=FAB, width=w)
            elif kind == 'poly': dr.line([T(p) for p in g] + [T(g[0])], fill=FAB, width=w)
            elif kind == 'circle':
                cx, cy = T(g[:2]); rr = g[2] * S; dr.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=FAB, width=w)
            elif kind == 'arc':
                cx, cy = T(g[:2]); rr = g[2] * S; a0, a1 = sorted((g[3], g[4]))
                dr.arc([cx - rr, cy - rr, cx + rr, cy + rr], a0, a1, fill=FAB, width=w)
        for p in f['pads']:
            for c in p['poly']:
                if len(c) >= 3: dr.polygon([T(q) for q in c], fill=PAD)
            hx, hy, hd = p['hole']
            if hd > 0:
                cx, cy = T((hx, hy)); rr = hd / 2 * S; dr.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=BG)
    for o in ov:
        for c in o['polys']:
            do.polygon([T(q) for q in c], fill=RED + (150,))
    im.paste(ov_l, (0, 0), ov_l); dr = ImageDraw.Draw(im)
    for r in refs:
        for c in fps[r]['crt']:
            dr.line([T(p) for p in c] + [T(c[0])], fill=CRT, width=crt_w)
    for o in ov:
        for c in o['polys']:
            dr.line([T(q) for q in c] + [T(c[0])], fill=RED, width=max(1, crt_w - 1))
    if labels:
        fl = font(max(14, round(S * 0.9)), 'SemiBold')
        for r in refs:
            bx = [p[0] for c in fps[r]['crt'] for p in c]; by = [p[1] for c in fps[r]['crt'] for p in c]
            cx, cy = T(((min(bx) + max(bx)) / 2, (min(by) + max(by)) / 2))
            if r.startswith('U'): cx, cy = T((min(bx) + 1.6, (min(by) + max(by)) / 2 - 1.6))
            if r.startswith('C'): cx, cy = T((max(bx) + 0.3, (min(by) + max(by)) / 2)); dr.text((cx, cy), r, font=fl, fill=INK, anchor='lm'); continue
            if r.startswith('R'): cx, cy = T((max(bx) + 0.3, (min(by) + max(by)) / 2)); dr.text((cx, cy), r, font=fl, fill=INK, anchor='lm'); continue
            dr.text((cx, cy), r, font=fl, fill=INK, anchor='lm')
    if depth:
        fd = font(max(13, round(S * 0.62)), 'Bold')
        for o in ov:
            xs_ = [q[0] for c in o['polys'] for q in c]; ys_ = [q[1] for c in o['polys'] for q in c]
            cx, cy = T((max(xs_) + 0.25, (min(ys_) + max(ys_)) / 2))
            if o['a'].startswith('R'):
                cx, cy = T((min(xs_) - 0.25, (min(ys_) + max(ys_)) / 2)); an = 'rm'
            else:
                cx, cy = T((max(xs_) + 0.15, (min(ys_) + max(ys_)) / 2)); an = 'lm'
            tb = dr.textbbox((cx, cy), '%.2f mm' % o['depth'], font=fd, anchor=an)
            dr.rounded_rectangle([tb[0] - 4, tb[1] - 3, tb[2] + 4, tb[3] + 3], radius=4, fill=(255, 255, 255), outline=RED, width=1)
            dr.text((cx, cy), '%.2f mm' % o['depth'], font=fd, fill=RED, anchor=an)
    return im, T

# main diagram
S = 24
diag, T = panel(X0, X1, Y0, Y1, S)

# 3D render crop, same area, scaled to the same mm/px
rp = ren.load(); RW, RH = ren.size
redp = lambda c: c[0] > 150 and c[1] < 110 and c[2] < 110
cols = [x for x in range(RW) if sum(redp(rp[x, y]) for y in range(0, RH, 8)) > 20]
rows = [y for y in range(RH) if sum(redp(rp[x, y]) for x in range(0, RW, 8)) > 20]
ex0, ey0, ex1, ey1 = d['edge']
s = (cols[-1] - cols[0] + 1) / (ex1 - ex0)
P = lambda x, y: (cols[0] + (x - ex0) * s, rows[0] + (y - ey0) * s)
a = P(X0, Y0); b = P(X1, Y1)
import numpy as np
dg = np.asarray(diag).astype(int); pm = (abs(dg[..., 0] - PAD[0]) < 8) & (abs(dg[..., 1] - PAD[1]) < 8) & (abs(dg[..., 2] - PAD[2]) < 8)
best = None
for dx in range(-50, 51, 2):
    for dy in range(-24, 25, 2):
        c = np.asarray(ren.crop((round(a[0] + dx), round(a[1] + dy), round(b[0] + dx), round(b[1] + dy))).resize(diag.size)).astype(int)
        gm = (c[..., 0] > 190) & (c[..., 1] > 150) & (c[..., 2] < 110)
        sc = (gm & pm).sum()
        if best is None or sc > best[0]: best = (sc, dx, dy)
print('render offset px', best)
dx, dy = best[1], best[2]
crop = ren.crop((round(a[0] + dx), round(a[1] + dy), round(b[0] + dx), round(b[1] + dy))).resize(diag.size, Image.LANCZOS)

# zoom insets: one disc/DIP pair and one resistor pair
def inset(o, half, S2):
    xs_ = [q[0] for c in o['polys'] for q in c]; ys_ = [q[1] for c in o['polys'] for q in c]
    cx, cy = (min(xs_) + max(xs_)) / 2, (min(ys_) + max(ys_)) / 2
    im, _ = panel(cx - half[0], cx + half[0], cy - half[1], cy + half[1], S2, crt_w=3, labels=False, depth=False)
    return im, (cx, cy)
oc = next(o for o in ov if {o['a'], o['b']} == {'C6', 'U7'})
orr = next(o for o in ov if {o['a'], o['b']} == {'R5', 'R6'})
in1, c1 = inset(oc, (4.2, 2.6), 90)
in2, c2 = inset(orr, (2.2, 2.6), 90)

# compose
pad = 36; gap = 36
ft = font(40, 'Bold'); fs = font(24); fh = font(27, 'SemiBold'); fsm = font(21)
head = 172
colw = diag.size[0]
insw = max(in1.size[0], in2.size[0])
W = pad + colw + gap + colw + gap + insw + pad
H = head + max(diag.size[1], in1.size[1] + in2.size[1] + 260) + 210
can = Image.new('RGB', (W, H), (255, 255, 255)); dr = ImageDraw.Draw(can)
dr.text((pad, 28), 'PicoGUS THT v1.3.2: the 12 courtyard overlaps', font=ft, fill=INK)
dr.text((pad, 84), 'The courtyard (blue) is each part\'s keep-out boundary for assembly. DRC flags any two that overlap (red).', font=fs, fill=(80, 80, 80))
x = pad; y = head
dr.text((x, y - 4), 'As built (3D render)', font=fh, fill=INK, anchor='lb'); can.paste(crop, (x, y + 8))
x2 = x + colw + gap
dr.text((x2, y - 4), 'Courtyards and overlaps', font=fh, fill=INK, anchor='lb'); can.paste(diag, (x2, y + 8))
dr.rectangle([x2, y + 8, x2 + colw - 1, y + 8 + diag.size[1] - 1], outline=(200, 200, 200))
x3 = x2 + colw + gap
dr.text((x3, y - 4), 'Zoom: disc capacitor between two DIPs', font=fh, fill=INK, anchor='lb')
can.paste(in1, (x3, y + 8)); dr.rectangle([x3, y + 8, x3 + in1.size[0] - 1, y + 8 + in1.size[1] - 1], outline=(200, 200, 200))
fz = font(23)
yy = y + 8 + in1.size[1] + 10
for ln in ('C6 (0.1 µF disc) sits in the gap between U10', 'and U7; its courtyard reaches 0.33 mm into', 'each DIP\'s. Bodies stay ~1.5 mm apart, pads 0.61 mm.'):
    dr.text((x3, yy), ln, font=fz, fill=(60, 60, 60)); yy += 30
yy += 34
dr.text((x3, yy), 'Zoom: vertical resistors end to end', font=fh, fill=INK, anchor='lt'); yy += 40
can.paste(in2, (x3, yy)); dr.rectangle([x3, yy, x3 + in2.size[0] - 1, yy + in2.size[1] - 1], outline=(200, 200, 200))
yy += in2.size[1] + 10
for ln in ('R5 and R6 stand vertically; the end pads are', '0.29 mm apart, and the courtyards overlap 0.20 mm.', 'Fine to solder, just with care.'):
    dr.text((x3, yy), ln, font=fz, fill=(60, 60, 60)); yy += 30

# legend + verdict
ly = H - 190
def sw(xx, col, fill=None, txt=''):
    if fill: dr.rectangle([xx, ly, xx + 34, ly + 22], fill=fill, outline=col, width=2)
    else: dr.rectangle([xx, ly, xx + 34, ly + 22], outline=col, width=3)
    dr.text((xx + 44, ly + 11), txt, font=fs, fill=INK, anchor='lm')
sw(pad, CRT, None, 'courtyard (F.CrtYd)')
sw(pad + 320, RED, (241, 150, 148), 'overlap (DRC error)')
sw(pad + 640, PAD, PAD, 'pad')
dr.line([pad + 820, ly + 11, pad + 854, ly + 11], fill=FAB, width=3); dr.text((pad + 864, ly + 11), 'part body (F.Fab)', font=fs, fill=INK, anchor='lm')
vy = ly + 48
for ln in ('Why accepted: these are through-hole parts placed by hand, so there is no pick-and-place head that needs the room.',
           '9 overlaps: a decoupling disc tucked between two DIPs (0.33 mm each side). 3 overlaps: vertical resistors end to end (0.20 mm).',
           'Copper clearances are all met (DRC finds no clearance errors); the overlaps only make jlcpcb.sh need FORCE=1.'):
    dr.text((pad, vy), ln, font=fsm, fill=(60, 60, 60)); vy += 30
can.save(OUT)
print(can.size, 'scale render px/mm', round(s, 2))
