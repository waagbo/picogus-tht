#!/usr/bin/env python3
"""Write the DigiKey BOM-upload files from docs/BOM-THT.csv.

Every line's Customer Reference is "PicoGUS-THT-2.1.x: <references>", so the bags of a mixed order say which board
they are for. DigiKey replaces a customer reference longer than 48 characters with "Character Limit Exceeded", so
references are written as ranges (C2-C7,C14-C16); if that is still too long, the letter is only written once per
group (C2-7,14-16,20). One BOM line stays one order line, so no line drops below a price break.

usage (from hw-tht-v2.1):  python3 tools/digikey_upload.py [boards]
  boards: how many cards to order for (default 1); files for more than one card get a "-<n>x" suffix.
Writes docs/BOM-THT-digikey.csv (the card) and docs/BOM-THT-digikey-optional.csv (the optional IC sockets).
Lines without a DigiKey part number (Core2350B2, M62429L) are listed at the end: buy them elsewhere.
"""
import csv
import re
import sys

PREFIX = 'PicoGUS-THT-2.1.x: '
LIMIT = 48
BOM = 'docs/BOM-THT.csv'


def split_ref(r):
    m = re.fullmatch(r'([A-Z]+)(\d+)', r)
    return (m.group(1), int(m.group(2))) if m else (r, None)


def ranges(refs, short=False):
    """['C2','C3','C4','C14'] -> 'C2-C4,C14' (short: 'C2-4,14')"""
    out, groups = [], []
    for r in refs:
        letter, n = split_ref(r)
        if n is None:
            groups.append((r, None, None)); continue
        if groups and groups[-1][0] == letter and groups[-1][2] is not None and n == groups[-1][2] + 1:
            groups[-1] = (letter, groups[-1][1], n)
        else:
            groups.append((letter, n, n))
    last = None
    for letter, a, b in groups:
        if a is None:
            out.append(letter); last = None; continue
        lead = '' if short and letter == last else letter
        tail = (letter if not short else '') + str(b)
        if a == b:
            out.append(f'{lead}{a}')
        elif b == a + 1:
            out.append(f'{lead}{a},{"" if short else letter}{b}')
        else:
            out.append(f'{lead}{a}-{tail}')
        last = letter
    return ','.join(out)


def reference(cell):
    """BOM 'References' cell -> customer reference"""
    cell = cell.strip()
    m = re.fullmatch(r'\((.*)\)', cell)          # '(U12 socket)', '(U3, U4, U5, U6 socket)'
    words = ''
    if m:
        cell = m.group(1)
        parts = cell.split(' ')
        words = ' ' + ' '.join(p for p in parts if not re.fullmatch(r'[A-Z]+\d+,?', p))
        cell = ' '.join(p for p in parts if re.fullmatch(r'[A-Z]+\d+,?', p))
    refs = [r.strip() for r in cell.split(',') if r.strip()]
    for short in (False, True):
        text = PREFIX + ranges(refs, short) + words
        if len(text) <= LIMIT:
            return text, short
    raise SystemExit(f'customer reference too long even when shortened: {text!r} ({len(text)})')


def main():
    boards = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    suffix = f'-{boards}x' if boards > 1 else ''
    rows = list(csv.DictReader(open(BOM, newline='', encoding='utf-8')))
    unit = next(k for k in rows[0] if k.startswith('DigiKey NOK each'))
    main_lines, socket_lines, elsewhere, shortened = [], [], [], []
    for r in rows:
        qty = int(r['Qty'] or 0)
        if qty == 0:
            continue
        pn = r['DigiKey PN'].strip()
        if not pn.endswith('-ND'):
            elsewhere.append(f"{r['References']}: {r['MPN']} ({pn or 'no DigiKey part'})")
            continue
        ref, short = reference(r['References'])
        if short:
            shortened.append(ref)
        line = [qty * boards, pn, r['MPN'], ref]
        price = float(r[unit]) * qty * boards if r[unit] else 0.0
        (socket_lines if r['THT status'] == 'THT (optional)' else main_lines).append((line, price))
    for name, lines in ((f'docs/BOM-THT-digikey{suffix}.csv', main_lines),
                        (f'docs/BOM-THT-digikey-optional{suffix}.csv', socket_lines)):
        with open(name, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f, lineterminator='\r\n')
            w.writerow(['Quantity', 'Digi-Key Part Number', 'Manufacturer Part Number', 'Customer Reference'])
            w.writerows(line for line, _ in lines)
        total = sum(p for _, p in lines)
        print(f'{name}: {len(lines)} lines, {total:.2f} NOK excl. MVA ({total * 1.25:.2f} incl.) at the BOM prices')
    longest = max((l[3] for l, _ in main_lines + socket_lines), key=len)
    print(f'longest customer reference: {len(longest)} characters: {longest}')
    for s in shortened:
        print('shortened to fit 48 characters:', s)
    for e in elsewhere:
        print('not at DigiKey:', e)


if __name__ == '__main__':
    main()
