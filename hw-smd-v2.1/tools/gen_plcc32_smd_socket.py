#!/usr/bin/env python3
"""Writes the footprint and a simple 3D model of a surface-mount PLCC-32 socket (PicoGUS v2.1 SMD boot ROM).

    python3 hw-smd-v2.1/tools/gen_plcc32_smd_socket.py

  hw-common/PiGUS library.pretty/PLCC-32_SMD-Socket.kicad_mod
  hw-common/parts/PLCC-32_SMD-Socket.wrl

KiCad's library has no surface-mount socket for the 32-pin PLCC. The land pattern here is the one the makers of the
sockets in distributor stock print (read 2026-11-27):

  Amphenol FCI 69802-x32LF, customer drawing 69802 rev AG: rows 11.02 mm (the 9-pin sides) and 13.56 mm (the 7-pin
      sides) centre to centre, pads 0.76 x 1.91 mm, pitch 1.27 mm; housing 17.78 x 20.32 mm, 4.47 mm high
  Assmann WSW A-CCS 032-Z-SM (drawing ASS 0142 CO), Mill-Max 940-44-032-17-400000, Kycon SMPX-32LCC: the same rows,
      pads 0.76 x 1.78 mm; housings up to 18.19 x 20.80 mm, 4.45 to 4.6 mm high

(Preci-Dip's series 540 has rows 10.67 and 13.21 mm apart and does not fit.) The pads are FCI's, the longest; the
outline is Assmann's housing, the largest. Take a socket without locating pegs (FCI -132LF/-432LF, Assmann without
/P): the footprint has no holes for them.

The lands of the PLCC-32 chip itself (KiCad's PLCC-32_11.4x14.0mm_P1.27mm: rows 11.125 and 13.675 mm, pads
0.6 x 1.475 mm) lie inside these pads, so the flash can also be soldered straight to the board.

On all these sockets the solder tails turn inwards under the housing: they cannot be reached from outside. They are
made for reflow; by hand it takes hot air, or a fine tip through the open frame before the chip goes in.

Pin 1 is in the middle of the top row and the numbers run counter-clockwise, as on KiCad's PLCC footprints (the
drawings mark pin 1 there, with the chamfered corner to its left, and print no other numbers).
The model is for the renders only (frame, contacts, chip with a label).
"""
import os, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MOD = os.path.join(ROOT, 'hw-common', 'PiGUS library.pretty', 'PLCC-32_SMD-Socket.kicad_mod')
WRL = os.path.join(ROOT, 'hw-common', 'parts', 'PLCC-32_SMD-Socket.wrl')
NAME = 'PLCC-32_SMD-Socket'

P = 1.27
NX, NY = 7, 9                         # pins on the top/bottom rows and on the left/right rows
HX, HY = (NX - 1) / 2 * P, (NY - 1) / 2 * P          # 3.81, 5.08: end pins of the rows
ROW = 1.70                            # row centre line beyond the end pins of the neighbouring rows
PAD_L, PAD_W = 1.91, 0.76
RX, RY = HX + ROW, HY + ROW           # 5.51, 6.78: pad row centre lines (rows 11.02 and 13.56 mm apart)
BX, BY = 18.19 / 2, 20.80 / 2         # housing half sizes (Assmann, the largest)
TOP = 4.5                             # housing height
CHIP_W, CHIP_H = 11.43, 13.97


def pads():
    """(number, x, y, size_x, size_y), numbered as the chip: 1 top centre, then counter-clockwise"""
    out = [(1, 0.0, -RY, PAD_W, PAD_L)]
    n = 2
    for k in range(1, (NX - 1) // 2 + 1): out.append((n, -k * P, -RY, PAD_W, PAD_L)); n += 1          # 2-4
    for k in range(NY): out.append((n, -RX, -HY + k * P, PAD_L, PAD_W)); n += 1                       # 5-13
    for k in range(NX): out.append((n, -HX + k * P, RY, PAD_W, PAD_L)); n += 1                        # 14-20
    for k in range(NY): out.append((n, RX, HY - k * P, PAD_L, PAD_W)); n += 1                         # 21-29
    for k in range((NX - 1) // 2): out.append((n, HX - k * P, -RY, PAD_W, PAD_L)); n += 1            # 30-32
    return out


def uid(s): return str(uuid.uuid5(uuid.NAMESPACE_URL, NAME + s))


def footprint():
    L = []
    def line(x1, y1, x2, y2, layer, w):
        L.append(f'\t(fp_line\n\t\t(start {x1:g} {y1:g})\n\t\t(end {x2:g} {y2:g})\n\t\t(stroke\n\t\t\t(width {w})\n\t\t\t(type solid)\n\t\t)\n'
                 f'\t\t(layer "{layer}")\n\t\t(uuid "{uid(f"{layer}{x1}{y1}{x2}{y2}")}")\n\t)')
    def poly(pts, layer, w):
        for a, b in zip(pts, pts[1:] + pts[:1]): line(a[0], a[1], b[0], b[1], layer, w)
    CH = 1.5                                       # chamfered corner at pin 1's end (top left), as KiCad draws it
    poly([(-BX + CH, -BY), (BX, -BY), (BX, BY), (-BX, BY), (-BX, -BY + CH)], 'F.Fab', 0.1)
    s = 0.11
    poly([(-BX - s + CH, -BY - s), (BX + s, -BY - s), (BX + s, BY + s), (-BX - s, BY + s), (-BX - s, -BY - s + CH)], 'F.SilkS', 0.12)
    c = 0.25
    poly([(-BX - c, -BY - c), (BX + c, -BY - c), (BX + c, BY + c), (-BX - c, BY + c)], 'F.CrtYd', 0.05)
    # the chip's outline inside, and a pin 1 notch on the silkscreen
    poly([(-CHIP_W / 2 + 1.0, -CHIP_H / 2), (CHIP_W / 2, -CHIP_H / 2), (CHIP_W / 2, CHIP_H / 2), (-CHIP_W / 2, CHIP_H / 2), (-CHIP_W / 2, -CHIP_H / 2 + 1.0)], 'F.Fab', 0.1)
    line(-0.5, -BY - s, 0, -BY - s + 0.7, 'F.SilkS', 0.12); line(0, -BY - s + 0.7, 0.5, -BY - s, 'F.SilkS', 0.12)
    P_ = []
    for n, x, y, sx, sy in pads():
        P_.append(f'\t(pad "{n}" smd rect\n\t\t(at {x:g} {y:g})\n\t\t(size {sx:g} {sy:g})\n\t\t(layers "F.Cu" "F.Mask" "F.Paste")\n\t\t(uuid "{uid(f"pad{n}")}")\n\t)')
    def prop(name, value, y, layer, hide=False):
        return (f'\t(property "{name}" "{value}"\n\t\t(at 0 {y:g} 0)\n\t\t(layer "{layer}")\n' + ('\t\t(hide yes)\n' if hide else '') +
                f'\t\t(uuid "{uid(name)}")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)')
    head = (f'(footprint "{NAME}"\n\t(version 20260206)\n\t(generator "pcbnew")\n\t(generator_version "10.0")\n\t(layer "F.Cu")\n'
            f'\t(descr "PLCC-32 socket, surface mount, without locating pegs: Amphenol FCI 69802-132LF/-432LF, Assmann A-CCS 032-Z-SM, Mill-Max 940-44-032-17-400000 '
            f'(pads {PAD_W} x {PAD_L} mm, rows {2 * RX:g} and {2 * RY:g} mm apart, housing up to {2 * BX:g} x {2 * BY:g} mm). The pads also cover the lands of the PLCC-32 chip itself. '
            'Generated by hw-smd-v2.1/tools/gen_plcc32_smd_socket.py")\n\t(tags "plcc smt socket")\n')
    props = [prop('Reference', 'REF**', -BY - 1.2, 'F.SilkS'), prop('Value', NAME, BY + 1.2, 'F.Fab'),
             prop('Datasheet', '', 0, 'F.Fab', True), prop('Description', '', 0, 'F.Fab', True)]
    model = ('\t(embedded_fonts no)\n\t(model "${KIPRJMOD}/parts/PLCC-32_SMD-Socket.wrl"\n\t\t(offset\n\t\t\t(xyz 0 0 0)\n\t\t)\n\t\t(scale\n\t\t\t(xyz 1 1 1)\n\t\t)\n'
             '\t\t(rotate\n\t\t\t(xyz 0 0 0)\n\t\t)\n\t)\n)\n')
    return head + '\n'.join(props) + '\n\t(attr smd)\n\t(duplicate_pad_numbers_are_jumpers no)\n' + '\n'.join(L) + '\n' + '\n'.join(P_) + '\n' + model


def wrl():
    U = 2.54
    shapes = []
    BLACK, CHIPC, TIN, PAPER = (0.13, 0.12, 0.12), (0.22, 0.22, 0.23), (0.78, 0.78, 0.80), (0.92, 0.90, 0.82)
    def box(x1, y1, z1, x2, y2, z2, rgb, shin=0.2):
        X = [v / U for v in (x1, x2)]; Y = [-v / U for v in (y1, y2)]; Z = [v / U for v in (z1, z2)]
        shapes.append(([(X[i], Y[j], Z[k]) for k in (0, 1) for j in (0, 1) for i in (0, 1)], rgb, shin))
    cav = (-CHIP_W / 2 - 0.9, -CHIP_H / 2 - 0.9, CHIP_W / 2 + 0.9, CHIP_H / 2 + 0.9)
    top = TOP
    box(-BX, -BY, 0.3, BX, cav[1], top, BLACK); box(-BX, cav[3], 0.3, BX, BY, top, BLACK)     # frame, open in the middle
    box(-BX, cav[1], 0.3, cav[0], cav[3], top, BLACK); box(cav[2], cav[1], 0.3, BX, cav[3], top, BLACK)
    for n, x, y, sx, sy in pads():                                                           # tails on the lands
        box(x - sx / 2 + 0.1, y - sy / 2 + 0.1, 0.0, x + sx / 2 - 0.1, y + sy / 2 - 0.1, 0.3, TIN, 0.7)
    for k in range(NX):
        x = (k - (NX - 1) / 2) * P
        box(x - 0.22, cav[1], 0.3, x + 0.22, cav[1] + 0.85, top - 0.3, TIN, 0.7); box(x - 0.22, cav[3] - 0.85, 0.3, x + 0.22, cav[3], top - 0.3, TIN, 0.7)
    for k in range(NY):
        y = (k - (NY - 1) / 2) * P
        box(cav[0], y - 0.22, 0.3, cav[0] + 0.85, y + 0.22, top - 0.3, TIN, 0.7); box(cav[2] - 0.85, y - 0.22, 0.3, cav[2], y + 0.22, top - 0.3, TIN, 0.7)
    box(-CHIP_W / 2, -CHIP_H / 2, 1.4, CHIP_W / 2, CHIP_H / 2, 4.9, CHIPC, 0.3)               # the flash
    box(-4.2, -2.6, 4.9, 4.2, 3.4, 5.05, PAPER, 0.05)                                         # its label
    box(-0.5, -CHIP_H / 2 + 0.7, 4.9, 0.5, -CHIP_H / 2 + 1.5, 5.05, TIN, 0.5)                 # pin 1 mark
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    out = ['#VRML V2.0 utf8', '# Surface-mount PLCC-32 socket with a PLCC-32 chip - simple generated model, origin at the centre.',
           '# Generated by hw-smd-v2.1/tools/gen_plcc32_smd_socket.py. KiCad VRML units (1 = 2.54 mm).']
    for pts, rgb, shin in shapes:
        out.append('Shape {\n appearance Appearance {\n  material Material {\n'
                   f'   diffuseColor {rgb[0]:.3f} {rgb[1]:.3f} {rgb[2]:.3f}\n   specularColor 0.200 0.200 0.200\n   shininess {shin}\n  }}\n }}\n'
                   ' geometry IndexedFaceSet {\n  solid TRUE\n  coord Coordinate { point [\n' + ',\n'.join(f'   {x:.5f} {y:.5f} {z:.5f}' for x, y, z in pts) +
                   ' ] }\n  coordIndex [ ' + ', '.join(' '.join(str(i) for i in f) + ' -1' for f in faces) + ' ]\n }\n}')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    ps = pads()
    assert len(ps) == 32 and sorted(p[0] for p in ps) == list(range(1, 33))
    open(MOD, 'w', encoding='utf-8').write(footprint())
    open(WRL, 'w').write(wrl())
    print('wrote', os.path.relpath(MOD, ROOT), 'and', os.path.relpath(WRL, ROOT))
