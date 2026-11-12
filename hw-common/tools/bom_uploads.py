#!/usr/bin/env python3
"""DigiKey, LCSC and Mouser upload files from a board BOM CSV (one board).

    python3 hw-common/tools/bom_uploads.py BOM.csv OUT_PREFIX --prefix "PicoGUS-THT-1.3.2: " \
        [--label "(jumpers)=J1,J7 shunts" ...]

Writes:
    OUT_PREFIX-digikey.csv  Quantity, Digi-Key Part Number, Manufacturer Part Number, Customer Reference
    OUT_PREFIX-lcsc.csv     Quantity, LCSC Part Number, Manufacturer Part Number, Customer Part Number
    OUT_PREFIX-mouser.csv   Quantity, Manufacturer Part Number, Manufacturer, Customer Part Number, Description

Each file lists the board's parts in BOM order, then the optional ones (lines whose Type says "optional"), with
"(optional)" after their customer reference, so the whole set arrives with one order and the optional bags are
labelled. Delete the optional lines in the shop's BOM tool for a minimal build.

The BOM needs "References", "Qty", "Type", "Value", "Manufacturer", "MPN (DigiKey line)", "DigiKey PN" and
"LCSC PN [MPN if different]" columns. DigiKey and LCSC cells may carry a note after the part number
("C2894974 [HCTL PZ254-2-13-Z-8.5]"): the first word is the part number and a bracketed MPN replaces the BOM's MPN
on the LCSC line. Lines with Qty 0 or no part number are skipped, and so are LCSC lines whose "LCSC stock ..." cell
starts with "n/a" (a part LCSC does not sell, e.g. one only in JLCPCB's assembly library). The Mouser file carries
no Mouser part numbers: Mouser's BOM tool matches each line by manufacturer and MPN (the DigiKey line's), and every
line with an MPN is listed.

The customer reference is PREFIX + the reference designators, so every bag arrives labelled with its board
positions. DigiKey allows 48 characters (LCSC gets the same text); longer lists are shortened to ranges
(C3-9,11-14), and an optional line too long with "(optional)" is marked "(opt.)". Mouser's Customer # takes only 21 characters, so the Mouser file carries the positions without
PREFIX; a line that is still too long gets its value and count instead ("0.1uF x20"), and an optional line that
does not fit with "(optional)" keeps only its designators, marked "(opt.)" ("U5,U6,U7,U10 (opt.)"). Lines whose
references are not designators, e.g. "(jumpers)", take their text from --label.
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


def first_fit(cands, maxlen):
    return next((c for c in cands if len(c) <= maxlen), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bom'); ap.add_argument('out_prefix')
    ap.add_argument('--prefix', default='')
    ap.add_argument('--label', action='append', default=[], help='REFS=TEXT for lines without designators')
    a = ap.parse_args()
    labels = dict(x.split('=', 1) for x in a.label)
    rows = list(csv.DictReader(open(a.bom, newline='', encoding='utf-8')))
    out = {k: ([], []) for k in ('digikey', 'lcsc', 'mouser')}  # (main lines, optional lines)
    for r in rows:
        qty = int(r['Qty'] or 0)
        if qty <= 0:
            continue
        refs_text = r['References']
        refs = designators(refs_text)
        # the positions text, long form first (one header strip for J6, J7: the quantity is what is bought)
        if refs_text in labels:
            forms = [labels[refs_text]]
        elif refs is not None:
            forms = [','.join(refs), compress(refs)]
        else:
            sys.exit('no --label for line %r' % refs_text)
        opt = 'optional' in r.get('Type', '').lower()
        tail = ' (optional)' if opt else ''
        ref = first_fit([a.prefix + t + tail for t in forms] + ([a.prefix + t + ' (opt.)' for t in forms] if opt else []),
                        MAXLEN)
        if not ref:
            sys.exit('customer reference too long (> %d): %s' % (MAXLEN, a.prefix + forms[-1] + tail))
        value = (r.get('Value') or '').strip()
        count = '%s x%d' % (value.replace(' ', ''), qty)
        cands = [t + tail for t in forms]
        if opt:  # "U5,U6,U7,U10 sockets" -> "U5,U6,U7,U10 (opt.)"
            cands += [m[1] + ' (opt.)' for m in (re.match(r'([A-Z]+\d+(?:,[A-Z]+\d+)*)\b', t) for t in forms) if m]
        cands += [count + tail, count + ' (opt.)'] if opt else [count]
        mref = first_fit(cands, MOUSER_MAXLEN)
        if not mref:
            sys.exit('Mouser customer reference too long (> %d): %s' % (MOUSER_MAXLEN, cands[0]))
        mpn_dk = (r['MPN (DigiKey line)'] or '').strip()
        dk = re.match(r'\s*([^\s\[]+)', r.get('DigiKey PN') or '')
        if dk:
            out['digikey'][opt].append([qty, dk[1], mpn_dk, ref])
        lc = re.match(r'\s*(C\d+)\s*(?:\[(.*?)\])?', r.get('LCSC PN [MPN if different]') or '')
        lcstock = next((v for k, v in r.items() if k and k.startswith('LCSC stock')), '') or ''
        if lc and not lcstock.strip().lower().startswith('n/a'):  # n/a: e.g. only in JLCPCB's assembly library
            mpn = (lc[2] or mpn_dk).strip()
            mpn = re.sub(r'\s*\(.*\)\s*$', '', mpn)  # drop a trailing note: "RH3.5x0.8x9P52E (100R@100MHz)"
            # "[UNI-ROYAL MFR0W4F1003A50]": the first word is the maker when it has no digit
            mpn = mpn.split(' ', 1)[1] if lc[2] and ' ' in mpn and not re.search(r'\d', mpn.split(' ', 1)[0]) else mpn
            out['lcsc'][opt].append([qty, lc[1], mpn, ref])
        if mpn_dk and mpn_dk != 'None':
            out['mouser'][opt].append([qty, mpn_dk, (r.get('Manufacturer') or '').strip(), mref, value])
    heads = {'digikey': ['Quantity', 'Digi-Key Part Number', 'Manufacturer Part Number', 'Customer Reference'],
             'lcsc': ['Quantity', 'LCSC Part Number', 'Manufacturer Part Number', 'Customer Part Number'],
             'mouser': ['Quantity', 'Manufacturer Part Number', 'Manufacturer', 'Customer Part Number', 'Description']}
    for k, (main_lines, opt_lines) in out.items():
        fn = '%s-%s.csv' % (a.out_prefix, k)
        with open(fn, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f, lineterminator='\r\n')
            w.writerow(heads[k]); w.writerows(main_lines + opt_lines)
        print('%-30s %3d lines, %4d parts (optional: %d lines, %d parts)' % (
            fn, len(main_lines) + len(opt_lines), sum(l[0] for l in main_lines + opt_lines),
            len(opt_lines), sum(l[0] for l in opt_lines)))


if __name__ == '__main__':
    main()
