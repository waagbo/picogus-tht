# Shared helpers for the copper figures (imported by gold_fingers_figure.py, b10_ground_figure.py,
# i2s_dac_figure.py). Not run on its own. Needs Pillow, numpy and shapely (python3.12 here).
# Reads the JSON written by copper_dump.py and draws board geometry with PIL in the style of
# courtyard_figure.py: white page, Inter fonts, bold title, grey subtitle, boxed red measurements.
import json, math, fnmatch
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import Polygon, MultiPolygon, LineString, Point, box, GeometryCollection
from shapely.ops import unary_union, nearest_points

FONT_DIR = '/usr/share/fonts/opentype/inter/'

def font(sz, w='Medium'):
    try:
        return ImageFont.truetype(FONT_DIR + 'Inter-%s.otf' % w, sz)
    except OSError:
        return ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', sz)

# palette (template colours first)
BG = (250, 250, 248); INK = (30, 32, 36); GREY = (80, 80, 80); SUB = (60, 60, 60)
PAD = (214, 168, 40); RED = (229, 57, 53); FRAME = (200, 200, 200)
OUTSIDE = (255, 255, 255)
NET_COL = {'+12V': (236, 122, 18), '-12V': (128, 64, 200), '+5V': (214, 45, 45), '+3V3': (190, 90, 150),
           'GND': (32, 140, 88)}
POUR = (212, 236, 220)        # GND pour on the layer shown
SIG = (150, 152, 158)         # other signal copper
SIG_DARK = (110, 112, 118)
MASK_OPEN = (240, 226, 196)   # bare laminate in a mask opening
HATCH = (150, 150, 150)

def ring(c):
    return [tuple(p) for p in c]

def poly_of(rings):
    """[[outer, hole...], ...] -> shapely geometry"""
    gs = []
    for r in rings:
        if len(r[0]) >= 3:
            gs.append(Polygon(r[0], [h for h in r[1:] if len(h) >= 3]).buffer(0))
    return unary_union(gs) if gs else GeometryCollection()

class Board:
    def __init__(self, path, pro=None):
        self.d = d = json.load(open(path))
        self.pat = []
        if pro:
            self.pat = json.load(open(pro))['net_settings'].get('netclass_patterns', [])
        for t in d['tracks']:
            t['cl'] = LineString([t['a'], t['b']])
            t['g'] = t['cl'].buffer(t['w'] / 2, quad_segs=24)
        for v in d['vias']:
            v['g'] = Point(v['pos']).buffer(v['dia'] / 2, quad_segs=24)
        for p in d['pads']:
            p['g'] = {L: poly_of(r) for L, r in p['cu'].items()}
        self.fill = {}
        for z in d['zones']:
            self.fill.setdefault((z['net'], z['layer']), []).append(poly_of(z['fill']))
        self.fill = {k: unary_union(v) for k, v in self.fill.items()}
        self.edges = [LineString(e) for e in d['edges']]

    def netclass(self, net):
        for p in self.pat:
            if fnmatch.fnmatchcase(net, p['pattern']):
                return p['netclass']
        return 'Default'

    def pad(self, ref, num):
        return next(p for p in self.d['pads'] if p['ref'] == ref and p['num'] == str(num))

    def copper(self, layer, nets=None, pred=None, pours=True):
        """union of copper on a layer for a net set (or predicate on net)"""
        ok = (lambda n: n in nets) if nets is not None else pred
        gs = [t['g'] for t in self.d['tracks'] if t['layer'] == layer and ok(t['net'])]
        gs += [v['g'] for v in self.d['vias'] if ok(v['net'])]
        gs += [p['g'][layer] for p in self.d['pads'] if layer in p['g'] and ok(p['net'])]
        if pours:
            gs += [g for (n, L), g in self.fill.items() if L == layer and ok(n)]
        return unary_union(gs) if gs else GeometryCollection()

def polys(g):
    if g.is_empty:
        return []
    if isinstance(g, Polygon):
        return [g]
    if isinstance(g, MultiPolygon):
        return list(g.geoms)
    if hasattr(g, 'geoms'):
        out = []
        for x in g.geoms:
            out += polys(x)
        return out
    return []

class Panel:
    """A board window x0..x1, y0..y1 (mm) drawn at S px/mm, supersampled by SS for the copper."""
    def __init__(self, x0, y0, x1, y1, S, SS=3, bg=BG, mirror=False):
        self.x0, self.y0, self.x1, self.y1, self.S, self.SS = x0, y0, x1, y1, S, SS
        self.W, self.H = round((x1 - x0) * S), round((y1 - y0) * S)
        self.mirror = mirror
        self.im = Image.new('RGB', (self.W * SS, self.H * SS), bg)
        self.win = box(x0 - 1, y0 - 1, x1 + 1, y1 + 1)
        self.final = None

    # coordinates
    def Tss(self, p):
        x = (self.x1 - p[0]) if self.mirror else (p[0] - self.x0)
        return (x * self.S * self.SS, (p[1] - self.y0) * self.S * self.SS)

    def T(self, p):
        x = (self.x1 - p[0]) if self.mirror else (p[0] - self.x0)
        return (x * self.S, (p[1] - self.y0) * self.S)

    # supersampled drawing
    def fill(self, g, col, alpha=255):
        if g.is_empty:
            return
        gb = g.bounds; wb = self.win.bounds
        if gb[2] < wb[0] or gb[0] > wb[2] or gb[3] < wb[1] or gb[1] > wb[3]:
            return
        g = g.intersection(self.win)
        ps = sorted(polys(g), key=lambda q: -q.area)
        if not ps:
            return
        # rasterise into a mask the size of the item's bounding box only
        xs, ys = [], []
        for q in ps:
            for c in q.exterior.coords:
                X, Y = self.Tss(c); xs.append(X); ys.append(Y)
        X0 = max(0, int(min(xs)) - 2); Y0 = max(0, int(min(ys)) - 2)
        X1 = min(self.im.size[0], int(max(xs)) + 3); Y1 = min(self.im.size[1], int(max(ys)) + 3)
        if X1 <= X0 or Y1 <= Y0:
            return
        m = Image.new('L', (X1 - X0, Y1 - Y0), 0)
        dm = ImageDraw.Draw(m)
        off = lambda c: (self.Tss(c)[0] - X0, self.Tss(c)[1] - Y0)
        for q in ps:
            dm.polygon([off(c) for c in q.exterior.coords], fill=alpha)
            for h in q.interiors:
                dm.polygon([off(c) for c in h.coords], fill=0)
        self.im.paste(Image.new('RGB', m.size, col), (X0, Y0), m)

    def outline(self, g, col, width=1.0, dash=None):
        """width in final px"""
        g = g.intersection(self.win.buffer(1)) if not g.is_empty else g
        dr = ImageDraw.Draw(self.im)
        w = max(1, round(width * self.SS))
        lines = []
        for q in polys(g):
            lines.append(list(q.exterior.coords))
            lines += [list(h.coords) for h in q.interiors]
        if isinstance(g, (LineString,)):
            lines.append(list(g.coords))
        for ln in lines:
            pts = [self.Tss(c) for c in ln]
            if dash:
                self._dash(dr, pts, col, w, dash)
            else:
                dr.line(pts, fill=col, width=w, joint='curve')

    def _dash(self, dr, pts, col, w, dash):
        on, off = dash[0] * self.SS, dash[1] * self.SS
        for a, b in zip(pts, pts[1:]):
            L = math.dist(a, b)
            if L == 0:
                continue
            t = 0
            while t < L:
                t2 = min(L, t + on)
                dr.line([(a[0] + (b[0] - a[0]) * t / L, a[1] + (b[1] - a[1]) * t / L),
                         (a[0] + (b[0] - a[0]) * t2 / L, a[1] + (b[1] - a[1]) * t2 / L)], fill=col, width=w)
                t += on + off

    def hatch(self, g, col, step=10, width=1.0, angle=45):
        """diagonal hatch (step and width in final px) clipped to g"""
        g = g.intersection(self.win)
        if g.is_empty:
            return
        m = Image.new('L', self.im.size, 0)
        dm = ImageDraw.Draw(m)
        for q in sorted(polys(g), key=lambda q: -q.area):
            dm.polygon([self.Tss(c) for c in q.exterior.coords], fill=255)
            for h in q.interiors:
                dm.polygon([self.Tss(c) for c in h.coords], fill=0)
        lines = Image.new('L', self.im.size, 0)
        dl = ImageDraw.Draw(lines)
        W, H = self.im.size
        st = step * self.SS
        for k in range(-H, W + H, max(1, round(st))):
            dl.line([(k, 0), (k + H, H)] if angle == 45 else [(k, H), (k + H, 0)], fill=255, width=max(1, round(width * self.SS)))
        from PIL import ImageChops
        mm_ = ImageChops.multiply(m, lines)
        self.im.paste(Image.new('RGB', self.im.size, col), (0, 0), mm_)

    def holes(self, items, col=BG):
        """drill holes: list of (x, y, d)"""
        dr = ImageDraw.Draw(self.im)
        for x, y, dd in items:
            cx, cy = self.Tss((x, y)); r = dd / 2 * self.S * self.SS
            dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)

    def done(self):
        if self.final is None:
            self.final = self.im.resize((self.W, self.H), Image.LANCZOS)
        return self.final

# final-resolution annotation helpers (draw on the composed canvas with an offset)
class Annot:
    def __init__(self, can, panel, ox, oy):
        self.can, self.p, self.ox, self.oy = can, panel, ox, oy
        self.dr = ImageDraw.Draw(can)

    def P(self, pt):
        x, y = self.p.T(pt)
        return (self.ox + x, self.oy + y)

    def measure(self, a, b, text, at=None, anchor='mm', size=19, col=RED, line=True, tick=True):
        """red dimension line between mm points a and b, boxed label at 'at' (mm point or px offset tuple ('px', dx, dy))"""
        A, B = self.P(a), self.P(b)
        if line:
            self.dr.line([A, B], fill=col, width=2)
            if tick:
                for c in (A, B):
                    self.dr.ellipse([c[0] - 3, c[1] - 3, c[0] + 3, c[1] + 3], fill=col)
        if at is None:
            at = ('px', 0, -22)
        if at[0] == 'px':
            mx, my = (A[0] + B[0]) / 2 + at[1], (A[1] + B[1]) / 2 + at[2]
        else:
            mx, my = self.P(at)
        self.box_label((mx, my), text, size=size, col=col, anchor=anchor, leader=((A[0] + B[0]) / 2, (A[1] + B[1]) / 2) if line else None)

    def box_label(self, xy, text, size=19, col=RED, anchor='mm', leader=None, fill=(255, 255, 255), w='Bold', width=1):
        f = font(size, w)
        tb = self.dr.textbbox(xy, text, font=f, anchor=anchor)
        r = [tb[0] - 5, tb[1] - 4, tb[2] + 5, tb[3] + 4]
        if leader is not None:
            # leader from the nearest point of the box to the target
            lx = min(max(leader[0], r[0]), r[2]); ly = min(max(leader[1], r[1]), r[3])
            if math.dist((lx, ly), leader) > 6:
                self.dr.line([(lx, ly), leader], fill=col, width=1)
        self.dr.rounded_rectangle(r, radius=4, fill=fill, outline=col, width=width)
        self.dr.text(xy, text, font=f, fill=col, anchor=anchor)
        return r

    def text(self, pt, text, size=18, col=INK, anchor='mm', w='SemiBold', halo=True, mmpt=True):
        f = font(size, w)
        xy = self.P(pt) if mmpt else pt
        if halo:
            self.dr.text(xy, text, font=f, fill=col, anchor=anchor, stroke_width=3, stroke_fill=(255, 255, 255))
        else:
            self.dr.text(xy, text, font=f, fill=col, anchor=anchor)

    def leader_text(self, target, xy_px, text, size=18, col=INK, anchor='lm', w='SemiBold', lcol=None):
        T = self.P(target)
        X = (T[0] + xy_px[0], T[1] + xy_px[1])
        self.dr.line([T, X], fill=lcol or col, width=1)
        self.dr.ellipse([T[0] - 2.5, T[1] - 2.5, T[0] + 2.5, T[1] + 2.5], fill=lcol or col)
        f = font(size, w)
        pad = 4 if anchor[0] == 'l' else -4
        self.dr.text((X[0] + pad, X[1]), text, font=f, fill=col, anchor=anchor, stroke_width=3, stroke_fill=(255, 255, 255))

def gap(g1, g2):
    """distance and the two nearest points"""
    a, b = nearest_points(g1, g2)
    return g1.distance(g2), (a.x, a.y), (b.x, b.y)

def heading(dr, xy, text, size=27):
    """panel heading; xy is the bottom-left above the panel (text sits on a common baseline)"""
    dr.text((xy[0], xy[1] - round(size * 0.28)), text, font=font(size, 'SemiBold'), fill=INK, anchor='ls')

def frame(dr, x, y, w, h):
    dr.rectangle([x, y, x + w - 1, y + h - 1], outline=FRAME)

def swatch(dr, x, y, txt, fill=None, outline=None, kind='box', fs=None, hatch=None):
    fs = fs or font(22)
    if kind == 'box':
        dr.rectangle([x, y, x + 34, y + 22], fill=fill, outline=outline, width=2 if outline else 0)
        if hatch:
            sw = Image.new('RGB', (35, 23), fill or (255, 255, 255))
            ds = ImageDraw.Draw(sw)
            for k in range(-23, 35, 7):
                ds.line([(k, 23), (k + 23, 0)], fill=hatch, width=1)
            dr._image.paste(sw, (round(x), round(y)))
            dr.rectangle([x, y, x + 34, y + 22], outline=outline or hatch, width=2)
    elif kind == 'line':
        dr.line([x, y + 11, x + 34, y + 11], fill=fill, width=6)
    elif kind == 'ring':
        dr.ellipse([x + 6, y, x + 28, y + 22], fill=fill)
        dr.ellipse([x + 12, y + 6, x + 22, y + 16], fill=(255, 255, 255))
    dr.text((x + 44, y + 11), txt, font=fs, fill=INK, anchor='lm')
    return x + 44 + dr.textlength(txt, font=fs) + 36
