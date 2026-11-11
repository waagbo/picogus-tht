# python3 placement_options_figure.py DATADIR OUT.png
# DATADIR holds cmp_A.json, cmp_A2.json, cmp_C.json, cmp_D.json from placement_options_dump.py (one per candidate board;
# the candidate boards were hand placements exchanged during the 2026-10-31 placement work and are not in git).
# The table values are from rats.py / zones.py / quick_i2s.py runs on the same boards.
import json, sys
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon as MP
from matplotlib import font_manager as fm
for f in ('Regular', 'SemiBold', 'Bold'):
    try: fm.fontManager.addfont('/usr/share/fonts/opentype/inter/Inter-%s.otf' % f)
    except Exception: pass
plt.rcParams['font.family'] = 'Inter'
d = sys.argv[1]
RED = '#e53935'; TEAL = '#00897b'; PUR = '#8e44ad'; BLUE = '#2f6fde'; INK = '#1e2024'; GRID = '#c8c8c8'; GREY = '#6b6b6b'
fig = plt.figure(figsize=(24, 15), dpi=90)
T = {'A': 'A: DAC block high in the middle', 'A2': 'A2: A with the DAC block turned 90°', 'C': 'C: turned block, mixer beside it', 'D': 'D: DAC above the Pico (this board)'}
LBL = {'U9': 'DAC', 'U11': 'mixer', 'U1': 'Pico', 'U8': 'U8', 'J5': 'MIDI', 'J8': 'LINE', 'J9': 'Waveblaster', 'U3': 'U3', 'U4': 'U4'}
for i, m in enumerate(['A', 'A2', 'C', 'D']):
    D = json.load(open('%s/cmp_%s.json' % (d, m)))
    ax = fig.add_axes([0.01 + i * 0.247, 0.42, 0.237, 0.50])
    ax.add_patch(MP(D['outline'], closed=True, fc='#fafaf8', ec='#888888', lw=1.0))
    for r, f in D['fps'].items():
        for c in f['crt']:
            if len(c) > 2: ax.add_patch(MP(c, closed=True, fc='none', ec=GRID, lw=0.5))
    ax.add_patch(MP(D['dac'], closed=True, fc=BLUE, alpha=0.14, ec=BLUE, lw=1.4))
    ax.add_patch(MP(D['mix'], closed=True, fc=PUR, alpha=0.08, ec=PUR, lw=1.1, ls='--'))
    for e in D['edges']:
        (x0, y0), (x1, y1) = e['a'], e['b']
        if e['k'] == 'i2s': ax.plot([x0, x1], [y0, y1], color=TEAL, lw=1.8, zorder=4)
        elif e['thr']: ax.plot([x0, x1], [y0, y1], color=RED, lw=1.4, zorder=5)
        elif e['k'] == 'audio': ax.plot([x0, x1], [y0, y1], color=PUR, lw=0.7, alpha=0.7, zorder=3)
    for r, t in LBL.items():
        f = D['fps'][r]; xs = [p[0] for c in f['crt'] for p in c] or [f['x']]; ys = [p[1] for c in f['crt'] for p in c] or [f['y']]
        ax.text((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, t, ha='center', va='center', fontsize=10, color=INK, weight='semibold', zorder=6,
                bbox=dict(boxstyle='round,pad=0.12', fc='white', ec='none', alpha=0.75))
    ax.set_xlim(115.5, 219); ax.set_ylim(137, 35.5); ax.set_aspect('equal'); ax.axis('off')
    ax.set_title(T[m], fontsize=14, weight='bold', color=INK, loc='left', pad=8)
fig.text(0.01, 0.975, 'Four placements of the DAC, mixer and jacks', fontsize=22, weight='bold', color=INK, va='top')
fig.text(0.01, 0.945, 'Ratsnest (straight pad-to-pad connections) before routing; every candidate has the DAC discs on their own pins. D was chosen and routed.', fontsize=13, color=GREY, va='top')
lx, ly = 0.01, 0.395
for col, lab, ls in ((BLUE, 'DAC area', '-'), (PUR, 'mixer area', '--')):
    fig.patches.append(MP([(lx, ly), (lx + 0.016, ly), (lx + 0.016, ly + 0.011), (lx, ly + 0.011)], closed=True, transform=fig.transFigure, fc=col, alpha=0.2, ec=col, ls=ls))
    fig.text(lx + 0.021, ly + 0.0055, lab, fontsize=12, va='center', color=INK); lx += 0.09
for col, lab in ((RED, 'digital ratsnest crossing the DAC area'), (TEAL, 'I2S'), (PUR, 'audio path')):
    fig.lines.append(plt.Line2D([lx, lx + 0.018], [ly + 0.0055] * 2, transform=fig.transFigure, color=col, lw=2.5))
    fig.text(lx + 0.023, ly + 0.0055, lab, fontsize=12, va='center', color=INK); lx += 0.21 if 'digital' in lab else 0.07
rows = [('', 'A', 'A2', 'C', 'D', 'B (DAC lowest)'),
        ('I2S, trial route of BCK+DIN+LRCK alone, DAC outputs kept clear', '171 mm', '232 mm', '238 mm', '122 mm', '-'),
        ('I2S ratsnest', '163 mm', '161 mm', '161 mm', '114 mm', '110 mm'),
        ('DAC area to the Pico / to U8 (PSRAM)', '20.2 / 19.0 mm', '11.9 / 18.3 mm', '11.9 / 18.3 mm', '7.3 / 5.1 mm', '2.2 / 13.1 mm'),
        ('Digital ratsnest through the DAC area / mixer area', '68 / 0 mm', '102 / 0 mm', '102 / 0 mm', '73 / 11 mm', '134 / 11 mm'),
        ('DAC to mixer (AUDIO_L+R) / Waveblaster WT_L+WT_R', '87 / 113 mm', '73 / 113 mm', '61 / 103 mm', '51 / 108 mm', '86 / 140 mm'),
        ('Mixer summing nodes U11A- / U11B-', '48 / 49 mm', '48 / 49 mm', '42 / 44 mm', '47 / 49 mm', '42 / 44 mm'),
        ('Line out / +-12V and +-12VA / SPI to U8', '28 / 291 / 106 mm', '28 / 291 / 106 mm', '31 / 263 / 106 mm', '31 / 264 / 93 mm', '40 / 262 / 106 mm'),
        ('Total ratsnest (all nets)', '4974 mm', '4974 mm', '4910 mm', '4842 mm', '4893 mm')]
y = 0.345; X = [0.012, 0.38, 0.49, 0.60, 0.71, 0.83]
for k, row in enumerate(rows):
    for j, t in enumerate(row):
        fig.text(X[j], y, t, fontsize=13, color=(GREY if j == 5 else INK), weight=('semibold' if k == 0 or (j == 4 and k in (1, 2, 5, 8)) else 'regular'))
    y -= 0.034
fig.savefig(sys.argv[2], dpi=90, facecolor='white')
