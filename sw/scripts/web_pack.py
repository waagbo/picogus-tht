#!/usr/bin/env python3
"""Pack the web page of the PicoGUS web server for the firmware.

usage: web_pack.py <www dir> [--bin FILE] [--header FILE] [--html FILE]
                   [--max-size BYTES] [--extra /path=FILE ...]

Reads <www dir>/index.html and puts the files it refers to with a
"data-inline" attribute into it:

    <link rel="stylesheet" href="app.css" data-inline>
    <script src="app.js" data-inline></script>

so the card serves the page as one document and one request (it handles few
connections at a time). Fonts and images that the style sheets load with
url(...) go in as data: URIs. Every file under <www dir>/static/ is packed as
well and served at /static/<its path>: the place for anything that is better
loaded separately.

The pack is a directory followed by the files, text files gzip-compressed
(see web_assets.c for the format). It is stored in flash - the firmware runs
from RAM, which is better spent on the sound card:

  --bin FILE     the pack as a binary file, for the multi-firmware image,
                 which stores it once for all modes
  --header FILE  the pack as a C array, for firmware built for a single mode
  --html FILE    the page as the browser gets it (not compressed), for the
                 mock card in web/test
"""

import argparse
import base64
import gzip
import os
import re
import struct
import sys
import zlib

MAGIC = b"PGWP"
VERSION = 1
PATH_LEN = 28
TYPE_LEN = 28
FLAG_GZIP = 1

TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css",
    ".js": "text/javascript",
    ".json": "application/json",
    ".txt": "text/plain; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".gif": "image/gif",
    ".ico": "image/x-icon",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
}
# worth compressing; the others are compressed formats already
TEXT = {".html", ".css", ".js", ".json", ".txt", ".svg"}


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def squeeze(text: str, kind: str) -> str:
    """Light minification: comments, indentation and blank lines. gzip does the rest."""
    if kind in ("css", "js"):
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    lines = [line.strip() for line in text.splitlines()]
    if kind == "js":
        # whole-line comments only: a "//" later in a line may be part of a string
        lines = [line for line in lines if not line.startswith("//")]
    return "\n".join(line for line in lines if line)


def inline_urls(css: str, css_path: str) -> str:
    """url(file) in a style sheet -> data: URI"""
    def repl(m: re.Match) -> str:
        ref = m.group(2)
        if ref.startswith(("data:", "http:", "https:", "#")):
            return m.group(0)
        path = os.path.join(os.path.dirname(css_path), ref)
        ext = os.path.splitext(path)[1].lower()
        with open(path, "rb") as f:
            data = base64.b64encode(f.read()).decode("ascii")
        return f'url("data:{TYPES.get(ext, "application/octet-stream")};base64,{data}")'
    return re.sub(r"url\(\s*(['\"]?)([^'\")]+)\1\s*\)", repl, css)


def build_page(www: str) -> str:
    html = read(os.path.join(www, "index.html"))

    def css(m: re.Match) -> str:
        path = os.path.join(www, m.group(1))
        return "<style>\n" + squeeze(inline_urls(read(path), path), "css") + "\n</style>"

    def js(m: re.Match) -> str:
        text = squeeze(read(os.path.join(www, m.group(1))), "js")
        if "</script" in text.lower():
            raise SystemExit(f"error: {m.group(1)} contains </script, which can't be put into the page")
        return "<script>\n" + text + "\n</script>"

    html = re.sub(r"<!--.*?-->", "", html, flags=re.S)
    html = re.sub(r'<link\s+rel="stylesheet"\s+href="([^"]+)"\s+data-inline\s*>', css, html)
    html = re.sub(r'<script\s+src="([^"]+)"\s+data-inline\s*>\s*</script>', js, html)
    if "data-inline" in html:
        raise SystemExit("error: index.html has a data-inline reference this script does not understand")
    return "\n".join(line for line in html.splitlines() if line.strip())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("www")
    ap.add_argument("--bin")
    ap.add_argument("--header")
    ap.add_argument("--html")
    ap.add_argument("--max-size", type=int, default=0)
    ap.add_argument("--extra", action="append", default=[], metavar="/path=FILE")
    args = ap.parse_args()

    page = build_page(args.www).encode("utf-8")
    files = [("/", ".html", page)]
    static = os.path.join(args.www, "static")
    for dirpath, dirnames, filenames in os.walk(static):
        dirnames.sort()
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            url = "/static/" + os.path.relpath(path, static).replace(os.sep, "/")
            with open(path, "rb") as f:
                files.append((url, os.path.splitext(name)[1].lower(), f.read()))
    for extra in args.extra:
        url, _, path = extra.partition("=")
        with open(path, "rb") as f:
            files.append((url, os.path.splitext(path)[1].lower(), f.read()))

    # header, directory, then the files, each starting on a 4 byte boundary
    entry = struct.Struct(f"<{PATH_LEN}s{TYPE_LEN}sIIII")
    offset = 16 + entry.size * len(files)
    directory, blobs, report = b"", b"", []
    for url, ext, data in files:
        ctype = TYPES.get(ext, "application/octet-stream")
        if len(url) >= PATH_LEN:
            raise SystemExit(f"error: {url}: path longer than {PATH_LEN - 1} characters")
        flags = 0
        stored = data
        if ext in TEXT:
            # mtime=0 keeps builds reproducible
            stored = gzip.compress(data, compresslevel=9, mtime=0)
            flags |= FLAG_GZIP
        etag = zlib.crc32(stored)
        directory += entry.pack(url.encode(), ctype.encode(), offset, len(stored), etag, flags)
        stored += b"\0" * (-len(stored) % 4)
        blobs += stored
        offset += len(stored)
        report.append(f"{url}: {len(data)} bytes, {len(stored)} stored")
    pack = MAGIC + struct.pack("<HHII", VERSION, len(files), offset, 0) + directory + blobs
    assert len(pack) == offset

    if args.max_size and len(pack) > args.max_size:
        print(f"error: the web page files take {len(pack)} bytes, but {args.max_size} are reserved for them "
              f"in the firmware image. Reserve more with -DWEB_ASSETS_KB=<KB> (CMake).", file=sys.stderr)
        return 1
    if args.html:
        with open(args.html, "wb") as f:
            f.write(page)
    if args.bin:
        with open(args.bin, "wb") as f:
            f.write(pack)
    if args.header:
        out = [
            "// Generated by scripts/web_pack.py from web/www - do not edit",
            "#pragma once",
            "#include <stdint.h>",
            '#include "pico.h"',
        ]
        out += ["// " + line for line in report]
        out += [
            "// in flash: only read while web_task() holds off core 0 flash writes",
            f'static const uint8_t __in_flash("web_ui") __attribute__((aligned(4))) web_assets_pack[{len(pack)}] = {{',
        ]
        for i in range(0, len(pack), 16):
            out.append("    " + ", ".join(f"0x{b:02x}" for b in pack[i:i + 16]) + ",")
        out.append("};")
        with open(args.header, "w") as f:
            f.write("\n".join(out) + "\n")
    print(f"web page pack: {len(pack)} bytes ({'; '.join(report)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
