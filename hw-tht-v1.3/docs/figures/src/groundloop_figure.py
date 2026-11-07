# usage:  python3 groundloop_figure.py gl.json gl.npz groundloop.png
#   gl.json / gl.npz  from groundloop_solve.py run on this board: the maps, the U11 values and every v1.3.2 number in
#                     the charts and the footer come from there; the 8dbb6c6 numbers are the README's (block BENCH).
# needs Pillow, numpy and scipy; Inter fonts in /usr/share/fonts/opentype/inter/ (falls back to DejaVu Sans).
# Draws the figure that explains the accepted line-out ground-loop trade-off (loop to the bracket vs to the fingers).
import sys, json
import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw, ImageFont

res = json.load(open(sys.argv[1]))
z = np.load(sys.argv[2])
OUT = sys.argv[3]

# --- numbers printed in the charts, worst channel ---
# 8dbb6c6: the README's table ("Line-out ground error ...") and its "Known weaker points" 10 mA values
BENCH = {'fingers': {'noWT': 410, 'WT': 715},  # uV per A of loop current, J8 sleeve -> fingers
         'bracket': {'noWT': 185, 'WT': 166},  # uV per A, J8 sleeve -> H1/H2
         'usb': {'noWT': 226, 'WT': 314},      # uV at 500 mA of USB-host load
         'hum10mA': {'bracket': 1.8, 'fingers': 4.1}}  # uV at 10 mA of loop current, no Waveblaster
# v1.3.2: this board's model run (gl.json), worst of the two channels
worst = lambda case, k: max(abs(res['cases'][case]['hum'][ch]['e_noWT' if k == 'noWT' else 'e_WTmax']) for ch in 'LR')
README = {c: {k: (BENCH[c][k], round(worst(c, k))) for k in ('noWT', 'WT')} for c in ('fingers', 'bracket', 'usb')}
README['hum10mA'] = {'v132': {c: round(worst(c, 'noWT') * 0.010, 1) for c in ('bracket', 'fingers')}, 'bench': BENCH['hum10mA']}
print('v1.3.2 worst channel (8dbb6c6, v1.3.2):', {c: README[c] for c in ('fingers', 'bracket', 'usb')}, '10 mA:', README['hum10mA']['v132'])
DAC_NOISE = 2.1 / 10 ** (100 / 20) * 1e6  # PCM5100A: 2.1 V rms full scale, 100 dB dynamic range -> 21 uV
CABLE = (0.1 * 0.010 * 1e6, 0.3 * 0.010 * 1e6)  # 10 mA through a 0.1-0.3 ohm cable shield (assumed), uV

F = '/usr/share/fonts/opentype/inter/'
def font(sz, w='Medium'):
    try: return ImageFont.truetype(F + 'Inter-%s.otf' % w, sz)
    except OSError: return ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', sz)

INK = (30, 32, 36); GREY = (80, 80, 80); MUTED = (120, 120, 120)
BOARD = (226, 226, 220); OTHER = (190, 190, 186); BG = (255, 255, 255)
BLUE = (30, 110, 220); OLD = (170, 170, 170); GOLD = (214, 168, 40); RED = (229, 57, 53)
VIR = np.array([(68, 1, 84), (72, 24, 106), (71, 45, 123), (66, 64, 134), (59, 82, 139), (51, 99, 141), (44, 114, 142),
                (38, 130, 142), (33, 145, 140), (31, 160, 136), (40, 174, 128), (63, 188, 115), (94, 201, 98),
                (132, 212, 75), (173, 220, 48), (216, 226, 25), (253, 231, 37)], float)  # viridis, 17 steps


def cmap(t):  # t in [0,1]: 0 = at the sleeve (yellow) ... 1 = far below it (purple)
    u = np.clip(1 - t, 0, 1) * (len(VIR) - 1)
    i = np.minimum(np.floor(u).astype(int), len(VIR) - 2); f = (u - i)[..., None]
    return VIR[i] * (1 - f) + VIR[i + 1] * f


QMAX, STEP, MAJOR = 2400.0, 100.0, 500.0  # colour range, contour steps (uV per A below the sleeve)
cov, other, board, holes = z['cov'], z['other'], z['board'], z['holes']
P, X0, Y0 = float(z['P']), float(z['x0']), float(z['y0'])
pads = res['meta']['pads']
SS = 2  # supersampling for the contours


def potential_map(case):
    V = z['V_' + case].astype(float)
    vs = res['cases'][case]['V_uV']['J8.S'] * 1e-6
    q = (vs - V) * 1e6  # uV per A below J8's sleeve
    w = np.where(np.isfinite(q), cov, 0.0)
    qs = np.nansum(np.where(np.isfinite(q), q, 0) * w, axis=0) / np.maximum(w.sum(axis=0), 1e-9)
    g = np.clip(w.max(axis=0), 0, 1)
    valid = g > 0.02
    # colour image at grid resolution, then supersampled
    rgb = np.zeros(g.shape + (3,)); rgb[:] = BG
    rgb = rgb * (1 - board[..., None]) + np.array(BOARD) * board[..., None]
    oc = np.clip(other.max(axis=0), 0, 1)[..., None]
    rgb = rgb * (1 - oc) + np.array(OTHER) * oc
    rgb = rgb * (1 - g[..., None]) + cmap(qs / QMAX) * g[..., None]
    hc = holes[..., None]; rgb = rgb * (1 - hc) + np.array(BG) * hc
    big = np.repeat(np.repeat(rgb, SS, 0), SS, 1)
    # equipotential lines: nearest-filled potential, bilinear upsampled, band edges inside GND copper
    idx = ndimage.distance_transform_edt(~valid, return_distances=False, return_indices=True)
    qf = qs[idx[0], idx[1]]
    q2 = ndimage.zoom(qf, SS, order=1)
    m2 = ndimage.binary_erosion(np.repeat(np.repeat(g > 0.5, SS, 0), SS, 1), iterations=1)
    lines = np.zeros(q2.shape); majors = np.zeros(q2.shape)
    for st, arr in ((STEP, lines), (MAJOR, majors)):
        b = np.floor(q2 / st)
        e = np.zeros(q2.shape, bool)
        e[:, :-1] |= b[:, :-1] != b[:, 1:]; e[:-1, :] |= b[:-1, :] != b[1:, :]
        arr[e & m2] = 1
    lines = ndimage.binary_dilation(lines > 0, iterations=1)
    majors = ndimage.binary_dilation(majors > 0, iterations=2)
    a = (0.55 * lines)[..., None]; big = big * (1 - a) + np.array((20, 20, 20)) * a
    a = (0.72 * majors)[..., None]; big = big * (1 - a) + np.array((20, 20, 20)) * a
    im = Image.fromarray(np.clip(big, 0, 255).astype(np.uint8)).resize((rgb.shape[1], rgb.shape[0]), Image.LANCZOS)
    return im


def T(x, y):  # board mm -> map px (1 px per 0.1 mm cell)
    return ((x - X0) / P, (y - Y0) / P)


def label(dr, xy, txt, fnt, anchor='lm', fill=INK, box=True):
    tb = dr.textbbox(xy, txt, font=fnt, anchor=anchor)
    if box:
        dr.rounded_rectangle([tb[0] - 6, tb[1] - 4, tb[2] + 6, tb[3] + 4], radius=5, fill=(255, 255, 255), outline=(150, 150, 150), width=1)
    dr.text(xy, txt, font=fnt, fill=fill, anchor=anchor)
    return tb


def leader(dr, a, b, col=INK):
    dr.line([a, b], fill=col, width=2)


def dot(dr, p, r, fill=(255, 255, 255), outline=INK, width=3):
    dr.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=fill, outline=outline, width=width)


fl = font(23, 'SemiBold'); fls = font(20, 'Medium')


def annotate(im, case):
    dr = ImageDraw.Draw(im)
    pj = T(*pads['J8.S']); p3 = T(*pads['U11.3']); p5 = T(*pads['U11.5']); p9 = T(*pads['U9.9'])
    h1 = T(*pads['H1.1']); h2 = T(*pads['H2.1'])
    # holes H1/H2 (6.4 mm pads)
    for h, n in ((h1, 'H1'), (h2, 'H2')):
        r = 3.2 / P
        w = 6 if case == 'bracket' else 3
        dr.ellipse([h[0] - r, h[1] - r, h[0] + r, h[1] + r], outline=(255, 255, 255), width=w + 4)
        dr.ellipse([h[0] - r, h[1] - r, h[0] + r, h[1] + r], outline=RED if case == 'bracket' else INK, width=w)
    t = '0.5 A out' if case == 'bracket' else 'no current'
    label(dr, (h1[0] - 50, h1[1] - 14), 'H1', fl, 'rm'); label(dr, (h1[0] - 50, h1[1] + 16), t, fls, 'rm', box=False if False else True)
    label(dr, (h2[0] - 50, h2[1] - 14), 'H2', fl, 'rm'); label(dr, (h2[0] - 50, h2[1] + 16), t, fls, 'rm')
    # J8 sleeve
    dot(dr, pj, 13, fill=(255, 255, 255), outline=RED, width=5)
    # below the sleeve when H1's labels sit just above it (J8 is the lower jack on v1.3.2)
    below = abs(pads['J8.S'][1] - pads['H1.1'][1]) < 12
    dyl = 60 if below else -60
    label(dr, (pj[0] - 30, pj[1] + dyl), 'J8 sleeve: 1 A in', fl, 'rm')
    leader(dr, (pj[0] - 28, pj[1] + dyl - (8 if not below else -8) * (1 if not below else 1)), (pj[0] - 10, pj[1] + (14 if below else -14)))
    # U11 pins 3 / 5 and U9.9
    for p, n, off in ((p3, 'U11.3', (0, 52)), (p5, 'U11.5', (0, -40))):
        q = (p[0] + off[0], p[1] + off[1])
        leader(dr, (q[0], q[1] - (12 if off[1] > 0 else -12)), p)
        label(dr, q, n, fl, 'mm')
        dot(dr, p, 9, outline=INK, width=3)
    q = (p9[0], p9[1] + 50)
    leader(dr, (q[0], q[1] - 12), p9); label(dr, q, 'U9.9 (DAC ground)', fls, 'mm'); dot(dr, p9, 8, outline=INK, width=3)
    # GND fingers
    fy = 128.4
    for k in ('J3.1', 'J3.10', 'J3.31'):
        x = pads[k][0]
        a = T(x, fy); dr.polygon([(a[0] - 11, a[1] - 18), (a[0] + 11, a[1] - 18), (a[0], a[1])],
                                 fill=RED if case == 'fingers' else (255, 255, 255), outline=INK)
    tf = 'ISA GND fingers B1 / B10 / B31: 1 A out' if case == 'fingers' else 'ISA GND fingers: no current'
    label(dr, T(156.0, 124.6), tf, fls, 'mm')
    return im


maps = {c: annotate(potential_map(c), c) for c in ('bracket', 'fingers')}
mw, mh = maps['bracket'].size

# --- canvas ---
pad, gap = 36, 40
W = pad + mw + gap + mw + pad
ft = font(40, 'Bold'); fs = font(24); fh = font(27, 'SemiBold'); fsm = font(21); fz = font(22)
head = 150
ymap = head + 122
ychart = ymap + mh + 120
H = ychart + 500 + 180
can = Image.new('RGB', (W, H), BG); dr = ImageDraw.Draw(can)
dr.text((pad, 28), 'PicoGUS THT v1.3.2: line-out ground loop to the bracket or to the fingers', font=ft, fill=INK)
dr.text((pad, 84), "A ground-loop current (cable shield, earthed amplifier) enters at the line-out jack's sleeve J8 and leaves by the bracket or the ISA fingers.",
        font=fs, fill=GREY)
dr.text((pad, 116), "The ground drop it makes between the mixer's reference pins (U11.3 left, U11.5 right) and the sleeve is added to the signal at the jack.",
        font=fs, fill=GREY)


def u11(case):
    h = res['cases'][case]['hum']
    return -h['L']['sleeve'], -h['R']['sleeve']


xA, xB = pad, pad + mw + gap
for x, case, ttl, sub in ((xA, 'bracket', 'A  Loop to the bracket (tab holes H1 / H2)', "1 A in at J8's sleeve, 0.5 A out at H1 and at H2 (as in the README's model)"),
                          (xB, 'fingers', 'B  Loop to the ISA GND fingers', "1 A in at J8's sleeve, 1 A out at the GND fingers")):
    dr.text((x, ymap - 80), ttl, font=fh, fill=INK, anchor='lb')
    a, b = u11(case)
    h = res['cases'][case]['hum']
    dr.text((x, ymap - 46), sub, font=fz, fill=GREY, anchor='lb')
    t2 = 'U11.3 / U11.5: %.0f / %.0f µV/A below the sleeve; at the jack %.0f / %.0f µV/A (L / R, no Waveblaster)' % (
        a, b, abs(h['L']['e_noWT']), abs(h['R']['e_noWT']))
    dr.text((x, ymap - 14), t2, font=fz, fill=GREY, anchor='lb')
    if dr.textlength(t2, font=fz) > mw: print('WARNING: panel subtitle wider than the map:', t2)
    can.paste(maps[case], (x, ymap)); dr.rectangle([x, ymap, x + mw - 1, ymap + mh - 1], outline=(200, 200, 200))

# colour bar + map legend
cy = ymap + mh + 26
cbw = 620
bar = cmap(np.linspace(0, 1, cbw))[None, :, :].repeat(22, 0)
can.paste(Image.fromarray(bar.astype(np.uint8)), (pad, cy))
dr.rectangle([pad, cy, pad + cbw - 1, cy + 21], outline=(150, 150, 150))
for v in range(0, int(QMAX) + 1, 500):
    xx = pad + v / QMAX * (cbw - 1)
    dr.line([xx, cy + 22, xx, cy + 30], fill=INK, width=2)
    dr.text((xx, cy + 32), '%d' % v, font=fsm, fill=INK, anchor='mt')
dr.text((pad + cbw + 18, cy + 11), 'µV per A of loop current below J8\'s sleeve (GND copper, both layers)', font=fsm, fill=INK, anchor='lm')
dr.text((pad + cbw + 18, cy + 44), 'thin lines: every 100 µV/A, bold: every 500; current crosses them at right angles',
        font=fsm, fill=GREY, anchor='lm')
lx = pad + cbw + 18 + 860
dr.rectangle([lx, cy, lx + 34, cy + 22], fill=OTHER, outline=(150, 150, 150)); dr.text((lx + 44, cy + 11), 'other nets', font=fsm, fill=INK, anchor='lm')
dr.rectangle([lx, cy + 33, lx + 34, cy + 55], fill=BOARD, outline=(150, 150, 150)); dr.text((lx + 44, cy + 44), 'no copper', font=fsm, fill=INK, anchor='lm')

# --- bar charts: README numbers, small multiples (one unit each) ---
yc = ychart
dr.text((pad, yc), 'C  Line-out ground error at the jack, worst channel', font=fh, fill=INK, anchor='lt')
lgx = pad
dr.rounded_rectangle([lgx, yc + 46, lgx + 30, yc + 68], radius=4, fill=OLD); dr.text((lgx + 40, yc + 57), '8dbb6c6 (103.7 mm layout)', font=fsm, fill=INK, anchor='lm')
dr.rounded_rectangle([lgx + 330, yc + 46, lgx + 360, yc + 68], radius=4, fill=BLUE); dr.text((lgx + 370, yc + 57), 'v1.3.2 (this board)', font=fsm, fill=INK, anchor='lm')
PH = 260  # plot height
by0 = yc + 140 + PH  # baseline
subw = 384
VM = max(800.0, 200.0 * -(-max(v for c_ in ('fingers', 'bracket', 'usb') for t_ in README[c_].values() for v in t_) // 200))
def verdict_of(key):
    b = [README[key][k][1] < README[key][k][0] for k in ('noWT', 'WT')]
    if all(b):
        return 'v1.3.2 better'
    if not any(b):
        return 'v1.3.2 worse'
    return 'v1.3.2 %s without, %s with WT' % ('better' if b[0] else 'worse', 'better' if b[1] else 'worse')


for k, (key, ttl, unit) in enumerate((('fingers', 'Loop to the fingers', 'µV per A'),
                                      ('bracket', 'Loop to the bracket', 'µV per A'),
                                      ('usb', 'USB-host load 500 mA', 'µV'))):
    verdict = verdict_of(key)
    better = [README[key][k_][1] < README[key][k_][0] for k_ in ('noWT', 'WT')]
    x0 = pad + k * (subw + 18)
    ax = x0 + 58
    dr.text((x0, yc + 92), ttl, font=font(22, 'SemiBold'), fill=INK, anchor='lt')
    dr.text((x0 + dr.textlength(ttl, font=font(22, 'SemiBold')) + 10, yc + 93), '(%s)' % unit, font=fsm, fill=GREY, anchor='lt')
    for v in range(0, int(VM) + 1, 200):
        yy = by0 - v / VM * PH
        dr.line([ax, yy, x0 + subw - 10, yy], fill=(225, 225, 225) if v else (150, 150, 150), width=1 if v else 2)
        dr.text((ax - 8, yy), '%d' % v, font=font(18), fill=MUTED, anchor='rm')
    bw, gp = 62, 4
    for j, (cs, glab) in enumerate((('noWT', 'no Waveblaster'), ('WT', 'Waveblaster full'))):
        gx = ax + 28 + j * 160
        for i, (val, col) in enumerate(zip(README[key][cs], (OLD, BLUE))):
            bx = gx + i * (bw + gp)
            top = by0 - val / VM * PH
            if by0 - 1 - top >= 10:
                dr.rounded_rectangle([bx, top, bx + bw, by0 - 1], radius=4, fill=col)
                dr.rectangle([bx, top + 6, bx + bw, by0 - 1], fill=col)
            else:                       # too short for rounded corners
                dr.rectangle([bx, top, bx + bw, by0 - 1], fill=col)
            dr.text((bx + bw / 2, top - 6), '%d' % val, font=font(20, 'SemiBold'), fill=INK, anchor='mb')
        dr.text((gx + bw + gp / 2, by0 + 10), glab, font=font(19), fill=INK, anchor='mt')
        if all(better) or not any(better):
            continue
        # mixed: one verdict per group
        dr.text((gx + bw + gp / 2, by0 + 38), 'v1.3.2 ' + ('better' if better[j] else 'worse'), font=font(20, 'SemiBold'),
                fill=INK if better[j] else RED, anchor='mt')
    if all(better) or not any(better):
        dr.text((ax + 28, by0 + 40), verdict, font=font(20, 'SemiBold'), fill=RED if 'worse' in verdict else INK, anchor='lt')

# --- scale chart (log): 10 mA of loop current, no Waveblaster ---
sx = pad + 3 * (subw + 18) + 36
dr.text((sx, yc), 'D  How big? 10 mA of loop current, no Waveblaster', font=fh, fill=INK, anchor='lt')
lab_w = 250
lx0 = sx + lab_w; lx1 = W - pad - 24
lo, hi = 0.0, 4.0  # log10 uV: 1 uV .. 10 mV
X = lambda uv: lx0 + (np.log10(uv) - lo) / (hi - lo) * (lx1 - lx0)
ry0 = yc + 70; rh = 62
gb = ry0 + 5 * rh + 6
for e, t in ((0, '1 µV'), (1, '10 µV'), (2, '100 µV'), (3, '1 mV'), (4, '10 mV')):
    xx = X(10 ** e)
    dr.line([xx, ry0 - 6, xx, gb], fill=(225, 225, 225), width=1)
    dr.text((xx, gb + 8), t, font=font(18), fill=MUTED, anchor='mt')
x100 = X(10.0)  # -100 dBV
for yy in range(int(ry0 - 6), int(gb), 10):
    dr.line([x100, yy, x100, yy + 5], fill=INK, width=2)
dr.text((x100 + 6, ry0 + 4 * rh + 20), '−100 dBV', font=font(18, 'SemiBold'), fill=INK, anchor='lt')
h10 = README['hum10mA']
# dots (single values) and ranges on a log axis: no bars, a bar's length means nothing on a log scale
v1, v0 = h10['v132'], h10['bench']
def span(v):
    (a_, x_), (b_, y_) = sorted(v.items(), key=lambda kv: kv[1])
    return '%.1f µV %s – %.1f %s' % (x_, a_, y_, b_)


rows = [('v1.3.2 (this board)', sorted(v1.values()), BLUE, span(v1), 'r'),
        ('8dbb6c6', sorted(v0.values()), OLD, span(v0), 'r'),
        ('DAC noise floor', [DAC_NOISE], GOLD, '≈ %.0f µV (PCM5100A: 2.1 V rms, 100 dB)' % DAC_NOISE, 'r'),
        ('cable shield 0.1–0.3 Ω', list(CABLE), RED, '%.0f–%.0f mV' % (CABLE[0] / 1000, CABLE[1] / 1000), 'l')]
fb = font(20, 'SemiBold')
for i, (nm, vals, col, txt, side) in enumerate(rows):
    yy = ry0 + i * rh + 25
    dr.text((sx, yy), nm, font=font(21, 'Medium'), fill=INK, anchor='lm')
    xa, xb = X(vals[0]), X(vals[-1])
    if len(vals) > 1:
        dr.rounded_rectangle([xa, yy - 5, xb, yy + 5], radius=5, fill=col)
    for xx in {xa, xb}:
        dr.ellipse([xx - 10, yy - 10, xx + 10, yy + 10], fill=col, outline=(255, 255, 255), width=2)
    if side == 'r':
        tb = dr.textbbox((xb + 18, yy), txt, font=fb, anchor='lm')
        dr.rectangle([tb[0] - 4, tb[1] - 3, tb[2] + 4, tb[3] + 3], fill=BG)
        dr.text((xb + 18, yy), txt, font=fb, fill=INK, anchor='lm')
    else:
        tb = dr.textbbox((xa - 18, yy), txt, font=fb, anchor='rm')
        dr.rectangle([tb[0] - 4, tb[1] - 3, tb[2] + 4, tb[3] + 3], fill=BG)
        dr.text((xa - 18, yy), txt, font=fb, fill=INK, anchor='rm')
r_lo = CABLE[0] / max(h10['v132'].values()); r_hi = CABLE[1] / max(h10['v132'].values())
dr.text((sx, gb + 44), "The cable shield's own drop is %d–%d× what this board adds." % (round(r_lo, -2), round(r_hi, -2)),
        font=fz, fill=GREY, anchor='lt')

# --- why accepted ---
vy = H - 180
hv = README['hum10mA']['v132']
hmax = max(hv, key=hv.get); hmin = min(hv, key=hv.get)
R_ = {c: README[c] for c in ('fingers', 'bracket', 'usb')}
def cmp(c, k, unit):
    a_, b_ = R_[c][k][1], R_[c][k][0]
    return '%s (%d against %d %s)' % ('better' if a_ < b_ else 'worse', a_, b_, unit)
for ln in ("Why accepted: with 10 mA of loop current the board's ground adds at most %.1f µV at the jack (through the %s; %.1f µV through the %s): below −100 dBV,"
           % (hv[hmax], hmax, hv[hmin], hmin),
           "below the DAC's noise floor and %d–%d× less than the drop along the cable shield itself. Against 8dbb6c6, without / with a Waveblaster: fingers %s / %s,"
           % (round(r_lo, -2), round(r_hi, -2), cmp('fingers', 'noWT', 'µV/A').split(' (')[0], cmp('fingers', 'WT', 'µV/A').split(' (')[0]),
           "bracket %s / %s, USB load %s / %s." % (cmp('bracket', 'noWT', 'µV/A'), cmp('bracket', 'WT', 'µV/A'), cmp('usb', 'noWT', 'µV'), cmp('usb', 'WT', 'µV')),
           "Bracket path (A): it exists only with a metal bracket whose screws tie H1/H2 to the PC chassis; with a 3D-printed (plastic) bracket, which this card's longer tabs call for, H1/H2 carry no loop current.",
           "Model: the README's DC copper model (refilled pours, 0.1 mm grid, Pico GND pins tied); e = (Vref − Vsleeve) + 0.977·(Vref − V(U9.9)) + 2·(Vref − Vwt). Cable shield: 0.1–0.3 Ω assumed."):
    dr.text((pad, vy), ln, font=fsm, fill=(60, 60, 60)); vy += 30
    if dr.textlength(ln, font=fsm) > W - 2 * pad: print('WARNING: footer line too long:', ln)
can.save(OUT)
print(can.size)
