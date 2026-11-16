#!/usr/bin/env python3
"""Bus-level check of the card's PGBOOT server, without QEMU or the ROM.

Speaks the pgbridge wire protocol to a running pgcard-sim and drives the card
exactly like the ROM would: knock/select on 1D0h, registers on 1D1h/1D2h,
frames through the data window as word accesses split into two byte cycles
(base, base+1). Checks the PGBOOT registers and the BDINFO/BDREAD/BDWRITE
frames against the image files it is given.

  buscheck.py SOCKET FD_IMAGE HD_IMAGE

FD_IMAGE/HD_IMAGE are host copies of FD.IMG/HD.IMG as they were put on the
stick (the sim must run with --fd FD.IMG --hd HD.IMG). Exit status 0 = pass.
"""
import socket
import struct
import sys

CONTROL, DLOW, DHIGH = 0x1D0, 0x1D1, 0x1D2
CMD_MAGIC, CMD_PROTOCOL = 0x00, 0x01
CMD_DFSSTAT, CMD_DFSREQ, CMD_DFSEXEC, CMD_DFSRESP = 0x80, 0x81, 0x82, 0x83
CMD_DFSMAXLEN, CMD_DFSPORT = 0x85, 0x87
CMD_BDFDNAME, CMD_BDHDNAME, CMD_BDOPTS = 0x88, 0x89, 0x8A


class Bus:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)

    def out(self, port, v):
        self.s.sendall(struct.pack('<BBHI', ord('w'), 1, port, v & 0xFF))

    def inp(self, port):
        self.s.sendall(struct.pack('<BBHI', ord('r'), 1, port, 0))
        buf = b''
        while len(buf) < 8:
            chunk = self.s.recv(8 - len(buf))
            if not chunk:
                raise EOFError('sim closed the connection')
            buf += chunk
        op, _, p, v = struct.unpack('<BBHI', buf)
        assert op == ord('R') and p == port, (op, p, port)
        return v & 0xFF

    def outw(self, port, v):          # 16-bit OUT on an 8-bit card: two byte cycles
        self.out(port, v)
        self.out(port + 1, v >> 8)

    def inw(self, port):
        lo = self.inp(port)
        return lo | (self.inp(port + 1) << 8)

    def select(self, reg):
        self.out(CONTROL, 0xCC)
        self.out(CONTROL, reg)


class Card:
    def __init__(self, bus):
        self.b = bus
        self.b.select(CMD_DFSPORT)
        self.port = self.b.inw(DLOW)
        self.b.select(CMD_DFSMAXLEN)
        self.maxlen = self.b.inw(DLOW)

    def reg8(self, reg):
        self.b.select(reg)
        return self.b.inp(DHIGH)

    def string(self, reg):
        self.b.select(reg)
        out = bytearray()
        while True:
            c = self.b.inp(DHIGH)
            if c == 0 or len(out) > 200:
                return out.decode('latin-1')
            out.append(c)

    def set_string(self, reg, s):
        self.b.select(reg)
        for c in s.encode('latin-1') + b'\0':
            self.b.out(DHIGH, c)

    def transact(self, al, payload, polls=100000):
        frame = struct.pack('<HBB', 4 + len(payload), 0, al) + payload
        b = self.b
        b.select(CMD_DFSREQ)
        for i in range(0, len(frame) - 1, 2):
            b.outw(self.port, frame[i] | (frame[i + 1] << 8))
        if len(frame) & 1:
            b.out(self.port, frame[-1])
        b.select(CMD_DFSEXEC)
        b.out(DHIGH, 1)
        b.select(CMD_DFSSTAT)
        seen = []
        for _ in range(polls):
            st = b.inp(DHIGH)
            if not seen or seen[-1] != st:
                seen.append(st)
            if st in (3, 0xFE, 0xFF):
                break
        if st != 3:
            raise RuntimeError('status %02x (sequence %s)' % (st, ' '.join('%02x' % x for x in seen)))
        b.select(CMD_DFSRESP)
        ln = b.inw(self.port)
        ax = b.inw(self.port)
        n = ln - 4
        data = bytearray()
        for _ in range(n // 2):
            w = b.inw(self.port)
            data += bytes((w & 0xFF, w >> 8))
        if n & 1:
            data.append(b.inp(self.port))
        return ax, bytes(data), seen


def main():
    sock, fd_path, hd_path = sys.argv[1:4]
    fd = open(fd_path, 'rb').read()
    hd = open(hd_path, 'rb').read()
    fails = []

    def check(cond, what):
        print(('  ok   ' if cond else '  FAIL ') + what)
        if not cond:
            fails.append(what)

    card = Card(Bus(sock))
    check(card.reg8(CMD_MAGIC) == 0xDD, 'CMD_MAGIC reads DDh')
    check(card.reg8(CMD_PROTOCOL) >= 5, 'CMD_PROTOCOL >= 5')
    check(512 <= card.maxlen <= 32768, 'CMD_DFSMAXLEN %d in 512..32768' % card.maxlen)
    check(card.port == 0x1D4, 'CMD_DFSPORT %03Xh' % card.port)
    opts = card.reg8(CMD_BDOPTS)
    check(opts & 0xC0 == 0x80, 'CMD_BDOPTS signature (%02Xh)' % opts)
    fdn, hdn = card.string(CMD_BDFDNAME), card.string(CMD_BDHDNAME)
    check(fdn.upper().endswith('FD.IMG') and hdn.upper().endswith('HD.IMG'),
          'image names read back: "%s", "%s"' % (fdn, hdn))

    units = {}
    tokens = {}

    def refresh(unit, flags=0):
        """BDINFO for unit; remembers the image token. Returns (ax, rec)."""
        ax, rec, _ = card.transact(0xF3, bytes((unit, flags)))
        if ax == 0 and len(rec) >= 32:
            tokens[unit] = struct.unpack_from('<I', rec, 20)[0]
        return ax, rec

    def io(al, unit, n, lba, data=b'', token=None):
        tok = tokens.get(unit, 0) if token is None else token
        return card.transact(al, struct.pack('<BBII', unit, n, lba, tok) + data)

    # after the card boots nothing is open until a BDINFO with OPEN
    for unit in (0, 1):
        ax, rec = refresh(unit)
        check(ax == 0 and rec[1] == 0 and struct.unpack_from('<I', rec, 20)[0] == 0,
              'unit %d before OPEN: state 0, token 0 (AX=%04X state %d)' % (unit, ax, rec[1] if rec else -1))
        ax, _, _ = io(0xF4, unit, 1, 0, token=0)
        check(ax == 0x80, 'BDREAD unit %d before OPEN answers 80h (AX=%04X)' % (unit, ax))

    for unit, img, kind in ((0, fd, 1), (1, hd, 2)):
        ax, rec, seen = card.transact(0xF3, bytes((unit, 1)))
        check(ax == 0 and len(rec) >= 32, 'BDINFO unit %d OPEN: AX=%04X, %d bytes, status sequence %s'
              % (unit, ax, len(rec), ' '.join('%02x' % x for x in seen)))
        if ax == 0 and len(rec) >= 32:
            tokens[unit] = struct.unpack_from('<I', rec, 20)[0]
            check(tokens[unit] != 0, 'unit %d image token %08X' % (unit, tokens[unit]))
        if ax or len(rec) < 32:
            continue
        ver, state, typ, flags, cyl, heads, spt, _, total, fdtype, gen, nlen = struct.unpack('<BBBBHHHHIBBB', rec[:19])
        name = rec[32:32 + nlen].decode('latin-1')
        print('       v%d state %d type %d flags %02x CHS %d/%d/%d total %d fdtype %d gen %d name "%s"'
              % (ver, state, typ, flags, cyl, heads, spt, total, fdtype, gen, name))
        check(state == 1 and typ == kind, 'unit %d ready, type %d' % (unit, kind))
        check(total == len(img) // 512, 'unit %d total sectors %d == image %d' % (unit, total, len(img) // 512))
        if unit == 0:
            check((cyl, heads, spt, fdtype) == (80, 2, 18, 4), 'floppy geometry 80/2/18, type 4 (1.44M)')
        units[unit] = total

    maxn = min(255, card.maxlen // 512)
    for unit, img in ((0, fd), (1, hd)):
        if unit not in units:
            continue
        total = units[unit]
        for lba, n in ((0, 1), (1, maxn), (total - 3, 3), (total // 2, 7)):
            ax, data, _ = io(0xF4, unit, n, lba)
            check(ax == 0 and data == img[lba * 512:(lba + n) * 512],
                  'BDREAD unit %d lba %d count %d (AX=%04X, %d bytes, content %s)'
                  % (unit, lba, n, ax, len(data), 'matches' if data == img[lba * 512:(lba + n) * 512] else 'DIFFERS'))
        ax, data, _ = io(0xF4, unit, 2, total - 1)
        check(ax == 0x04 and not data, 'BDREAD past the end answers 04h (AX=%04X)' % ax)
        ax, data, _ = io(0xF4, unit, 1, 0, token=tokens[unit] ^ 0x5A5A5A5A)
        want = 0x06 if unit == 0 else 0x80
        check(ax == want and not data, 'BDREAD unit %d with a wrong token answers %02Xh (AX=%04X)' % (unit, want, ax))

    # write, read back, restore (hard disk unit, a sector in the free area)
    if 1 in units:
        lba = units[1] - 10
        orig = hd[lba * 512:(lba + 2) * 512]
        pat = bytes((i * 7 + 3) & 0xFF for i in range(1024))
        ax, _, _ = io(0xF5, 1, 2, lba, pat)
        check(ax == 0, 'BDWRITE unit 1 lba %d count 2 (AX=%04X)' % (lba, ax))
        ax, data, _ = io(0xF4, 1, 2, lba)
        check(ax == 0 and data == pat, 'BDREAD after BDWRITE returns the written data')
        ax, _, _ = io(0xF5, 1, 2, lba, orig, token=tokens[1] + 1)
        check(ax == 0x80, 'BDWRITE unit 1 with a wrong token answers 80h (AX=%04X)' % ax)
        ax, _, _ = io(0xF5, 1, 2, lba, orig)
        check(ax == 0, 'BDWRITE restores the original sectors')
        wmax = min(255, (card.maxlen - 10) // 512)
        orig = hd[lba * 512:(lba + wmax) * 512]
        ax, _, _ = io(0xF5, 1, wmax, lba - wmax, hd[(lba - wmax) * 512:lba * 512])
        check(ax == 0, 'BDWRITE of %d sectors (the largest frame, unchanged data) (AX=%04X)' % (wmax, ax))

    # read-only option: CMD_BDOPTS bit 0, then BDINFO OPEN reports it and BDWRITE is refused
    if 0 in units:
        card.b.select(CMD_BDOPTS)
        card.b.out(DHIGH, 0x01)
        check(card.reg8(CMD_BDOPTS) == 0x81, 'CMD_BDOPTS write 01h reads back 81h')
        old_token = tokens[0]
        ax, rec = refresh(0, 1)
        check(ax == 0 and rec[3] & 1, 'BDINFO unit 0 flags bit 0 (read-only) with option bit 0 (flags %02x)' % rec[3])
        check(tokens[0] != old_token, 'BDINFO OPEN gives a new token (%08X -> %08X)' % (old_token, tokens[0]))
        ax, _, _ = io(0xF4, 0, 1, 0, token=old_token)
        check(ax == 0x06, 'BDREAD with the token from before the OPEN answers 06h (AX=%04X)' % ax)
        ax, _, _ = io(0xF5, 0, 1, 100, fd[100 * 512:101 * 512])
        check(ax == 0x03, 'BDWRITE to the read-only floppy answers 03h (AX=%04X)' % ax)
        card.b.select(CMD_BDOPTS)
        card.b.out(DHIGH, 0x00)
        ax, rec = refresh(0, 1)
        check(ax == 0 and not rec[3] & 1, 'option cleared: floppy writable again (flags %02x)' % rec[3])

    # floppy swap through the name register: media change flag and generation
    if 0 in units:
        ax, rec = refresh(0)
        gen0 = rec[17]
        old_token = tokens[0]
        card.set_string(CMD_BDFDNAME, 'FD.IMG')     # the same file again is a swap too
        ax, _, _ = io(0xF4, 0, 1, 0)
        check(ax == 0x06, 'BDREAD after a floppy name commit answers 06h (media changed) (AX=%04X)' % ax)
        ax, rec = refresh(0)
        check(ax == 0 and rec[3] & 4 and rec[17] != gen0 and tokens[0] != old_token,
              'name commit: BDINFO flags bit 2 (media changed), a new generation and token (flags %02x, gen %d -> %d, state %d)'
              % (rec[3], gen0, rec[17], rec[1]))
        ax, data, _ = io(0xF4, 0, 1, 0)
        check(ax == 0 and data == fd[:512], 'BDREAD with the new token works (AX=%04X)' % ax)
        ax, rec = refresh(0)
        check(ax == 0 and not rec[3] & 4, 'media-changed flag cleared by the next BDINFO (flags %02x)' % rec[3])
        card.set_string(CMD_BDFDNAME, 'FD.IMG')
        ax, rec = refresh(0)
        name = rec[32:32 + rec[18]].decode('latin-1')
        check(ax == 0 and rec[1] == 1 and name.upper().endswith('FD.IMG'), 'swapped back to FD.IMG (state %d, "%s")' % (rec[1], name))

    # an old/unknown unit
    ax, _, _ = card.transact(0xF3, bytes((5, 0)))
    check(ax == 0x01, 'BDINFO unit 5 answers 01h (AX=%04X)' % ax)

    print('buscheck: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
