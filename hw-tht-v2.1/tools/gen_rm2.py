#!/usr/bin/env python3
"""Generate the Raspberry Pi Radio Module 2 (RM2) footprint and its simple 3D model (PicoGUS v2.1 THT).

  python3 hw-tht-v2.1/tools/gen_rm2.py

Writes hw-common/PiGUS library.pretty/RaspberryPi_RM2_HandSolder.kicad_mod and
hw-common/parts/RaspberryPi_RM2.wrl, and adds or replaces the symbol "RM2" in hw-tht-v2.1/PiGUS library.kicad_sym.

The RM2 (RMC20452T, CYW43439 WiFi 4 + Bluetooth 5.2) is 14.5 x 16.5 x 2.55 mm with 21 castellated pads on a
1.5 mm pitch: 7 down the left edge (1-7), 7 along the bottom (8-14) and 7 up the right edge (15-21), seen from the
top with the antenna end up. Land pattern from the RM2 datasheet (release 7), figure 6: lands 2.0 x 1.0 mm, 0.5 mm
of each outside the module edge; the side lands' centres start 1.8 mm above the module's bottom edge, the bottom
lands' centres 2.75 mm from its left edge. This footprint makes the lands 0.5 mm longer on the outside (2.5 x 1.0
mm), for a soldering iron. Origin: module centre, antenna end up (-y).

The datasheet's RF keep-out (35.5 x 18.5 mm with no metal: the antenna end of the module from 11.5 mm above its
bottom edge, 13.5 mm beyond it and 10.5 mm either side) is drawn on Cmts.User. It is not a rule area of the
footprint: on a board that cannot keep all of it clear, draw the rule area on the board.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
NAME = 'RaspberryPi_RM2_HandSolder'

W, H = 14.5, 16.5                 # module
PITCH = 1.5
LAND_IN, LAND_OUT, LAND_W = 1.5, 1.0, 1.0   # land: under the module, outside it (datasheet: 0.5), width
SIDE_FIRST = 1.8                  # centre of the lowest side land above the module's bottom edge
BOTTOM_FIRST = 2.75               # centre of the first bottom land from the module's left edge
KEEP_FROM_BOTTOM, KEEP_H, KEEP_W = 11.5, 18.5, 35.5

SIGNALS = {
    1: 'GND', 2: 'NC', 3: 'SCLK', 4: 'GND', 5: 'DATA_IN', 6: 'DATA_OUT', 7: 'GND',
    8: 'GPIO0', 9: 'CS', 10: 'nIRQ', 11: 'GND', 12: 'WL_ON', 13: 'BT_ON', 14: 'VDDIO',
    15: 'GND', 16: 'VIN', 17: 'GPIO2', 18: 'GPIO1', 19: 'NC', 20: 'NC', 21: 'GND',
}


def land(pad):
    """(x, y, size_x, size_y) of a land; origin module centre, y down"""
    length = LAND_IN + LAND_OUT
    off = (LAND_OUT - LAND_IN) / 2            # land centre outside (+) the module edge
    bottom = H / 2
    if pad <= 7:                              # left edge, 1 (top) to 7 (bottom)
        return (-W / 2 - off, bottom - SIDE_FIRST - PITCH * (7 - pad), length, LAND_W)
    if pad <= 14:                             # bottom edge, 8 (left) to 14 (right)
        return (-W / 2 + BOTTOM_FIRST + PITCH * (pad - 8), bottom + off, LAND_W, length)
    return (W / 2 + off, bottom - SIDE_FIRST - PITCH * (pad - 15), length, LAND_W)   # right edge, 15 (bottom) to 21


def footprint():
    s = []
    a = s.append
    hw, hh = W / 2, H / 2
    keep_y2 = hh - KEEP_FROM_BOTTOM           # lower edge of the keep-out
    keep_y1 = keep_y2 - KEEP_H
    a(f'(footprint "{NAME}"')
    a('\t(version 20260206)')
    a('\t(generator "pcbnew")')
    a('\t(layer "F.Cu")')
    a('\t(descr "Raspberry Pi Radio Module 2 (RM2, RMC20452T): CYW43439 WiFi 4 + Bluetooth 5.2, 14.5x16.5x2.55 mm, '
      '21 castellated pads on a 1.5 mm pitch. Datasheet figure 6 land pattern with the lands 0.5 mm longer on the '
      'outside (2.5x1.0 mm) for hand soldering. Antenna end up; RF keep-out (no metal) drawn on Cmts.User. '
      'https://datasheets.raspberrypi.com/rm2/rm2-datasheet.pdf")')
    a('\t(tags "RM2 RMC20452T CYW43439 WiFi Bluetooth Raspberry Pi radio module castellated")')
    a(f'\t(property "Reference" "REF**" (at 0 {hh + 2.6:.2f} 0) (layer "F.SilkS") '
      '(effects (font (size 1 1) (thickness 0.15))))')
    a(f'\t(property "Value" "RM2" (at 0 {hh + 4.2:.2f} 0) (layer "F.Fab") '
      '(effects (font (size 1 1) (thickness 0.15))))')
    a('\t(attr smd)')
    # silkscreen: corner marks at the end away from the antenna (the antenna end usually sits on the board edge)
    # and a pin 1 mark; everything else would be under the module or on the lands
    o = 0.12
    c1 = land(7)[1] + LAND_W / 2 + 0.35           # below pads 7 and 15
    c2 = land(8)[0] - LAND_W / 2 - 0.35           # left of pad 8
    for (x1, y1, x2, y2) in ((-hw - o, c1, -hw - o, hh + o), (-hw - o, hh + o, c2, hh + o),
                             (hw + o, c1, hw + o, hh + o), (hw + o, hh + o, -c2, hh + o)):
        a(f'\t(fp_line (start {x1:.2f} {y1:.2f}) (end {x2:.2f} {y2:.2f}) '
          '(stroke (width 0.12) (type solid)) (layer "F.SilkS"))')
    px, py = land(1)[0] - (LAND_IN + LAND_OUT) / 2 - 0.45, land(1)[1]
    a(f'\t(fp_circle (center {px:.2f} {py:.2f}) (end {px + 0.15:.2f} {py:.2f}) '
      '(stroke (width 0.2) (type solid)) (fill yes) (layer "F.SilkS"))')
    # fab: module outline, antenna section, shield can
    a(f'\t(fp_rect (start {-hw:.2f} {-hh:.2f}) (end {hw:.2f} {hh:.2f}) (stroke (width 0.1) (type solid)) '
      '(fill no) (layer "F.Fab"))')
    a(f'\t(fp_line (start {-hw:.2f} {keep_y2:.2f}) (end {hw:.2f} {keep_y2:.2f}) '
      '(stroke (width 0.1) (type dash)) (layer "F.Fab"))')
    a(f'\t(fp_text user "antenna" (at 0 {(-hh + keep_y2) / 2:.2f} 0) (layer "F.Fab") '
      '(effects (font (size 0.8 0.8) (thickness 0.12))))')
    a('\t(fp_text user "${REFERENCE}" (at 0 2 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))')
    # courtyard: module and lands + 0.25 mm
    cx = hw + LAND_OUT + 0.25
    a(f'\t(fp_rect (start {-cx:.2f} {-hh - 0.25:.2f}) (end {cx:.2f} {hh + LAND_OUT + 0.25:.2f}) '
      '(stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))')
    # the datasheet's RF keep-out, for reference
    a(f'\t(fp_rect (start {-KEEP_W / 2:.2f} {keep_y1:.2f}) (end {KEEP_W / 2:.2f} {keep_y2:.2f}) '
      '(stroke (width 0.1) (type dash)) (fill no) (layer "Cmts.User"))')
    a(f'\t(fp_text user "RM2 RF keep-out 35.5 x 18.5 mm: no metal (datasheet fig. 6)" (at 0 {keep_y1 + 1.2:.2f} 0) '
      '(layer "Cmts.User") (effects (font (size 0.8 0.8) (thickness 0.12))))')
    for p in range(1, 22):
        x, y, sx, sy = land(p)
        a(f'\t(pad "{p}" smd roundrect (at {x:.3f} {y:.3f}) (size {sx} {sy}) (layers "F.Cu" "F.Mask" "F.Paste") '
          f'(roundrect_rratio 0.2) (pinfunction "{SIGNALS[p]}") (pintype "passive"))')
    # no tracks, vias or pours on the top layer under the module (inside the land rows)
    ix, iy1, iy2 = hw - LAND_IN - 0.4, keep_y2, hh - LAND_IN - 0.4
    a('\t(zone (layer "F.Cu") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25)')
    a('\t\t(keepout (tracks not_allowed) (vias not_allowed) (pads allowed) (copperpour not_allowed) '
      '(footprints allowed))')
    a('\t\t(placement (enabled no) (sheetname ""))')
    a('\t\t(fill (thermal_gap 0.5) (thermal_bridge_width 0.5))')
    a(f'\t\t(polygon (pts (xy {-ix:.2f} {iy1:.2f}) (xy {ix:.2f} {iy1:.2f}) (xy {ix:.2f} {iy2:.2f}) '
      f'(xy {-ix:.2f} {iy2:.2f})))')
    a('\t)')
    a('\t(embedded_fonts no)')
    a('\t(model "${KIPRJMOD}/parts/RaspberryPi_RM2.wrl" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) '
      '(rotate (xyz 0 0 0)))')
    a(')')
    return '\n'.join(s) + '\n'


def symbol():
    """KiCad symbol "RM2" (library file format), pins on a 2.54 mm grid"""
    F = '(effects (font (size 1.27 1.27)))'
    left = [(3, 'SCLK', 'input'), (5, 'DATA_IN', 'input'), (6, 'DATA_OUT', 'output'), (10, '~{IRQ}', 'output'),
            (9, 'CS', 'input'), (12, 'WL_ON', 'input'), (13, 'BT_ON', 'input')]
    right = [(16, 'VIN', 'power_in'), (14, 'VDDIO', 'power_in'), None, (8, 'GPIO0', 'bidirectional'),
             (18, 'GPIO1', 'bidirectional'), (17, 'GPIO2', 'bidirectional'), None, (2, 'NC', 'no_connect'),
             (19, 'NC', 'no_connect'), (20, 'NC', 'no_connect')]
    gnd = [1, 4, 7, 11, 15, 21]
    s = []
    a = s.append
    a('\t(symbol "RM2"')
    a('\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (in_pos_files yes) (duplicate_pin_numbers_are_jumpers no)')
    for key, val, at, hide in (('Reference', 'U', '-12.7 13.97 0', False), ('Value', 'RM2', '12.7 13.97 0', False),
                               ('Footprint', 'PiGUS library:RaspberryPi_RM2_HandSolder', '0 0 0', True),
                               ('Datasheet', 'https://datasheets.raspberrypi.com/rm2/rm2-datasheet.pdf', '0 0 0', True),
                               ('Description', 'Raspberry Pi Radio Module 2 (RMC20452T): CYW43439 WiFi 4 + Bluetooth 5.2, '
                                               'gSPI host interface, 14.5 x 16.5 mm castellated module', '0 0 0', True)):
        just = ' (justify left)' if key == 'Reference' else ' (justify right)' if key == 'Value' else ''
        a(f'\t\t(property "{key}" "{val}" (at {at}) (show_name no) (do_not_autoplace no)'
          f'{" (hide yes)" if hide else ""} (effects (font (size 1.27 1.27)){just}))')
    a('\t\t(symbol "RM2_0_1"')
    a('\t\t\t(rectangle (start -12.7 12.7) (end 12.7 -15.24) (stroke (width 0.254) (type default)) '
      '(fill (type background)))')
    a('\t\t)')
    a('\t\t(symbol "RM2_1_1"')

    def pin(num, name, typ, x, y, ang):
        a(f'\t\t\t(pin {typ} line (at {x} {y} {ang}) (length 2.54) (name "{name}" {F}) (number "{num}" {F}))')
    # the data-out and IRQ rows are spaced out: each takes a series resistor in the schematic
    for (num, name, typ), y in zip(left, (10.16, 7.62, 2.54, -2.54, -7.62, -10.16, -12.7)):
        pin(num, name, typ, -15.24, y, 0)
    for i, ent in enumerate(right):
        if ent:
            pin(ent[0], ent[1], ent[2], 15.24, round(10.16 - 2.54 * i, 2), 180)
    for i, num in enumerate(gnd):
        pin(num, 'GND', 'power_in', round(-6.35 + 2.54 * i, 2), -17.78, 90)
    a('\t\t)')
    a('\t\t(embedded_fonts no)')
    a('\t)')
    return '\n'.join(s) + '\n'


def write_symbol(path):
    """add or replace (symbol "RM2" ...) in a .kicad_sym file"""
    t = open(path, encoding='utf-8').read()
    i = t.find('\n\t(symbol "RM2"')
    if i >= 0:
        depth = 0
        j = i + 1
        while True:
            c = t[j]
            if c == '"':
                j += 1
                while t[j] != '"':
                    j += 2 if t[j] == '\\' else 1
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        t = t[:i + 1] + t[j + 2:]
    k = t.rstrip().rfind(')')                 # the library's closing bracket
    t = t[:k] + symbol() + ')\n'
    open(path, 'w', encoding='utf-8').write(t)


def wrl():
    P = 2.54
    shapes = []

    def box(x1, y1, z1, x2, y2, z2, rgb, shin=0.2):
        X = [v / P for v in (x1, x2)]
        Y = [-v / P for v in (y1, y2)]
        Z = [v / P for v in (z1, z2)]
        shapes.append(([(X[i], Y[j], Z[k]) for k in (0, 1) for j in (0, 1) for i in (0, 1)], rgb, shin))

    hw, hh = W / 2, H / 2
    pcb = 0.8
    keep_y2 = hh - KEEP_FROM_BOTTOM
    box(-hw, -hh, 0, hw, hh, pcb, (0.05, 0.25, 0.12))                              # module PCB
    box(-hw + 1.0, keep_y2 + 0.5, pcb, hw - 1.0, hh - 0.9, 2.55, (0.80, 0.80, 0.82), 0.7)   # shield can
    # inverted-F antenna on the PCB's top, schematic
    box(-hw + 1.2, -hh + 0.8, pcb, hw - 1.2, -hh + 1.4, pcb + 0.04, (0.85, 0.68, 0.25), 0.6)
    box(-hw + 1.2, -hh + 0.8, pcb, -hw + 1.8, keep_y2 - 0.4, pcb + 0.04, (0.85, 0.68, 0.25), 0.6)
    box(-hw + 3.6, -hh + 0.8, pcb, -hw + 4.2, keep_y2 - 0.4, pcb + 0.04, (0.85, 0.68, 0.25), 0.6)
    for p in range(1, 22):                                                         # castellations, gold
        x, y, sx, sy = land(p)
        if p <= 7:
            box(-hw - 0.02, y - 0.4, 0, -hw + 0.9, y + 0.4, pcb + 0.04, (0.85, 0.68, 0.25), 0.6)
        elif p <= 14:
            box(x - 0.4, hh - 0.9, 0, x + 0.4, hh + 0.02, pcb + 0.04, (0.85, 0.68, 0.25), 0.6)
        else:
            box(hw - 0.9, y - 0.4, 0, hw + 0.02, y + 0.4, pcb + 0.04, (0.85, 0.68, 0.25), 0.6)
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    out = ['#VRML V2.0 utf8',
           '# Raspberry Pi Radio Module 2 (RM2) - simple generated model: PCB, shield can, antenna, castellations',
           '# For footprint PiGUS library:RaspberryPi_RM2_HandSolder, offset 0, rotate 0; 14.5 x 16.5 x 2.55 mm.',
           '# Generated by hw-tht-v2.1/tools/gen_rm2.py.',
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


def check():
    lands = [land(p) for p in range(1, 22)]
    assert abs(lands[0][1] - lands[20][1]) < 1e-9 and abs(lands[6][1] - lands[14][1]) < 1e-9   # 1/21 and 7/15 level
    assert abs(lands[7][0] + lands[13][0]) < 1e-9                                              # 8..14 centred
    for i, (x1, y1, sx1, sy1) in enumerate(lands):                                             # 0.5 mm between lands
        for (x2, y2, sx2, sy2) in lands[i + 1:]:
            gap = max(abs(x1 - x2) - (sx1 + sx2) / 2, abs(y1 - y2) - (sy1 + sy2) / 2)
            assert gap >= 0.5 - 1e-9, (i + 1, gap)


if __name__ == '__main__':
    check()
    fp = os.path.join(ROOT, 'hw-common', 'PiGUS library.pretty', NAME + '.kicad_mod')
    model = os.path.join(ROOT, 'hw-common', 'parts', 'RaspberryPi_RM2.wrl')
    open(fp, 'w').write(footprint())
    open(model, 'w').write(wrl())
    lib = os.path.join(ROOT, 'hw-tht-v2.1', 'PiGUS library.kicad_sym')
    write_symbol(lib)
    print('wrote', fp)
    print('wrote', model)
    print('symbol RM2 in', lib)
