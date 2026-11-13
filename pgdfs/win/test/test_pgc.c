/*
 * Host test for the PGDFS client core of the Windows drivers (../common/pgc.c).
 *
 * The core talks to an emulated PicoGUS at the I/O port level: the knock /
 * register select / data window decoding of sw/picogus.cpp is mirrored here,
 * and behind it run the real firmware transport (sw/dfs/dfs_transport.c), the
 * real EDF5 server (sw/dfs/dfs_server.c, dfs_fs.c) and FatFs on a RAM disk.
 * "Core 1" (dfs_tasks()) runs whenever the core polls the status byte.
 *
 * Covered: the transaction sequence, files bigger than a frame, more open
 * files than the card has ids, long names, directory listing, and the
 * removable medium: unplugged, plugged back (noticed and unnoticed), and
 * replaced by a different one; the control requests of a setup program
 * (pgc_ctl()) and, through them, the card part of PGTRAY (../tray/pgcard.c)
 * against the setup registers of emuctl.c.
 *
 * Copyright (C) 2026 PicoGUS contributors. MIT license, see ../../LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ff.h"
#include "dfs.h"
#include "dfs_server.h"
#include "ramdisk_diskio.h"
#include "pgc.h"
#include "emuctl.h"
#include "../tray/pgcard.h"

/* ---- checks ------------------------------------------------------------------ */

static int n_checks, n_fails;

#define CHECK(cond, ...) do { \
    n_checks++; \
    if (!(cond)) { \
        n_fails++; \
        printf("  FAIL line %d: ", __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define SECTION(name) printf("== %s\n", name)

/* ---- firmware platform stubs ----------------------------------------------------- */

static uint32_t clock_ms = 1000;
static FATFS fatfs;

uint32_t dfs_platform_millis(void) { return clock_ms; }
FATFS *dfs_platform_fatfs(void) { return &fatfs; }

/* ---- the emulated card (sw/picogus.cpp, PGDFS part) --------------------------------- */

static bool     knocked;
static uint8_t  selreg;
static uint16_t dfs_port = DFS_DEFAULT_DATA_PORT;
static bool     hang;           /* core 1 stops serving: the timeout path */
static uint32_t port_ops;
static int      dead_reads;     /* the card restarts: nobody drives the bus for this many reads */
static uint32_t pgdfs_selects;  /* selects of PGDFS registers, firmware programming, defaults */

uint8_t pgc_plat_inb(uint16_t port) {
    port_ops++;
    if (dead_reads > 0) {
        dead_reads--;
        return 0xFF;
    }
    if (dfs_port && (port & ~1u) == dfs_port) return dfs_data_read();
    if (port == CONTROL_PORT) return selreg;
    if (port == DATA_PORT_LOW) {
        switch (selreg) {
        case CMD_DFSMAXLEN: return dfs_port ? (uint8_t)(DFS_MAX_PAYLOAD & 0xFF) : 0;
        case CMD_DFSPORT:   return (uint8_t)(dfs_port & 0xFF);
        default:            return emuctl_read_low(selreg);
        }
    }
    if (port == DATA_PORT_HIGH) {
        switch (selreg) {
        case CMD_DFSSTAT:   return dfs_ctl_status();
        case CMD_DFSINFO:   return dfs_ctl_info_read();
        case CMD_DFSMAXLEN: return dfs_port ? (uint8_t)(DFS_MAX_PAYLOAD >> 8) : 0;
        case CMD_DFSPORT:   return (uint8_t)(dfs_port >> 8);
        default:            return emuctl_read_high(selreg);
        }
    }
    return 0xFF;
}

void pgc_plat_outb(uint16_t port, uint8_t v) {
    port_ops++;
    if (dfs_port && (port & ~1u) == dfs_port) {
        dfs_data_write(v);
        return;
    }
    if (port == CONTROL_PORT) {
        if (v == 0xCC) {
            knocked = true;
        } else if (knocked) {
            selreg = v;
            if ((v >= CMD_DFSSTAT && v <= CMD_DFSPORT) || v == CMD_FLASH || v == CMD_DEFAULTS) pgdfs_selects++;
            switch (v) {
            case CMD_MAGIC: case CMD_PROTOCOL: case CMD_DFSSTAT: case CMD_DFSEXEC: case CMD_DFSMAXLEN:
                break;
            case CMD_DFSREQ:  dfs_ctl_select_req(); break;
            case CMD_DFSRESP: dfs_ctl_select_resp(); break;
            case CMD_DFSINFO: dfs_ctl_info_rewind(); break;
            case CMD_DFSTIME: dfs_ctl_time_rewind(); break;
            case CMD_DFSPORT: break;
            default:          if (!emuctl_select(v)) knocked = false; break;
            }
        }
        return;
    }
    if (port == DATA_PORT_LOW && knocked) {
        emuctl_write_low(selreg, v);
        return;
    }
    if (port == DATA_PORT_HIGH && knocked) {
        switch (selreg) {
        case CMD_DFSSTAT: dfs_ctl_abort(); break;
        case CMD_DFSEXEC: dfs_ctl_exec(); break;
        case CMD_DFSTIME: dfs_ctl_time_write(v); break;
        default:          emuctl_write_high(selreg, v); break;
        }
    }
}

/* the motherboard splits each word into base (low byte) and base+1 (high) */
void pgc_plat_insw(uint16_t port, void *dst, uint16_t words) {
    uint8_t *d = dst;
    while (words--) {
        *d++ = pgc_plat_inb(port);
        *d++ = pgc_plat_inb((uint16_t)(port + 1));
    }
}

void pgc_plat_outsw(uint16_t port, const void *src, uint16_t words) {
    const uint8_t *s = src;
    while (words--) {
        pgc_plat_outb(port, *s++);
        pgc_plat_outb((uint16_t)(port + 1), *s++);
    }
}

uint32_t pgc_plat_ms(void) { return clock_ms; }

void pgc_plat_poll(uint32_t waited_ms) {
    (void)waited_ms;
    clock_ms++;
    if (!hang) dfs_tasks();     /* core 1 */
}

/* ---- PGTRAY's way to the card: the driver's control request ------------------------- */

static pgc_card card;
static int ctl_requests;

int pgcard_plat_ctl(const pgctl_req *rq, const unsigned char *wr, unsigned char *rd, unsigned *got) {
    uint16_t n = 0;
    int r = pgc_ctl(&card, rq, wr, rd, &n);
    ctl_requests++;
    *got = n;
    return r == PGC_OK ? PGCARD_OK : PGCARD_EIO;
}

void pgcard_plat_sleep(unsigned ms) {
    clock_ms += ms;
    emuctl_task();              /* core 1 */
}

/* ---- the USB drive ---------------------------------------------------------------- */

static uint8_t work[4096];

static void format(const char *label) {
    MKFS_PARM opt = { FM_FAT32 | FM_SFD, 1, 0, 0, 512 };
    FRESULT fr;
    CHECK(ramdisk_init(64u * 1024 * 1024 / RAMDISK_SECTOR_SIZE), "ramdisk alloc");
    fr = f_mkfs("", &opt, work, sizeof(work));
    CHECK(fr == FR_OK, "f_mkfs -> %d", fr);
    fr = f_mount(&fatfs, "", 1);
    CHECK(fr == FR_OK, "f_mount -> %d", fr);
    fr = f_setlabel(label);
    CHECK(fr == FR_OK, "f_setlabel -> %d", fr);
    f_unmount("");
}

static void plug(void) {
    FRESULT fr = f_mount(&fatfs, "", 1);
    CHECK(fr == FR_OK, "f_mount -> %d", fr);
    dfs_on_drive_mounted();
}

static void unplug(void) {
    dfs_on_drive_unmounted();
    f_unmount("");
}

/* ---- helpers ---------------------------------------------------------------------- */

static void pattern(uint8_t *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + seed * 13 + (i >> 8));
}

static int bound_ids(void) {
    int n = 0;
    for (int i = 0; i < PGC_SLOTS; i++) if (card.slot[i]) n++;
    return n;
}

static int make_file(const char *path, uint32_t size, uint32_t seed) {
    static uint8_t data[20000];
    pgc_file f;
    uint32_t done = 0;
    int res = 0;
    int r = pgc_open(&card, &f, path, PGC_CREATE_ALWAYS, PGC_READWRITE, 0, &res, NULL, NULL, NULL, NULL);
    if (r != PGC_OK) return r;
    pattern(data, size, seed);
    r = pgc_write(&card, &f, 0, data, size, &done);
    if (r == PGC_OK && done != size) r = -1;
    if (pgc_close(&card, &f) != PGC_OK && r == PGC_OK) r = -2;
    return r;
}

static bool verify(pgc_file *f, uint32_t size, uint32_t seed) {
    static uint8_t want[20000], got[20000];
    uint32_t n = 0;
    int r;
    pattern(want, size, seed);
    memset(got, 0, sizeof(got));
    r = pgc_read(&card, f, 0, got, size + 100, &n);
    if (r != PGC_OK || n != size) {
        printf("    read r=%d n=%u (want %u)\n", r, n, size);
        return false;
    }
    return memcmp(want, got, size) == 0;
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_probe(void) {
    SECTION("probe");
    emuctl_reset();
    dfs_init();
    dfs_port = 0;
    CHECK(pgc_probe(&card) == PGC_ENOCARD, "PGDFS disabled: no card");
    dfs_port = DFS_DEFAULT_DATA_PORT;
    CHECK(pgc_probe(&card) == PGC_OK, "probe");
    CHECK(card.port == 0x1D4 && card.maxpl == DFS_MAX_PAYLOAD, "port %X maxpl %u", card.port, card.maxpl);
    CHECK(!card.present, "no drive yet");
    CHECK(pgc_media(&card, 1) == PGC_ENOTREADY, "media: not ready");
    {
        uint8_t attr;
        CHECK(pgc_getattr(&card, "\\X", &attr, NULL, NULL, NULL) == PGC_ENOTREADY, "getattr without drive");
    }
    format("PGTEST");
    plug();
    CHECK(pgc_media(&card, 1) == PGC_OK && card.present, "media present after plug");
    CHECK(card.mounts_known == 1 && card.mounts != 0, "the card counts its mounts: known %u, count %u",
          card.mounts_known, card.mounts);
    {
        pgc_volinfo v;
        CHECK(pgc_volinfo_get(&card, &v) == PGC_OK, "volinfo");
        CHECK(strcmp(v.label, "PGTEST") == 0 && strcmp(v.fs, "FAT32") == 0, "label '%s' fs '%s'", v.label, v.fs);
        CHECK(v.size_mb > 50 && v.size_mb < 70, "size %u MB", v.size_mb);
        CHECK(v.serial != 0, "serial %08X", v.serial);
    }
    pgc_set_time(&card, (12 << 11) | (34 << 5) | 28, ((2026 - 1980) << 9) | (11 << 5) | 11);
}

static void test_files(void) {
    static uint8_t data[20000], back[20000];
    pgc_file f;
    int res = 0;
    uint8_t attr = 0;
    uint32_t size = 0, n = 0;
    uint16_t t = 0, d = 0;
    SECTION("files");
    CHECK(pgc_open(&card, &f, "\\HELLO.TXT", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_ENOFILE,
          "open missing file");
    CHECK(pgc_open(&card, &f, "\\HELLO.TXT", PGC_CREATE_NEW, PGC_READWRITE, PGC_ATTR_ARCHIVE, &res, &attr, &size, &t, &d) == PGC_OK,
          "create new");
    CHECK(res == PGC_CREATED && size == 0, "created: res %d size %u", res, size);
    CHECK(d == (((2026 - 1980) << 9) | (11 << 5) | 11), "created with the pushed date %04X", d);
    pattern(data, 10000, 1);
    CHECK(pgc_write(&card, &f, 0, data, 10000, &n) == PGC_OK && n == 10000, "write 10000 (3 frames): %u", n);
    CHECK(pgc_get_size(&card, &f, &size) == PGC_OK && size == 10000, "size %u", size);
    memset(back, 0, sizeof(back));
    CHECK(pgc_read(&card, &f, 0, back, 20000, &n) == PGC_OK && n == 10000, "read to EOF: %u", n);
    CHECK(memcmp(data, back, 10000) == 0, "data");
    CHECK(pgc_read(&card, &f, 9990, back, 100, &n) == PGC_OK && n == 10 && memcmp(back, data + 9990, 10) == 0, "tail read %u", n);
    CHECK(pgc_read(&card, &f, 20000, back, 100, &n) == PGC_OK && n == 0, "read past EOF: %u", n);
    CHECK(pgc_set_size(&card, &f, 5000) == PGC_OK, "truncate");
    CHECK(pgc_get_size(&card, &f, &size) == PGC_OK && size == 5000, "size after truncate %u", size);
    CHECK(pgc_set_size(&card, &f, 6000) == PGC_OK, "extend");
    CHECK(pgc_read(&card, &f, 0, back, 20000, &n) == PGC_OK && n == 6000, "size after extend %u", n);
    CHECK(pgc_set_ftime(&card, &f, (1 << 11), ((2000 - 1980) << 9) | (1 << 5) | 1) == PGC_OK, "set time");
    CHECK(pgc_close(&card, &f) == PGC_OK, "close");
    CHECK(bound_ids() == 0, "no ids bound after close: %d", bound_ids());
    CHECK(pgc_getattr(&card, "\\HELLO.TXT", &attr, &size, &t, &d) == PGC_OK, "getattr");
    CHECK(size == 6000 && d == (((2000 - 1980) << 9) | (1 << 5) | 1) && t == (1 << 11), "attr: size %u date %04X time %04X", size, d, t);
    CHECK(pgc_open(&card, &f, "\\HELLO.TXT", PGC_CREATE_NEW, PGC_READWRITE, 0, &res, NULL, NULL, NULL, NULL) == PGC_EEXISTS,
          "create new on existing -> 50h");
    CHECK(pgc_open(&card, &f, "\\HELLO.TXT", PGC_OPEN_ALWAYS, PGC_READ, 0, &res, NULL, &size, NULL, NULL) == PGC_OK &&
          res == PGC_OPENED && size == 6000, "open always, existing: res %d size %u", res, size);
    CHECK(pgc_write(&card, &f, 0, data, 10, &n) == PGC_EACCESS, "write on a read handle");
    CHECK(pgc_close(&card, &f) == PGC_OK, "close");
    CHECK(pgc_open(&card, &f, "\\HELLO.TXT", PGC_TRUNCATE_EXISTING, PGC_READWRITE, 0, &res, NULL, &size, NULL, NULL) == PGC_OK &&
          res == PGC_TRUNCATED && size == 0, "truncate existing: res %d size %u", res, size);
    CHECK(pgc_close(&card, &f) == PGC_OK, "close");

    /* attach: open without the card, bind on first use, upgrade to write */
    CHECK(make_file("\\ATTACH.BIN", 3000, 5) == PGC_OK, "make attach.bin");
    CHECK(pgc_file_attach(&card, &f, "\\ATTACH.BIN", PGC_READ) == PGC_OK && f.id == 0, "attach: no id yet");
    CHECK(verify(&f, 3000, 5), "attached file reads");
    CHECK(f.id != 0 && f.mode == PGC_READ, "bound read-only");
    pgc_file_need(&f, PGC_READWRITE);
    pattern(data, 3000, 6);
    CHECK(pgc_write(&card, &f, 0, data, 3000, &n) == PGC_OK && n == 3000, "write after upgrade");
    CHECK(f.mode == PGC_READWRITE, "rebound read/write");
    CHECK(verify(&f, 3000, 6), "rewritten");
    CHECK(bound_ids() == 1, "one id: %d", bound_ids());
    CHECK(pgc_close(&card, &f) == PGC_OK, "close");

    /* attributes */
    CHECK(pgc_setattr(&card, "\\ATTACH.BIN", PGC_ATTR_RDONLY) == PGC_OK, "set read-only");
    CHECK(pgc_open(&card, &f, "\\ATTACH.BIN", PGC_OPEN_EXISTING, PGC_READWRITE, 0, &res, NULL, NULL, NULL, NULL) == PGC_EACCESS,
          "open read-only file for writing");
    CHECK(pgc_delete(&card, "\\ATTACH.BIN") == PGC_EACCESS, "delete read-only");
    CHECK(pgc_setattr(&card, "\\ATTACH.BIN", PGC_ATTR_ARCHIVE) == PGC_OK, "clear read-only");
    CHECK(pgc_delete(&card, "\\ATTACH.BIN") == PGC_OK, "delete");
    CHECK(pgc_getattr(&card, "\\ATTACH.BIN", &attr, NULL, NULL, NULL) == PGC_ENOFILE, "gone");
    CHECK(pgc_delete(&card, "\\*.TXT") == PGC_ENOFILE, "no wildcard deletes");
    CHECK(pgc_getattr(&card, "\\HELLO.TXT", &attr, NULL, NULL, NULL) == PGC_OK, "hello.txt survived");

    /* root */
    CHECK(pgc_getattr(&card, "", &attr, NULL, NULL, NULL) == PGC_OK && (attr & PGC_ATTR_DIR), "root is a directory");
    CHECK(pgc_open(&card, &f, "\\", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_EACCESS, "open root");
    {
        char longp[200];
        memset(longp, 'A', sizeof(longp));
        longp[0] = '\\';
        longp[150] = 0;
        CHECK(pgc_getattr(&card, longp, &attr, NULL, NULL, NULL) == PGC_ETOOLONG, "path too long");
    }
}

static void test_names(void) {
    pgc_find s;
    char name[PGC_NAME_MAX];
    int r, n = 0, seen_long = 0, seen_dot = 0, seen_dotdot = 0, seen_file = 0;
    uint8_t attr;
    SECTION("long names and directories");
    CHECK(pgc_mkdir(&card, "\\Program Files") == PGC_OK, "mkdir long");
    CHECK(pgc_mkdir(&card, "\\Program Files") == PGC_EACCESS, "mkdir again -> 5");
    CHECK(make_file("\\Program Files\\Read Me First.txt", 100, 9) == PGC_OK, "create long name in long dir");
    CHECK(make_file("\\Program Files\\short.txt", 10, 1) == PGC_OK, "create short lowercase name");
    CHECK(pgc_getattr(&card, "\\PROGRA~1\\README~1.TXT", &attr, NULL, NULL, NULL) == PGC_OK, "the 8.3 alias works too");
    CHECK(pgc_getattr(&card, "\\program files\\read me first.TXT", &attr, NULL, NULL, NULL) == PGC_OK, "case-insensitive");
    CHECK(pgc_longname(&card, "\\PROGRA~1", name, sizeof(name)) == PGC_OK && strcmp(name, "Program Files") == 0,
          "longname of the dir: '%s'", name);
    {
        char canon[PGC_PATH_MAX];
        CHECK(pgc_canonical(&card, "\\PROGRA~1\\README~1.TXT", canon) == PGC_OK &&
              strcmp(canon, "\\Program Files\\Read Me First.txt") == 0, "canonical alias path: '%s'", canon);
        CHECK(pgc_canonical(&card, "\\Program Files\\short.txt", canon) == PGC_OK &&
              strcmp(canon, "\\Program Files\\short.txt") == 0, "canonical long path unchanged: '%s'", canon);
        CHECK(pgc_canonical(&card, "\\PROGRA~1\\NEW~1.TXT", canon) == PGC_OK &&
              strcmp(canon, "\\Program Files\\NEW~1.TXT") == 0, "a new name with a tilde stays: '%s'", canon);
        CHECK(pgc_canonical(&card, "\\", canon) == PGC_OK && strcmp(canon, "\\") == 0, "root: '%s'", canon);
        CHECK(pgc_canonical(&card, "", canon) == PGC_OK && strcmp(canon, "") == 0, "empty root: '%s'", canon);
        strcpy(canon, "\\PROGRA~1");
        CHECK(pgc_canonical(&card, canon, canon) == PGC_OK && strcmp(canon, "\\Program Files") == 0, "in place: '%s'", canon);
    }
    for (r = pgc_find_first(&card, &s, "\\Program Files", 0x16, 1); r == PGC_OK; r = pgc_find_next(&card, &s)) {
        n++;
        if (strcmp(s.shortname, ".") == 0) seen_dot++;
        if (strcmp(s.shortname, "..") == 0) seen_dotdot++;
        if (strcmp(s.longname, "Read Me First.txt") == 0 && strcmp(s.shortname, "README~1.TXT") == 0 && s.size == 100) seen_long++;
        if (strcmp(s.longname, "short.txt") == 0 && strcmp(s.shortname, "SHORT.TXT") == 0) seen_file++;
    }
    CHECK(r == PGC_ENOMORE, "listing ends with 12h: %d", r);
    CHECK(n == 4 && seen_dot == 1 && seen_dotdot == 1 && seen_long == 1 && seen_file == 1,
          "listing: %d entries, . %d .. %d long %d short %d", n, seen_dot, seen_dotdot, seen_long, seen_file);
    n = 0;
    for (r = pgc_find_first(&card, &s, "", 0x16, 1); r == PGC_OK; r = pgc_find_next(&card, &s)) {
        n++;
        if (strcmp(s.longname, "Program Files") == 0 && (s.attr & PGC_ATTR_DIR)) seen_long++;
    }
    CHECK(r == PGC_ENOMORE && seen_long == 2, "root listing has the long dir: %d entries", n);
    n = 0;
    for (r = pgc_find_mask(&card, &s, "\\Program Files", "*.TXT", 0x16, 0); r == PGC_OK; r = pgc_find_next(&card, &s)) {
        n++;
        CHECK(strstr(s.shortname, ".TXT") != NULL, "DOS pattern match: %s", s.shortname);
    }
    CHECK(r == PGC_ENOMORE && n == 2, "*.TXT finds the two text files: %d", n);
    n = 0;
    for (r = pgc_find_mask(&card, &s, "\\Program Files", "SHORT.TXT", 0x06, 0); r == PGC_OK; r = pgc_find_next(&card, &s)) n++;
    CHECK(r == PGC_ENOMORE && n == 1, "exact 8.3 name: %d", n);
    CHECK(pgc_find_mask(&card, &s, "\\Program Files", "*.BAK", 0x16, 0) == PGC_ENOMORE, "no match -> 12h");
    CHECK(pgc_find_mask(&card, &s, "", "????????.???", 0x08, 0) == PGC_OK && strcmp(s.shortname, "PGTEST") == 0,
          "volume label search: '%s'", s.shortname);
    r = pgc_find_first(&card, &s, "\\NOPE", 0x16, 1);
    CHECK(r == PGC_ENOPATH, "listing a missing dir -> 3: %d", r);
    CHECK(pgc_mkdir(&card, "\\Empty") == PGC_OK, "mkdir empty");
    n = 0;
    for (r = pgc_find_first(&card, &s, "\\Empty", 0x16, 1); r == PGC_OK; r = pgc_find_next(&card, &s)) n++;
    CHECK(r == PGC_ENOMORE && n == 2, "empty dir has . and ..: %d", n);
    CHECK(pgc_rename(&card, "\\Program Files\\short.txt", "\\Empty\\Moved Here.txt") == PGC_OK, "rename across dirs");
    CHECK(pgc_getattr(&card, "\\Empty\\Moved Here.txt", &attr, NULL, NULL, NULL) == PGC_OK, "moved");
    CHECK(pgc_rename(&card, "\\HELLO.TXT", "\\Empty\\Moved Here.txt") == PGC_EACCESS, "rename onto existing -> 5");
    CHECK(pgc_rmdir(&card, "\\Empty") == PGC_EACCESS, "rmdir non-empty -> 5");
    CHECK(pgc_delete(&card, "\\Empty\\Moved Here.txt") == PGC_OK, "delete moved");
    CHECK(pgc_rmdir(&card, "\\Empty") == PGC_OK, "rmdir");
    CHECK(pgc_getattr(&card, "\\Empty", &attr, NULL, NULL, NULL) == PGC_ENOFILE, "rmdir'ed");
}

#define NFILES 20

static void test_many(void) {
    static pgc_file files[NFILES];
    char path[32];
    int i, ok = 1, max_bound = 0;
    SECTION("more files than card ids");
    for (i = 0; i < NFILES; i++) {
        sprintf(path, "\\MANY\\F%02d.BIN", i);
        if (i == 0) CHECK(pgc_mkdir(&card, "\\MANY") == PGC_OK, "mkdir");
        CHECK(make_file(path, 1000 + i * 37, 100 + i) == PGC_OK, "make %s", path);
    }
    for (i = 0; i < NFILES; i++) {
        int res;
        sprintf(path, "\\MANY\\F%02d.BIN", i);
        CHECK(pgc_open(&card, &files[i], path, PGC_OPEN_EXISTING, PGC_READWRITE, 0, &res, NULL, NULL, NULL, NULL) == PGC_OK,
              "open %s", path);
        if (bound_ids() > max_bound) max_bound = bound_ids();
    }
    CHECK(max_bound <= PGC_CARD_FILES, "never more than %d ids: %d", PGC_CARD_FILES, max_bound);
    for (int round = 0; round < 3; round++) {
        for (i = NFILES - 1; i >= 0; i--) {
            if (!verify(&files[i], 1000 + i * 37, 100 + i)) ok = 0;
        }
    }
    CHECK(ok, "every file reads back through the shared ids");
    {
        /* writes interleaved across all of them */
        uint8_t b = 0x5A;
        uint32_t n;
        for (i = 0; i < NFILES; i++) CHECK(pgc_write(&card, &files[i], 10, &b, 1, &n) == PGC_OK && n == 1, "poke %d", i);
        for (i = 0; i < NFILES; i++) {
            uint8_t x = 0;
            CHECK(pgc_read(&card, &files[i], 10, &x, 1, &n) == PGC_OK && n == 1 && x == 0x5A, "peek %d: %02X", i, x);
        }
    }
    for (i = 0; i < NFILES; i++) CHECK(pgc_close(&card, &files[i]) == PGC_OK, "close %d", i);
    CHECK(bound_ids() == 0, "all ids back: %d", bound_ids());

    /* someone else (a DOS program before Windows) left ids open on the card */
    {
        pgc_file f;
        int res;
        uint8_t req[64];
        for (i = 0; i < 8; i++) {
            /* OPEN \HELLO.TXT behind the core's back, through the core's own transport */
            const char *p = "\\HELLO.TXT";
            uint16_t len = (uint16_t)(4 + 6 + strlen(p));
            memset(req, 0, sizeof(req));
            req[0] = (uint8_t)len; req[1] = 0; req[2] = 0; req[3] = 0x16;
            memcpy(req + 10, p, strlen(p));
            memcpy(card.buf, req, len);
            {
                /* run it the way pgc does: through the card registers */
                dfs_ctl_select_req();
                for (uint16_t k = 0; k < len; k++) dfs_data_write(req[k]);
                dfs_ctl_exec();
                dfs_tasks();
                dfs_ctl_abort();    /* nobody reads the answer */
            }
        }
        CHECK(pgc_open(&card, &f, "\\HELLO.TXT", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_OK,
              "open with the card's table full of foreign ids");
        CHECK(pgc_close(&card, &f) == PGC_OK, "close");
    }
}

static void test_removal(void) {
    static pgc_file a, b, c2, d;
    int res;
    uint8_t attr;
    uint32_t n, gen;
    SECTION("removable medium");
    CHECK(make_file("\\A.BIN", 5000, 1) == PGC_OK && make_file("\\B.BIN", 6000, 2) == PGC_OK &&
          make_file("\\C.BIN", 7000, 3) == PGC_OK, "make a b c");
    CHECK(pgc_open(&card, &a, "\\A.BIN", PGC_OPEN_EXISTING, PGC_READWRITE, 0, &res, NULL, NULL, NULL, NULL) == PGC_OK, "open a");
    CHECK(pgc_open(&card, &b, "\\B.BIN", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_OK, "open b");
    CHECK(verify(&a, 5000, 1) && verify(&b, 6000, 2), "a b read");
    gen = card.media_gen;

    /* 1. unplugged while open: not ready, then the same stick comes back */
    unplug();
    CHECK(pgc_read(&card, &a, 0, &attr, 1, &n) == PGC_ENOTREADY, "read without the stick -> not ready");
    CHECK(!card.present, "absent");
    CHECK(pgc_getattr(&card, "\\A.BIN", &attr, NULL, NULL, NULL) == PGC_ENOTREADY, "getattr -> not ready");
    CHECK(pgc_open(&card, &c2, "\\C.BIN", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_ENOTREADY,
          "open -> not ready");
    plug();
    CHECK(verify(&a, 5000, 1) && verify(&b, 6000, 2), "same stick back: open files carry on");
    CHECK(card.present && card.media_gen == gen, "same medium");

    /* 1b. the same stick replugged between two looks: the card's mount count
     * tells, the ids are dropped before any is used, the files carry on */
    {
        uint32_t mg = card.mount_gen, cc = card.change_count;
        uint8_t count = card.mounts;
        CHECK(a.id != 0 && b.id != 0, "a and b hold card ids");
        unplug();
        plug();
        CHECK(pgc_media(&card, 1) == PGC_OK, "present");
        CHECK(card.mounts != count && card.mounts != 0, "mount count moved: %u -> %u", count, card.mounts);
        CHECK(card.mount_gen != mg && card.change_count != cc, "the replug was noticed: mount_gen %u -> %u, changes %u -> %u",
              mg, card.mount_gen, cc, card.change_count);
        CHECK(card.media_gen == gen, "the same medium still");
        CHECK(a.id == 0 && b.id == 0, "the stale ids are gone: %u %u", a.id, b.id);
        CHECK(verify(&a, 5000, 1) && verify(&b, 6000, 2), "open files carry on");
        mg = card.mount_gen;
        cc = card.change_count;
        CHECK(pgc_media(&card, 1) == PGC_OK && card.mount_gen == mg && card.change_count == cc,
              "another look without a replug changes nothing");
    }

    /* 2. replugged between two requests, unnoticed: a stale id answers 6 */
    unplug();
    plug();
    CHECK(verify(&a, 5000, 1), "unnoticed replug: a re-opened by path");
    CHECK(verify(&b, 6000, 2), "unnoticed replug: b re-opened by path");

    /* 3. unnoticed replug, then a new open gets an id the table still holds */
    unplug();
    plug();
    CHECK(pgc_open(&card, &c2, "\\C.BIN", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_OK, "open c");
    CHECK(verify(&c2, 7000, 3), "c reads");
    CHECK(verify(&a, 5000, 1) && verify(&b, 6000, 2), "a and b survive the duplicate id");
    {
        uint8_t x = 0x77;
        CHECK(pgc_write(&card, &a, 0, &x, 1, &n) == PGC_OK, "write a");
        CHECK(verify(&c2, 7000, 3), "the write went to a, not to c");
        x = 0;
        CHECK(pgc_read(&card, &a, 0, &x, 1, &n) == PGC_OK && x == 0x77, "a has it");
    }

    /* 4. a different stick: old files are stale, new opens work */
    unplug();
    CHECK(pgc_media(&card, 1) == PGC_ENOTREADY, "absent");
    format("OTHER");
    plug();
    CHECK(pgc_media(&card, 1) == PGC_OK && card.media_gen != gen, "different medium noticed");
    CHECK(pgc_read(&card, &a, 0, &attr, 1, &n) == PGC_ESTALE, "old file on the new stick -> stale");
    CHECK(pgc_write(&card, &b, 0, &attr, 1, &n) == PGC_EACCESS || pgc_read(&card, &b, 0, &attr, 1, &n) == PGC_ESTALE, "b stale");
    CHECK(pgc_close(&card, &a) == PGC_OK && pgc_close(&card, &b) == PGC_OK && pgc_close(&card, &c2) == PGC_OK,
          "closing stale files is fine");
    CHECK(make_file("\\A.BIN", 100, 42) == PGC_OK, "a file of the same name on the new stick");
    CHECK(pgc_open(&card, &d, "\\A.BIN", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_OK &&
          verify(&d, 100, 42), "new file reads");

    /* 5. swapped between two looks without a NODRIVE in between */
    gen = card.media_gen;
    unplug();
    format("THIRD");
    plug();
    CHECK(pgc_getattr(&card, "\\A.BIN", &attr, NULL, NULL, NULL) == PGC_ENOFILE || card.media_gen != gen,
          "swap is seen at the latest by the next medium check");
    clock_ms += PGC_RECHECK_MS;
    CHECK(pgc_read(&card, &d, 0, &attr, 1, &n) == PGC_ESTALE, "file of the previous stick -> stale: media %u/%u", card.media_gen, gen);
    CHECK(pgc_close(&card, &d) == PGC_OK, "close");
    CHECK(bound_ids() == 0, "no ids: %d", bound_ids());
}

static void test_timeout(void) {
    uint8_t attr;
    uint32_t t0;
    SECTION("hung card");
    hang = true;
    t0 = clock_ms;
    CHECK(pgc_getattr(&card, "\\", &attr, NULL, NULL, NULL) == PGC_ENOTREADY, "no answer -> not ready");
    CHECK(clock_ms - t0 >= PGC_TIMEOUT_MS, "after the timeout: %u ms", clock_ms - t0);
    CHECK(card.timeouts == 1, "counted");
    hang = false;
    /* the aborted request finishes, the card reports ABORTED once, then works */
    CHECK(pgc_getattr(&card, "\\", &attr, NULL, NULL, NULL) == PGC_OK, "card back");
}

static void test_space(void) {
    uint32_t clusters = 0, freec = 0, bytes = 0;
    DWORD nfree;
    FATFS *fs;
    SECTION("space");
    CHECK(pgc_space(&card, &clusters, &freec, &bytes) == PGC_OK, "space");
    CHECK(f_getfree("", &nfree, &fs) == FR_OK, "f_getfree");
    CHECK(clusters == fs->n_fatent - 2 && freec == nfree && bytes == fs->csize * 512u,
          "exact: %u/%u clusters of %u (FatFs %u/%u)", freec, clusters, bytes, (unsigned)nfree, (unsigned)(fs->n_fatent - 2));
}

/* ---- control requests ---------------------------------------------------------------- */

static int ctl(uint8_t reg, uint8_t flags, uint8_t stop, const void *wr, uint16_t nwrite, uint32_t skip,
               void *rd, uint16_t nread, uint16_t *got) {
    pgctl_req rq = { PGCTL_VERSION, reg, flags, stop, nwrite, nread, skip };
    return pgc_ctl(&card, &rq, wr, rd, got);
}

/* the card restarts: PGDFS starts afresh without a drive, the bus is dead for a while */
static void card_reboot(void) {
    dfs_on_drive_unmounted();
    f_unmount("");
    dfs_init();
    dead_reads = 40;
}

static void card_dies(void) {
    card_reboot();
    dead_reads = 1000000;
}

static void test_ctl(void) {
    static pgc_file a;
    uint8_t buf[PGCTL_MAX_DATA];
    uint16_t got = 77;
    uint32_t ops, sels, t0, n;
    int res;
    const char *fw = emuctl.fw;
    SECTION("control requests");
    CHECK(sizeof(pgctl_req) == 12, "request header is 12 bytes: %u", (unsigned)sizeof(pgctl_req));

    /* malformed and refused requests never reach the bus */
    ops = port_ops;
    {
        pgctl_req rq = { 0, CMD_FWSTRING, 0, 0, 0, 1, 0 };
        CHECK(pgc_ctl(&card, &rq, buf, buf, &got) == PGC_EFUNC && got == 0, "version 0 refused");
    }
    CHECK(ctl(CMD_FWSTRING, 0, 0, NULL, 0, 0, buf, PGCTL_MAX_DATA + 1, &got) == PGC_EFUNC, "too long a read");
    CHECK(ctl(CMD_FWSTRING, 0, 0, buf, PGCTL_MAX_DATA + 1, 0, buf, 0, &got) == PGC_EFUNC, "too long a write");
    CHECK(ctl(CMD_FWSTRING, 0x80, 0, NULL, 0, 0, buf, 1, &got) == PGC_EFUNC, "unknown flag");
    CHECK(ctl(CMD_FWSTRING, 0, 0, NULL, 0, PGCTL_MAX_SKIP + 1, buf, 1, &got) == PGC_EFUNC, "too long a skip");
    for (int reg = CMD_DFSSTAT; reg <= 0xFF; reg++) {
        bool ok = reg == CMD_SAVE || reg == CMD_REBOOT || reg == CMD_HWTYPE || reg == CMD_CHIPTYPE;
        if (ok) continue;
        res = ctl((uint8_t)reg, 0, 0, buf, 1, 0, buf, 1, &got);
        if (res != PGC_EACCESS) CHECK(0, "register %02X -> %d, not refused", reg, res);
    }
    CHECK(ctl(CMD_FLASH, 0, 0, NULL, 0, 0, buf, 1, &got) == PGC_EACCESS, "firmware programming refused");
    CHECK(ctl(CMD_DFSPORT, PGCTL_WORD, 0, buf, 2, 0, NULL, 0, &got) == PGC_EACCESS, "PGDFS port refused");
    CHECK(ctl(CMD_DEFAULTS, 0, 0, buf, 1, 0, NULL, 0, &got) == PGC_EACCESS, "defaults refused");
    CHECK(port_ops == ops, "nothing on the bus: %u port operations", port_ops - ops);

    /* a string register, to its NUL */
    memset(buf, 0xAA, sizeof(buf));
    CHECK(ctl(CMD_FWSTRING, PGCTL_STOP, 0, NULL, 0, 0, buf, 200, &got) == PGC_OK, "firmware string");
    CHECK(got == strlen(fw) + 1 && strcmp((char *)buf, fw) == 0 && buf[got] == 0xAA, "read to the NUL: %u '%s'", got, buf);
    CHECK(ctl(CMD_FWSTRING, 0, 0, NULL, 0, 0, buf, 4, &got) == PGC_OK && got == 4 && memcmp(buf, fw, 4) == 0, "fixed count");
    CHECK(ctl(CMD_FWSTRING, PGCTL_STOP, 0, NULL, 0, 3, buf, 200, &got) == PGC_OK && strcmp((char *)buf, fw + 3) == 0,
          "skip 3: '%s'", buf);
    CHECK(ctl(CMD_FWSTRING, PGCTL_STOP, 0, NULL, 0, 150, buf, 200, &got) == PGC_OK && got == 0, "the end is among the skipped: %u", got);
    CHECK(ctl(CMD_MAGIC, 0, 0, NULL, 0, 0, buf, 1, &got) == PGC_OK && got == 1 && buf[0] == 0xDD, "magic");

    /* 16-bit registers */
    CHECK(ctl(CMD_CDPORT, PGCTL_WORD, 0, NULL, 0, 0, buf, 2, &got) == PGC_OK && got == 2 && buf[0] == 0x50 && buf[1] == 0x02,
          "CD port read: %02X %02X", buf[0], buf[1]);
    buf[0] = 0x30;
    buf[1] = 0x02;
    CHECK(ctl(CMD_CDPORT, PGCTL_WORD, 0, buf, 2, 0, NULL, 0, &got) == PGC_OK && emuctl.cd_port == 0x230, "CD port written: %X",
          emuctl.cd_port);
    buf[0] = 0x50;
    CHECK(ctl(CMD_CDPORT, PGCTL_WORD, 0, buf, 2, 0, buf, 2, &got) == PGC_OK && emuctl.cd_port == 0x250 && buf[0] == 0x50 &&
          buf[1] == 0x02, "written and read back in one request (same buffer)");

    /* written strings */
    CHECK(ctl(CMD_WIFISSID, 0, 0, "abc", 4, 0, NULL, 0, &got) == PGC_OK && strcmp(emuctl.ssid, "abc") == 0, "string written: '%s'",
          emuctl.ssid);

    /* saving: the card is left alone while it writes its flash */
    t0 = clock_ms;
    buf[0] = 0xFF;
    CHECK(ctl(CMD_SAVE, 0, 0, buf, 1, 0, NULL, 0, &got) == PGC_OK && emuctl.saves == 1, "save");
    CHECK(clock_ms - t0 >= 150, "waited %u ms", clock_ms - t0);

    /* between the requests of an open file */
    CHECK(make_file("\\CTL.BIN", 9000, 5) == PGC_OK, "make");
    CHECK(pgc_open(&card, &a, "\\CTL.BIN", PGC_OPEN_EXISTING, PGC_READ, 0, &res, NULL, NULL, NULL, NULL) == PGC_OK, "open");
    CHECK(pgc_read(&card, &a, 0, buf, 100, &n) == PGC_OK && n == 100, "read");
    sels = pgdfs_selects;
    CHECK(ctl(CMD_FWSTRING, PGCTL_STOP, 0, NULL, 0, 0, buf, 200, &got) == PGC_OK, "request while a file is open");
    CHECK(pgdfs_selects == sels, "no PGDFS register touched");
    CHECK(verify(&a, 9000, 5), "the file reads on");

    /* restart: the drive is gone until the card has mounted it again, the
     * open file then carries on */
    {
        uint32_t cc = card.change_count, mg = card.mount_gen;
        emuctl.on_reboot = card_reboot;
        emuctl.boot_request = SB_MODE;
        buf[0] = 0xFF;
        CHECK(a.id != 0, "the file holds a card id");
        CHECK(ctl(CMD_REBOOT, 0, 0, buf, 1, 0, NULL, 0, &got) == PGC_OK && emuctl.reboots == 1, "reboot");
        CHECK(!card.present && card.change_count == cc + 1 && card.mount_gen != mg && a.id == 0 && card.mounts == 0,
              "the drive counts as unplugged, the ids are gone");
        CHECK(pgc_read(&card, &a, 0, buf, 10, &n) == PGC_ENOTREADY, "not ready while the card restarts");
        dead_reads = 0;
        CHECK(pgc_read(&card, &a, 0, buf, 10, &n) == PGC_ENOTREADY, "not ready before the drive is mounted");
        clock_ms += 2500;
        plug();
        CHECK(verify(&a, 9000, 5), "mounted again: the open file carries on");
        CHECK(card.present && card.change_count == cc + 2, "arrival counted");
    }
    CHECK(pgc_close(&card, &a) == PGC_OK, "close");
    CHECK(pgc_delete(&card, "\\CTL.BIN") == PGC_OK, "delete");
}

/* ---- PGTRAY's card part ------------------------------------------------------------- */

static void test_tray(void) {
    static char names[200][48];
    static char list[16384];
    char err[PGCARD_TEXT], name[PGCARD_TEXT], url[64];
    pgcard_info info;
    unsigned count = 99;
    unsigned char idx = 99, now = 0;
    uint16_t got;
    uint8_t st;
    int r, saves;
    const char *p;
    SECTION("PGTRAY: the card");

    CHECK(pgcard_get_info(&info) == PGCARD_OK, "info");
    CHECK(strcmp(info.fw, emuctl.fw) == 0 && info.protocol == PICOGUS_PROTOCOL_VER && info.mode == SB_MODE && info.has_cd &&
          info.cd_port == 0x250, "fw '%s' protocol %u mode %u cd %u port %X", info.fw, info.protocol, info.mode, info.has_cd,
          info.cd_port);
    CHECK(strcmp(pgcard_mode_name(SB_MODE), "Sound Blaster") == 0 && strcmp(pgcard_mode_name(0), "?") == 0, "mode names");

    /* CD images */
    emuctl.image[0] = "Doom II.cue";
    emuctl.image[1] = "QUAKE.ISO";
    emuctl.image[2] = "Tomb Raider (Disc 1).cue";
    emuctl.images = 3;
    emuctl.busy_polls = 3;
    CHECK(pgcard_cd_current(&idx, name, sizeof(name)) == PGCARD_OK && idx == 0 && name[0] == 0, "nothing loaded");
    CHECK(pgcard_cd_list(list, sizeof(list), &count, err, sizeof(err)) == PGCARD_OK && count == 3, "list: %u", count);
    p = list;
    CHECK(strcmp(p, "Doom II.cue") == 0, "1: '%s'", p);
    p += strlen(p) + 1;
    CHECK(strcmp(p, "QUAKE.ISO") == 0, "2: '%s'", p);
    p += strlen(p) + 1;
    CHECK(strcmp(p, "Tomb Raider (Disc 1).cue") == 0, "3: '%s'", p);
    CHECK(ctl(CMD_CDSTATUS, 0, 0, NULL, 0, 0, &st, 1, &got) == PGC_OK && st == CD_STATUS_IDLE, "the card let go of its list: %u", st);
    CHECK(emuctl.list_overruns == 0, "the list was never read past its end");

    CHECK(pgcard_cd_load(2, err, sizeof(err)) == PGCARD_OK, "load 2: '%s'", err);
    CHECK(pgcard_cd_current(&idx, name, sizeof(name)) == PGCARD_OK && idx == 2 && strcmp(name, "QUAKE.ISO") == 0, "loaded: %u '%s'",
          idx, name);
    emuctl.bad_image = "Doom II.cue";
    r = pgcard_cd_load(1, err, sizeof(err));
    CHECK(r == PGCARD_ECARD && strcmp(err, "Error loading iso 'Doom II.cue'") == 0, "bad image: %d '%s'", r, err);
    CHECK(pgcard_cd_current(&idx, name, sizeof(name)) == PGCARD_OK && idx == 0, "nothing loaded after the failure");
    emuctl.bad_image = NULL;
    CHECK(pgcard_cd_load(3, err, sizeof(err)) == PGCARD_OK && pgcard_cd_current(&idx, name, sizeof(name)) == PGCARD_OK && idx == 3,
          "load 3");
    CHECK(pgcard_cd_load(0, err, sizeof(err)) == PGCARD_OK && pgcard_cd_current(&idx, name, sizeof(name)) == PGCARD_OK && idx == 0 &&
          name[0] == 0, "unload");

    /* a command that takes longer than the patience */
    emuctl.busy_polls = 100000;
    r = pgcard_cd_load(1, err, sizeof(err));
    CHECK(r == PGCARD_ETIMEOUT, "a card that stays busy: %d", r);
    emuctl.busy_polls = 0;
    emuctl_task();
    emuctl_task();

    /* the card's own errors */
    emuctl.drive = false;
    r = pgcard_cd_list(list, sizeof(list), &count, err, sizeof(err));
    CHECK(r == PGCARD_ECARD && count == 0 && strcmp(err, "No USB disk or error mounting it") == 0, "no drive: %d '%s'", r, err);
    emuctl.drive = true;
    emuctl.images = 0;
    r = pgcard_cd_list(list, sizeof(list), &count, err, sizeof(err));
    CHECK(r == PGCARD_ECARD && strcmp(err, "No image files on USB disk") == 0, "no images: %d '%s'", r, err);

    /* a list longer than one request, and one longer than the buffer */
    for (int i = 0; i < 200; i++) {
        snprintf(names[i], sizeof(names[i]), "Image number %03d of the collection.iso", i + 1);
        emuctl.image[i] = names[i];
    }
    emuctl.images = 200;
    emuctl.busy_polls = 1;
    r = pgcard_cd_list(list, sizeof(list), &count, err, sizeof(err));
    CHECK(r == PGCARD_OK && count == 200, "200 images: %d, %u", r, count);
    p = list;
    for (int i = 0; i < 200 && count == 200; i++) {
        if (strcmp(p, names[i]) != 0) {
            CHECK(0, "name %d: '%s'", i + 1, p);
            break;
        }
        p += strlen(p) + 1;
    }
    r = pgcard_cd_list(list, 100, &count, err, sizeof(err));
    CHECK(r == PGCARD_OK && count == 2 && strcmp(list, names[0]) == 0 && strcmp(list + strlen(list) + 1, names[1]) == 0,
          "a small buffer takes the names that fit: %d, %u", r, count);
    CHECK(ctl(CMD_CDSTATUS, 0, 0, NULL, 0, 0, &st, 1, &got) == PGC_OK && st == CD_STATUS_IDLE, "and the list is read to its end");
    CHECK(emuctl.list_overruns == 0, "the list was never read past its end: %d", emuctl.list_overruns);
    emuctl.images = 3;
    emuctl.image[0] = "Doom II.cue";
    emuctl.image[1] = "QUAKE.ISO";
    emuctl.image[2] = "Tomb Raider (Disc 1).cue";

    /* the mode: saved, restarted, waited for */
    saves = emuctl.saves;
    emuctl.on_reboot = card_reboot;
    CHECK(pgcard_set_mode(0, 0, &now) == PGCARD_EINVAL && pgcard_set_mode(NE2000_MODE, 0, &now) == PGCARD_EINVAL, "modes that are none");
    r = pgcard_set_mode(GUS_MODE, 1, &now);
    CHECK(r == PGCARD_OK && now == GUS_MODE && emuctl.mode == GUS_MODE && emuctl.saves == saves + 1 && emuctl.reboots == 2,
          "GUS mode, saved: %d now %u", r, now);
    CHECK(pgcard_get_info(&info) == PGCARD_OK && info.mode == GUS_MODE && !info.has_cd, "no CD-ROM in GUS mode");
    CHECK(pgcard_cd_list(list, sizeof(list), &count, err, sizeof(err)) == PGCARD_ENOTSUP, "list: not in this mode");
    CHECK(pgcard_cd_current(&idx, name, sizeof(name)) == PGCARD_ENOTSUP && idx == 0, "current: not in this mode");
    CHECK(pgcard_cd_load(1, err, sizeof(err)) == PGCARD_ENOTSUP, "load: not in this mode");
    r = pgcard_set_mode(SB_MODE, 0, &now);
    CHECK(r == PGCARD_OK && now == SB_MODE && emuctl.saves == saves + 1 && emuctl.reboots == 3, "back to SB, not saved: %d", r);
    clock_ms += 2500;
    plug();
    CHECK(pgc_media(&card, 1) == PGC_OK, "the drive is back after the restarts");
    /* a card that is not there, and one that does not come back */
    dead_reads = 1000000;
    r = pgcard_get_info(&info);
    CHECK(r == PGCARD_ENOCARD, "a dead bus is no card: %d", r);
    r = pgcard_set_mode(GUS_MODE, 0, &now);
    CHECK(r == PGCARD_ENOCARD && emuctl.reboots == 3, "nor is its mode set: %d", r);
    dead_reads = 0;
    emuctl.on_reboot = card_dies;
    {
        uint32_t t0 = clock_ms;
        r = pgcard_set_mode(USB_MODE, 0, &now);
        CHECK(r == PGCARD_ETIMEOUT && clock_ms - t0 >= 10000, "restarted and never seen again: %d after %u ms", r, clock_ms - t0);
    }
    dead_reads = 0;
    emuctl.on_reboot = card_reboot;
    r = pgcard_set_mode(SB_MODE, 0, &now);
    CHECK(r == PGCARD_OK && now == SB_MODE, "SB again: %d", r);
    clock_ms += 2500;
    plug();
    CHECK(pgc_media(&card, 1) == PGC_OK, "drive mounted");

    /* WiFi */
    r = pgcard_wifi_status(name, sizeof(name));
    CHECK(r == PGCARD_ENOTSUP && name[0] == 0, "firmware without WiFi: %d", r);
    emuctl.has_wifi = true;
    emuctl.wifi_status = "Connected to SSID home, Signal -55 dB, web UI: http://192.168.1.42/";
    r = pgcard_wifi_status(name, sizeof(name));
    CHECK(r == PGCARD_OK && strcmp(name, emuctl.wifi_status) == 0, "status: %d '%s'", r, name);
    CHECK(pgcard_wifi_url(name, url, sizeof(url)) && strcmp(url, "http://192.168.1.42/") == 0, "url '%s'", url);
    CHECK(!pgcard_wifi_url("Err: SSID not found", url, sizeof(url)) && url[0] == 0, "no url");
    CHECK(!pgcard_wifi_url(name, url, 10), "url that does not fit");
    emuctl.wifi_busy_reads = 5;
    r = pgcard_wifi_status(name, sizeof(name));
    CHECK(r == PGCARD_OK && strcmp(name, emuctl.wifi_status) == 0, "status after 'not ready': %d '%s'", r, name);
    emuctl.wifi_busy_reads = 0;
    saves = emuctl.saves;
    CHECK(pgcard_wifi_set("home", "secret", 1) == PGCARD_OK && strcmp(emuctl.ssid, "home") == 0 &&
          strcmp(emuctl.pass, "secret") == 0 && emuctl.applies == 1 && emuctl.saves == saves + 1, "set and saved: '%s' '%s'",
          emuctl.ssid, emuctl.pass);
    CHECK(pgcard_wifi_set("cafe", "", 0) == PGCARD_OK && strcmp(emuctl.ssid, "cafe") == 0 && emuctl.pass[0] == 0 &&
          emuctl.applies == 2 && emuctl.saves == saves + 1, "open network, not saved");
    CHECK(pgcard_wifi_set("", "x", 0) == PGCARD_EINVAL, "no SSID");
    CHECK(pgcard_wifi_set("123456789012345678901234567890123", "x", 0) == PGCARD_EINVAL, "SSID of 33");
    CHECK(pgcard_wifi_set("12345678901234567890123456789012", "x", 0) == PGCARD_OK && strlen(emuctl.ssid) == 32, "SSID of 32");
    {
        char pass[65];
        memset(pass, 'p', 64);
        pass[64] = 0;
        CHECK(pgcard_wifi_set("a", pass, 0) == PGCARD_EINVAL, "password of 64");
        pass[63] = 0;
        CHECK(pgcard_wifi_set("a", pass, 0) == PGCARD_OK && strlen(emuctl.pass) == 63, "password of 63");
    }
    CHECK(card.present && pgc_media(&card, 1) == PGC_OK, "PGDFS untouched by all of it");
}

int main(void) {
    test_probe();
    test_files();
    test_names();
    test_many();
    test_removal();
    test_timeout();
    test_space();
    test_ctl();
    test_tray();
    printf("%d checks, %d failures (%u transactions, %u port operations)\n", n_checks, n_fails,
           card.transactions, port_ops);
    ramdisk_free();
    return n_fails ? 1 : 0;
}
