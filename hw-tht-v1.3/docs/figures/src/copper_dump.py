# Usage: kipython copper_dump.py BOARD.kicad_pcb OUT.json [--refill]
#   Dumps the copper of a KiCad board to JSON for the figure scripts (gold_fingers_figure.py,
#   b10_ground_figure.py, i2s_dac_figure.py): tracks and arcs, vias, pads (copper polygon per layer, hole, mask
#   expansion), filled zones per layer, rule areas, footprint mask graphics, footprint fab/courtyard outlines,
#   Edge.Cuts and the net class of every net. All coordinates in mm (KiCad page coordinates, y down).
#   --refill refills the zones in memory first (the board file is never written).
import sys, json, pcbnew

mm = pcbnew.ToMM
b = pcbnew.LoadBoard(sys.argv[1])
if '--refill' in sys.argv:
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
LN = b.GetLayerName

def chain(lc):
    return [(round(mm(lc.CPoint(i).x), 5), round(mm(lc.CPoint(i).y), 5)) for i in range(lc.PointCount())]

def polyset(ps):
    """outlines with holes: [[outer, hole, hole...], ...]"""
    out = []
    for k in range(ps.OutlineCount()):
        rings = [chain(ps.Outline(k))]
        for h in range(ps.HoleCount(k)):
            rings.append(chain(ps.Hole(k, h)))
        out.append(rings)
    return out

def nc_of(item):
    try:
        return item.GetEffectiveNetClass().GetName()
    except Exception:
        return ''

nets = {}
for name, ni in b.GetNetsByName().items():
    n = str(name)
    try:
        nets[n] = ni.GetNetClass().GetName()
    except Exception:
        nets[n] = ''

tracks, vias = [], []
for t in b.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        vias.append({'pos': (mm(t.GetPosition().x), mm(t.GetPosition().y)), 'dia': mm(t.GetWidth(pcbnew.F_Cu)),
                     'drill': mm(t.GetDrillValue()), 'net': t.GetNetname(), 'cls': nc_of(t)})
    elif isinstance(t, pcbnew.PCB_ARC):
        tracks.append({'kind': 'arc', 'layer': LN(t.GetLayer()), 'net': t.GetNetname(), 'cls': nc_of(t), 'w': mm(t.GetWidth()),
                       'a': (mm(t.GetStart().x), mm(t.GetStart().y)), 'm': (mm(t.GetMid().x), mm(t.GetMid().y)),
                       'b': (mm(t.GetEnd().x), mm(t.GetEnd().y))})
    else:
        tracks.append({'kind': 'seg', 'layer': LN(t.GetLayer()), 'net': t.GetNetname(), 'cls': nc_of(t), 'w': mm(t.GetWidth()),
                       'a': (mm(t.GetStart().x), mm(t.GetStart().y)), 'b': (mm(t.GetEnd().x), mm(t.GetEnd().y))})

pads, fps = [], {}
for f in b.GetFootprints():
    ref = f.GetReference()
    f.BuildCourtyardCaches()
    graphics = []
    for g in f.GraphicalItems():
        if not isinstance(g, pcbnew.PCB_SHAPE):
            continue
        lay = LN(g.GetLayer())
        if lay not in ('F.Fab', 'B.Fab', 'F.Mask', 'B.Mask', 'F.Cu', 'B.Cu'):
            continue
        s = g.GetShape()
        if s == pcbnew.SHAPE_T_SEGMENT:
            geo = ['line', [(mm(g.GetStart().x), mm(g.GetStart().y)), (mm(g.GetEnd().x), mm(g.GetEnd().y))]]
        elif s in (pcbnew.SHAPE_T_RECT, pcbnew.SHAPE_T_RECTANGLE):
            geo = ['poly', [(mm(p.x), mm(p.y)) for p in g.GetRectCorners()]]
        elif s == pcbnew.SHAPE_T_CIRCLE:
            geo = ['circle', [mm(g.GetCenter().x), mm(g.GetCenter().y), mm(g.GetRadius())]]
        elif s == pcbnew.SHAPE_T_POLY:
            ps = g.GetPolyShape()
            geo = ['polys', [chain(ps.Outline(k)) for k in range(ps.OutlineCount())]]
        elif s == pcbnew.SHAPE_T_ARC:
            c = g.GetCenter(); a0 = g.GetArcAngleStart().AsDegrees()
            geo = ['arc', [mm(c.x), mm(c.y), mm(g.GetRadius()), a0, a0 + g.GetArcAngle().AsDegrees()]]
        else:
            continue
        fill = False
        try:
            fill = bool(g.IsSolidFill())
        except Exception:
            pass
        graphics.append({'layer': lay, 'w': mm(g.GetWidth()), 'fill': fill, 'geo': geo})
    fps[ref] = {'pos': (mm(f.GetPosition().x), mm(f.GetPosition().y)), 'rot': f.GetOrientationDegrees(),
                'side': LN(f.GetLayer()), 'value': f.GetValue(), 'graphics': graphics,
                'crt': {L: [r[0] for r in polyset(f.GetCourtyard(getattr(pcbnew, L.replace('.', '_'))))] for L in ('F.CrtYd', 'B.CrtYd')}}
    for p in f.Pads():
        layers = [LN(l) for l in p.GetLayerSet().Seq()]
        cu = {}
        for L in ('F.Cu', 'B.Cu'):
            if L in layers:
                cu[L] = polyset(p.GetEffectivePolygon(getattr(pcbnew, L.replace('.', '_')), pcbnew.ERROR_INSIDE))
        mexp = {}
        for L in ('F.Mask', 'B.Mask'):
            if L in layers:
                try:
                    mexp[L] = mm(p.GetSolderMaskExpansion(getattr(pcbnew, L.replace('.', '_'))))
                except Exception:
                    mexp[L] = mm(p.GetSolderMaskExpansion())
        ds = p.GetDrillSize()
        pads.append({'ref': ref, 'num': p.GetNumber(), 'net': p.GetNetname(), 'cls': nc_of(p), 'layers': layers, 'cu': cu,
                     'mask_exp': mexp, 'pos': (mm(p.GetPosition().x), mm(p.GetPosition().y)),
                     'hole': [mm(ds.x), mm(ds.y)] if ds.x > 0 else None,
                     'pth': p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH})

zones, rules = [], []
for z in b.Zones():
    lays = [LN(l) for l in z.GetLayerSet().Seq()]
    if z.GetIsRuleArea():
        rules.append({'name': z.GetZoneName(), 'layers': lays, 'outline': polyset(z.Outline()),
                      'no_fill': z.GetDoNotAllowZoneFills(), 'no_tracks': z.GetDoNotAllowTracks(), 'no_vias': z.GetDoNotAllowVias()})
        continue
    for l in z.GetLayerSet().Seq():
        if z.HasFilledPolysForLayer(l):
            zones.append({'net': z.GetNetname(), 'layer': LN(l), 'fill': polyset(z.GetFilledPolysList(l))})

edges = []
for d in b.GetDrawings():
    if LN(d.GetLayer()) != 'Edge.Cuts':
        continue
    if d.GetShape() == pcbnew.SHAPE_T_SEGMENT:
        edges.append([(mm(d.GetStart().x), mm(d.GetStart().y)), (mm(d.GetEnd().x), mm(d.GetEnd().y))])
for f in b.GetFootprints():
    for g in f.GraphicalItems():
        if LN(g.GetLayer()) == 'Edge.Cuts' and isinstance(g, pcbnew.PCB_SHAPE) and g.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            edges.append([(mm(g.GetStart().x), mm(g.GetStart().y)), (mm(g.GetEnd().x), mm(g.GetEnd().y))])

ec = b.GetBoardEdgesBoundingBox()
json.dump({'board': sys.argv[1], 'nets': nets, 'tracks': tracks, 'vias': vias, 'pads': pads, 'fps': fps,
           'zones': zones, 'rules': rules, 'edges': edges,
           'edge_bbox': [mm(ec.GetX()), mm(ec.GetY()), mm(ec.GetRight()), mm(ec.GetBottom())]},
          open(sys.argv[2], 'w'))
print('%d tracks, %d vias, %d pads, %d zone layers, %d rule areas' % (len(tracks), len(vias), len(pads), len(zones), len(rules)))
