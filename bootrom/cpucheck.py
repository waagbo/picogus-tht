#!/usr/bin/env python3
"""cpucheck.py: second opinion on NASM's `cpu 8086`.

usage: cpucheck.py <listing.lst> [allowed_label...]

Walks a NASM listing and reports every instruction whose opcode (after
prefixes) exists only on the 80186 or later: PUSHA/POPA, BOUND, PUSH imm,
IMUL imm, INS/OUTS, shifts by an immediate count (C0/C1), ENTER/LEAVE, and
any 0F-prefixed opcode (286+). Instructions after one of the allowed labels
(up to the next non-local label) are skipped: those are the 186+ transfer
routines that only run after the CPU check. Exit status 1 on findings.
"""
import re
import sys

OPC_186 = {0x60: "pusha", 0x61: "popa", 0x62: "bound", 0x68: "push imm16",
           0x69: "imul imm16", 0x6A: "push imm8", 0x6B: "imul imm8",
           0x6C: "insb", 0x6D: "insw", 0x6E: "outsb", 0x6F: "outsw",
           0xC0: "shift imm8", 0xC1: "shift imm8", 0xC8: "enter", 0xC9: "leave",
           0x0F: "0F xx (286+)"}
PREFIXES = {0xF2, 0xF3, 0x26, 0x2E, 0x36, 0x3E, 0xF0}
DATA = re.compile(r"^\s*(?:[\w.]+:?\s+)?(db|dw|dd|dq|times|resb|resw|resd|incbin)\b", re.I)
LINE = re.compile(r"^\s*\d+\s+([0-9A-F]{8})\s+([0-9A-F\[\]()]+)-?\s*(?:<\d+>\s*)?(.*)$")
LABEL = re.compile(r"^\s*(?:<\d+>\s*)?([A-Za-z_][\w]*):")


def main():
    lst = sys.argv[1]
    allowed = set(sys.argv[2:])
    skip = False
    bad = 0
    checked = 0
    for raw in open(lst, errors="replace"):
        # a non-local label switches the allowed region on or off
        src = re.sub(r"^\s*\d+\s+(?:[0-9A-F]{8}\s+[0-9A-F\[\]()]+-?\s*)?(?:<\d+>\s*)?", "", raw)
        m = LABEL.match(src)
        if m:
            skip = m.group(1) in allowed
        m = LINE.match(raw)
        if not m:
            continue
        hexs, text = m.group(2), m.group(3)
        if DATA.match(text) or not text.strip() or text.strip().startswith(";"):
            continue
        hexs = re.sub(r"[\[\]()]", "", hexs)
        try:
            code = bytes.fromhex(hexs)
        except ValueError:
            continue
        i = 0
        while i < len(code) and code[i] in PREFIXES:
            i += 1
        if i >= len(code):
            continue
        checked += 1
        op = code[i]
        if op in OPC_186 and not skip:
            print("%s: %s  %s  (%s)" % (m.group(1), hexs, text.strip(), OPC_186[op]))
            bad += 1
    print("cpucheck: %d instructions checked, %d 186+ opcodes outside %s"
          % (checked, bad, ", ".join(sorted(allowed)) or "-"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
