"""Map one or more board BOMs onto the lines of a DigiKey order export, for render.py.

usage: python3 -I build_mapping.py CONFIG.json OUT_mapping.json

One "square" per fitted BOM line: reference designators, per-board quantity, display value, the ordered
part (MPN + DigiKey description, i.e. what is printed on the bag) and the order line index (IDX).
BOM lines are matched to order lines by DigiKey part number (packaging variants such as -1-ND / -2-ND /
CT-ND / TR-ND count as the same part), then by manufacturer part number. Anything else is either an
explicit substitution from the config or "not in this order". See references/config.md for the config.

Prints a report (unmatched lines with candidate order lines, shortages, unused order lines) and stores it
in OUT_mapping.json under "report" as well.
"""
import csv, json, os, re, sys

CONFIG, OUT = sys.argv[1:3]
cfg = json.load(open(CONFIG, encoding='utf-8'))
base = os.path.dirname(os.path.abspath(CONFIG))


def path(p):
    return p if os.path.isabs(p) else os.path.join(base, p)


# ---------------------------------------------------------------- CSV helpers
def read_csv(p):
    raw = open(path(p), 'rb').read()
    for enc in ('utf-8-sig', 'cp1252', 'latin-1'):
        try:
            text = raw.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    dialect = csv.Sniffer().sniff(text[:4096], delimiters=',;\t') if text.strip() else csv.excel
    return list(csv.DictReader(text.splitlines(), dialect=dialect))


def pick(header, patterns, required=True, what=''):
    """first column whose name matches one of the regexes (case-insensitive)"""
    for pat in patterns:
        for h in header:
            if h and re.search(pat, h.strip(), re.I):
                return h
    if required:
        sys.exit(f'{what}: no column matching {patterns} in {header}')
    return None


def num(s, default=0):
    m = re.search(r'-?\d+(?:[.,]\d+)?', str(s or '').replace(' ', '').replace(' ', ''))
    return float(m.group(0).replace(',', '.')) if m else default


# DigiKey export headers in several locales (English, Norwegian, German, Swedish, Danish, Dutch, French)
ORDER_COLS = {
    'idx': [r'^index$', r'^indeks$', r'^#$', r'^line', r'^pos', r'^nr\.?$', r'^index', r'^indeks'],
    'dkpn': [r'digi-?key.*(part|del|teil|art|num|nr|pn)', r'^digi-?key'],
    'mpn': [r'manufacturer part', r'^mpn$', r'herstellerteil', r'tillverkar.*nummer', r'fabrikant.*nummer'],
    'mfr': [r'^manufacturer$', r'^produsent$', r'^hersteller$', r'^tillverkare$', r'^producent$', r'^fabrikant$', r'^fabricant$'],
    'desc': [r'description', r'beskrivelse', r'beschreibung', r'beskrivning', r'omschrijving'],
    'ref': [r'customer ref', r'kundereferanse', r'kundenreferenz', r'kundreferens', r'kundereference', r'klantreferentie', r'r.f.rence client'],
    'qty': [r'^quantity$', r'^antall$', r'^menge$', r'^antal$', r'^aantal$', r'^quantit', r'^qty$', r'quantity', r'antall'],
    'back': [r'back ?order', r'restordre', r'r.ckstand', r'restorder', r'nachlieferung'],
}
BOM_COLS = {
    'refs': [r'^references?$', r'^refs?$', r'^designators?$', r'^reference', r'designator'],
    'qty': [r'^qty$', r'^quantity$', r'^qty', r'quantity'],
    'value': [r'^value$', r'^val$', r'^comment$', r'value'],
    'type': [r'^type$', r'^package$', r'^footprint$'],
    'mfr': [r'^manufacturer$', r'^mfr$', r'manufacturer'],
    'mpn': [r'^mpn', r'manufacturer part', r'^mfr.?pn', r'^part ?number$'],
    'dkpn': [r'digi-?key ?(pn|part|#|no)', r'^digi-?key$', r'^dk ?pn$'],
}

# ---------------------------------------------------------------- the order
order_rows = read_csv(cfg['order_csv'])
oh = list(order_rows[0].keys()) if order_rows else []
oc = {k: pick(oh, v, k in ('dkpn', 'qty'), 'order CSV') for k, v in ORDER_COLS.items()}
order = {}
for n, r in enumerate(order_rows, 1):
    dk = (r.get(oc['dkpn']) or '').strip()
    if not dk:
        continue                                        # total / summary lines
    idx = int(num(r.get(oc['idx']), n)) if oc['idx'] else n
    order[idx] = dict(idx=idx, dkpn=dk, mpn=(r.get(oc['mpn']) or '').strip() if oc['mpn'] else '',
                      mfr=(r.get(oc['mfr']) or '').strip() if oc['mfr'] else '',
                      desc=(r.get(oc['desc']) or '').strip() if oc['desc'] else '',
                      ref=(r.get(oc['ref']) or '').strip() if oc['ref'] else '',
                      qty=int(num(r.get(oc['qty']))), back=int(num(r.get(oc['back']))) if oc['back'] else 0)


def norm(s):
    return re.sub(r'\s+', '', (s or '').upper())


def dk_base(pn):
    """DigiKey packaging variants of one part: 296-17850-1-ND / -2-ND / -6-ND, BC1038CT-ND / TR-ND / DKR-ND"""
    p = norm(pn)
    p = re.sub(r'-(1|2|3|6)-ND$', '-ND', p)
    return re.sub(r'(CT|TR|DKR)-ND$', '-ND', p)


by_dk, by_dkbase, by_mpn = {}, {}, {}
for o in order.values():
    by_dk.setdefault(norm(o['dkpn']), []).append(o['idx'])
    by_dkbase.setdefault(dk_base(o['dkpn']), []).append(o['idx'])
    if o['mpn']:
        by_mpn.setdefault(norm(o['mpn']), []).append(o['idx'])


def find_line(key):
    """order line(s) for a DigiKey PN, an MPN, or 'idx:N'"""
    if not key:
        return []
    if str(key).lower().startswith('idx:'):
        i = int(str(key)[4:])
        return [i] if i in order else []
    k = norm(key)
    return by_dk.get(k) or by_mpn.get(k) or by_dkbase.get(dk_base(key)) or []


# ---------------------------------------------------------------- config
SKIP = [s.lower() for s in cfg.get('skip_types', ['not fitted', 'optional', 'dnp', 'do not'])]
SUBST = {}
for s in cfg.get('substitutions', []):
    for b in s.get('boards', ['*']):
        SUBST[(b, s['ref'])] = s
VALUES, LABELS, ALT = cfg.get('values', {}), cfg.get('labels', {}), cfg.get('alt', {})
ORDER_NOTES = {norm(k): v for k, v in cfg.get('order_notes', {}).items()}
CATS = cfg.get('category_order', ['U', 'IC', 'Q', 'D', 'LED', 'Y', 'X', 'C', 'R', 'RN', 'L', 'FB', 'F', 'RV', 'VR',
                                     'SW', 'S', 'J', 'P', 'CN', 'K', 'BT', 'TP'])


def board_get(table, board, ref):
    return table.get(f'{board}:{ref}', table.get(ref))


def cat_rank(ref):
    if ref.startswith('('):                              # non-designator lines keep their BOM order
        return (len(CATS) + 1, '')
    p = re.match(r'([A-Za-z]+)', ref).group(1).upper() if re.match(r'[A-Za-z]', ref) else ref
    return (CATS.index(p), '') if p in CATS else (len(CATS), p)


def refnum(ref):
    m = re.search(r'(\d+)', ref)
    return int(m.group(1)) if m else 0


def split_refs(s):
    out = []
    for part in re.split(r'[,;\s]+', s.strip()):
        m = re.fullmatch(r'([A-Za-z]+)(\d+)-(?:[A-Za-z]+)?(\d+)', part)   # R10-R13 or R10-13
        if m:
            out += [f'{m.group(1)}{i}' for i in range(int(m.group(2)), int(m.group(3)) + 1)]
        elif part:
            out.append(part)
    return out


def pretty_value(v):
    v = (v or '').strip()
    v = re.sub(r'(\d)\s*u([FH])\b', r'\1 µ\2', v)
    v = re.sub(r'(\d)\s*([np])([FH])\b', r'\1 \2\3', v)
    m = re.fullmatch(r'(\d+(?:\.\d+)?)([kKMR])(\d*)(.*)', v)
    if m and not (m.group(4)[:1].isalpha()):            # 10k, 4k7, 100R, 1k@100MHz - but not 16MHz
        num_, unit, frac, rest = m.groups()
        num_ = f'{num_}.{frac}' if frac else num_
        v = f"{num_} {'kΩ' if unit in 'kK' else 'MΩ' if unit == 'M' else 'Ω'}{rest}"
    return v


def norm_units(t):
    """'10 µF' -> '10UF', '20 kΩ' -> '20K', '2x13' -> '2X13' (DigiKey descriptions write units that way)"""
    t = (t or '').upper().replace('Μ', 'U').replace('µ', 'U').replace('Ω', '').replace('OHM', '')
    return re.sub(r'(\d)\s+(?=(?:[PNUM]?F|[KM]|R|M?V|[UM]?A|[UNM]?H|[KM]?HZ|M?W)\b)', r'\1', t)


def value_tokens(v):
    """value words that identify a part: numbers with units (10UF, 20K, 2X13) and longer words (HEADER, JACK)"""
    t = norm_units(v)
    return {w for w in re.split(r'[^A-Z0-9.]+', t)
            if (re.search(r'\d', w) and re.search(r'[A-Z]', w)) or (w.isalpha() and len(w) >= 4)} - {'THIN', 'FILM', 'BIPOLAR'}


def package_tokens(typ):
    return {w for w in re.split(r'[^A-Z0-9]+', (typ or '').upper()) if re.fullmatch(r'\d{4}|[A-Z]{4,}\d*', w)} - {'ONLY', 'PART'}


# ---------------------------------------------------------------- squares per board
squares, report = {}, dict(unmatched=[], ambiguous=[], short=[], backorder=[], unused=[])
boards = cfg['boards']
for bd in boards:
    B = bd['key']
    rows = read_csv(bd['bom'])
    bh = list(rows[0].keys()) if rows else []
    bc = {k: pick(bh, v, k in ('refs',), f"BOM {bd['bom']}") for k, v in BOM_COLS.items()}
    lst = squares[B] = []
    for r in rows:
        refs = split_refs(r.get(bc['refs']) or '')
        typ = (r.get(bc['type']) or '').strip() if bc['type'] else ''
        qty = int(num(r.get(bc['qty']), len(refs))) if bc['qty'] else len(refs)
        if not refs or qty == 0 or any(s in typ.lower() for s in SKIP):
            continue
        first = refs[0]
        dk = re.split(r'\s*[\[(]', (r.get(bc['dkpn']) or '').strip())[0] if bc['dkpn'] else ''
        mpn = re.split(r'\s*\[', (r.get(bc['mpn']) or '').strip())[0] if bc['mpn'] else ''
        val = pretty_value(r.get(bc['value']) if bc['value'] else '')
        mfilm = re.search(r'\s((?:thin|thick|metal|carbon) film)$', val, re.I)
        if mfilm:                                        # "470 Ω 0.1% thin film" -> value "470 Ω 0.1%", type "thin film ..."
            val, typ = val[:mfilm.start()], f'{mfilm.group(1)} {typ}'.strip()
        sq = dict(board=B, refs=refs, qty=qty, value=val, type=typ, row=len(lst), bom_mfr=(r.get(bc['mfr']) or '').strip() if bc['mfr'] else '', bom_mpn=mpn, bom_dkpn=dk,
                  idx=None, note='', alt=board_get(ALT, B, first) or '')
        v = board_get(VALUES, B, first)
        if v:
            sq['value'] = v
        lab = board_get(LABELS, B, first)
        if lab:
            sq.update({k: lab[k] for k in ('label', 'sub') if k in lab})
        s = SUBST.get((B, first)) or SUBST.get(('*', first))
        if s:
            cand = find_line(s['use'])
            if not cand:
                report['unmatched'].append(dict(board=B, refs=refs, why=f"substitution target {s['use']!r} not in the order"))
            else:
                sq['idx'] = cand[0]
                sq['note'] = s.get('note', '')
        else:
            cand = find_line(dk) or find_line(mpn)
            if len(cand) > 1:
                report['ambiguous'].append(dict(board=B, refs=refs, lines=cand))
            if cand:
                sq['idx'] = cand[0]
                if len(cand) > 1:
                    sq['note'] = 'also on order line(s) ' + ', '.join(f'IDX {i}' for i in cand[1:])
        if not sq['idx']:
            sq['note'] = sq['note'] or 'not in this DigiKey order'
        lst.append(sq)
    for e in cfg.get('extra_squares', []):
        if B not in e.get('boards', [B]):
            continue
        sq = dict(board=B, refs=e['refs'], qty=e.get('qty', 1), value=e.get('value', ''), type=e.get('type', ''), row=len(lst),
                  bom_mfr='', bom_mpn=e.get('bom_mpn', ''), bom_dkpn=e.get('bom_dkpn', ''), idx=None,
                  note=e.get('note', ''), alt=e.get('alt', ''))
        for k in ('label', 'sub', 'qty_unit', 'qty_text'):
            if k in e:
                sq[k] = e[k]
        cand = find_line(e.get('use'))
        if cand:
            sq['idx'] = cand[0]
        lst.append(sq)
    lst.sort(key=lambda s: (cat_rank(s['refs'][0]), s['row'] if s['refs'][0].startswith('(') else refnum(s['refs'][0]),
                            s['refs'][0]))

# a part listed with a vague type on one board ("SMD only") takes the package another board gives it
specific = {}
for lst in squares.values():
    for s in lst:
        if s['bom_dkpn'] and re.search(r'\d', s['type']):
            specific.setdefault(dk_base(s['bom_dkpn']), s['type'])
for lst in squares.values():
    for s in lst:
        if s['bom_dkpn'] and not re.search(r'\d', s['type']) and dk_base(s['bom_dkpn']) in specific:
            s['type'] = specific[dk_base(s['bom_dkpn'])]

# ---------------------------------------------------------------- ordered-quantity check (all boards, all builds)
builds = {bd['key']: bd.get('builds', 1) for bd in boards}
use = {}
for B, lst in squares.items():
    for s in lst:
        if s['idx']:
            use.setdefault(s['idx'], {}).setdefault(B, 0)
            use[s['idx']][B] += s['qty'] * builds[B]
for B, lst in squares.items():
    for s in lst:
        if not s['idx']:
            continue
        o = order[s['idx']]
        extra = ORDER_NOTES.get(norm(o['dkpn'])) or ORDER_NOTES.get(norm(o['mpn']))
        if extra:
            s['note'] = '; '.join(t for t in (s['note'], extra) if t)
        u = use[s['idx']]
        need = sum(u.values())
        s.update(o_mpn=o['mpn'], o_mfr=o['mfr'], o_desc=o['desc'], o_qty=o['qty'], o_ref=o['ref'], o_dkpn=o['dkpn'],
                 o_back=o['back'], shared_with=sorted(b for b in u if b != B), need_total=need,
                 short=o['qty'] < need)
for i, u in sorted(use.items()):
    o, need = order[i], sum(use[i].values())
    if o['qty'] < need:
        report['short'].append(dict(idx=i, mpn=o['mpn'], ordered=o['qty'], needed=need, per_board=u))
    if o['back']:
        report['backorder'].append(dict(idx=i, mpn=o['mpn'], backorder=o['back']))

# ---------------------------------------------------------------- combine / companions (presentation)
for c in cfg.get('combine', []):
    for B, lst in squares.items():
        if B not in c.get('boards', [B]):
            continue
        parts = [next((s for s in lst if s['refs'][0] == r), None) for r in c['refs']]
        if any(p is None for p in parts) or len({p['idx'] for p in parts}) > 1:
            continue                                     # only lines on the same order line (or all not ordered)
        head = parts[0]
        for p in parts[1:]:
            lst.remove(p)
        head.update(refs=[f"({c['label']})"] if all(p['refs'][0].startswith('(') for p in parts) else
                    sum((p['refs'] for p in parts), []),
                    label=c['label'], qty_text=' + '.join(str(p['qty']) for p in parts), qty=sum(p['qty'] for p in parts),
                    value=c.get('value', ', '.join(f"{p['value']} ×{p['qty']}" for p in parts)), type=c.get('type', ''),
                    bom_mpn=', '.join(f"{p['bom_mpn']}" + (f" ({p['bom_dkpn']})" if p['bom_dkpn'] else '') for p in parts[:-1])
                    + (', ' if len(parts) > 1 else '') + parts[-1]['bom_mpn'], bom_dkpn=parts[-1]['bom_dkpn'])
        if 'sub' in c:
            head['sub'] = c['sub']
for c in cfg.get('companions', []):
    for B, lst in squares.items():
        main = next((s for s in lst if s['refs'][0] == c['main']), None)
        comp = next((s for s in lst if s['refs'][0] == c['companion']), None)
        if main and comp:
            lst.remove(comp)
            main['companion'] = comp
            main['qty_text'] = c.get('qty_text', f"{main['qty']} + {comp['qty']}" + (f" {comp['qty_unit']}" if comp.get('qty_unit') else ''))

# ---------------------------------------------------------------- report
used = set(use)
for B, lst in squares.items():
    for s in lst:
        if s['idx'] is None and not any(u.get('refs') == s['refs'] and u.get('board') == B for u in report['unmatched']):
            toks, pkg = value_tokens(s['value']), package_tokens(s['type'])
            cands = []
            for o in order.values():
                if o['idx'] in used:
                    continue
                words = set(re.split(r'[^A-Z0-9.]+', norm_units(o['desc'] + ' ' + o['mpn'])))
                score = len(toks & words) + 1.5 * len(pkg & words) * bool(toks & words)   # same package counts most
                if score and set(s['refs']) & set(re.split(r'[^A-Z0-9]+', norm(o['ref']))):
                    score += 1                                  # its customer reference names this part
                if score >= 1:
                    cands.append((score, o['idx']))
            report['unmatched'].append(dict(board=B, refs=s['refs'], value=s['value'], bom_mpn=s['bom_mpn'], bom_dkpn=s['bom_dkpn'],
                                            candidates=[dict(idx=i, mpn=order[i]['mpn'], desc=order[i]['desc'], ref=order[i]['ref'])
                                                        for _, i in sorted(cands, reverse=True)[:3]]))
report['unused'] = [dict(idx=i, mpn=o['mpn'], qty=o['qty'], ref=o['ref'], desc=o['desc'][:60]) for i, o in sorted(order.items()) if i not in used]

label = cfg.get('order_label')
if not label:
    m = re.search(r'(\d{6,})', os.path.basename(cfg['order_csv']))
    label = f'DigiKey order {m.group(1)}' if m else 'DigiKey order'
meta = dict(order_label=label, boards=[dict(key=b['key'], title=b.get('title', b['key']), builds=builds[b['key']]) for b in boards])
json.dump(dict(meta=meta, squares=squares, order=order, report=report), open(OUT, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)

for b in boards:
    lst = squares[b['key']]
    print(f"{b['key']}: {len(lst)} squares, {sum(1 for s in lst if not s['idx'])} not in the order"
          + (f", building {builds[b['key']]}" if builds[b['key']] != 1 else ''))
for u in report['unmatched']:
    c = '; '.join(f"IDX {x['idx']} {x['mpn']} ({x['desc'][:40]})" for x in u.get('candidates', []))
    print(f"  NOT IN ORDER {u['board']} {','.join(u['refs'])}: {u.get('why') or (u.get('bom_mpn') or u.get('value'))}"
          + (f"  -> candidates: {c}" if c else ''))
for a in report['ambiguous']:
    print(f"  AMBIGUOUS {a['board']} {','.join(a['refs'])}: order lines {a['lines']} (first one used)")
for s in report['short']:
    print(f"  SHORT IDX {s['idx']} {s['mpn']}: ordered {s['ordered']}, needed {s['needed']} {s['per_board']}")
for s in report['backorder']:
    print(f"  BACKORDER IDX {s['idx']} {s['mpn']}: {s['backorder']}")
print(f"  {len(report['unused'])} order lines not used by these boards:", ' '.join(str(u['idx']) for u in report['unused']))
