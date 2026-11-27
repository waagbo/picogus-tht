#!/usr/bin/env python3
"""Draws docs/figures/v2.1.0-flash-write-mod.png: the two cuts and two wires that make the boot ROM flash writable on
a PicoGUS THT v2.1.0 board (see docs/v2.1.0-flash-write-mod.md).

    kipython hw-tht-v2.1/tools/fig_flash_write_mod.py V210_BOARD.kicad_pcb [OUT.png]

V210_BOARD is the v2.1.0 board file: git show d2338c3:hw-tht-v2.1/PicoGUS-THT.kicad_pcb > v210.kicad_pcb
(the copper of v2.1.0 did not change up to commit a8325fb). Needs KiCad's Python (pcbnew) and matplotlib.
"""
import os, sys, math
import pcbnew
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon, Circle, FancyBboxPatch
import matplotlib.patheffects as pe

T = pcbnew.ToMM
board = pcbnew.LoadBoard(sys.argv[1])
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'figures', 'v2.1.0-flash-write-mod.png')

fp = {f.GetReference(): f for f in board.GetFootprints()}
def pad(ref, num):
    p = fp[ref].FindPadByNumber(num).GetPosition(); return T(p.x), T(p.y)
P31, P32, C48P, C49P = pad('U15', '31'), pad('U15', '32'), pad('C48', '1'), pad('C49', '1')
B11 = [p for p in fp['J1'].Pads() if p.GetNumber() == '11'][0]
assert all(fp[r].FindPadByNumber('1').GetNetname() == '+5V' for r in ('C48', 'C49')) and fp['U15'].FindPadByNumber('31').GetNetname() == '+5V' and 'unconnected' in B11.GetNetname(), 'this is not a v2.1.0 board'
bb = B11.GetBoundingBox(); B11TOP = (T(B11.GetPosition().x), T(bb.GetTop()) + 0.5)
CUT_F = (122.25, 119.82)            # on the front track into pad 31
CUT_B = (124.63, 122.20)            # on the back track between pads 31 and 32

COPPER, COPPER5, PADC, FINGER, MASK = '#b9c6d8', '#e6c98a', '#c9a227', '#d4af37', '#f4f1ea'
WIRE1, WIRE2, CUT = '#d9480f', '#1c5fd4', '#c00000'
HALO = [pe.withStroke(linewidth=3, foreground='white')]

def draw(ax, layer, x0, y0, x1, y1, mirror, hl=('+5V',)):
    sx = -1 if mirror else 1
    ax.set_xlim(*(sorted((sx * x0, sx * x1)))); ax.set_ylim(y1, y0); ax.set_aspect('equal'); ax.axis('off')
    ax.add_patch(Polygon([(sx * x0, y0), (sx * x1, y0), (sx * x1, y1), (sx * x0, y1)], closed=True, fc=MASK, ec='none', zorder=0))
    fig = ax.figure; fig.canvas.draw()
    ppm = ax.get_window_extent().width / abs(x1 - x0) * 72 / fig.dpi          # points per mm
    for t in board.GetTracks():
        if t.GetClass() == 'PCB_VIA':
            x, y = T(t.GetPosition().x), T(t.GetPosition().y)
            if x0 - 1 < x < x1 + 1 and y0 - 1 < y < y1 + 1: ax.add_patch(Circle((sx * x, y), 0.3, fc='#9aa7b8', ec='none', zorder=2))
        elif t.GetLayer() == layer:
            xa, ya, xb, yb = T(t.GetStart().x), T(t.GetStart().y), T(t.GetEnd().x), T(t.GetEnd().y)
            if max(xa, xb) < x0 - 1 or min(xa, xb) > x1 + 1 or max(ya, yb) < y0 - 1 or min(ya, yb) > y1 + 1: continue
            five = t.GetNetname() in hl
            ax.plot([sx * xa, sx * xb], [ya, yb], color=COPPER5 if five else COPPER, lw=max(T(t.GetWidth()) * ppm, 0.6), solid_capstyle='round', zorder=3 if five else 1)
    for f in board.GetFootprints():
        for p in f.Pads():
            if not p.IsOnLayer(layer): continue
            c = p.GetPosition()
            if not (x0 - 4 < T(c.x) < x1 + 4 and y0 - 4 < T(c.y) < y1 + 6): continue
            sh = p.GetEffectivePolygon(layer)
            for i in range(sh.OutlineCount()):
                o = sh.Outline(i)
                ax.add_patch(Polygon([(sx * T(o.CPoint(k).x), T(o.CPoint(k).y)) for k in range(o.PointCount())], closed=True,
                                     fc=FINGER if f.GetReference() == 'J1' else PADC, ec='none', zorder=4))
            if p.GetDrillSize().x > 0: ax.add_patch(Circle((sx * T(c.x), T(c.y)), T(p.GetDrillSize().x) / 2, fc='white', ec='none', zorder=5))
    for d in board.GetDrawings():
        if d.GetLayer() == pcbnew.Edge_Cuts and d.GetClass() == 'PCB_SHAPE' and d.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            ax.plot([sx * T(d.GetStart().x), sx * T(d.GetEnd().x)], [T(d.GetStart().y), T(d.GetEnd().y)], color='#555', lw=1.2, zorder=6)
    return sx, ppm

def cut(ax, sx, pt, r=0.55):
    for dx, dy in ((1, 1), (1, -1)):
        ax.plot([sx * pt[0] - r * dx, sx * pt[0] + r * dx], [pt[1] - r * dy, pt[1] + r * dy], color=CUT, lw=3.2, solid_capstyle='round', zorder=20, path_effects=[pe.withStroke(linewidth=6, foreground='white')])

def note(ax, sx, pt, text, to, color='#111', ha='center', size=10.5, weight='bold'):
    ax.annotate(text, xy=(sx * pt[0], pt[1]), xytext=(sx * to[0], to[1]), ha=ha, va='center', fontsize=size, color=color, weight=weight, zorder=30,
                arrowprops=dict(arrowstyle='-', color=color, lw=1.1, shrinkA=2, shrinkB=3), path_effects=HALO)

fig = plt.figure(figsize=(15, 12.6), dpi=130)
fig.patch.set_facecolor('white')
fig.text(0.03, 0.978, 'PicoGUS THT v2.1.0: making the boot ROM flash writable', fontsize=18, weight='bold', va='top')
fig.text(0.03, 0.951, 'Two cuts and two wires, for cards made from the v2.1.0 files. From v2.1.1 on this is in the copper. Not tested on a card yet.', fontsize=11, va='top', color='#444', style='italic')
# ---------------- panel A: front, the cut at pad 31
fig.text(0.03, 0.925, 'A   FRONT: cut the track into pad 31', fontsize=12.5, weight='bold', va='top')
fig.text(0.03, 0.903, 'Do this before the PLCC socket is soldered: it covers this spot.', fontsize=10.5, va='top', color='#333')
axA = fig.add_axes([0.03, 0.555, 0.34, 0.33])
sx, _ = draw(axA, pcbnew.F_Cu, 116.6, 114.9, 130.6, 126.4, False)
cut(axA, sx, CUT_F, 0.42)
note(axA, sx, P31, 'pad 31\n(write enable)', (127.6, 117.4))
note(axA, sx, P32, 'pad 32 (+5 V)', (127.9, 125.6))
note(axA, sx, CUT_F, 'CUT 1', (119.6, 123.2), CUT, size=14)
fig.text(0.03, 0.548, 'Tan: +5 V copper. Grey: other tracks and vias. Gold: pads.', fontsize=9.5, va='top', color='#555')
# ---------------- text
axT = fig.add_axes([0.40, 0.535, 0.58, 0.39]); axT.axis('off'); axT.set_xlim(0, 1); axT.set_ylim(0, 1)
steps = [
    ('CUT 1', CUT, 'Front. Cut the 0.4 mm track where it runs into pad 31 of the flash\nsocket U15 (panel A).'),
    ('CUT 2', CUT, 'Back. Cut the thin track between pads 31 and 32 of U15 (panel B).'),
    ('CHECK', '#111', 'With a meter: pad 31 no longer connects to pad 32.\nThen solder the card as usual.'),
    ('WIRE 1', WIRE1, 'Back. +5 V bridge between the capacitors C48 and C49 above the socket:\njoin their two outer pins (the inner two are GND). Without it U6 and U17\nhave no supply: pad 31 was their only +5 V link.'),
    ('WIRE 2', WIRE2, 'Back. Write enable: U15 pad 31 to the top end of ISA finger B11 (SMEMW),\nthe 11th finger from the bracket end. Thin insulated wire; solder only on\nthe top millimetre of the finger, flat, so the card still goes fully into\nthe slot. Fix the wire with a drop of glue or tape.'),
    ('CHECK', '#111', 'Pad 31 connects to finger B11 and not to +5 V or GND.\nU6 pin 14 connects to +5 V (finger B29 or B3).'),
]
y = 1.0
for tag, col, txt in steps:
    axT.text(0.0, y, tag, fontsize=12.5, weight='bold', color=col, va='top')
    axT.text(0.125, y, txt, fontsize=11.5, va='top', linespacing=1.35)
    y -= 0.045 + 0.062 * (txt.count('\n') + 1)
# ---------------- panel B: back
fig.text(0.03, 0.512, 'B   BACK (solder side), as you see it with the card turned over: the bracket is on the left', fontsize=12.5, weight='bold', va='top')
axB = fig.add_axes([0.03, 0.012, 0.95, 0.475])
X0, X1, Y0, Y1 = 114.6, 178.6, 99.2, 138.6
sx, ppm = draw(axB, pcbnew.B_Cu, X0, Y0, X1, Y1, True)
cut(axB, sx, CUT_B, 0.5)
w1 = [C49P, (134.2, 105.3), (120.8, 105.3), C48P]
w2 = [P31, (122.0, 122.3), (122.0, 126.6), (168.6, 126.6), B11TOP]
for pts, col in ((w1, WIRE1), (w2, WIRE2)):
    axB.plot([sx * p[0] for p in pts], [p[1] for p in pts], color=col, lw=3.0, solid_capstyle='round', solid_joinstyle='round', zorder=25, path_effects=[pe.withStroke(linewidth=5.5, foreground='white')])
    for p in (pts[0], pts[-1]): axB.add_patch(Circle((sx * p[0], p[1]), 0.62, fc=col, ec='white', lw=1.2, zorder=26))
def label(pt, text, color='#111', size=10.5, ha='center', weight='bold'):
    axB.text(sx * pt[0], pt[1], text, fontsize=size, ha=ha, va='center', color=color, weight=weight, zorder=30, path_effects=HALO, linespacing=1.15)
note(axB, sx, CUT_B, 'CUT 2', (118.6, 123.6), CUT, size=14)
note(axB, sx, P31, 'U15 pad 31', (119.4, 118.2))
note(axB, sx, P32, 'U15 pad 32\n(+5 V)', (130.2, 124.3), size=10)
note(axB, sx, C48P, 'C48\n+5 V pin', (117.6, 101.0), size=10)
note(axB, sx, C49P, 'C49\n+5 V pin', (138.4, 101.0), size=10)
label((127.5, 106.5), 'WIRE 1: +5 V bridge', WIRE1, 12)
label((146.0, 127.75), 'WIRE 2: write enable (SMEMW)', WIRE2, 12)
note(axB, sx, B11TOP, 'ISA finger B11:\nsolder on its top end only', (175.4, 126.3), WIRE2, size=10.5)
bx = lambda n: 195.0 - (n - 1) * 2.54
for n in (10, 11, 12):
    axB.text(sx * bx(n), 133.4, 'B%d' % n, fontsize=9.5, ha='center', va='center', weight='bold', color='#3b2f00', zorder=30)
label((177.9, 137.5), '← bracket end (B1)', '#444', 10, ha='left', weight='normal')
label((117.0, 137.5), 'B31 →', '#444', 10, ha='right', weight='normal')
label((127.2, 116.4), 'pins of the flash socket U15\n(U16 sits between them)', '#333', 9.5, weight='normal')
fig.savefig(out, facecolor='white')
print('wrote', out)
