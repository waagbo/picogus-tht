#!/usr/bin/env python3
"""Writes hw-common/PiGUS library.pretty/TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder.kicad_mod.

It is the TSSOP-20 footprint of the THT boards (hand-solder toes out to +-3.9 mm) without the round test pad per
pin. The test pads take 1.3 to 2 mm beyond the toes; without them a decoupling capacitor can sit 0.65 mm from the
pin it belongs to. Pin 1 is marked by the longer silkscreen line on its end of the package.

    python3 hw-smd-v2.1/tools/gen_tssop20_handsolder.py
"""
import os, re, uuid

here = os.path.dirname(os.path.abspath(__file__))
lib = os.path.join(here, '..', '..', 'hw-common', 'PiGUS library.pretty')
src = os.path.join(lib, 'TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder_TestPads.kicad_mod')
dst = os.path.join(lib, 'TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder.kicad_mod')
NAME = 'TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder'

def block_end(t, i):
    depth = 0; j = i
    while True:
        c = t[j]
        if c == '"':
            j += 1
            while t[j] != '"': j += 2 if t[j] == '\\' else 1
        elif c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: return j + 1
        j += 1

t = open(src, encoding='utf-8').read()
# the test pads (round) and the four pin numbers that stood beside them
out, pos = [], 0
for m in re.finditer(r'\n\t\((pad "\d+" smd circle|fp_text user "\d+")', t):
    s, e = m.start(), block_end(t, m.start() + 2)
    out.append(t[pos:s]); pos = e
out.append(t[pos:]); t = ''.join(out)
t = t.replace('TSSOP-20_4.4x6.5mm_P0.65mm_HandSolder_TestPads', NAME)
t = re.sub(r'\(descr "[^"]*"\)', '(descr "TSSOP-20, 0.65 mm pitch, with hand-solder toes (pads out to +-3.9 mm) and no test pads: '
           'the PicoGUS THT footprint without its test pad per pin, so that capacitors can sit at the pins. '
           'Pin 1 is at the end with the longer silkscreen line.")', t)
t = t.replace('(tags "TSSOP test pad")', '(tags "TSSOP hand solder")')
# pin 1 mark: the silkscreen line on pin 1's end runs out over pin 1's toe
t = t.replace('(start -2.11 -3.37)\n\t\t(end 2.11 -3.37)', '(start -3.9 -3.37)\n\t\t(end 2.11 -3.37)', 1)
# courtyard: 0.25 mm round the toes
t = t.replace('-5.7 ', '-4.15 ').replace('5.7 ', '4.15 ')
t = re.sub(r'\(uuid "[^"]*"\)', lambda m: '(uuid "%s")' % uuid.uuid5(uuid.NAMESPACE_URL, NAME + str(m.start())), t)
assert t.count('(pad "') == 20 and 'circle' not in t, t.count('(pad "')
open(dst, 'w', encoding='utf-8').write(t)
print('wrote', os.path.relpath(dst))
