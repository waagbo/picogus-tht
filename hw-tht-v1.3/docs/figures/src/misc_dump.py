# Usage: kipython misc_dump.py BOARD.kicad_pcb OUT.json [--refill] [--no-zones]
#
# Dumps the geometry the figure scripts need from a KiCad board to JSON (all in mm, board coordinates):
#   edge      Edge.Cuts items as polylines (arcs and circles approximated)
#   drawings  User.Drawings items as polylines
#   fps       per footprint: ref, value, position, orientation, pads (copper polygon per layer, hole,
#             net, number, SMD/PTH), courtyard, silkscreen graphics polygons and visible silk text polygons
#   tracks    segments and arcs (as polylines) with width, layer and net
#   vias      position, pad diameter, drill, net
#   zones     filled polygons of every copper zone per layer (outline + holes), after an optional refill
#   texts     board-level silkscreen texts (string + glyph polygons)
# --refill refills all zones in memory first (the board file is not written).
# The board is only read; nothing is saved.
import sys, json, math
import pcbnew

args = [a for a in sys.argv[1:] if not a.startswith('--')]
REFILL = '--refill' in sys.argv
NOZONES = '--no-zones' in sys.argv
b = pcbnew.LoadBoard(args[0])
mm = pcbnew.ToMM
MAXERR = pcbnew.FromMM(0.001)

if REFILL:
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())


def chain(lc):
    return [(round(mm(lc.CPoint(i).x), 5), round(mm(lc.CPoint(i).y), 5)) for i in range(lc.PointCount())]


def polys(ps):
    """SHAPE_POLY_SET -> [[outline, hole, hole...], ...]"""
    ps = ps.CloneDropTriangulation() if hasattr(ps, 'CloneDropTriangulation') else ps
    try:
        ps.Unfracture()
    except TypeError:
        ps.Unfracture(pcbnew.SHAPE_POLY_SET.PM_FAST)
    out = []
    for k in range(ps.OutlineCount()):
        rings = [chain(ps.Outline(k))]
        for h in range(ps.HoleCount(k)):
            rings.append(chain(ps.Hole(k, h)))
        out.append(rings)
    return out


def shape_poly(item, layer):
    ps = pcbnew.SHAPE_POLY_SET()
    item.TransformShapeToPolygon(ps, layer, 0, MAXERR, pcbnew.ERROR_INSIDE)
    return polys(ps)


def text_poly(t, layer):
    ps = pcbnew.SHAPE_POLY_SET()
    t.TransformTextToPolySet(ps, 0, MAXERR, pcbnew.ERROR_INSIDE)
    if ps.OutlineCount() == 0:
        t.TransformShapeToPolygon(ps, layer, 0, MAXERR, pcbnew.ERROR_INSIDE)
    return polys(ps)


def gshape(g):
    """PCB_SHAPE -> list of polylines (centre lines) + width + filled flag"""
    s = g.GetShape()
    w = mm(g.GetWidth())
    if s == pcbnew.SHAPE_T_SEGMENT:
        pl = [[(mm(g.GetStart().x), mm(g.GetStart().y)), (mm(g.GetEnd().x), mm(g.GetEnd().y))]]
    elif s in (pcbnew.SHAPE_T_RECT, pcbnew.SHAPE_T_RECTANGLE):
        c = [(mm(p.x), mm(p.y)) for p in g.GetRectCorners()]
        pl = [c + [c[0]]]
    elif s == pcbnew.SHAPE_T_CIRCLE:
        cx, cy, r = mm(g.GetCenter().x), mm(g.GetCenter().y), mm(g.GetRadius())
        pl = [[(cx + r * math.cos(a * math.pi / 90), cy + r * math.sin(a * math.pi / 90)) for a in range(181)]]
    elif s == pcbnew.SHAPE_T_ARC:
        cx, cy, r = mm(g.GetCenter().x), mm(g.GetCenter().y), mm(g.GetRadius())
        a0 = g.GetArcAngleStart().AsDegrees(); da = g.GetArcAngle().AsDegrees()
        n = max(4, int(abs(da) / 2))
        pl = [[(cx + r * math.cos(math.radians(a0 + da * i / n)), cy + r * math.sin(math.radians(a0 + da * i / n))) for i in range(n + 1)]]
    elif s == pcbnew.SHAPE_T_POLY:
        pl = []
        for p in polys(g.GetPolyShape()):
            pl.append(p[0] + [p[0][0]])
    elif s == pcbnew.SHAPE_T_BEZIER:
        g.RebuildBezierToSegmentsPointsList(MAXERR)
        pl = [[(mm(p.x), mm(p.y)) for p in g.GetBezierPoints()]]
    else:
        pl = []
    filled = bool(g.IsSolidFill()) if hasattr(g, 'IsSolidFill') else False
    return {'pl': pl, 'w': w, 'filled': filled, 'shape': int(s)}


SILK = {pcbnew.F_SilkS: 'F', pcbnew.B_SilkS: 'B'}
CU = {pcbnew.F_Cu: 'F', pcbnew.B_Cu: 'B'}

out = {'edge': [], 'drawings': [], 'fps': {}, 'tracks': [], 'vias': [], 'zones': [], 'texts': []}

for d in b.GetDrawings():
    L = d.GetLayer()
    if isinstance(d, pcbnew.PCB_SHAPE):
        if L == pcbnew.Edge_Cuts:
            out['edge'].append(gshape(d))
        elif L == pcbnew.Dwgs_User:
            out['drawings'].append(gshape(d))
        elif L in SILK:
            gg = gshape(d); gg['side'] = SILK[L]; gg['poly'] = shape_poly(d, L)
            out['texts'].append({'kind': 'shape', **gg})
    elif isinstance(d, pcbnew.PCB_TEXT) and L in SILK:
        out['texts'].append({'kind': 'text', 'side': SILK[L], 'text': d.GetShownText(True),
                             'pos': (mm(d.GetPosition().x), mm(d.GetPosition().y)),
                             'h': mm(d.GetTextHeight()), 'poly': text_poly(d, L)})
    elif isinstance(d, pcbnew.PCB_TEXT) and L == pcbnew.Dwgs_User:
        out['drawings'].append({'text': d.GetShownText(True), 'pos': (mm(d.GetPosition().x), mm(d.GetPosition().y)), 'pl': [], 'w': 0})

for f in b.GetFootprints():
    ref = f.GetReference()
    f.BuildCourtyardCaches()
    fp = {'pos': (mm(f.GetPosition().x), mm(f.GetPosition().y)), 'rot': f.GetOrientationDegrees(),
          'value': f.GetValue(), 'side': 'B' if f.IsFlipped() else 'F',
          'crt': polys(f.GetCourtyard(pcbnew.F_CrtYd)) + polys(f.GetCourtyard(pcbnew.B_CrtYd)),
          'pads': [], 'silk': [], 'silktext': [], 'edge': [], 'fab': []}
    for p in f.Pads():
        pd = {'num': p.GetNumber(), 'net': p.GetNetname(), 'spoke_angle': p.GetThermalSpokeAngle().AsDegrees(), 'smd': p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD,
              'npth': p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH,
              'pos': (mm(p.GetPosition().x), mm(p.GetPosition().y)),
              'drill': (mm(p.GetDrillSize().x), mm(p.GetDrillSize().y)),
              'hole': None, 'cu': {}}
        if p.GetDrillSize().x > 0:
            hs = p.GetEffectiveHoleShape()
            pd['hole'] = {'a': (mm(hs.GetSeg().A.x), mm(hs.GetSeg().A.y)), 'b': (mm(hs.GetSeg().B.x), mm(hs.GetSeg().B.y)),
                          'r': mm(hs.GetWidth()) / 2}
        for L, s in CU.items():
            if p.IsOnLayer(L) and (p.FlashLayer(L) if hasattr(p, 'FlashLayer') else True):
                ps = pcbnew.SHAPE_POLY_SET()
                p.TransformShapeToPolygon(ps, L, 0, MAXERR, pcbnew.ERROR_INSIDE)
                pd['cu'][s] = polys(ps)
        fp['pads'].append(pd)
    for g in f.GraphicalItems():
        L = g.GetLayer()
        if isinstance(g, pcbnew.PCB_SHAPE):
            if L in SILK:
                gg = gshape(g); gg['side'] = SILK[L]; gg['poly'] = shape_poly(g, L); fp['silk'].append(gg)
            elif L == pcbnew.Edge_Cuts:
                fp['edge'].append(gshape(g))
            elif L in (pcbnew.F_Fab, pcbnew.B_Fab):
                gg = gshape(g); gg['side'] = 'F' if L == pcbnew.F_Fab else 'B'; fp['fab'].append(gg)
        elif isinstance(g, pcbnew.PCB_TEXT) and L in SILK and g.IsVisible():
            fp['silktext'].append({'text': g.GetShownText(True), 'side': SILK[L], 'field': False,
                                   'pos': (mm(g.GetPosition().x), mm(g.GetPosition().y)), 'h': mm(g.GetTextHeight()),
                                   'poly': text_poly(g, L)})
    for fld in f.GetFields():
        L = fld.GetLayer()
        if L in SILK and fld.IsVisible():
            fp['silktext'].append({'text': fld.GetShownText(True), 'side': SILK[L], 'field': fld.GetName(),
                                   'pos': (mm(fld.GetPosition().x), mm(fld.GetPosition().y)), 'h': mm(fld.GetTextHeight()),
                                   'poly': text_poly(fld, L)})
    out['fps'][ref] = fp

for t in b.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        out['vias'].append({'pos': (mm(t.GetPosition().x), mm(t.GetPosition().y)), 'd': mm(t.GetWidth(pcbnew.F_Cu)),
                            'drill': mm(t.GetDrillValue()), 'net': t.GetNetname()})
    elif t.GetLayer() in CU:
        if isinstance(t, pcbnew.PCB_ARC):
            c = t.GetCenter(); r = mm(t.GetRadius())
            a0 = t.GetArcAngleStart().AsDegrees(); da = t.GetAngle().AsDegrees()
            n = max(4, int(abs(da) / 3))
            pl = [(mm(c.x) + r * math.cos(math.radians(a0 + da * i / n)), mm(c.y) + r * math.sin(math.radians(a0 + da * i / n))) for i in range(n + 1)]
        else:
            pl = [(mm(t.GetStart().x), mm(t.GetStart().y)), (mm(t.GetEnd().x), mm(t.GetEnd().y))]
        out['tracks'].append({'pl': pl, 'w': mm(t.GetWidth()), 'layer': CU[t.GetLayer()], 'net': t.GetNetname()})

if not NOZONES:
    for z in b.Zones():
        if z.GetIsRuleArea():
            zz = {'rule': True, 'name': z.GetZoneName(), 'layers': [CU.get(l, pcbnew.LayerName(l)) for l in z.GetLayerSet().Seq()],
                  'outline': polys(z.Outline())}
            out['zones'].append(zz)
            continue
        for L, s in CU.items():
            if z.IsOnLayer(L):
                out['zones'].append({'rule': False, 'net': z.GetNetname(), 'layer': s,
                                     'clearance': mm(z.GetLocalClearance() or 0), 'thermal_gap': mm(z.GetThermalReliefGap()),
                                     'spoke': mm(z.GetThermalReliefSpokeWidth()), 'minw': mm(z.GetMinThickness()),
                                     'fill': polys(z.GetFilledPolysList(L))})

ec = b.GetBoardEdgesBoundingBox()
out['bbox'] = [mm(ec.GetX()), mm(ec.GetY()), mm(ec.GetRight()), mm(ec.GetBottom())]
json.dump(out, open(args[1], 'w'))
print('dumped', args[1], len(out['fps']), 'footprints', len(out['tracks']), 'tracks', len(out['vias']), 'vias',
      len(out['zones']), 'zones; bbox', out['bbox'])
