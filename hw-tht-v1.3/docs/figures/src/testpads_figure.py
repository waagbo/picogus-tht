# python3 testpads_figure.py <TSSOP-20 ..._TestPads.kicad_mod> <board_crop.png or -> <out.png>
# board_crop.png: the U9 area (x 154.5-183.5, y 70.0-95.5) cut from 'kicad-cli pcb render --side top' of the board
# Draws the figure that explains the U3/U4/U9 test pads: the footprint geometry to scale (left) and,
# when a crop of the routed board render is given, the real U9 (right). Needs Pillow.
import sys, re, math
from PIL import Image, ImageDraw, ImageFont

FONT_DIR = '/usr/share/fonts/opentype/inter/'
def font(sz, w='Medium'):
    try: return ImageFont.truetype(FONT_DIR + 'Inter-%s.otf' % w, sz)
    except OSError: return ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', sz)
BG = (250, 250, 248); INK = (30, 32, 36); SUB = (70, 70, 70); PAD = (214, 168, 40); RED = (229, 57, 53)
TRK = (176, 98, 44); STUB = (120, 140, 200); BODY = (58, 60, 66); FRAME = (200, 200, 200)

s = open(sys.argv[1]).read()
pins, tps = {}, {}
for m in re.finditer(r'\(pad "(\d+)" smd (roundrect|circle)\s*\(at ([-\d.]+) ([-\d.]+)\)\s*\(size ([\d.]+) ([\d.]+)\)', s):
    n, kind, x, y, w, h = m.group(1), m.group(2), *map(float, m.groups()[2:])
    (tps if kind == 'circle' else pins)[n] = (x, y, w, h)
ESC = 5.80
K = 78.0                      # px per mm
W0, H0 = 1440, 960            # left panel
cx, cy = 760, 420
def P(x, y): return (cx + x * K, cy + y * K)

def panel_geometry():
    im = Image.new('RGB', (W0, H0), BG); d = ImageDraw.Draw(im)
    # chip body
    x0, y0 = P(-2.2, -3.25); x1, y1 = P(2.2, 3.25)
    # tracks first (under the pads)
    for n, (tx, ty, tw, th) in tps.items():
        px, py = pins[n][0], pins[n][1]
        d.line([P(px, py), P(tx, ty)], fill=TRK, width=round(0.2 * K))
        if abs(tx) < 4.8:
            sx = math.copysign(ESC, tx)
            d.line([P(tx, ty), P(sx, ty)], fill=STUB, width=round(0.2 * K))
            r = 0.1 * K; ex, ey = P(sx, ty); d.ellipse([ex - r, ey - r, ex + r, ey + r], fill=STUB)
    for n, (x, y, w, h) in pins.items():
        a, b = P(x - w / 2, y - h / 2); c, e = P(x + w / 2, y + h / 2)
        d.rounded_rectangle([a, b, c, e], radius=0.1 * K, fill=PAD)
    for n, (x, y, w, h) in tps.items():
        a, b = P(x - w / 2, y - h / 2); c, e = P(x + w / 2, y + h / 2)
        d.ellipse([a, b, c, e], fill=PAD)
    d.rectangle([x0, y0, x1, y1], fill=BODY)
    r = 0.25 * K; dx, dy = P(-1.5, -2.6); d.ellipse([dx - r, dy - r, dx + r, dy + r], fill=(110, 112, 120))
    d.text(P(0, 0), 'U9\nTSSOP-20', font=font(30, 'SemiBold'), fill=(235, 235, 235), anchor='mm', align='center')
    # pin numbers at the column ends
    f = font(24, 'SemiBold')
    for n in ('1', '10', '11', '20'):
        x, y = tps[n][0], tps[n][1]
        d.text((P(x, y)[0], P(x, y)[1] + (-0.75 * K if y < 0 else 0.75 * K)), n, font=f, fill=INK, anchor='mm')
    # dimensions
    fd = font(23, 'Medium'); RB = dict(fill=RED, width=3)
    def dim_h(xa, xb, y, text, above=True):
        a, b = P(xa, y), P(xb, y)
        d.line([a, b], **RB)
        for q in (a, b): d.line([(q[0], q[1] - 12), (q[0], q[1] + 12)], **RB)
        d.text(((a[0] + b[0]) / 2, a[1] + (-22 if above else 24)), text, font=fd, fill=RED, anchor='mm')
    def dim_v(x, ya, yb, text, right=True):
        a, b = P(x, ya), P(x, yb)
        d.line([a, b], **RB)
        for q in (a, b): d.line([(q[0] - 12, q[1]), (q[0] + 12, q[1])], **RB)
        d.text((a[0] + (16 if right else -16), (a[1] + b[1]) / 2), text, font=fd, fill=RED, anchor='lm' if right else 'rm')
    dim_h(0, 4.45, -3.95, 'inner column 4.45')
    dim_h(0, 5.10, 4.25, 'outer column 5.10', above=False)
    dim_v(6.25, -2.925, -2.275, '0.65 pin pitch', right=True)
    dim_v(-6.25, 1.625, 2.925, '', right=False)
    # 1.3 mm between pads in a column
    a, b = P(-6.25, 1.625), P(-6.25, 2.925)
    d.text((b[0] - 16, (a[1] + b[1]) / 2), '1.3 between two\npads of one column', font=fd, fill=RED, anchor='rm', align='right')
    # clearance callout: the 0.2 track to outer pad 4 passes inner pads 3 and 5
    gx, gy = P(-4.45, -0.975)
    d.ellipse([gx - 0.62 * K, gy - 0.62 * K, gx + 0.62 * K, gy + 0.62 * K], outline=RED, width=3)
    d.line([(gx - 0.62 * K, gy), (gx - 1.6 * K, gy)], fill=RED, width=3)
    d.text((gx - 1.7 * K, gy), 'a 0.2 track between\ntwo pads: 0.2\nclearance each side', font=fd, fill=RED, anchor='rm', align='right')
    # legend
    ly = H0 - 150; lx = 50; fl = font(23, 'Medium')
    items = [(PAD, 'pin / test pad (0.7 round, bare copper)'), (TRK, 'pin to test pad, 0.2 track'),
             (STUB, 'escape stub to the routing (inner-column pins that are used)')]
    for i, (col, t) in enumerate(items):
        y = ly + i * 40
        d.rectangle([lx, y - 10, lx + 46, y + 10], fill=col)
        d.text((lx + 62, y), t, font=fl, fill=INK, anchor='lm')
    d.text((W0 - 40, H0 - 40), 'dimensions in mm, to scale', font=font(20, 'Regular'), fill=SUB, anchor='rb')
    return im

left = panel_geometry()
right = None
if len(sys.argv) > 3 and sys.argv[2] != '-':
    right = Image.open(sys.argv[2]).convert('RGB')
    right = right.resize((round(right.width * H0 / right.height), H0), Image.LANCZOS)
W = 60 + left.width + (40 + right.width if right else 0) + 60
H = 230 + H0 + 40
out = Image.new('RGB', (W, H), (255, 255, 255)); d = ImageDraw.Draw(out)
d.text((60, 40), 'Test pads on U3, U4 and U9', font=font(40, 'Bold'), fill=INK)
sub = ('The TSSOPs are soldered straight to the card. Every pin has its own 0.7 mm round test pad straight out from it,\n'
       'for a probe or a meter. The pads alternate between two columns so each one sits on its own pin\'s line. A 0.2 mm track\n'
       'to an outer pad passes between two inner pads with the board\'s normal 0.2 mm clearance; a used inner-column pin\n'
       'reaches the routing the same way, through a short stub between two outer pads.')
d.multiline_text((60, 98), sub, font=font(23, 'Regular'), fill=SUB, spacing=8)
out.paste(left, (60, 230)); d.rectangle([60, 230, 60 + left.width - 1, 230 + H0 - 1], outline=FRAME, width=2)
if right:
    x = 100 + left.width
    out.paste(right, (x, 230)); d.rectangle([x, 230, x + right.width - 1, 230 + H0 - 1], outline=FRAME, width=2)
    lab = 'U9 on the routed board (3D render, red mask)'; fl = font(26, 'SemiBold')
    tw = d.textlength(lab, font=fl); d.rectangle([x + 14, 244, x + 34 + tw, 284], fill=(255, 255, 255)); d.text((x + 24, 264), lab, font=fl, fill=INK, anchor='lm')
out.save(sys.argv[-1])
print(out.size)
