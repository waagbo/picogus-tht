# kipython placement_options_dump.py BOARD PRO OUT.json : geometry and ratsnest of one candidate placement for placement_options_figure.py
import sys, math, json, pcbnew
from shapely.geometry import LineString, box
from shapely.ops import unary_union
from pcbnew import ToMM as M
b = pcbnew.LoadBoard(sys.argv[1])
cls = {p['pattern']: p['netclass'] for p in json.load(open(sys.argv[2]))['net_settings']['netclass_patterns']}
ANA = ('DAC_OUT', 'CAPP', 'CAPM', 'VNEG', 'LDOO', 'AVDD', 'LINE_', '12VA', 'FB1', 'FB2', 'R3-Pad1')
fps = {}; nets = {}; fpbox = {}
for f in b.GetFootprints():
    r = f.GetReference(); cy = f.GetCourtyard(pcbnew.F_CrtYd)
    polys = []
    for k in range(cy.OutlineCount()):
        o = cy.Outline(k); polys.append([(M(o.CPoint(i).x), M(o.CPoint(i).y)) for i in range(o.PointCount())])
    bb = cy.BBox() if cy.OutlineCount() else f.GetBoundingBox(False)
    fpbox[r] = box(M(bb.GetX()), M(bb.GetY()), M(bb.GetRight()), M(bb.GetBottom()))
    fps[r] = dict(x=M(f.GetPosition().x), y=M(f.GetPosition().y), crt=polys)
    for p in f.Pads():
        n = p.GetNetname()
        if n: nets.setdefault(n, []).append((r, M(p.GetPosition().x), M(p.GetPosition().y)))
DAC = unary_union([fpbox[r] for r in ('U9', 'C12', 'C13', 'C14', 'C15', 'C16', 'C17', 'C18', 'C36', 'C19', 'C20', 'R7', 'R8')]).envelope.buffer(1.0)
MIX = unary_union([fpbox[r] for r in ('U11', 'R10', 'R11', 'R12', 'R13', 'R19', 'R20', 'R21', 'R22', 'C30', 'C31', 'C22', 'C23')]).envelope.buffer(1.0)
def digital(n):
    return cls.get(n) not in ('audio', 'pwr', 'GND') and n not in ('GND', '+5V', '+12V', '-12V') and not any(k in n for k in ANA) and not n.startswith('unconnected')
def mst_edges(pts):
    if len(pts) < 2: return []
    inT = [0]; rest = list(range(1, len(pts))); E = []
    while rest:
        a, j = min(((a, j) for a in inT for j in rest), key=lambda e: math.dist(pts[e[0]][1:], pts[e[1]][1:]))
        E.append((pts[a][1:], pts[j][1:])); inT.append(j); rest.remove(j)
    return E
edges = []
for n, pts in nets.items():
    if n.startswith('unconnected') or n == 'GND': continue
    kind = 'i2s' if n in ('BCK', 'DIN', 'LRCK') else ('dig' if digital(n) else ('audio' if cls.get(n) == 'audio' or any(k in n for k in ('DAC_OUT', 'LINE_')) else 'other'))
    for a, c in mst_edges(pts):
        thr = LineString([a, c]).intersection(DAC).length > 0 and kind == 'dig'
        edges.append(dict(n=n, k=kind, a=a, b=c, thr=thr))
ol = pcbnew.SHAPE_POLY_SET(); b.GetBoardPolygonOutlines(ol, True); o = ol.Outline(0)
json.dump(dict(fps=fps, edges=edges, dac=list(DAC.exterior.coords), mix=list(MIX.exterior.coords),
               outline=[(M(o.CPoint(i).x), M(o.CPoint(i).y)) for i in range(o.PointCount())]), open(sys.argv[3], 'w'))
