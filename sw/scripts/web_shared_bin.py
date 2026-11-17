#!/usr/bin/env python3
"""Join what the modes of the multi-firmware image share on a Pico W into one file.

usage: web_shared_bin.py <output> <file> <size> [<file> <size> ...]

Each file is padded with FFh (erased flash) to its size. The firmwares find
the parts at fixed offsets: the WiFi chip's firmware first, then the pack of
web page files (web/webserver.cmake).
"""

import sys


def main(argv: list[str]) -> int:
    if len(argv) < 4 or len(argv) % 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = b""
    for path, size in zip(argv[2::2], argv[3::2]):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) > int(size):
            print(f"error: {path} is {len(data)} bytes, more than the {size} reserved for it", file=sys.stderr)
            return 1
        out += data + b"\xff" * (int(size) - len(data))
    with open(argv[1], "wb") as f:
        f.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
