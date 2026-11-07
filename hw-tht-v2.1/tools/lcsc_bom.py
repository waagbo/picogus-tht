#!/usr/bin/env python3
"""Write the LCSC order for the whole card from docs/BOM-THT.csv, with LCSC's live stock and prices.

The LCSC part of each BOM line is the first C-number in its `LCSC PN` column; a line whose `LCSC PN` starts with
"not at LCSC" is listed as bought elsewhere (the DigiKey part number is given). Order quantities follow LCSC's
minimum order and order multiple. Customer references are "PicoGUS-THT-2.1.x: <references>", at most 48 characters
(as in the DigiKey uploads).

usage (from hw-tht-v2.1):  python3 tools/lcsc_bom.py [cards]
Writes docs/BOM-THT-lcsc-order.csv (every BOM line: what to order where, stock and price on the day),
docs/BOM-THT-lcsc.csv and docs/BOM-THT-lcsc-optional.csv (the card and the optional IC sockets, for LCSC's BOM
tool: Quantity, LCSC Part Number, Manufacturer Part Number, Customer Reference); files for more than one card get a
"-<n>x" suffix.
Needs network access to wmsc.lcsc.com (curl).
"""
import csv
import datetime
import json
import math
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from digikey_upload import reference  # noqa: E402  (same customer-reference format and length limit)

BOM = 'docs/BOM-THT.csv'
API = 'https://wmsc.lcsc.com/ftps/wm/product/detail?productCode='
NOTES = {   # why an LCSC line is not the BOM's part, or what to watch for
    'U4, U5': 'Only 60 in stock at LCSC on the day, and dearer than at DigiKey (296-4510-5-ND, about US$0.90).',
    'U7': 'SN74LVC244AN instead of SN74AHC244N (none in stock): same pinout, 5 V-tolerant inputs at 3.3 V like the '
          'AHC244, stronger outputs.',
    'J7': 'XKB clone of the CUI SJ1-3535NG (as on THT v1.3.2): its TN pin sits about 0.4 mm further out (TN is '
          'unused); clip it if it does not fit. Genuine CUI SJ1-3535NG: LCSC C4991610.',
    'J9': 'XKB clone of the CUI SJ1-3535NG-GR (as on THT v1.3.2): see J7 about the TN pin.',
    'C21, C22': 'C0G (NP0) is needed in the DAC filter. Do not take C2761747 (X7R).',
    'R27, R28': 'Same Yageo MFR-25 1 % resistor, taped (FTF) instead of bulk (FBF).',
    'R31, R32': 'Same Yageo MFR-25 1 % resistor, taped (FTF) instead of bulk (FBF).',
    'U13': 'M62429L (3-5.5 V, Wuxi I-core): the only buyable M62429 for AVDD = 3.3 V.',
}


def fetch(code):
    raw = subprocess.run(['curl', '-sS', '-m', '30', '-A', 'Mozilla/5.0', API + code],
                         capture_output=True, text=True).stdout
    d = json.loads(raw).get('result')
    if not d:
        raise SystemExit(f'LCSC has no product {code}')
    return d


def unit_price(d, qty):
    ladder = sorted(d.get('productPriceList') or [], key=lambda p: p['ladder'])
    price = ladder[0]['usdPrice'] if ladder else 0.0
    for p in ladder:
        if qty >= p['ladder']:
            price = p['usdPrice']
    return price


def norm(s):
    return re.sub(r'[^A-Z0-9]', '', s.upper()).lstrip('0')


def main():
    cards = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    suffix = f'-{cards}x' if cards > 1 else ''
    today = datetime.date.today().isoformat()
    rows = list(csv.DictReader(open(BOM, newline='', encoding='utf-8')))
    overview, card, sockets, elsewhere = [], [], [], []
    shared = {}                                   # LCSC code -> all references and the total need
    for r in rows:
        m = re.match(r'(C\d+)\b', r['LCSC PN'].strip())
        if m and int(r['Qty'] or 0):
            s = shared.setdefault(m.group(1), {'refs': [], 'need': 0})
            s['refs'].append(r['References']); s['need'] += int(r['Qty']) * cards
    done = set()
    for r in rows:
        qty = int(r['Qty'] or 0)
        if qty == 0:
            continue
        need = qty * cards
        cref, _ = reference(r['References'])
        cell = r['LCSC PN'].strip()
        m = re.match(r'(C\d+)\b', cell)
        base = {'References': r['References'], 'Value': r['Value'], 'Needed': need, 'Customer Reference': cref}
        if not m:
            where = (f"DigiKey {r['DigiKey PN']}" if r['DigiKey PN'].endswith('-ND') else r['DigiKey PN'])
            overview.append({**base, 'Buy at': 'elsewhere: ' + where, 'BOM part': f"{r['Manufacturer']} {r['MPN']}",
                             'Note': cell})
            elsewhere.append((r['References'], r['MPN'], where))
            continue
        code = m.group(1)
        d = fetch(code)
        moq, step = max(1, d['minBuyNumber']), max(1, d['split'])
        together = shared[code]['refs']
        if len(together) > 1:
            cref, _ = reference(', '.join(together))
            base['Customer Reference'] = cref
            need = shared[code]['need']
        order = max(need, moq)
        order = math.ceil(order / step) * step
        price = unit_price(d, order)
        same = norm(d['productModel']).startswith(norm(r['MPN'])[:10]) or norm(r['MPN']).startswith(norm(d['productModel'])[:10])
        stock = d['stockNumber']
        note = NOTES.get(r['References'], '')
        if len(together) > 1:
            note = (f"One LCSC line with {' + '.join(x for x in together if x != r['References'])}. " + note).strip()
        if stock < order:
            note = (f'ONLY {stock} IN STOCK. ' + note).strip()
        line = {**base, 'Buy at': 'LCSC', 'Order qty': order, 'LCSC Part Number': code,
                'LCSC part': f"{d['brandNameEn']} {d['productModel']}", 'Package': d['encapStandard'],
                'BOM part': f"{r['Manufacturer']} {r['MPN']}", 'Same part as the BOM': 'yes' if same else 'equivalent',
                f'LCSC stock {today}': stock, 'MOQ / multiple': f'{moq} / {step}',
                'Unit USD': f'{price:.4f}', 'Line USD': f'{price * order:.2f}', 'Note': note}
        if code in done:
            line.update({'Order qty': '(above)', 'Line USD': '(above)'})
        overview.append(line)
        if code not in done:
            upload = [order, code, d['productModel'], base['Customer Reference']]
            (sockets if r['THT status'] == 'THT (optional)' else card).append((upload, price * order))
        done.add(code)
    cols = ['References', 'Value', 'Needed', 'Order qty', 'Buy at', 'LCSC Part Number', 'LCSC part', 'Package',
            'BOM part', 'Same part as the BOM', f'LCSC stock {today}', 'MOQ / multiple', 'Unit USD', 'Line USD',
            'Customer Reference', 'Note']
    with open(f'docs/BOM-THT-lcsc-order{suffix}.csv', 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator='\r\n')
        w.writeheader(); w.writerows(overview)
    for name, lines in ((f'docs/BOM-THT-lcsc{suffix}.csv', card), (f'docs/BOM-THT-lcsc-optional{suffix}.csv', sockets)):
        with open(name, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f, lineterminator='\r\n')
            w.writerow(['Quantity', 'LCSC Part Number', 'Manufacturer Part Number', 'Customer Reference'])
            w.writerows(u for u, _ in lines)
        print(f'{name}: {len(lines)} lines, US${sum(p for _, p in lines):.2f} at LCSC prices on {today}')
    for line in overview:
        if line['Buy at'] == 'LCSC' and (line['Same part as the BOM'] != 'yes' or line['Note'].startswith('ONLY')):
            print(f"  {line['References']}: {line['LCSC Part Number']} {line['LCSC part']} ({line['Same part as the BOM']})"
                  f"{' - ' + line['Note'][:60] if line['Note'] else ''}")
    for refs, mpn, where in elsewhere:
        print(f'not at LCSC: {refs} {mpn}: {where}')


if __name__ == '__main__':
    main()
