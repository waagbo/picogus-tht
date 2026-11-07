# Usage: kipython starved_thermals_figure.py DUMP.json DRC.json OUT.png
#   DUMP.json  from 'kipython misc_dump.py PicoGUS-THT.kicad_pcb DUMP.json --refill'
#   DRC.json   from 'kicad-cli pcb drc --severity-all --refill-zones --schematic-parity --format json -o DRC.json <board>'
#              (run it on a copy of the board directory: kicad-cli rewrites the .kicad_pro next to the board)
# Needs misc_figlib.py next to this script.
#
# Draws starved-thermals.png: every starved_thermal warning of the DRC report, grouped by pad, as a front (F.Cu) and a
# back (B.Cu) close-up with the GND pour, the thermal spokes the zone filler made (green), a red cross on each side
# where a spoke has no room with the copper in the way and its distance from the pad, and hatching on copper that is a
# dead end (it reaches GND only through the pad itself). The column captions say how each pad is joined to GND.
# Everything printed is measured here: spokes = poured copper crossing the thermal gap around the pad; "joins GND" =
# connected to the main GND pour without passing through the pad (two-layer connectivity of pour, GND tracks, GND
# vias and plated GND holes).
import sys, os, json, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from misc_figlib import *

d = load(sys.argv[1]); drc = json.load(open(sys.argv[2])); OUT = sys.argv[3]

# ---------------------------------------------------------------- DRC list -> sites
warn = []
for v in drc['violations']:
    if v['type'] != 'starved_thermal':
        continue
    layer = 'F' if 'layer F.Cu' in v['description'] else 'B'
    for it in v['items']:
        desc = it['description']
        if ' of ' in desc and desc.split()[0] in ('PTH', 'Pad'):
            ref = desc.split(' of ')[1].split()[0]
            num = desc.split('pad ' if 'pad ' in desc else 'Pad ')[1].split()[0]
            pos = (it['pos']['x'], it['pos']['y']) if 'pos' in it else None
            if desc.startswith('PTH'):
                kind = 'THT pad'
            else:   # an SMD pad: the Pico's castellation pads, or an SMD part's pin
                kind = 'castellation pad' if ref == 'U1' else 'SMD pad'
            warn.append({'ref': ref, 'num': num, 'layer': layer, 'kind': kind, 'island': 'isolated island' in v['description'],
                         'msg': v['description'], 'pos': pos})
print('%d starved_thermal warnings' % len(warn))
for w in warn:
    print('  %s.%s %s.Cu %s: %s' % (w['ref'], w['num'], w['layer'], w['kind'], w['msg']))
order = []
for w in warn:
    if (w['ref'], w['num']) not in order:
        order.append((w['ref'], w['num']))
order.sort(key=lambda k: (k[0] != 'U1', -sum(1 for w in warn if (w['ref'], w['num']) == k), int(k[1])))

zone = [z for z in d['zones'] if not z['rule'] and z['net'] == 'GND'][0]
GAP, SPW, ZCL, MINW = zone['thermal_gap'], zone['spoke'], zone['clearance'], zone['minw']
ROOM = GAP + MINW + ZCL
fills = {L: [rings_to_poly(p) for z in d['zones'] if not z['rule'] and z['layer'] == L for p in z['fill']] for L in 'FB'}
pour = {L: unary_union([g for g in fills[L] if g is not None]) for L in 'FB'}

cu = {'F': [], 'B': []}
for t in d['tracks']:
    cu[t['layer']].append({'net': t['net'], 'kind': 'track', 'w': t['w'], 'g': track_geom(t), 'pl': t['pl']})
for v in d['vias']:
    for L in 'FB':
        cu[L].append({'net': v['net'], 'kind': 'via', 'g': Point(v['pos']).buffer(v['d'] / 2, quad_segs=24), 'pos': v['pos'], 'drill': v['drill']})
for r, f in d['fps'].items():
    for p in f['pads']:
        for L, pl in p['cu'].items():
            cu[L].append({'net': p['net'], 'kind': 'pad', 'ref': r, 'num': p['num'], 'g': polys_to_geom(pl), 'pos': p['pos'],
                          'hole': p['hole'] is not None})


# ---------------------------------------------------------------- GND connectivity without the pad under test
def connectivity(excl):
    """per layer: components of GND copper (pour, GND tracks, GND vias, GND pads except excl); union-find across layers
    through vias and plated holes. Returns comps, find(), and the root of the main GND (largest front component)."""
    comps = {}
    for L in 'FB':
        # the pad under test is cut out of everything, so that a spoke or a track that only meets another item inside
        # the pad (KiCad draws spokes from the pad centre) does not count as a connection
        EX = unary_union([c['g'] for c in cu[L] if c['kind'] == 'pad' and (c['ref'], c['num']) == excl]).buffer(0.01)
        items = [g.difference(EX) for g in fills[L]] + [c['g'].difference(EX) for c in cu[L] if c['net'] == 'GND' and not (c['kind'] == 'pad' and (c['ref'], c['num']) == excl)]
        U = unary_union([g.buffer(0.001) for g in items if not g.is_empty])
        comps[L] = sorted(getattr(U, 'geoms', [U]), key=lambda g: -g.area)
    par = {}

    def find(x):
        par.setdefault(x, x)
        while par[x] != x:
            x = par[x]
        return x

    def idx(L, g):
        for i, c in enumerate(comps[L]):
            if c.intersects(g):
                return i
    for c in cu['F']:
        if c['net'] != 'GND':
            continue
        if c['kind'] == 'via' or (c['kind'] == 'pad' and c['hole'] and (c['ref'], c['num']) != excl):
            a = idx('F', c['g']); b = idx('B', Point(c['pos']).buffer(0.05))
            if a is not None and b is not None:
                par[find(('F', a))] = find(('B', b))
    return comps, find, idx


def name_of(c, ref=None):
    if c['kind'] == 'pad':
        if c['ref'] == ref:
            return 'next pin'
        return '%s %s' % (c['ref'], 'shield pad' if c['num'] == 'SH' else 'pad ' + c['num'])
    n = c['net'].replace('{slash}', '/')
    if n.startswith('~{'):
        n = '~' + n[2:]
    n = n.replace('}', '')
    return n.split('/')[-1] if n.startswith('/') else n


def analyse(ref, num, L, conn):
    comps, find, idx = conn
    main = find(('F', 0))
    at = [w['pos'] for w in warn if (w['ref'], w['num']) == (ref, num) and w['pos']]
    pads = [p for p in d['fps'][ref]['pads'] if p['num'] == num and L in p['cu']
            and (not at or any(math.dist(p['pos'], q) < 0.01 for q in at))]
    if not pads:
        return None
    shp = unary_union([polys_to_geom(p['cu'][L]) for p in pads])
    cx, cy = pads[0]['pos']
    ring = shp.buffer(GAP - 0.02).difference(shp.buffer(0.001))
    spokes = []
    for f in fills[L]:
        x = f.intersection(ring)
        for part in getattr(x, 'geoms', [x]):
            if part.area > 0.05:
                i = idx(L, f)
                touch = sorted({'%s.%s' % (c['ref'], c['num']) for c in cu[L] if c['net'] == 'GND' and c['kind'] == 'pad'
                                and (c['ref'], c['num']) != (ref, num) and c['g'].intersects(f.buffer(0.01))})
                spokes.append({'g': f.intersection(shp.buffer(GAP - 0.01)).difference(shp), 'piece': f, 'area': f.area,
                               'comp': comps[L][i], 'joins': find((L, i)) == main, 'touch': touch})
    gtr = []
    for c in cu[L]:
        if c['net'] == 'GND' and c['kind'] == 'track' and c['g'].intersects(shp.buffer(0.01)):
            rest = c['g'].difference(shp.buffer(0.05))
            i = idx(L, rest) if not rest.is_empty else None
            if i is None:        # a stub inside the pad
                continue
            tp = any(q['kind'] == 'pad' and q['ref'] == ref and q['num'] == num and q['g'].intersects(c['g'])
                     and not any(math.dist(q['pos'], p_['pos']) < 0.01 for p_ in pads) for q in cu[L])
            gtr.append({'w': c['w'], 'comp': comps[L][i], 'joins': find((L, i)) == main, 'c': c, 'testpad': tp})
    off = pads[0]['spoke_angle'] % 90
    dirs = []
    for k in range(4):
        a = math.radians(off + 90 * k)
        ux, uy = math.cos(a), math.sin(a)
        strip = LineString([(cx, cy), (cx + 6 * ux, cy + 6 * uy)]).buffer(SPW / 2, cap_style=2).difference(shp)
        has = [s for s in spokes if s['g'].intersects(strip)]
        obs = []
        for c in cu[L]:
            if c['kind'] == 'pad' and c['ref'] == ref and c['num'] == num:
                continue
            if c['net'] == 'GND' and c['kind'] != 'pad':
                continue
            if c['g'].intersects(strip):
                hit = c['g'].intersection(strip)
                obs.append((c['g'].distance(shp), hit.distance(shp), c, hit))
        obs.sort(key=lambda o: o[1])
        ray = LineString([(cx, cy), (cx + 8 * ux, cy + 8 * uy)])
        e = ray.intersection(shp.boundary)
        pts = [e] if e.geom_type == 'Point' else list(getattr(e, 'geoms', []))
        ep = max(pts, key=lambda p: math.hypot(p.x - cx, p.y - cy))
        dirs.append({'ang': round(off + 90 * k) % 360, 'u': (ux, uy), 'spokes': has, 'obs': obs[:1], 'edge': (ep.x, ep.y)})
    return {'shp': shp, 'pads': pads, 'spokes': spokes, 'dirs': dirs, 'gtr': gtr, 'c': (cx, cy)}


A = {}
for ref, num in order:
    conn = connectivity((ref, num))
    for L in 'FB':
        a = analyse(ref, num, L, conn)
        if a is None:
            print('%s.%s %s.Cu: no copper on this layer (SMD pad)' % (ref, num, L))
            continue
        A[(ref, num, L)] = a
        print('%s.%s %s.Cu: %d spoke(s) %s; GND tracks %s' % (ref, num, L, len(a['spokes']),
              ['into %.2f mm2 of pour, %s%s' % (s['area'], 'joins GND' if s['joins'] else 'DEAD END', ' (touches %s)' % s['touch'] if s['touch'] else '') for s in a['spokes']],
              ['%.2f mm %s' % (t['w'], 'joins GND' if t['joins'] else 'DEAD END') for t in a['gtr']]))
        for dd in a['dirs']:
            print('   %3d deg: %s; nearest in line: %s' % (dd['ang'], 'spoke' if dd['spokes'] else 'no spoke',
                  ['%s %.2f mm (in line %.2f)' % (name_of(o[2], ref), o[0], o[1]) for o in dd['obs']]))



# ---------------------------------------------------------------- panels
# Sites of one connector that has three or more warnings (the Waveblaster header J9) share one strip per layer; every
# other site gets a square tile per layer (an SMD pad only on its own layer).
W = 2190
pad = 40; gap = 34
fls = font(18, 'SemiBold')
DEADCOL = (70, 70, 70)
byref = {}
for k in order:
    byref.setdefault(k[0], []).append(k)
strips = [r for r, ks in byref.items() if len(ks) >= 3]
tiles_k = [k for k in order if k[0] not in strips]
NT = max(1, len(tiles_k))
TW = (W - 2 * pad - (NT - 1) * gap) // NT
ST = 76
HALF = TW / ST / 2
SITE = {k: A[k + ('F',)]['c'] if k + ('F',) in A else A[k + ('B',)]['c'] for k in order}
# blocker label placement overrides: (ref, num, layer, angle) -> (dx, dy) px from the point beyond the blocker, anchor
LAB = {('J9', '25', 'F', 180): (12, 100, 'lt')}
# 'dead end' callouts: (ref, num, layer) -> (dx, dy) px from the dead piece's centre, anchor
DEADLAB = {('U1', '28', 'F'): (70, -40, 'lb')}

U1body = None
xs = [p[0] for g in d['fps']['U1']['fab'] if g['side'] == 'F' for pl in g['pl'] for p in pl]
ys = [p[1] for g in d['fps']['U1']['fab'] if g['side'] == 'F' for pl in g['pl'] for p in pl]
if xs:
    U1body = box(min(xs), min(ys), max(xs), max(ys))


def default_lab(ang):
    a = ang % 360
    return {0: (14, 0, 'lm'), 90: (0, 16, 'mt'), 180: (-14, 0, 'rm'), 270: (0, -16, 'mb'),
            45: (10, 10, 'lt'), 135: (-10, 10, 'rt'), 225: (-10, -10, 'rb'), 315: (10, -10, 'lb')}.get(a, (0, -16, 'mb'))


def panel(win, S_, sites, L):
    x0, y0, x1, y1 = win
    v = View(x0, y0, x1, y1, S_)
    v.grid(0.5)
    v.fill(pour[L], FPOUR if L == 'F' else BPOUR)
    for c in cu[L]:
        if c['kind'] == 'track' and c['g'].intersects(v.clip):
            col = (GNDTRK_F if L == 'F' else GNDTRK_B) if c['net'] == 'GND' else (FTRK if L == 'F' else BTRK)
            v.fill(c['g'], col, 235)
    for k in sites:
        a = A.get(k + (L,))
        if a is None:
            continue
        dead = [s_['comp'] for s_ in a['spokes'] if not s_['joins']] + [t['comp'] for t in a['gtr'] if not t['joins']]
        for g in dead:
            v.hatch(g.difference(a['shp']), DEADCOL, 8, 1.4)
    for c in cu[L]:
        if c['kind'] == 'pad' and c['g'].intersects(v.clip):
            v.fill(c['g'], PAD)
    for c in cu[L]:
        if c['kind'] == 'via' and c['g'].intersects(v.clip):
            v.fill(c['g'], (125, 125, 125) if c['net'] != 'GND' else ((120, 60, 55) if L == 'F' else (40, 65, 115)))
            v.fill(Point(c['pos']).buffer(c['drill'] / 2), HOLE)
    for f in d['fps'].values():
        for p in f['pads']:
            h = hole_geom(p)
            if h is not None and h.intersects(v.clip):
                v.fill(h, HOLE)
    for k in sites:
        a = A.get(k + (L,))
        if a is not None:
            for s_ in a['spokes']:
                v.fill(s_['g'], SPOKE)
    if U1body is not None and U1body.intersects(v.clip) and any(k[0] == 'U1' for k in sites):
        v.outline(U1body.exterior, (90, 90, 90), 1.6, dash=(7, 5))
    im = v.finish()
    dr = ImageDraw.Draw(im)
    CL = (0, 0, im.size[0], im.size[1])
    done = set()
    for k in sites:
        ref, num = k
        a = A.get(k + (L,))
        if a is None:
            continue
        for s_ in [x for x in a['spokes'] if not x['joins']] + [x for x in a['gtr'] if not x['joins']]:
            g = s_['comp'].difference(a['shp'])
            c = g.representative_point()
            dx, dy, an = DEADLAB.get((ref, num, L), (-40, 34, 'rt'))
            q = v.T((c.x, c.y))
            tag(dr, (q[0] + dx, q[1] + dy), 'dead end', fls, DEADCOL, anchor=an, clamp=CL)
            arrow(dr, (q[0] + dx * 0.55, q[1] + dy * 0.55), q, DEADCOL, 2, 8)
            break
        for dd in a['dirs']:
            ex, ey = dd['edge']; ux, uy = dd['u']
            dead_spoke = dd['spokes'] and not any(s_['joins'] for s_ in dd['spokes'])
            if not dd['spokes']:
                px, py = v.T((ex + ux * GAP / 2, ey + uy * GAP / 2))
                r = 9
                dr.line([px - r, py - r, px + r, py + r], fill=RED, width=4)
                dr.line([px - r, py + r, px + r, py - r], fill=RED, width=4)
            if dd['obs'] and (not dd['spokes'] or dead_spoke):
                dist, dline, c, hit = dd['obs'][0]
                nm = name_of(c, ref)
                if nm == 'next pin' or dline > (1.0 if dd['spokes'] else 1.6) or (nm, round(dist, 2)) in done:
                    continue
                done.add((nm, round(dist, 2)))
                lp = LAB.get((ref, num, L, dd['ang'])) or default_lab(dd['ang'])
                q = v.T((ex + ux * (dline + 0.12), ey + uy * (dline + 0.12)))
                tag(dr, (q[0] + lp[0], q[1] + lp[1]), '%s %.2f mm' % (nm, dist), fls, RED, anchor=lp[2], clamp=CL)
    return im, v


def pin_tags(im, v, sites, L):
    """pin numbers of a strip's warned pads, at the top of the strip with a leader down to the pad"""
    dr = ImageDraw.Draw(im)
    for k in sites:
        cx, cy = SITE[k]
        q = v.T((cx, cy))
        yt = 12
        dr.line([(q[0], yt + 22), (q[0], q[1] - 0.80 * v.S)], fill=RED, width=2)
        tag(dr, (q[0], yt), 'pin %s' % k[1], font(17, 'Bold'), RED, anchor='mt')


panels = {}
for r in strips:
    ks = byref[r]
    xs_ = [SITE[k][0] for k in ks]; ys_ = [SITE[k][1] for k in ks]
    mx = 2.0
    x0_, x1_ = min(xs_) - mx, max(xs_) + mx
    S_ = (W - 2 * pad) / (x1_ - x0_)
    yc = sum(ys_) / len(ys_)
    win = (x0_, yc - 3.0, x1_, yc + 3.4)
    for L in 'FB':
        im, v = panel(win, S_, ks, L)
        pin_tags(im, v, ks, L)
        panels[(r, L)] = (im, v)
for k in tiles_k:
    cx, cy = SITE[k]
    for L in 'FB':
        if k + (L,) in A:
            im, v = panels[k + (L,)] = panel((cx - HALF, cy - HALF, cx + HALF, cy + HALF), ST, [k], L)
            if k + ('B',) not in A:      # an SMD pin among others of its kind: name it
                q = v.T((cx, cy)); dr_ = ImageDraw.Draw(im)
                X = (q[0] + 150, q[1] + 120)
                dr_.line([(q[0] + 30, q[1] + 22), (X[0] - 10, X[1] - 4)], fill=RED, width=2)
                tag(dr_, X, 'pin %s' % k[1], font(17, 'Bold'), RED, anchor='lm')


# ---------------------------------------------------------------- captions (numbers from the analysis)
def area_txt(x):
    return '%.2f' % x if x < 2 else '%.1f' % x


def caption(ref, num):
    out = []
    for L in 'FB':
        a = A.get((ref, num, L))
        if a is None:
            continue
        good = [x for x in a['spokes'] if x['joins']]
        bad = [x for x in a['spokes'] if not x['joins']]
        tj = [t for t in a['gtr'] if t['joins']]
        tb = [t for t in a['gtr'] if not t['joins']]
        parts = []
        if good:
            big = [x for x in good if x['area'] >= 2]
            small = [x for x in good if x['area'] < 2]
            if big:
                parts.append('%d spoke%s into the pour' % (len(big), '' if len(big) == 1 else 's'))
            for x in small:
                via = (' shared with ' + ', '.join('pin ' + t.split('.')[1] if t.split('.')[0] == ref else t for t in x['touch'])) if x['touch'] else ''
                parts.append('1 spoke into a %s mm² piece of pour%s' % (area_txt(x['area']), via))
        if bad:
            if len(bad) == 1 and bad[0]['area'] < 0.6:
                parts.append('1 spoke, a %s mm² stub that joins nothing' % area_txt(bad[0]['area']))
            else:
                parts.append('%d spoke%s into %s mm² of pour that joins nothing else' % (
                    len(bad), '' if len(bad) == 1 else 's', ' + '.join(area_txt(x['area']) for x in bad)))
        if tj:
            ws = sorted({t['w'] for t in tj})
            parts.append('%s %s mm GND track%s' % ({1: 'a', 2: 'two', 3: 'three', 4: 'four'}.get(len(tj), str(len(tj))), '/'.join('%.1f' % w for w in ws), '' if len(tj) == 1 else 's'))
        for t in tb:
            if t['testpad']:
                parts.append('a %.2f mm track to its own test pad, which joins nothing else' % t['w'])
            else:
                parts.append('a %.1f mm GND track that joins nothing else' % t['w'])
        if not parts:
            parts.append('no spoke')
        txt = ', '.join(parts[:-1]) + (' and ' if len(parts) > 1 else '') + parts[-1]
        txt = txt[0].upper() + txt[1:] + '.'
        joined = bool(good or tj)
        out.append((L, joined, txt + (' Joined here.' if joined else '')))
    return out


def joined_on(k, L):
    a = A.get(k + (L,))
    return a is not None and (any(s_['joins'] and s_['area'] >= 2 for s_ in a['spokes']) or any(t['joins'] for t in a['gtr']))


# blocking copper beside the pads (sides without a spoke): other-net copper only
blk = [o[0] for (ref, num, L), a in A.items() for dd in a['dirs'] for o in dd['obs']
       if name_of(o[2], ref) != 'next pin' and o[1] <= 1.6 and not dd['spokes']]
pitch = {}
for (ref, num, L), a in A.items():
    for dd in a['dirs']:
        for o in dd['obs']:
            if name_of(o[2], ref) == 'next pin':
                pitch[ref] = min(pitch.get(ref, 9), o[0])


def nms(ks):
    by = {}
    for r, n in ks:
        by.setdefault(r, []).append(n)
    t = []
    for r, ns in by.items():
        nm = {'U1': 'Pico', 'J9': 'J9'}.get(r, r)
        t.append('%s pin%s %s' % (nm, 's' if len(ns) > 1 else '', ', '.join(ns[:-1]) + (' and ' if len(ns) > 1 else '') + ns[-1]))
    return ', '.join(t[:-1]) + (' and ' if len(t) > 1 else '') + t[-1] if t else ''


thtB = [k for k in order if joined_on(k, 'B')]
smdF = [k for k in order if k + ('B',) not in A and joined_on(k, 'F')]
print('joined on B.Cu:', thtB, ' SMD joined on F.Cu:', smdF, ' blockers %.2f-%.2f mm' % (min(blk), max(blk)), ' pitch', pitch)

# ---------------------------------------------------------------- compose
head = 200
TITLES = {('U1', '8'): 'Pico U1 pin 8 (GND)', ('U1', '28'): 'Pico U1 pin 28 (GND)', ('U1', '3'): 'Pico U1 pin 3 (GND)',
          ('U9', '16'): 'DAC U9 pin 16 (FMT, tied to GND)', ('C5', '2'): 'C5 pad 2 (GND)'}
STRIP_T = {'J9': 'Waveblaster header J9, pins %s (GND, top row)'}
H = 6000
can = Image.new('RGB', (W, H), WHITE); dr = ImageDraw.Draw(can)
ft = font(40, 'Bold'); fs = font(24); fh = font(26, 'SemiBold'); fsm = font(21); fz = font(20); fzb = font(20, 'SemiBold')
dr.text((pad, 28), 'PicoGUS THT v1.3.2: the %d starved thermals' % len(warn), font=ft, fill=INK)
pt = ', '.join('%.2f mm (%s)' % (v_, {'U1': 'Pico', 'J9': 'J9'}.get(r_, r_)) for r_, v_ in sorted(pitch.items(), key=lambda kv: -kv[1]))
for k_, ln in enumerate((
        'DRC wants at least two thermal spokes (green) from a GND pad into the pour; these pads get one, all on F.Cu. A red cross marks each',
        'side where a spoke has no room, labelled with the copper in the way and its distance from the pad. Pour beside a pad needs %.2f mm' % ROOM,
        '(%.1f mm thermal gap + %.2f mm minimum pour + %.1f mm clearance); between neighbouring pins there is %s.' % (GAP, MINW, ZCL, pt))):
    dr.text((pad, 84 + 32 * k_), ln, font=fs, fill=GREY)


def wrap(text, f, width):
    out = []; cur = ''
    for w_ in text.split(' '):
        t = (cur + ' ' + w_).strip()
        if dr.textlength(t, font=f) <= width:
            cur = t
        else:
            out.append(cur); cur = w_
    out.append(cur)
    return out


def layer_tag(x, y, L, bottom=None):
    lab = 'F.Cu (front)' if L == 'F' else 'B.Cu (back)'
    if bottom:
        y = y + bottom - 40
    tb = dr.textbbox((x + 10, y + 10), lab, font=fls)
    dr.rectangle([tb[0] - 5, tb[1] - 4, tb[2] + 5, tb[3] + 4], fill=WHITE, outline=(170, 170, 170))
    dr.text((x + 10, y + 10), lab, font=fls, fill=INK)


def warn_tag(x, y, ks):
    ws = [w for w in warn if (w['ref'], w['num']) in ks]
    kinds = []
    for L in 'FB':
        k = sorted({w['kind'] for w in ws if w['layer'] == L}, reverse=True)
        if k:
            kinds.append('%s.Cu %s' % (L, ' + '.join(x_.replace(' pad', '') for x_ in k) + (' pad' if len(k) == 1 else ' pads')))
    return tag(dr, (x + 5, y), '%d warning%s: %s' % (len(ws), '' if len(ws) == 1 else 's', ', '.join(kinds)), font(17, 'Bold'), RED, anchor='lm')


def write_caps(x, y, caps, width):
    for L, joined, txt in caps:
        head_ = '%s.Cu: ' % L
        for k_, ln in enumerate(wrap(head_ + txt, fz, width - 4)):
            col = (10, 110, 50) if joined else TXT2
            if k_ == 0:
                dr.text((x, y), head_, font=fzb, fill=INK)
                dr.text((x + dr.textlength(head_, font=fzb), y), ln[len(head_):], font=fz, fill=col)
            else:
                dr.text((x, y), ln, font=fz, fill=col)
            y += 27
        y += 6
    return y


y = head
for r in strips:
    ks = byref[r]
    dr.text((pad, y + 6), STRIP_T.get(r, '%s pins %%s (GND)' % r) % (', '.join(k[1] for k in ks[:-1]) + ' and ' + ks[-1][1]), font=fh, fill=INK, anchor='lt')
    warn_tag(pad, y + 56, ks)
    y += 78
    for L in 'FB':
        im, v = panels[(r, L)]
        can.paste(im, (pad, y)); frame(dr, pad, y, im); layer_tag(pad, y, L, bottom=im.size[1])
        y += im.size[1] + 16
    # one caption per layer for all pins of the strip
    yy = y + 4
    for L in 'FB':
        per = {k: A[k + (L,)] for k in ks if k + (L,) in A}
        big = {k: sum(1 for x in a['spokes'] if x['area'] >= 2 and x['joins']) for k, a in per.items()}
        small = {k: [x for x in a['spokes'] if x['area'] < 2] for k, a in per.items()}
        if not any(big.values()):
            ar = sorted({round(x['area'], 2) for v_ in small.values() for x in v_})
            nb = ', '.join('%s → %s' % (k[1], '/'.join(t.split('.')[1] for x in small[k] for t in x['touch'])) for k in ks)
            t = ('each pin has 1 spoke, into a %s mm² piece of pour that only reaches the next GND pin (pin %s); none of them '
                 'reaches the front pour.' % ('/'.join('%.2f' % a_ for a_ in ar), nb))
            j = False
        else:
            cnt = {}
            for k in ks:
                cnt.setdefault(big[k], []).append(k[1])
            def pl(ns):
                return ('pins ' if len(ns) > 1 else 'pin ') + ', '.join(ns[:-1]) + (' and ' if len(ns) > 1 else '') + ns[-1]
            t = ', '.join('%s %s %s' % (pl(ns), 'have' if len(ns) > 1 else 'has', ('%d spokes' % n if n > 1 else '1 spoke') + (' each' if len(ns) > 1 else ''))
                          for n, ns in sorted(cnt.items(), reverse=True)) + ' into the back pour'
            if any(small.values()):
                t += ', plus spokes into the small pieces between neighbouring GND pins'
            t = t[0].upper() + t[1:] + '. Joined here.'
            j = True
        lead = '%s.Cu: ' % L
        for k_, ln in enumerate(wrap(lead + t, fz, W - 2 * pad)):
            col = (10, 110, 50) if j else TXT2
            if k_ == 0:
                dr.text((pad, yy), lead, font=fzb, fill=INK)
                dr.text((pad + dr.textlength(lead, font=fzb), yy), ln[len(lead):], font=fz, fill=col)
            else:
                dr.text((pad, yy), ln, font=fz, fill=col)
            yy += 27
        yy += 6
    y = yy + 34

if tiles_k:
    y0 = y
    TH = TW
    for i, k in enumerate(tiles_k):
        x = pad + i * (TW + gap)
        dr.text((x, y0 + 6), TITLES.get(k, '%s pad %s' % k), font=fh, fill=INK, anchor='lt')
        warn_tag(x, y0 + 56, [k])
    yt = y0 + 78
    for i, k in enumerate(tiles_k):
        x = pad + i * (TW + gap)
        for j, L in enumerate('FB'):
            yy = yt + j * (TH + 30)
            if k + (L,) in panels:
                im, v = panels[k + (L,)]
                can.paste(im, (x, yy)); frame(dr, x, yy, im); layer_tag(x, yy, L)
            else:
                dr.rectangle([x, yy, x + TW - 1, yy + TH - 1], fill=(246, 246, 244), outline=(200, 200, 200))
                for k_, ln in enumerate(('SMD pad: front only.', 'Nothing of it on B.Cu.')):
                    dr.text((x + TW / 2, yy + TH / 2 + (k_ - 0.5) * 30), ln, font=fsm, fill=TXT2, anchor='mm')
    yc = yt + 2 * TH + 30 + 18
    ymax = yc
    for i, k in enumerate(tiles_k):
        x = pad + i * (TW + gap)
        ymax = max(ymax, write_caps(x, yc, caption(*k), TW))
    y = ymax + 10

# legend + why
ly = y + 20
xx = pad
xx = swatch(dr, xx, ly, FPOUR, 'GND pour, front', fsm) + 26
xx = swatch(dr, xx, ly, BPOUR, 'GND pour, back', fsm) + 26
xx = swatch(dr, xx, ly, SPOKE, 'thermal spoke', fsm) + 26
xx = swatch(dr, xx, ly, PAD, 'pad (white: hole)', fsm) + 26
xx = swatch(dr, xx, ly, FTRK, 'other-net track', fsm) + 26
xx = swatch(dr, xx, ly, GNDTRK_F, 'GND track', fsm) + 26
for k_ in range(-22, 34, 8):
    dr.line([xx + max(k_, 0), ly + 22 - max(0, -k_), xx + min(k_ + 22, 34), ly + max(0, k_ + 22 - 34)], fill=DEADCOL, width=2)
dr.rectangle([xx, ly, xx + 34, ly + 22], outline=DEADCOL, width=1)
dr.text((xx + 44, ly + 11), 'dead end (reaches GND only through this pad)', font=fsm, fill=INK, anchor='lm')
ly2 = ly + 38
for k_ in range(0, 34, 11):
    dr.line([pad + k_, ly2 + 11, pad + min(k_ + 7, 34), ly2 + 11], fill=(90, 90, 90), width=2)
dr.text((pad + 44, ly2 + 11), 'Pico module outline', font=fsm, fill=INK, anchor='lm')
xr = pad + 44 + dr.textlength('Pico module outline', font=fsm) + 26
dr.line([xr + 4, ly2 + 2, xr + 22, ly2 + 20], fill=RED, width=4); dr.line([xr + 4, ly2 + 20, xr + 22, ly2 + 2], fill=RED, width=4)
dr.text((xr + 44, ly2 + 11), 'no room for a spoke on this side', font=fsm, fill=INK, anchor='lm')
vy = ly2 + 52
nB = sorted({sum(1 for x in A[k + ('B',)]['spokes'] if x['joins'] and x['area'] >= 2) for k in thtB})
twB = sorted({t['w'] for k in thtB for t in A[k + ('B',)]['gtr'] if t['joins']})
trk_refs = sorted({k[0] for k in thtB if A[k + ('B',)]['gtr']})
WHY = ['Why accepted: each through-hole pad is joined to GND on B.Cu (%s), by %s spoke%s into the back pour'
       % (nms(thtB), '–'.join(str(n) for n in nB), '' if nB == [1] else 's')
       + (' and, at the %s pins, a %s mm GND track.' % (' and '.join({'U1': 'Pico'}.get(r_, r_) for r_ in trk_refs), '/'.join('%.1f' % w for w in twB)) if twB else '.'),
       '%s is an SMD pad, joined by its one front spoke into the front pour (its test pad hangs on it). These are low-current GND returns (J9\'s'
       % nms(smdF),
       'GND pins, the Pico\'s GND pins, which are also tied inside the module) and the DAC\'s FMT strap, an input. A second front spoke would need',
       'the tracks beside the pads moved: they pass %.2f–%.2f mm from them; the WT_MIDI track runs along J9\'s GND row.' % (min(blk), max(blk))]
for ln in WHY:
    dr.text((pad, vy), ln, font=fsm, fill=TXT2); vy += 30
    if dr.textlength(ln, font=fsm) > W - 2 * pad:
        print('WARNING: footer line too long:', ln)
can = can.crop((0, 0, W, vy + 22))
can.save(OUT)
print('saved', OUT, can.size)
