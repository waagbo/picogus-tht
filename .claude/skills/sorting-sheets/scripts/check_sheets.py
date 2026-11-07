"""Check rendered sorting sheets against mapping.json, and optionally write PNG previews.

usage: python3 -I check_sheets.py MAPPING.json SHEETS.pdf [SHEETS2.pdf ...] [--boards A,B] [--layout L]
                                  [--png-dir DIR] [--dpi 90]

Pass the same --boards/--layout as render.py (all PDFs together must cover exactly those boards).
Checks: one "Qty:" per square, every reference designator printed, every expected "IDX n" printed,
"NOT IN THIS ORDER" on every unordered square, no "…" (text cut off), all text at least 5 mm from the
paper edge, and for the a5fold layouts nothing but the fold note within 5 mm of the fold.
Uses poppler-utils (pdftotext, pdftoppm) when installed, otherwise the Python package pdfplumber.
Exit status 1 when a check fails.
"""
import argparse, collections, json, re, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('mapping'); ap.add_argument('pdfs', nargs='+')
ap.add_argument('--boards', default=''); ap.add_argument('--layout', default='a5fold')
ap.add_argument('--png-dir', default=''); ap.add_argument('--dpi', type=int, default=90)
A = ap.parse_args()
A.layout = 'a5fold' if A.layout == 'a5fold-large' else A.layout
D = json.load(open(A.mapping, encoding='utf-8'))
boards = A.boards.split(',') if A.boards else [b['key'] for b in D.get('meta', {}).get('boards', [])] or list(D['squares'])
MM = 72 / 25.4

expected = []
for b in boards:
    for s in D['squares'][b]:
        comp = s.get('companion')
        if comp and A.layout == 'a5fold':            # the large-square layout prints the companion as its own square
            expected += [dict(s, companion=None), comp]
        else:
            expected.append(s)

words, fails, notes = [], [], []
POPPLER = bool(shutil.which('pdftotext'))
if not POPPLER:
    try:
        import pdfplumber
    except ImportError:
        sys.exit('needs poppler-utils (pdftotext) or the Python package pdfplumber')
for pdf in A.pdfs:
    stem = re.sub(r'\.pdf$', '', pdf.split('/')[-1])
    if POPPLER:                     # word boxes in points, origin top-left
        x = subprocess.run(['pdftotext', '-bbox', pdf, '-'], capture_output=True, text=True, check=True).stdout
        for pn, pg in enumerate(x.split('<page ')[1:], 1):
            W = float(re.search(r'width="([\d.]+)"', pg).group(1)); H = float(re.search(r'height="([\d.]+)"', pg).group(1))
            for m in re.finditer(r'<word xMin="([\d.]+)" yMin="([\d.]+)" xMax="([\d.]+)" yMax="([\d.]+)">([^<]*)</word>', pg):
                x0, y0, x1, y1 = map(float, m.groups()[:4])
                t = m.group(5).replace('&amp;', '&').replace('&lt;', '<').replace('&gt;', '>').replace('&quot;', '"')
                words.append((pdf, pn, W, H, x0, y0, x1, y1, t))
        if A.png_dir and shutil.which('pdftoppm'):
            subprocess.run(['pdftoppm', '-r', str(A.dpi), '-png', pdf, f'{A.png_dir}/{stem}'], check=False)
    else:                           # same coordinates from pdfplumber
        with pdfplumber.open(pdf) as doc:
            for pn, page in enumerate(doc.pages, 1):
                for w in page.extract_words(keep_blank_chars=False, use_text_flow=False):
                    words.append((pdf, pn, float(page.width), float(page.height), w['x0'], w['top'], w['x1'], w['bottom'], w['text']))
                if A.png_dir:
                    try:
                        page.to_image(resolution=A.dpi).save(f'{A.png_dir}/{stem}-{pn}.png')
                    except Exception as e:  # rendering needs pypdfium2; the checks above do not
                        notes.append(f'no PNG preview ({e.__class__.__name__})')

toks = collections.Counter(w[8].strip(',;:.()') for w in words)
text = ' '.join(w[8] for w in words)

n_qty = sum(1 for w in words if w[8] == 'Qty:')
if n_qty != len(expected):
    fails.append(f'{n_qty} "Qty:" labels printed, {len(expected)} squares expected')
for s in expected:
    for r in s['refs']:
        if not r.startswith('(') and toks[r] < 1:
            fails.append(f"reference {r} ({s['board']}) not printed")
idx_printed = collections.Counter(words[i + 1][8] for i, w in enumerate(words[:-1]) if w[8] == 'IDX')
idx_need = collections.Counter(str((s.get('companion') or s)['idx']) for s in expected if (s.get('companion') or s).get('idx'))
for i, n in idx_need.items():
    if idx_printed[i] < n:
        fails.append(f'IDX {i} printed {idx_printed[i]}x, expected {n}x')
missing = sum(1 for s in expected if not (s.get('companion') or s).get('idx'))
if text.count('NOT IN THIS ORDER') < missing:
    fails.append(f"'NOT IN THIS ORDER' printed {text.count('NOT IN THIS ORDER')}x, {missing} unordered squares")
cut = [w[8] for w in words if '…' in w[8]]
if cut:
    fails.append(f'text cut off with an ellipsis: {cut[:5]}')
for pdf, pn, W, H, x0, y0, x1, y1, t in words:
    if x0 < 5 * MM or y0 < 5 * MM or x1 > W - 5 * MM or y1 > H - 5 * MM:
        fails.append(f'{pdf} p{pn}: "{t}" within 5 mm of the paper edge')
    if A.layout.startswith('a5fold') and y1 > H / 2 - 5 * MM and y0 < H / 2 + 5 * MM:
        if not (H / 2 - 3.5 * MM < y1 <= H / 2 + 0.5):    # the small fold note just above the crease is allowed
            fails.append(f'{pdf} p{pn}: "{t}" in the fold band')
pages = len({(w[0], w[1]) for w in words})
print(f'{len(expected)} squares on {pages} pages: {len(words)} words checked'
      + ('' if POPPLER else ' (pdfplumber)') + (f'; {notes[0]}' if notes else ''))
for f in fails[:40]:
    print('FAIL', f)
if len(fails) > 40:
    print(f'... and {len(fails) - 40} more')
print('OK' if not fails else f'{len(fails)} problem(s)')
sys.exit(1 if fails else 0)
