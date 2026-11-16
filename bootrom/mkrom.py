#!/usr/bin/env python3
"""mkrom.py: finish a PGBOOT option ROM image.

usage: mkrom.py <in.bin> <out.rom>     set the checksum byte, verify
       mkrom.py --check <file.rom>     verify only

Checks: exactly 8192 bytes, 55 AA, size byte 10h (16 * 512), a near or short
jump at offset 3, the 'PGBT' loader signature at offset 6, and an 8-bit sum
of 0 over the whole image (the last byte is the checksum byte).
"""
import struct
import sys

ROM_SIZE = 8192


def check(img, name):
    errs = []
    if len(img) != ROM_SIZE:
        errs.append("size %d, expected %d" % (len(img), ROM_SIZE))
    if img[0:2] != b"\x55\xAA":
        errs.append("no 55 AA signature")
    if img[2] != ROM_SIZE // 512:
        errs.append("size byte %02Xh, expected %02Xh" % (img[2], ROM_SIZE // 512))
    if img[3] not in (0xE9, 0xEB):
        errs.append("no jump at offset 3")
    if img[6:10] != b"PGBT":
        errs.append("no PGBT signature at offset 6")
    if sum(img) & 0xFF:
        errs.append("checksum %02Xh, expected 0" % (sum(img) & 0xFF))
    if errs:
        for e in errs:
            print("%s: %s" % (name, e), file=sys.stderr)
        return False
    used = struct.unpack_from("<H", img, 0x14)[0]
    print("%s: OK, %d bytes, %d used (%d free), checksum byte %02Xh"
          % (name, len(img), used, ROM_SIZE - 1 - used, img[-1]))
    return True


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        img = open(sys.argv[2], "rb").read()
        sys.exit(0 if check(img, sys.argv[2]) else 1)
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    img = bytearray(open(sys.argv[1], "rb").read())
    if len(img) != ROM_SIZE:
        print("%s: size %d, expected %d" % (sys.argv[1], len(img), ROM_SIZE), file=sys.stderr)
        sys.exit(1)
    img[-1] = 0
    img[-1] = (-sum(img)) & 0xFF
    if not check(img, sys.argv[2]):
        sys.exit(1)
    open(sys.argv[2], "wb").write(img)


if __name__ == "__main__":
    main()
