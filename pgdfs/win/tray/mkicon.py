#!/usr/bin/env python3
"""Draws PGTRAY's two icons: a compact disc (an image is loaded) and a blank
one (none is). 32x32 and 16x16 in the 16 colours every Windows 95 display
has.

  mkicon.py cd.ico empty.ico

Copyright (C) 2026 PicoGUS contributors. MIT license, see ../../LICENSE.
"""
import math
import struct
import sys

# the VGA palette, as RGB
PALETTE = [
    (0x00, 0x00, 0x00), (0x80, 0x00, 0x00), (0x00, 0x80, 0x00), (0x80, 0x80, 0x00),
    (0x00, 0x00, 0x80), (0x80, 0x00, 0x80), (0x00, 0x80, 0x80), (0xC0, 0xC0, 0xC0),
    (0x80, 0x80, 0x80), (0xFF, 0x00, 0x00), (0x00, 0xFF, 0x00), (0xFF, 0xFF, 0x00),
    (0x00, 0x00, 0xFF), (0xFF, 0x00, 0xFF), (0x00, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF),
]
BLACK, TEAL, SILVER, GRAY, BLUE, AQUA, WHITE = 0, 6, 7, 8, 12, 14, 15
CLEAR = None


def disc(size, loaded):
    """size x size pixels, top row first: palette indexes, CLEAR = transparent"""
    c = (size - 1) / 2.0
    outer = size / 2.0 - 0.5
    hole = outer * 0.16
    hub = outer * 0.42
    edge = 1.0 if size <= 16 else 1.3
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            dx, dy = x - c, y - c
            d = math.hypot(dx, dy)
            a = math.degrees(math.atan2(-dy, dx)) % 360
            if d > outer or d < hole:
                px = CLEAR
            elif d > outer - edge or d < hole + edge:
                px = BLACK if loaded else GRAY
            elif not loaded:
                # a blank: white, with the ring of the hub
                px = GRAY if hub - edge < d < hub else WHITE
            elif d < hub:
                px = WHITE if d > hub - edge else SILVER
            else:
                # the sheen of a disc: light from the top left, its mirror image
                # at the bottom right, a band of colour next to each
                shine = min(abs(a - 135), abs(a - 315))
                if shine < 14:
                    px = WHITE
                elif shine < 30:
                    px = AQUA
                elif shine < 42:
                    px = BLUE if size > 16 else TEAL
                else:
                    px = SILVER
            row.append(px)
        rows.append(row)
    return rows


def dib(rows):
    """BITMAPINFOHEADER + palette + 4-bit XOR image + 1-bit AND mask, bottom-up"""
    size = len(rows)
    xor = bytearray()
    mask = bytearray()
    for row in reversed(rows):
        line = bytearray()
        for i in range(0, size, 2):
            line.append(((row[i] or 0) << 4) | (row[i + 1] or 0))
        line += bytes(-len(line) % 4)
        xor += line
        bits = bytearray((size + 7) // 8)
        for i, px in enumerate(row):
            if px is CLEAR:
                bits[i // 8] |= 0x80 >> (i % 8)
        bits += bytes(-len(bits) % 4)
        mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, size, 2 * size, 1, 4, 0, len(xor) + len(mask), 0, 0, 16, 0)
    palette = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in PALETTE)
    return header + palette + bytes(xor) + bytes(mask)


def ico(path, loaded):
    images = [dib(disc(size, loaded)) for size in (32, 16)]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, image in zip((32, 16), images):
        out += struct.pack("<BBBBHHII", size, size, 16, 0, 1, 4, len(image), offset)
        offset += len(image)
    with open(path, "wb") as f:
        f.write(out + b"".join(images))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ico(sys.argv[1], True)
    ico(sys.argv[2], False)
