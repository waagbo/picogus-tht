# Extract the copper geometry of a routed board into a pickle (no solving here): per net and layer the copper
# (pours, tracks, vias, pads), every pad and via, the outline and all copper of every net.
# usage: REFILL=1 kipython gnd_extract.py <board>.kicad_pcb out.pkl [NET ...]      (default NET: GND)
#   kipython = Python with KiCad's pcbnew (KiCad 10) and shapely. The board's .kicad_pro must sit next to it: with
#   REFILL=1 (the default) the zones are refilled in memory with the project's rules (the board file is not written).
import sys, pickle
sys.path.insert(0, '/usr/lib/python3/dist-packages')
import pcbnew
from shapely.geometry import Polygon, MultiPolygon
from shapely.ops import unary_union

brd, out = sys.argv[1], sys.argv[2]
NETS = sys.argv[3:] or ['GND']
b = pcbnew.LoadBoard(brd)
import os
if os.environ.get('REFILL','1')=='1':
    fa={z.GetNetname()+str(z.GetLayerSet().Seq()[0]):sum(z.GetFilledPolysList(l).Area() for l in z.GetLayerSet().Seq() if z.HasFilledPolysForLayer(l)) for z in b.Zones() if not z.GetIsRuleArea()}
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    fb={z.GetNetname()+str(z.GetLayerSet().Seq()[0]):sum(z.GetFilledPolysList(l).Area() for l in z.GetLayerSet().Seq() if z.HasFilledPolysForLayer(l)) for z in b.Zones() if not z.GetIsRuleArea()}
    print('refilled; stored vs refilled fill area mm2', {k:(round(fa[k]/1e12,2), round(fb[k]/1e12,2)) for k in fb})
MM = 1e-6
ERR = 2000  # 2 um polygon approximation error
LAYERS = {'F.Cu': pcbnew.F_Cu, 'B.Cu': pcbnew.B_Cu}


def ps_to_shapely(ps):
    polys = []
    for i in range(ps.OutlineCount()):
        o = ps.Outline(i)
        ext = [(o.CPoint(j).x * MM, o.CPoint(j).y * MM) for j in range(o.PointCount())]
        holes = []
        for h in range(ps.HoleCount(i)):
            hh = ps.Hole(i, h)
            holes.append([(hh.CPoint(j).x * MM, hh.CPoint(j).y * MM) for j in range(hh.PointCount())])
        if len(ext) >= 3:
            p = Polygon(ext, holes)
            if not p.is_valid:
                p = p.buffer(0)
            polys.append(p)
    return polys


def item_poly(item, layer):
    ps = pcbnew.SHAPE_POLY_SET()
    item.TransformShapeToPolygon(ps, layer, 0, ERR, pcbnew.ERROR_INSIDE)
    return ps_to_shapely(ps)


data = {'nets': {}, 'pads': [], 'vias': [], 'tracks': []}
for net in NETS:
    per = {}
    for lname, lid in LAYERS.items():
        parts = []
        nzone = ntrk = npad = 0
        for z in b.Zones():
            if z.GetIsRuleArea() or z.GetNetname() != net:
                continue
            if z.IsOnLayer(lid) and z.HasFilledPolysForLayer(lid):
                parts += ps_to_shapely(z.GetFilledPolysList(lid))
                nzone += 1
        for t in b.GetTracks():
            if t.GetNetname() != net:
                continue
            if t.GetClass() == 'PCB_VIA':
                if t.FlashLayer(lid):
                    parts += item_poly(t, lid)
                continue
            if t.GetLayer() == lid:
                parts += item_poly(t, lid)
                ntrk += 1
        for fp in b.GetFootprints():
            for p in fp.Pads():
                if p.GetNetname() == net and p.IsOnLayer(lid) and p.FlashLayer(lid):
                    parts += item_poly(p, lid)
                    npad += 1
        per[lname] = unary_union(parts)
        print(net, lname, 'zones', nzone, 'tracks', ntrk, 'pads', npad, 'area mm2', round(per[lname].area, 1))
    data['nets'][net] = per

# all pads and vias (any net) for antipads / links
for fp in b.GetFootprints():
    for p in fp.Pads():
        pos = p.GetPosition()
        d = {'ref': fp.GetReference(), 'num': p.GetNumber(), 'net': p.GetNetname(),
             'x': pos.x * MM, 'y': pos.y * MM, 'attr': p.GetAttribute(),
             'drill': (p.GetDrillSize().x * MM, p.GetDrillSize().y * MM),
             'layers': [ln for ln, lid in LAYERS.items() if p.IsOnLayer(lid)], 'shape': {}}
        for ln, lid in LAYERS.items():
            if p.IsOnLayer(lid):
                d['shape'][ln] = unary_union(item_poly(p, lid))
        if p.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH) and p.GetDrillSize().x > 0:
            hp = pcbnew.SHAPE_POLY_SET()
            p.TransformHoleToPolygon(hp, 0, ERR, pcbnew.ERROR_INSIDE)
            d['hole'] = unary_union(ps_to_shapely(hp))
        data['pads'].append(d)
for t in b.GetTracks():
    if t.GetClass() == 'PCB_VIA':
        pos = t.GetPosition()
        data['vias'].append({'net': t.GetNetname(), 'x': pos.x * MM, 'y': pos.y * MM,
                             'drill': t.GetDrillValue() * MM, 'dia': t.GetWidth(pcbnew.F_Cu) * MM})
    else:
        s, e = t.GetStart(), t.GetEnd()
        rec = {'net': t.GetNetname(), 'layer': b.GetLayerName(t.GetLayer()), 'w': t.GetWidth() * MM,
               'x0': s.x * MM, 'y0': s.y * MM, 'x1': e.x * MM, 'y1': e.y * MM, 'cls': t.GetClass()}
        if t.GetClass() == 'PCB_ARC':
            m = t.GetMid()
            rec['xm'], rec['ym'] = m.x * MM, m.y * MM
        data['tracks'].append(rec)

# board outline
ps = pcbnew.SHAPE_POLY_SET()
b.GetBoardPolygonOutlines(ps, False)
data['outline'] = unary_union(ps_to_shapely(ps))
print('outline area', round(data['outline'].area, 1), 'bounds', data['outline'].bounds)
# all copper (any net) per layer, for cross-sections
allcu = {}
for lname, lid in LAYERS.items():
    parts = {}
    for z in b.Zones():
        if z.GetIsRuleArea() or not z.IsOnLayer(lid) or not z.HasFilledPolysForLayer(lid):
            continue
        parts.setdefault(z.GetNetname(), []).extend(ps_to_shapely(z.GetFilledPolysList(lid)))
    for t in b.GetTracks():
        if (t.GetClass() == 'PCB_VIA' and t.FlashLayer(lid)) or (t.GetClass() != 'PCB_VIA' and t.GetLayer() == lid):
            parts.setdefault(t.GetNetname(), []).extend(item_poly(t, lid))
    for fp in b.GetFootprints():
        for p in fp.Pads():
            if p.IsOnLayer(lid) and p.FlashLayer(lid):
                parts.setdefault(p.GetNetname(), []).extend(item_poly(p, lid))
    allcu[lname] = {n: unary_union(v) for n, v in parts.items()}
data['allcu'] = allcu
pickle.dump(data, open(out, 'wb'))
print('pads', len(data['pads']), 'vias', len(data['vias']), 'tracks', len(data['tracks']))
