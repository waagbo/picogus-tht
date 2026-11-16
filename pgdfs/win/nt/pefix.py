#!/usr/bin/env python3
"""Turns the image GNU ld produces for pgdfs.sys into a kernel driver as the
Windows NT loader expects it: no DLL flag (ld sets it for -shared, which is
what gives the image its base relocations), no DllCharacteristics bits that
Windows NT 4.0 does not know, and a correct PE checksum, which the kernel
verifies when it loads a driver.

    pefix.py pgdfs.sys
"""
import struct
import sys

IMAGE_FILE_DLL = 0x2000
IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002
IMAGE_FILE_32BIT_MACHINE = 0x0100


def checksum(data, csum_off):
    total = 0
    n = len(data)
    padded = data + (b"\0" if n & 1 else b"")
    for i in range(0, len(padded), 2):
        if i == csum_off or i == csum_off + 2:
            continue                      # the checksum field itself counts as 0
        total += padded[i] | (padded[i + 1] << 8)
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + n) & 0xFFFFFFFF


def main(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if data[:2] != b"MZ":
        sys.exit(path + ": not an MZ image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        sys.exit(path + ": no PE header")
    fh = pe + 4                           # IMAGE_FILE_HEADER
    machine, = struct.unpack_from("<H", data, fh)
    if machine != 0x14C:
        sys.exit(path + ": not an i386 image")
    chars, = struct.unpack_from("<H", data, fh + 18)
    chars = (chars & ~IMAGE_FILE_DLL) | IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE
    struct.pack_into("<H", data, fh + 18, chars)
    oh = fh + 20                          # IMAGE_OPTIONAL_HEADER32
    if struct.unpack_from("<H", data, oh)[0] != 0x10B:
        sys.exit(path + ": not PE32")
    subsystem, = struct.unpack_from("<H", data, oh + 68)
    if subsystem != 1:
        sys.exit(path + ": subsystem %d, expected 1 (native)" % subsystem)
    struct.pack_into("<H", data, oh + 70, 0)          # DllCharacteristics
    csum_off = oh + 64
    struct.pack_into("<I", data, csum_off, 0)
    struct.pack_into("<I", data, csum_off, checksum(bytes(data), csum_off))
    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
