#!/usr/bin/env python3
"""DigiKey, LCSC, Mouser and Farnell upload files from a board BOM CSV (one board).

    python3 hw-common/tools/bom_uploads.py BOM.csv OUT_PREFIX --prefix "PicoGUS-THT-1.3.2: " \
        [--label "(jumpers)=J1,J7 shunts" ...] [--all-optional] [--main U12 ...]

Writes (only the files that get lines):
    OUT_PREFIX-digikey.csv           Quantity, Digi-Key Part Number, Manufacturer Part Number, Customer Reference
    OUT_PREFIX-digikey-optional.csv  the same for lines whose Type says "optional"
    OUT_PREFIX-lcsc.csv              Quantity, LCSC Part Number, Manufacturer Part Number, Customer Part Number
    OUT_PREFIX-lcsc-optional.csv
    OUT_PREFIX-mouser.csv            Quantity, Manufacturer Part Number, Manufacturer, Customer Part Number, Description
    OUT_PREFIX-mouser-optional.csv
    OUT_PREFIX-farnell.csv           the same columns
    OUT_PREFIX-farnell-optional.csv

The BOM needs a references column ("References" or "Socket for"), "Qty", "Type", an MPN column
("MPN (DigiKey line)" or "MPN"), "DigiKey PN" and "LCSC PN [MPN if different]". DigiKey and LCSC cells may carry
a note after the part number ("C2894974 [HCTL PZ254-2-13-Z-8.5]"): the first word is the part number and a
bracketed MPN replaces the BOM's MPN on the LCSC line. Lines with Qty 0 or no part number are skipped, and so are
LCSC lines whose "LCSC stock ..." cell starts with "n/a" (a part LCSC does not sell, e.g. one only in JLCPCB's
assembly library). The Mouser and Farnell files carry no shop part numbers: their BOM tools match each line by
manufacturer and MPN (the DigiKey line's), and every line with an MPN is listed.

The customer reference is PREFIX + the reference designators, so every bag arrives labelled with its board
positions. --main REFS puts that optional line in the main files, marked "(optional)", so it is ordered by
default. DigiKey allows 48 characters (LCSC and Farnell get the same text); longer lists are shortened to ranges
(C3-9,11-14). Mouser's Customer # takes only 21 characters, so the Mouser files carry the positions without
PREFIX, and a line that is still too long gets its value and count instead ("0.1uF x20"). Lines whose references
are not designators, e.g. "(jumpers)", take their text from --label; for "Socket for" BOMs, "U2 (74AHC244)"
becomes "U2 socket".
"""
import argparse, csv, re, sys

MAXLEN = 48
MOUSER_MAXLEN = 21  # CartItemRequest.CustomerPartNumber in Mouser's API (api.mouser.com/api/docs/V1)


def designators(text):
    toks = [t for t in re.split(r'[,\s]+', text.strip()) if t]
    out = []
    for t in toks:
        m = re.fullmatch(r'([A-Z]+)(\d+)-(?:[A-Z]+)?(\d+)', t)
        if m:
            out += ['%s%d' % (m[1], i) for i in range(int(m[2]), int(m[3]) + 1)]
        elif re.fullmatch(r'[A-Z]+\d+', t):
            out.append(t)
        else:
            return None
    return out


def compress(refs):
    by = {}
    for r in refs:
        p, n = re.fullmatch(r'([A-Z]+)(\d+)', r).groups()
        by.setdefault(p, []).append(int(n))
    parts = []
    for p in sorted(by, key=lambda p: min(by[p])):
        ns = sorted(by[p]); runs = []; a = b = ns[0]
        for n in ns[1:]:
            if n == b + 1: b = n
            else: runs.append((a, b)); a = b = n
        runs.append((a, b))
        toks = []
        for a, b in runs:
            toks += ['%d-%d' % (a, b)] if b - a >= 2 else [str(a), str(b)] if b == a + 1 else [str(a)]
        parts.append(p + ','.join(toks))
    return ','.join(parts)


def socket_label(text):
    m = re.fullmatch(r'\s*([A-Z0-9, ]+?)\s*(\(.*?\))?\s*(:\s*(.*))?', text)
    refs = designators(m[1]) if m else None
    if not refs:
        return None
    what = m[4] if m and m[4] else ('sockets' if len(refs) > 1 else 'socket')
    return ','.join(refs) + ' ' + what


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bom'); ap.add_argument('out_prefix')
    ap.add_argument('--prefix', default='')
    ap.add_argument('--label', action='append', default=[], help='REFS=TEXT for lines without designators')
    ap.add_argument('--all-optional', action='store_true', help='treat every line as optional')
    ap.add_argument('--main', action='append', default=[],
                    help='REFS of an optional line to put in the main files instead, marked "(optional)"')
    a = ap.parse_args()
    labels = dict(x.split('=', 1) for x in a.label)
    rows = list(csv.DictReader(open(a.bom, newline='', encoding='utf-8')))
    H = rows[0].keys()
    refcol = 'References' if 'References' in H else 'Socket for'
    mpncol = 'MPN (DigiKey line)' if 'MPN (DigiKey line)' in H else 'MPN'
    out = {k + o: [] for k in ('digikey', 'lcsc', 'mouser', 'farnell') for o in ('', '-optional')}
    for r in rows:
        qty = int(r['Qty'] or 0)
        if qty <= 0:
            continue
        refs_text = r[refcol]
        refs = designators(refs_text)
        # the positions text, in its long and short forms (one header strip for J6, J7: qty is what is bought)
        if refs_text in labels:
            forms = [labels[refs_text]]
        elif refs is not None:
            forms = [','.join(refs), compress(refs)]
        else:
            text = socket_label(refs_text) if refcol == 'Socket for' else None
            if not text:
                sys.exit('no --label for line %r' % refs_text)
            forms = [text]
        opt = '-optional' if (a.all_optional or 'optional' in r.get('Type', '').lower()) else ''
        tail = ''
        if opt and refs_text in a.main:  # an optional part ordered by default
            tail = ' (optional)'; opt = ''
        ref = next((a.prefix + t + tail for t in forms if len(a.prefix + t + tail) <= MAXLEN), None)
        if not ref:
            sys.exit('customer reference too long (> %d): %s' % (MAXLEN, a.prefix + forms[-1] + tail))
        value = (r.get('Value') or r.get('Type') or '').strip()
        mref = next((t + tail for t in forms + ['%s x%d' % (value.replace(' ', ''), qty)]
                     if len(t + tail) <= MOUSER_MAXLEN), None)
        if not mref:
            sys.exit('Mouser customer reference too long (> %d): %s' % (MOUSER_MAXLEN, forms[-1] + tail))
        dk = re.match(r'\s*([^\s\[]+)', r.get('DigiKey PN') or '')
        if dk:
            out['digikey' + opt].append([qty, dk[1], r[mpncol].strip(), ref])
        lc = re.match(r'\s*(C\d+)\s*(?:\[(.*?)\])?', r.get('LCSC PN [MPN if different]') or '')
        lcstock = next((v for k, v in r.items() if k and k.startswith('LCSC stock')), '') or ''
        if lc and not lcstock.strip().lower().startswith('n/a'):  # n/a: e.g. only in JLCPCB's assembly library
            mpn = (lc[2] or r[mpncol]).strip()
            mpn = re.sub(r'\s*\(.*\)\s*$', '', mpn)  # drop a trailing note: "RH3.5x0.8x9P52E (100R@100MHz)"
            # "[UNI-ROYAL MFR0W4F1003A50]": the first word is the maker when it has no digit
            mpn = mpn.split(' ', 1)[1] if lc[2] and ' ' in mpn and not re.search(r'\d', mpn.split(' ', 1)[0]) else mpn
            out['lcsc' + opt].append([qty, lc[1], mpn, ref])
        mpn = (r[mpncol] or '').strip()
        if mpn and mpn != 'None':
            maker = (r.get('Manufacturer') or '').strip()
            out['mouser' + opt].append([qty, mpn, maker, mref, value])
            out['farnell' + opt].append([qty, mpn, maker, ref, value])
    heads = {'digikey': ['Quantity', 'Digi-Key Part Number', 'Manufacturer Part Number', 'Customer Reference'],
             'lcsc': ['Quantity', 'LCSC Part Number', 'Manufacturer Part Number', 'Customer Part Number'],
             'mouser': ['Quantity', 'Manufacturer Part Number', 'Manufacturer', 'Customer Part Number', 'Description'],
             'farnell': ['Quantity', 'Manufacturer Part Number', 'Manufacturer', 'Customer Part Number', 'Description']}
    for k, lines in out.items():
        if not lines:
            continue
        fn = '%s-%s.csv' % (a.out_prefix, k)
        with open(fn, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f, lineterminator='\r\n')
            w.writerow(heads[k.split('-')[0]]); w.writerows(lines)
        print('%-45s %3d lines, %4d parts' % (fn, len(lines), sum(l[0] for l in lines)))


if __name__ == '__main__':
    main()
