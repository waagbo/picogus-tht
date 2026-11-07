# Line-out ground-loop DC model: GND potentials for 1 A entering at J8's sleeve and leaving at the bracket or the fingers.
#
# usage:  kipython groundloop_solve.py geom.pkl out.npz [--pitch 0.1]
#   geom.pkl  from gnd_extract.py (same folder):  REFILL=1 kipython gnd_extract.py <board>.kicad_pcb geom.pkl GND
#             (the board's .kicad_pro must sit next to it so that the refill uses the project's rules)
#   out.npz   potential maps of both copper layers (V per A, NaN where there is no GND copper) for the cases below,
#             the raster of all other copper and the outline, for groundloop_figure.py; out.json next to it holds the
#             node potentials and the line-out ground errors e (uV/A, uV for the USB load) per channel.
# needs: gndmodel.py (same folder), numpy, scipy, shapely, Pillow and pyamg (pip install pyamg, or put it on PYTHONPATH).
#
# The model is the README's "tied" GND model (run_dc.py of the layout review, 2-layer, 0.1 mm grid, 35 um copper,
# 20 um via plating, THT pads equipotential through their lead, the Pico's GND pins one node, J3.1/J3.10/J3.31 tied
# and taken as 0 V). Cases (the first three are the README's):
#   fingers : +1 A at J8.S, out at the ISA GND fingers
#   bracket : +1 A at J8.S, -0.5 A at H1 and at H2 (the bracket tab holes)
#   usb     : +0.5 A at J11.4 (USB-host return), out at the fingers
#   h1, h2  : +1 A at J8.S, -1 A at H1 / at H2 only (used to work out the split when the bracket ties H1 and H2)
# Line-out error, inverting mixer, measured against the sleeve (README):
#   e = (Vref - Vsleeve) + 0.977*(Vref - V(U9.9)) + 2*(Vref - Vwt),  Vref = U11.3 (left) / U11.5 (right),
#   Vwt = RV1.1 / RV1.4 (no Waveblaster, RV1 turned down) or the mean of J9.21/23/25 (Waveblaster at full volume).
import sys, os, json, argparse, pickle, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from shapely.geometry import Point
from shapely.ops import unary_union
import gndmodel as gm

ap = argparse.ArgumentParser()
ap.add_argument('geom'); ap.add_argument('out')
ap.add_argument('--pitch', type=float, default=0.1)
a = ap.parse_args()
t0 = time.time()
d = pickle.load(open(a.geom, 'rb'))
outline = d['outline']
grid = gm.Grid(outline.bounds, a.pitch, 4)
gnd = d['nets']['GND']
names = ['F.Cu', 'B.Cu']; zc = [0.0, 1.565]; T_CU = 35e-6; T_PLATE = 20e-6
layers = [{'name': n, 'cov': grid.coverage(gnd[n]), 't': T_CU} for n in names]
M = gm.Model(grid, layers)
li = {n: i for i, n in enumerate(names)}
pad_nodes = {}
for p in d['pads']:
    if p['net'] != 'GND':
        continue
    key = '%s.%s' % (p['ref'], p['num'])
    cells = []
    for ln, shp in p['shape'].items():
        cells += list(M.cells_in(li[ln], shp))
    if 'hole' in p and p['attr'] == 0:  # PTH: the soldered lead joins both layers
        ring = p['hole'].buffer(0.15)
        for i in range(2):
            if names[i] not in p['shape']:
                cells += list(M.cells_in(i, ring))
    r = M.merge(cells)
    if r is None:
        continue
    if key in pad_nodes:
        r = M.merge([pad_nodes[key], r])
    pad_nodes[key] = r
for v in d['vias']:
    if v['net'] != 'GND':
        continue
    disk = Point(v['x'], v['y']).buffer(v['dia'] / 2, 24)
    n1 = M.merge(list(M.cells_in(0, disk))); n2 = M.merge(list(M.cells_in(1, disk)))
    if n1 is None or n2 is None:
        continue
    R = gm.RHO * (zc[1] - zc[0]) * 1e-3 / (np.pi * (v['drill'] * 1e-3 + T_PLATE) * T_PLATE)
    M.link(n1, n2, 1.0 / R)
fing = M.merge([pad_nodes['J3.1'], pad_nodes['J3.10'], pad_nodes['J3.31']])
M.merge([pad_nodes[k] for k in pad_nodes if k.startswith('U1.')])  # Pico GND pins tied ("tied" model)
M.assemble()
N = lambda k: M.node(pad_nodes[k])
cases = {'fingers': {N('J8.S'): 1.0},
         'bracket': {N('J8.S'): 1.0, N('H1.1'): -0.5, N('H2.1'): -0.5},
         'usb': {N('J11.4'): 0.5},
         'h1': {N('J8.S'): 1.0, N('H1.1'): -1.0},
         'h2': {N('J8.S'): 1.0, N('H2.1'): -1.0}}
print('nodes', M.nnode, 'build', round(time.time() - t0, 1), 's', flush=True)
Vs = dict(zip(cases, M.solve(M.node(fing), list(cases.values()))))
print('solved', round(time.time() - t0, 1), 's; max rel residual', M.max_rel_residual, flush=True)

KEYS = ['J8.S', 'U11.3', 'U11.5', 'U9.9', 'RV1.1', 'RV1.4', 'J9.21', 'J9.23', 'J9.25', 'H1.1', 'H2.1', 'J11.4']
GDAC, GWT = 20.0 / 20.47, 2.0


def hum(V):
    out = {}
    for ch, ref, pot in (('L', 'U11.3', 'RV1.1'), ('R', 'U11.5', 'RV1.4')):
        r = V[ref]; j9 = (V['J9.21'] + V['J9.23'] + V['J9.25']) / 3
        base = (r - V['J8.S']) + GDAC * (r - V['U9.9'])
        out[ch] = {'sleeve': r - V['J8.S'], 'dac': r - V['U9.9'], 'pot': r - V[pot], 'j9': r - j9,
                   'e_noWT': base + GWT * (r - V[pot]), 'e_WTmax': base + GWT * (r - j9)}
    return out


res = {'cases': {}}
for c, V in Vs.items():
    nv = {k: float(V[N(k)]) * 1e6 for k in KEYS}  # uV (per A for the loops)
    res['cases'][c] = {'V_uV': nv, 'hum': hum(nv)}
# bracket tabs tied by the bracket (ideal conductor): split a at H1, 1-a at H2 so that V(H1) = V(H2)
A, B = res['cases']['h1']['V_uV'], res['cases']['h2']['V_uV']
al = (B['H2.1'] - B['H1.1']) / ((A['H1.1'] - A['H2.1']) - (B['H1.1'] - B['H2.1']))
tied = {k: al * A[k] + (1 - al) * B[k] for k in KEYS}
res['cases']['bracket_tied'] = {'V_uV': tied, 'hum': hum(tied), 'share_H1': al}
for c, r in res['cases'].items():
    print('%-13s' % c, ' '.join('%s e_noWT %7.1f e_WTmax %7.1f sleeve %7.1f' % (ch, r['hum'][ch]['e_noWT'], r['hum'][ch]['e_WTmax'],
                                                                            r['hum'][ch]['sleeve']) for ch in 'LR'),
          ('H1 share %.3f' % r['share_H1']) if 'share_H1' in r else '')
res['meta'] = {'geom': os.path.abspath(a.geom), 'pitch': a.pitch, 'nodes': int(M.nnode), 'max_rel_residual': float(M.max_rel_residual),
               'pads': {k: [p['x'], p['y']] for p in d['pads'] for k in ['%s.%s' % (p['ref'], p['num'])]
                        if p['ref'] in ('J8', 'U11', 'H1', 'H2', 'U9', 'RV1', 'J9', 'J3', 'J11', 'U1')},
               'outline_bounds': outline.bounds, 'grid': {'x0': grid.x0, 'y0': grid.y0, 'P': grid.P, 'nx': grid.nx, 'ny': grid.ny}}
json.dump(res, open(os.path.splitext(a.out)[0] + '.json', 'w'), indent=1)

# maps: per layer potential (V per A), NaN where no GND copper on that layer
cid = M.cell_id
maps = {}
for c in ('fingers', 'bracket'):
    V = Vs[c]
    m = np.full(cid.shape, np.nan, np.float32)
    ok = cid >= 0
    m[ok] = V[M.node_of_cell[cid[ok]]]
    maps['V_' + c] = m
cov = np.stack([L['cov'] for L in layers]).astype(np.float32)
other = []
for n in names:
    geoms = [g for net, g in d['allcu'][n].items() if net != 'GND']
    other.append(grid.coverage(unary_union(geoms)))
board = grid.coverage(outline)
holes = unary_union([p['hole'] for p in d['pads'] if 'hole' in p] + [Point(v['x'], v['y']).buffer(v['drill'] / 2, 12) for v in d['vias']])
np.savez_compressed(a.out, cov=cov, other=np.stack(other).astype(np.float32), board=board.astype(np.float32),
                    holes=grid.coverage(holes).astype(np.float32), x0=grid.x0, y0=grid.y0, P=grid.P, **maps)
print('wrote', a.out, round(time.time() - t0, 1), 's')
