#!/usr/bin/env python3
"""Generate the Waveshare Core2350B footprint and its simple 3D model (PicoGUS v2.1 THT).

  python3 hw-tht-v2.1/tools/gen_core2350b.py

Writes hw-common/PiGUS library.pretty/Waveshare_Core2350B.kicad_mod and
hw-common/parts/Waveshare_Core2350B.wrl.

The Core2350B is a 25.4 x 25.4 mm RP2350B module with 64 plated holes on a
2.54 mm grid: a 10x10 outer ring and an 8x8 inner ring. Waveshare's schematic
groups them as four 2x8 headers P1-P4 laid out as a pinwheel, so four standard
2x8 sockets take the module. Pad number = 16 * (header - 1) + header pin:
  P1  1-16  left side    odd pins outer column, even pins inner column
  P2 17-32  top side     (USB, SWD, RUN, BOOTSEL, 3V3, VBUS)
  P3 33-48  bottom side
  P4 49-64  right side
Origin: module centre. Pad 1 (P1.1, GND) is the top-left corner, the module's
chamfered corner. Grid cell (gx, gy), 0..9 from the top-left, is at
((gx - 4.5) * 2.54, (gy - 4.5) * 2.54).
"""
import os

P = 2.54
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# signal per pad, from Waveshare's Core2350B schematic (P1-P4) and pinout drawing
SIGNALS = {
    1: 'GND', 2: 'GP0', 3: 'GP2', 4: 'GP1', 5: 'GP4', 6: 'GP3', 7: 'GP6', 8: 'GP5',
    9: 'GP8', 10: 'GP7', 11: 'GP10', 12: 'GP9', 13: 'GP12', 14: 'GP11', 15: 'GP14', 16: 'GP13',
    17: 'GND', 18: 'GP45', 19: 'GP47', 20: 'GP46', 21: 'BOOTSEL', 22: 'SWDIO', 23: 'USB_DM', 24: 'GND',
    25: 'USB_DP', 26: 'SWCLK', 27: 'RUN', 28: 'ADC_VREF', 29: '3V3_EN', 30: 'GND', 31: 'VBUS', 32: '3V3',
    33: 'GND', 34: 'GP15', 35: 'GP17', 36: 'GP16', 37: 'GP19', 38: 'GP18', 39: 'GP21', 40: 'GP20',
    41: 'GP23', 42: 'GP22', 43: 'GP25', 44: 'GP24', 45: 'GP27', 46: 'GP26', 47: 'GP29', 48: 'GP28',
    49: 'GND', 50: 'GP30', 51: 'GP32', 52: 'GP31', 53: 'GP34', 54: 'GP33', 55: 'GP36', 56: 'GP35',
    57: 'GP38', 58: 'GP37', 59: 'GP40', 60: 'GP39', 61: 'GP42', 62: 'GP41', 63: 'GP44', 64: 'GP43',
}


def grid(pad):
    """(gx, gy) grid cell of a pad."""
    hdr, n = divmod(pad - 1, 16)
    n += 1
    k = (n + 1) // 2          # position along the header, 1..8
    outer = n % 2 == 1
    if hdr == 0:              # P1, left: column 0 (outer) / 1, rows 0..7
        return (0 if outer else 1, k - 1)
    if hdr == 1:              # P2, top: row 0 (outer) / 1, columns 9..2
        return (10 - k, 0 if outer else 1)
    if hdr == 2:              # P3, bottom: row 9 (outer) / 8, columns 0..7
        return (k - 1, 9 if outer else 8)
    return (9 if outer else 8, 10 - k)  # P4, right: column 9 (outer) / 8, rows 9..2


def xy(pad):
    gx, gy = grid(pad)
    return round((gx - 4.5) * P, 3), round((gy - 4.5) * P, 3)


def check():
    cells = {grid(p) for p in SIGNALS}
    ring = {(x, y) for x in range(10) for y in range(10)
            if x in (0, 1, 8, 9) or y in (0, 1, 8, 9)}
    assert cells == ring and len(cells) == 64
    gpios = sorted(int(s[2:]) for s in SIGNALS.values() if s.startswith('GP'))
    assert gpios == list(range(48))
    # spot checks against the pinout drawing
    assert grid(1) == (0, 0) and grid(2) == (1, 0) and grid(25) == (5, 0)
    assert grid(47) == (7, 9) and grid(63) == (9, 2) and grid(64) == (8, 2)


def footprint():
    H = 12.7          # half module size
    s = []
    a = s.append
    a('(footprint "Waveshare_Core2350B"')
    a('\t(version 20260206)')
    a('\t(generator "pcbnew")')
    a('\t(layer "F.Cu")')
    a('\t(descr "Waveshare Core2350B RP2350B module, 25.4x25.4 mm, 64 pins on a 2.54 mm grid in two rings '
      '(four 2x8 headers P1-P4 in a pinwheel; pad = 16*(header-1)+pin). Socket it on four 2x8 2.54 mm female headers '
      'or solder it on 2x8 male headers. https://www.waveshare.com/wiki/Core2350B0")')
    a('\t(tags "RP2350 RP2350B Core2350B Waveshare module THT")')
    a('\t(property "Reference" "REF**" (at -9.5 -13.8 0) (layer "F.SilkS") '
      '(effects (font (size 1 1) (thickness 0.15))))')
    a('\t(property "Value" "Core2350B" (at 0 14 0) (layer "F.Fab") '
      '(effects (font (size 1 1) (thickness 0.15))))')
    a('\t(attr through_hole)')
    # silkscreen: module outline 0.11 mm outside the edge, chamfered pin-1 corner, pin-1 triangle
    o = H + 0.11
    c = 1.2
    pts = [(-o + c, -o), (o, -o), (o, o), (-o, o), (-o, -o + c)]
    for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]):
        a(f'\t(fp_line (start {x1:.2f} {y1:.2f}) (end {x2:.2f} {y2:.2f}) '
          '(stroke (width 0.12) (type solid)) (layer "F.SilkS"))')
    a(f'\t(fp_poly (pts (xy {-o - 0.3:.2f} {-o - 0.3:.2f}) (xy {-o - 1.1:.2f} {-o - 0.3:.2f}) '
      f'(xy {-o - 0.3:.2f} {-o - 1.1:.2f})) (stroke (width 0.12) (type solid)) (fill yes) (layer "F.SilkS"))')
    # inner edge of the pin rings (where the four sockets meet the open centre)
    i = 3 * P
    a(f'\t(fp_rect (start {-i:.2f} {-i:.2f}) (end {i:.2f} {i:.2f}) (stroke (width 0.12) (type solid)) '
      '(fill no) (layer "F.SilkS"))')
    a('\t(fp_text user "Core2350B" (at 0 -1 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))')
    a('\t(fp_text user "P1-P4 pin 1 (GND) at the corners" (at 0 1.2 0) (layer "F.Fab") '
      '(effects (font (size 0.6 0.6) (thickness 0.09))))')
    # courtyard 0.25 mm around the module (and the sockets, which are the module outline)
    k = H + 0.25
    a(f'\t(fp_rect (start {-k:.2f} {-k:.2f}) (end {k:.2f} {k:.2f}) (stroke (width 0.05) (type solid)) '
      '(fill no) (layer "F.CrtYd"))')
    # fab: module outline with the chamfer, the four 2x8 socket bodies, the FPC connector
    pts = [(-H + c, -H), (H, -H), (H, H), (-H, H), (-H, -H + c)]
    for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]):
        a(f'\t(fp_line (start {x1:.2f} {y1:.2f}) (end {x2:.2f} {y2:.2f}) '
          '(stroke (width 0.1) (type solid)) (layer "F.Fab"))')
    for (x1, y1, x2, y2) in SOCKETS:
        a(f'\t(fp_rect (start {x1:.2f} {y1:.2f}) (end {x2:.2f} {y2:.2f}) (stroke (width 0.1) (type solid)) '
          '(fill no) (layer "F.Fab"))')
    a('\t(fp_rect (start 2.6 -8.4) (end 7.1 -3.9) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))')
    a('\t(fp_text user "FPC" (at 4.85 -6.15 0) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))))')
    a('\t(fp_text user "${REFERENCE}" (at 0 -3 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))')
    for p in range(1, 65):
        x, y = xy(p)
        shape = 'roundrect' if p == 1 else 'circle'
        rr = ' (roundrect_rratio 0.147059)' if p == 1 else ''
        a(f'\t(pad "{p}" thru_hole {shape} (at {x} {y}) (size 1.7 1.7) (drill 1.0) (layers "*.Cu" "*.Mask") '
          f'(remove_unused_layers no){rr} (pinfunction "{SIGNALS[p]}") (pintype "passive"))')
    a('\t(embedded_fonts no)')
    a('\t(model "${KIPRJMOD}/parts/Waveshare_Core2350B.wrl" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) '
      '(rotate (xyz 0 0 0)))')
    a(')')
    return '\n'.join(s) + '\n'


# four 2x8 socket bodies (x1, y1, x2, y2), mm, board coordinates (y down)
SOCKETS = [
    (-5 * P, -5 * P, -3 * P, 3 * P),   # P1 left
    (-3 * P, -5 * P, 5 * P, -3 * P),   # P2 top
    (-5 * P, 3 * P, 3 * P, 5 * P),     # P3 bottom
    (3 * P, -3 * P, 5 * P, 5 * P),     # P4 right
]

# 3D model heights (mm above the board): 8.5 mm female sockets, 2.5 mm male header spacer, 1.0 mm module PCB
Z_SOCKET, Z_SPACER, Z_PCB = 8.5, 11.0, 12.0


def wrl():
    shapes = []

    def box(x1, y1, z1, x2, y2, z2, rgb, shin=0.2):
        # board coordinates in mm (y down) -> KiCad VRML units (0.1 in, y up)
        X = [v / P for v in (x1, x2)]
        Y = [-v / P for v in (y1, y2)]
        Z = [v / P for v in (z1, z2)]
        pts = [(X[i], Y[j], Z[k]) for k in (0, 1) for j in (0, 1) for i in (0, 1)]
        shapes.append((pts, rgb, shin))

    for (x1, y1, x2, y2) in SOCKETS:
        box(x1 + 0.05, y1 + 0.05, 0, x2 - 0.05, y2 - 0.05, Z_SOCKET, (0.08, 0.08, 0.08))
        box(x1 + 0.05, y1 + 0.05, Z_SOCKET, x2 - 0.05, y2 - 0.05, Z_SPACER, (0.12, 0.12, 0.12))
    H = 12.7
    box(-H, -H, Z_SPACER, H, H, Z_PCB, (0.13, 0.33, 0.78))
    top = Z_PCB
    box(-7.0, -3.05, top, 3.0, 6.95, top + 0.85, (0.05, 0.05, 0.05), 0.4)        # RP2350B QFN-80
    box(2.6, -8.4, top, 7.1, -3.9, top + 1.0, (0.93, 0.93, 0.88))               # FPC connector
    box(-5.3, -7.7, top, -2.1, -4.4, top + 1.1, (0.05, 0.05, 0.05))             # LDO
    box(4.5, 5.4, top, 6.9, 7.5, top + 0.8, (0.78, 0.78, 0.80), 0.6)           # 12 MHz crystal
    box(5.0, 2.5, top, 6.6, 3.3, top + 0.5, (0.85, 0.10, 0.10))                 # LED (GP39)
    for p in range(1, 65):
        x, y = xy(p)
        # pin: 0.64 mm square from 3 mm under the board to just above the module, gold
        box(x - 0.32, y - 0.32, -3.0, x + 0.32, y + 0.32, top + 0.6, (0.85, 0.68, 0.25), 0.6)
        # module pad ring, gold, on the module's top
        box(x - 0.8, y - 0.8, top, x + 0.8, y + 0.8, top + 0.04, (0.85, 0.68, 0.25), 0.6)

    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    out = ['#VRML V2.0 utf8',
           '# Waveshare Core2350B (RP2350B module) on four 2x8 female sockets - simple generated model',
           '# For footprint PiGUS library:Waveshare_Core2350B, offset 0, rotate 0. Sockets 8.5 mm, male header',
           '# spacer 2.5 mm, module PCB 1.0 mm: module top at 12.0 mm. Generated by hw-tht-v2.1/tools/gen_core2350b.py.',
           '# Coordinates are in KiCad VRML units (1 = 0.1 inch = 2.54 mm); no Transform node on purpose.']
    for pts, rgb, shin in shapes:
        out.append('Shape {\n appearance Appearance {\n  material Material {\n'
                   f'   diffuseColor {rgb[0]:.3f} {rgb[1]:.3f} {rgb[2]:.3f}\n'
                   '   specularColor 0.200 0.200 0.200\n'
                   f'   shininess {shin}\n  }}\n }}\n geometry IndexedFaceSet {{\n  solid TRUE\n'
                   '  coord Coordinate { point [\n' +
                   ',\n'.join(f'   {x:.5f} {y:.5f} {z:.5f}' for x, y, z in pts) +
                   ' ] }\n  coordIndex [ ' +
                   ', '.join(' '.join(str(i) for i in f) + ' -1' for f in faces) + ' ]\n }\n}')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    check()
    fp = os.path.join(ROOT, 'hw-common', 'PiGUS library.pretty', 'Waveshare_Core2350B.kicad_mod')
    model = os.path.join(ROOT, 'hw-common', 'parts', 'Waveshare_Core2350B.wrl')
    open(fp, 'w').write(footprint())
    open(model, 'w').write(wrl())
    print('wrote', fp)
    print('wrote', model)
