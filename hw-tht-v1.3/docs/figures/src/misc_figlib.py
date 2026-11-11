# Shared helpers for starved_thermals_figure.py, silk_vias_figure.py and bracket_trim_figure.py (PicoGUS THT v1.3.2
# README figures). Not run on its own; the figure scripts run with kipython, which has shapely, numpy and Pillow.
# Style follows docs/figures/src/courtyard_figure.py: white page, Inter fonts, bold title, grey subtitle, labelled
# panels, red boxed measurement labels, legend and a short footer.
import json, math
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import Polygon, MultiPolygon, LineString, Point, box, GeometryCollection
from shapely.ops import unary_union

FONTDIR = '/usr/share/fonts/opentype/inter/'


def font(sz, w='Medium'):
    try:
        return ImageFont.truetype(FONTDIR + 'Inter-%s.otf' % w, sz)
    except OSError:
        return ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', sz)


# colours (the courtyard figure's, plus layer colours)
WHITE = (255, 255, 255)
BG = (250, 250, 248)
INK = (30, 32, 36)
GREY = (80, 80, 80)
TXT2 = (60, 60, 60)
PAD = (214, 168, 40)
FAB = (140, 140, 140)
CRT = (30, 110, 220)
RED = (229, 57, 53)
FPOUR = (236, 160, 150)     # F.Cu GND pour (light copper red)
BPOUR = (150, 182, 228)     # B.Cu GND pour (light blue)
FTRK = (200, 70, 60)        # F.Cu tracks of other nets
BTRK = (55, 100, 180)       # B.Cu tracks of other nets
GNDTRK_F = (150, 40, 35)
GNDTRK_B = (25, 60, 130)
SPOKE = (20, 150, 70)       # highlighted thermal spokes
HOLE = (255, 255, 255)
SILK = (60, 60, 60)
EDGE = (200, 160, 0)


def load(path):
    return json.load(open(path))


def rings_to_poly(p):
    """[[outline, hole...]] item -> shapely Polygon"""
    if len(p[0]) < 3:
        return None
    g = Polygon(p[0], [h for h in p[1:] if len(h) >= 3])
    return g if g.is_valid else g.buffer(0)


def polys_to_geom(pl):
    gs = [g for g in (rings_to_poly(p) for p in pl) if g is not None and not g.is_empty]
    return unary_union(gs) if gs else GeometryCollection()


def track_geom(t):
    return LineString(t['pl']).buffer(t['w'] / 2, quad_segs=16) if len(t['pl']) > 1 else Point(t['pl'][0]).buffer(t['w'] / 2)


def hole_geom(pd):
    h = pd['hole']
    if not h:
        return None
    if math.dist(h['a'], h['b']) < 1e-6:
        return Point(h['a']).buffer(h['r'], quad_segs=32)
    return LineString([h['a'], h['b']]).buffer(h['r'], quad_segs=32)


class View:
    """Board-mm window rendered at S px/mm; geometry drawn at SS x supersampling, then reduced."""

    def __init__(self, x0, y0, x1, y1, S, ss=3, bg=BG):
        self.x0, self.y0, self.x1, self.y1, self.S, self.ss = x0, y0, x1, y1, S, ss
        self.W, self.H = round((x1 - x0) * S), round((y1 - y0) * S)
        self.big = Image.new('RGB', (self.W * ss, self.H * ss), bg)
        self.clip = box(x0 - 1, y0 - 1, x1 + 1, y1 + 1)
        self.img = None

    def T(self, p, k=1):
        return ((p[0] - self.x0) * self.S * k, (p[1] - self.y0) * self.S * k)

    # --- geometry, drawn on the supersampled canvas
    def _mask(self, geom):
        m = Image.new('L', self.big.size, 0)
        d = ImageDraw.Draw(m)
        k = self.ss
        geom = geom.intersection(self.clip) if not geom.is_empty else geom
        for g in getattr(geom, 'geoms', [geom]):
            if g.is_empty:
                continue
            if g.geom_type == 'Polygon':
                d.polygon([self.T(q, k) for q in g.exterior.coords], fill=255)
                for r in g.interiors:
                    d.polygon([self.T(q, k) for q in r.coords], fill=0)
            elif g.geom_type in ('MultiPolygon', 'GeometryCollection'):
                m2 = self._mask(g)
                m.paste(255, (0, 0), m2)
        return m

    def fill(self, geom, col, alpha=255):
        if geom is None or geom.is_empty:
            return
        m = self._mask(geom)
        if alpha < 255:
            m = m.point(lambda v: v * alpha // 255)
        self.big.paste(col, (0, 0), m)

    def outline(self, geom, col, width_px=2, dash=None):
        if geom is None or geom.is_empty:
            return
        d = ImageDraw.Draw(self.big)
        k = self.ss
        geom = geom.intersection(self.clip)
        lines = []
        def walk(g):
            if g.is_empty:
                return
            if g.geom_type == 'Polygon':
                lines.append(list(g.exterior.coords)); lines.extend(list(r.coords) for r in g.interiors)
            elif g.geom_type in ('LineString', 'LinearRing'):
                lines.append(list(g.coords))
            elif hasattr(g, 'geoms'):
                for h in g.geoms:
                    walk(h)
        walk(geom)
        for ln in lines:
            if len(ln) < 2:
                continue
            pts = [self.T(q, k) for q in ln]
            if dash:
                self._dashed(d, pts, col, width_px * k, dash[0] * k, dash[1] * k)
            else:
                d.line(pts, fill=col, width=round(width_px * k), joint='curve')

    @staticmethod
    def _dashed(d, pts, col, w, on, off):
        acc = 0.0; draw = True; lim = on
        for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
            L = math.hypot(xb - xa, yb - ya); t = 0.0
            while t < L:
                step = min(lim - acc, L - t)
                x1, y1 = xa + (xb - xa) * t / L, ya + (yb - ya) * t / L
                x2, y2 = xa + (xb - xa) * (t + step) / L, ya + (yb - ya) * (t + step) / L
                if draw:
                    d.line([(x1, y1), (x2, y2)], fill=col, width=round(w))
                t += step; acc += step
                if acc >= lim - 1e-9:
                    acc = 0.0; draw = not draw; lim = on if draw else off

    def hatch(self, geom, col, spacing_px=7, width_px=1.6):
        """diagonal hatch over geom"""
        if geom is None or geom.is_empty:
            return
        k = self.ss
        pat = Image.new('L', self.big.size, 0)
        d = ImageDraw.Draw(pat)
        W, H = self.big.size
        sp = spacing_px * k
        for x in range(-H, W, round(sp)):
            d.line([(x, H), (x + H, 0)], fill=255, width=round(width_px * k))
        m = self._mask(geom)
        from PIL import ImageChops
        self.big.paste(col, (0, 0), ImageChops.multiply(m, pat))

    def grid(self, step=1.0, col=(232, 232, 228)):
        d = ImageDraw.Draw(self.big); k = self.ss
        x = math.ceil(self.x0 / step) * step
        while x <= self.x1:
            d.line([self.T((x, self.y0), k), self.T((x, self.y1), k)], fill=col, width=k); x += step
        y = math.ceil(self.y0 / step) * step
        while y <= self.y1:
            d.line([self.T((self.x0, y), k), self.T((self.x1, y), k)], fill=col, width=k); y += step

    def finish(self):
        self.img = self.big.resize((self.W, self.H), Image.LANCZOS)
        return self.img

    # --- annotations, drawn on the final image
    def draw(self):
        return ImageDraw.Draw(self.img)


def tag(dr, xy, text, f, col=RED, anchor='lm', bg=WHITE, pad=(5, 3), width=1, clamp=None):
    """boxed label; clamp=(x0, y0, x1, y1) keeps the box inside that rectangle"""
    tb = dr.textbbox(xy, text, font=f, anchor=anchor)
    if clamp:
        dx = max(0, clamp[0] + pad[0] + 2 - tb[0]) - max(0, tb[2] + pad[0] + 2 - clamp[2])
        dy = max(0, clamp[1] + pad[1] + 2 - tb[1]) - max(0, tb[3] + pad[1] + 2 - clamp[3])
        xy = (xy[0] + dx, xy[1] + dy)
        tb = dr.textbbox(xy, text, font=f, anchor=anchor)
    dr.rounded_rectangle([tb[0] - pad[0], tb[1] - pad[1], tb[2] + pad[0], tb[3] + pad[1]], radius=4, fill=bg, outline=col, width=width)
    dr.text(xy, text, font=f, fill=col, anchor=anchor)
    return tb


def arrow(dr, a, b, col, w=2, head=10):
    dr.line([a, b], fill=col, width=w)
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    p1 = (b[0] - head * math.cos(ang - 0.45), b[1] - head * math.sin(ang - 0.45))
    p2 = (b[0] - head * math.cos(ang + 0.45), b[1] - head * math.sin(ang + 0.45))
    dr.polygon([b, p1, p2], fill=col)


def dim(dr, a, b, text, f, col=RED, w=2, head=9, side='above', off=6):
    """two-headed dimension arrow a<->b with a boxed label"""
    arrow(dr, b, a, col, w, head); arrow(dr, a, b, col, w, head)
    mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
    if side == 'above':
        tag(dr, (mx, my - off), text, f, col, anchor='mb')
    elif side == 'below':
        tag(dr, (mx, my + off), text, f, col, anchor='mt')
    elif side == 'right':
        tag(dr, (mx + off, my), text, f, col, anchor='lm')
    else:
        tag(dr, (mx - off, my), text, f, col, anchor='rm')


def frame(dr, x, y, im, col=(200, 200, 200)):
    dr.rectangle([x, y, x + im.size[0] - 1, y + im.size[1] - 1], outline=col)


def swatch(dr, x, y, col, txt, f, kind='fill', edge=None):
    if kind == 'fill':
        dr.rectangle([x, y, x + 34, y + 22], fill=col, outline=edge or col, width=2)
    elif kind == 'box':
        dr.rectangle([x, y, x + 34, y + 22], outline=col, width=3)
    elif kind == 'line':
        dr.line([x, y + 11, x + 34, y + 11], fill=col, width=4)
    elif kind == 'dash':
        for k in range(0, 34, 10):
            dr.line([x + k, y + 11, x + min(k + 6, 34), y + 11], fill=col, width=3)
    elif kind == 'ring':
        dr.ellipse([x + 6, y, x + 28, y + 22], fill=col)
        dr.ellipse([x + 12, y + 6, x + 22, y + 16], fill=WHITE)
    dr.text((x + 44, y + 11), txt, font=f, fill=INK, anchor='lm')
    return x + 44 + dr.textlength(txt, font=f)
