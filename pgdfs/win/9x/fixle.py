#!/usr/bin/env python3
"""Post-link fixes for an Open Watcom LE VxD, so that the Windows VMM loads it
the way it loads a VxD linked by Microsoft's LINK:

  - every object (segment) executable and based at address 0: a VxD runs in
    the flat model, the VMM places each object and applies the fixups;
  - the entry of the device descriptor block flagged exported and shared;
  - the count of preload pages in the header;
  - the data pages starting on a 512-byte boundary (the Windows 3.1 loader
    insists, newer ones do not mind);
  - the MZ header's image size covering the whole file.

    fixle.py PGDFS.VXD
"""
import struct
import sys

OBJ_EXECUTABLE = 0x0004
OBJ_PRELOAD = 0x0040
ENTRY_EXPORTED = 0x01
ENTRY_SHARED = 0x02


def main(path):
    with open(path, "rb") as f:
        d = bytearray(f.read())
    if d[:2] != b"MZ":
        sys.exit(path + ": not an MZ image")
    le = struct.unpack_from("<I", d, 0x3C)[0]
    if d[le:le + 2] != b"LE":
        sys.exit(path + ": no LE header")
    if struct.unpack_from("<H", d, le + 0x0A)[0] != 4:
        sys.exit(path + ": not a Windows VxD (LE OS type 4)")

    # data pages on a 512-byte boundary; the tables after them move along
    datapages = struct.unpack_from("<I", d, le + 0x80)[0]
    pad = (-datapages) % 512
    if pad:
        d[datapages:datapages] = bytes(pad)
        struct.pack_into("<I", d, le + 0x80, datapages + pad)
        for off in (0x88, 0x98):        # non-resident names, debug info
            v = struct.unpack_from("<I", d, le + off)[0]
            if v >= datapages:
                struct.pack_into("<I", d, le + off, v + pad)

    objtab = le + struct.unpack_from("<I", d, le + 0x40)[0]
    nobj = struct.unpack_from("<I", d, le + 0x44)[0]
    preload = 0
    for i in range(nobj):
        o = objtab + i * 24
        vsize, base, flags, first, count, _ = struct.unpack_from("<6I", d, o)
        flags |= OBJ_EXECUTABLE
        if flags & OBJ_PRELOAD:
            preload += count
        struct.pack_into("<I", d, o + 4, 0)
        struct.pack_into("<I", d, o + 8, flags)
    struct.pack_into("<I", d, le + 0x84, preload)

    # entry table: bundles of (count, type[, object]); type 3 = 32-bit entries
    et = le + struct.unpack_from("<I", d, le + 0x5C)[0]
    found = False
    while d[et] != 0:
        count, kind = d[et], d[et + 1]
        if kind == 0:
            et += 2
            continue
        if kind != 3:
            sys.exit(path + ": unexpected entry bundle type %d" % kind)
        for i in range(count):
            d[et + 4 + i * 5] |= ENTRY_EXPORTED | ENTRY_SHARED
            found = True
        et += 4 + count * 5
    if not found:
        sys.exit(path + ": no 32-bit entry (the DDB must be exported as ordinal 1)")

    n = len(d)
    struct.pack_into("<HH", d, 2, n % 512, (n + 511) // 512)
    with open(path, "wb") as f:
        f.write(d)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
