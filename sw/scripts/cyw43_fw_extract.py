#!/usr/bin/env python3
"""Extract the CYW43 WiFi firmware blob from the Pico SDK's C header.

usage: cyw43_fw_extract.py <wNNNNN_combined.h> <output .bin>

The multi-firmware PicoGUS image for Pico W boards stores one copy of the blob
at a fixed flash offset that all modes share (see web/web_cyw43_fw_shared.h),
instead of every mode carrying its own ~225KB copy.
"""

import re
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = argv[1], argv[2]
    with open(src) as f:
        text = f.read()
    body = re.search(r"\[\]\s*[^=]*=\s*\{(.*?)\};", text, re.S)
    fw_len = re.search(r"#define\s+CYW43_WIFI_FW_LEN\s+\((\d+)\)", text)
    clm_len = re.search(r"#define\s+CYW43_CLM_LEN\s+\((\d+)\)", text)
    if not body or not fw_len or not clm_len:
        print(f"error: unexpected format of {src}", file=sys.stderr)
        return 1
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body.group(1)))
    # The CLM data follows the firmware padded to 512 bytes
    expected = ((int(fw_len.group(1)) + 511) & ~511) + int(clm_len.group(1))
    if len(data) < expected:
        print(f"error: blob is {len(data)} bytes, expected at least {expected}", file=sys.stderr)
        return 1
    with open(dst, "wb") as f:
        f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
