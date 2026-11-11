# kipython courtyard_dump.py PicoGUS-THT.kicad_pcb crt.json : courtyards, fab outlines, pads of the logic column + pairwise courtyard overlaps
import sys, json, math, pcbnew
from shapely.geometry import Polygon
from shapely.ops import unary_union

b = pcbnew.LoadBoard(sys.argv[1])
mm = pcbnew.ToMM

def chain(lc):
    return [(mm(lc.CPoint(i).x), mm(lc.CPoint(i).y)) for i in range(lc.PointCount())]

def polyset(ps):
    out = []
    for k in range(ps.OutlineCount()):
        out.append(chain(ps.Outline(k)))
    return out

fps = {}
for f in b.GetFootprints():
    ref = f.GetReference()
    f.BuildCourtyardCaches()
    crt = polyset(f.GetCourtyard(pcbnew.F_CrtYd))
    fab = []
    for g in f.GraphicalItems():
        if g.GetLayer() != pcbnew.F_Fab or not isinstance(g, pcbnew.PCB_SHAPE):
            continue
        s = g.GetShape()
        if s == pcbnew.SHAPE_T_SEGMENT:
            fab.append(['line', [(mm(g.GetStart().x), mm(g.GetStart().y)), (mm(g.GetEnd().x), mm(g.GetEnd().y))]])
        elif s in (pcbnew.SHAPE_T_RECT, pcbnew.SHAPE_T_RECTANGLE):
            fab.append(['poly', [(mm(p.x), mm(p.y)) for p in g.GetRectCorners()]])
        elif s == pcbnew.SHAPE_T_CIRCLE:
            fab.append(['circle', [mm(g.GetCenter().x), mm(g.GetCenter().y), mm(g.GetRadius())]])
        elif s == pcbnew.SHAPE_T_POLY:
            fab.extend(['poly', p] for p in polyset(g.GetPolyShape()))
        elif s == pcbnew.SHAPE_T_ARC:
            c = g.GetCenter(); r = mm(g.GetRadius())
            a0 = g.GetArcAngleStart().AsDegrees(); a1 = a0 + g.GetArcAngle().AsDegrees()
            fab.append(['arc', [mm(c.x), mm(c.y), r, a0, a1]])
    pads = []
    for p in f.Pads():
        ps = p.GetEffectivePolygon(pcbnew.F_Cu, pcbnew.ERROR_INSIDE)
        pads.append({'num': p.GetNumber(), 'net': p.GetNetname(), 'poly': polyset(ps), 'hole': [mm(p.GetPosition().x), mm(p.GetPosition().y), mm(p.GetDrillSize().x)]})
    pos = f.GetPosition()
    fps[ref] = {'pos': (mm(pos.x), mm(pos.y)), 'crt': crt, 'fab': fab, 'pads': pads, 'value': f.GetValue()}

# pairwise overlaps of F.CrtYd
ov = []
refs = [r for r in fps if fps[r]['crt']]
P = {r: unary_union([Polygon(c) for c in fps[r]['crt'] if len(c) >= 3]) for r in refs}
for i, a in enumerate(refs):
    for bref in refs[i + 1:]:
        if not P[a].intersects(P[bref]):
            continue
        x = P[a].intersection(P[bref])
        if x.area < 1e-6:
            continue
        x0, y0, x1, y1 = x.bounds
        ov.append({'a': a, 'b': bref, 'area': x.area, 'depth': min(x1 - x0, y1 - y0),
                   'polys': [list(g.exterior.coords) for g in getattr(x, 'geoms', [x])]})

ec = b.GetBoardEdgesBoundingBox()
json.dump({'fps': fps, 'overlaps': ov,
           'edge': [mm(ec.GetX()), mm(ec.GetY()), mm(ec.GetRight()), mm(ec.GetBottom())]},
          open(sys.argv[2], 'w'))
for o in ov:
    print('%-4s %-4s depth %.3f mm area %.3f mm2' % (o['a'], o['b'], o['depth'], o['area']))
