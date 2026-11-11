#!/usr/bin/env python3
"""DigiKey and LCSC upload files from a board BOM CSV (one board).

    python3 hw-common/tools/bom_uploads.py BOM.csv OUT_PREFIX --prefix "PicoGUS-THT-1.3.2: " \
        [--label "(jumpers)=J1,J7 shunts" ...] [--all-optional] [--main U12 ...]

Writes (only the files that get lines):
    OUT_PREFIX-digikey.csv           Quantity, Digi-Key Part Number, Manufacturer Part Number, Customer Reference
    OUT_PREFIX-digikey-optional.csv  the same for lines whose Type says "optional"
    OUT_PREFIX-lcsc.csv              Quantity, LCSC Part Number, Manufacturer Part Number, Customer Part Number
    OUT_PREFIX-lcsc-optional.csv

The BOM needs a references column ("References" or "Socket for"), "Qty", "Type", an MPN column
("MPN (DigiKey line)" or "MPN"), "DigiKey PN" and "LCSC PN [MPN if different]". DigiKey and LCSC cells may carry
a note after the part number ("C2894974 [HCTL PZ254-2-13-Z-8.5]"): the first word is the part number and a
bracketed MPN replaces the BOM's MPN on the LCSC line. Lines with Qty 0 or no part number are skipped, and so are
LCSC lines whose "LCSC stock ..." cell starts with "n/a" (a part LCSC does not sell, e.g. one only in JLCPCB's
assembly library).

The customer reference is PREFIX + the reference designators, so every bag arrives labelled with its board
positions. --main REFS puts that optional line in the main DigiKey and LCSC files, marked "(optional)", so it
is ordered by default. DigiKey allows 48 characters; longer lists are shortened to ranges (C3-9,11-14). Lines whose
references are not designators, e.g. "(jumpers)", take their text from --label; for "Socket for" BOMs,
"U2 (74AHC244)" becomes "U2 socket".
"""
import argparse, csv, re, sys

MAXLEN = 48


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
    out = {k: [] for k in ('digikey', 'digikey-optional', 'lcsc', 'lcsc-optional')}
    for r in rows:
        qty = int(r['Qty'] or 0)
        if qty <= 0:
            continue
        refs_text = r[refcol]
        refs = designators(refs_text)
        if refs is not None:
            if len(refs) != qty and refs_text not in labels:
                pass  # e.g. one header strip for J6, J7: the quantity is what is bought
            ref = a.prefix + ','.join(refs)
            if len(ref) > MAXLEN:
                ref = a.prefix + compress(refs)
        else:
            text = labels.get(refs_text) or (socket_label(refs_text) if refcol == 'Socket for' else None)
            if not text:
                sys.exit('no --label for line %r' % refs_text)
            ref = a.prefix + text
        if refs_text in labels and refs is not None:
            ref = a.prefix + labels[refs_text]
        if len(ref) > MAXLEN:
            sys.exit('customer reference too long (%d > %d): %s' % (len(ref), MAXLEN, ref))
        opt = '-optional' if (a.all_optional or 'optional' in r.get('Type', '').lower()) else ''
        if opt and refs_text in a.main:  # an optional part ordered by default
            ref += ' (optional)'; opt = ''
            if len(ref) > MAXLEN:
                sys.exit('customer reference too long (%d > %d): %s' % (len(ref), MAXLEN, ref))
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
    heads = {'digikey': ['Quantity', 'Digi-Key Part Number', 'Manufacturer Part Number', 'Customer Reference'],
             'lcsc': ['Quantity', 'LCSC Part Number', 'Manufacturer Part Number', 'Customer Part Number']}
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
