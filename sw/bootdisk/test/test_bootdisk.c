/*
 * PGBOOT host tests.
 *
 * Part 1 checks the geometry detection and the core 0 register handlers
 * directly. Part 2 formats a RAM disk, writes image files onto it with
 * FatFs, and drives the card side the way the boot ROM does: complete
 * PGDFS frames (4-byte header + payload) through dfs_process(), whose answer
 * overwrites the request in the same buffer. Configuration goes through the
 * bd_ctl_* register handlers as core 0 would call them, followed by
 * bd_tasks() as the core 1 loop would.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ff.h"
#include "dfs.h"            /* DFS_BUF_SIZE */
#include "dfs_server.h"
#include "bootdisk.h"
#include "bd_geometry.h"
#include "ramdisk_diskio.h"
#include "../usb_msc/msc_app.h"     /* msc_app_get_stats(): the RAM disk keeps the counters */

bool bd_test_fastseek(int u);   /* bootdisk.c, BD_HOST_TEST */

/* ---- platform stubs -------------------------------------------------------- */

static FATFS fatfs;
static uint32_t test_ms = 1000;

uint32_t dfs_platform_millis(void) { return test_ms; }
FATFS *dfs_platform_fatfs(void) { return &fatfs; }

/* ---- check infrastructure -------------------------------------------------- */

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

/* ---- helpers ---------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static void wbe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wbe32(uint8_t *p, uint32_t v) { wbe16(p, (uint16_t)(v >> 16)); wbe16(p + 2, (uint16_t)v); }

/* settings storage the card binds to */
static char    fd_name[BD_NAME_BUF];
static char    hd_name[BD_NAME_BUF];
static uint8_t bd_opts;

/* the shared request/answer buffer, as in dfs_transport.c */
static uint8_t  buf[DFS_BUF_SIZE];
static uint8_t *const pl = buf + DFS_HDR_LEN;
static uint16_t ans_len;        /* answer payload length */
static uint32_t tok[BD_UNITS];  /* image tokens from the last BDINFO, echoed like the ROM does */

/* One frame the way the ROM sends it: header + payload in, answer out. */
static uint16_t frame(uint8_t drive, uint8_t al, const uint8_t *payload, uint32_t plen) {
    uint32_t len = DFS_HDR_LEN + plen;
    uint16_t total;
    if (len > sizeof(buf)) return 0xFFFF;
    wr16(buf, (uint16_t)len);
    buf[2] = drive;
    buf[3] = al;
    if (plen && payload != pl) memmove(pl, payload, plen);
    total = dfs_process(buf, (uint16_t)len, sizeof(buf));
    CHECK(total == rd16(buf), "answer length header %u != returned %u", rd16(buf), total);
    CHECK(total >= DFS_HDR_LEN && total <= sizeof(buf), "answer length %u out of range", total);
    ans_len = (uint16_t)(total - DFS_HDR_LEN);
    return rd16(buf + 2);
}

static uint16_t bdinfo(uint8_t unit, uint8_t flags) {
    uint8_t p[2] = { unit, flags };
    return frame(0, BD_AL_INFO, p, 2);
}

static uint16_t bdread_t(uint8_t unit, uint8_t count, uint32_t lba, uint32_t token) {
    uint8_t p[BD_IO_HDR_LEN];
    p[0] = unit;
    p[1] = count;
    wr32(p + 2, lba);
    wr32(p + 6, token);
    return frame(0, BD_AL_READ, p, BD_IO_HDR_LEN);
}

static uint16_t bdread(uint8_t unit, uint8_t count, uint32_t lba) {
    return bdread_t(unit, count, lba, unit < BD_UNITS ? tok[unit] : 0);
}

/* data: count sectors, built directly in the frame buffer */
static uint16_t bdwrite(uint8_t unit, uint8_t count, uint32_t lba, const uint8_t *data, uint32_t datalen) {
    static uint8_t req[DFS_BUF_SIZE + 1024];
    if (BD_IO_HDR_LEN + datalen > sizeof(req)) return 0xFFFF;
    req[0] = unit;
    req[1] = count;
    wr32(req + 2, lba);
    wr32(req + 6, unit < BD_UNITS ? tok[unit] : 0);
    memcpy(req + BD_IO_HDR_LEN, data, datalen);
    return frame(0, BD_AL_WRITE, req, BD_IO_HDR_LEN + datalen);
}

/* core 0 writes a name register, then the core 1 loop runs */
static void set_name(uint8_t unit, const char *s) {
    bd_ctl_name_select(unit);
    while (*s) bd_ctl_name_write(unit, (uint8_t)*s++);
    bd_ctl_name_write(unit, 0);
    bd_tasks();
}

static void read_name(uint8_t unit, char *out, size_t cap) {
    size_t i = 0;
    uint8_t c;
    bd_ctl_name_select(unit);
    while ((c = bd_ctl_name_read(unit)) != 0 && i + 1 < cap) out[i++] = (char)c;
    out[i] = 0;
}

typedef struct {
    uint8_t  ver, state, type, flags, drvtype, gen, nlen;
    uint16_t cyl, heads, spt;
    uint32_t total, token;
    char     name[64];
} info_t;

static uint16_t get_info(uint8_t unit, uint8_t flags, info_t *i) {
    uint16_t ax = bdinfo(unit, flags);
    memset(i, 0, sizeof(*i));
    if (ax != 0) return ax;
    CHECK(ans_len >= BD_INFO_LEN, "info answer length %u", ans_len);
    i->ver = pl[0];
    i->state = pl[1];
    i->type = pl[2];
    i->flags = pl[3];
    i->cyl = rd16(pl + 4);
    i->heads = rd16(pl + 6);
    i->spt = rd16(pl + 8);
    i->total = rd32(pl + 12);
    i->drvtype = pl[16];
    i->gen = pl[17];
    i->nlen = pl[18];
    i->token = rd32(pl + 20);
    CHECK((i->state == BD_STATE_READY) == (i->token != 0), "token %08X in state %u", i->token, i->state);
    tok[unit] = i->token;
    CHECK(ans_len == BD_INFO_LEN + i->nlen, "info length %u != 32 + %u", ans_len, i->nlen);
    if (i->nlen < sizeof(i->name)) {
        memcpy(i->name, pl + BD_INFO_LEN, i->nlen);
        i->name[i->nlen] = 0;
    }
    return ax;
}

/* sector content: LBA and a tag, so a misplaced sector shows */
static void fill_sector(uint8_t *s, uint32_t lba, uint8_t tag) {
    wr32(s, lba);
    s[4] = tag;
    for (int k = 5; k < 512; k++) s[k] = (uint8_t)(lba * 7u + (uint32_t)k + tag);
}

static bool sector_ok(const uint8_t *s, uint32_t lba, uint8_t tag) {
    uint8_t ref[512];
    fill_sector(ref, lba, tag);
    return memcmp(s, ref, 512) == 0;
}

/* image file of 'sectors' patterned sectors, sector 0 optionally replaced,
 * extra bytes appended (a VHD footer) */
static bool make_image(const char *path, uint32_t sectors, uint8_t tag, const uint8_t *sec0,
                       const uint8_t *tail, uint32_t tail_len) {
    FIL f;
    UINT bw;
    uint8_t s[512];
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    for (uint32_t i = 0; i < sectors; i++) {
        if (i == 0 && sec0) memcpy(s, sec0, 512); else fill_sector(s, i, tag);
        if (f_write(&f, s, 512, &bw) != FR_OK || bw != 512) { f_close(&f); return false; }
    }
    if (tail_len && (f_write(&f, tail, tail_len, &bw) != FR_OK || bw != tail_len)) { f_close(&f); return false; }
    return f_close(&f) == FR_OK;
}

static bool file_sector(const char *path, uint32_t lba, uint8_t *out) {
    FIL f;
    UINT br;
    bool ok;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    ok = f_lseek(&f, lba * 512u) == FR_OK && f_read(&f, out, 512, &br) == FR_OK && br == 512;
    f_close(&f);
    return ok;
}

static bool file_exists(const char *path) {
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

/* FAT boot sector with a BPB */
static void make_bpb(uint8_t *s, uint16_t spt, uint16_t heads, uint32_t total, uint8_t media) {
    memset(s, 0, 512);
    s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
    memcpy(s + 3, "MSDOS5.0", 8);
    wr16(s + 11, 512);
    s[13] = 1;                  /* sectors per cluster */
    wr16(s + 14, 1);            /* reserved */
    s[16] = 2;                  /* FATs */
    wr16(s + 17, 224);          /* root entries */
    if (total < 65536) wr16(s + 19, (uint16_t)total); else wr32(s + 32, total);
    s[21] = media;
    wr16(s + 22, 9);            /* sectors per FAT */
    wr16(s + 24, spt);
    wr16(s + 26, heads);
    s[510] = 0x55; s[511] = 0xAA;
}

/* MBR with one partition entry ending at (end_head, end_sector) */
static void make_mbr(uint8_t *s, uint8_t end_head, uint8_t end_sector, uint32_t lba_start, uint32_t lba_count) {
    uint8_t *e = s + 446;
    memset(s, 0, 512);
    s[0] = 0xFA;                /* boot code, not a jump: no BPB */
    e[0] = 0x80;
    e[1] = 1; e[2] = 1; e[3] = 0;       /* start CHS 0/1/1 */
    e[4] = 0x06;                        /* FAT16 */
    e[5] = end_head;
    e[6] = (uint8_t)(end_sector & 0x3F);
    e[7] = 0x20;
    wr32(e + 8, lba_start);
    wr32(e + 12, lba_count);
    s[510] = 0x55; s[511] = 0xAA;
}

/* VHD footer */
static void make_vhd_footer(uint8_t *f, uint32_t data_size, uint16_t c, uint8_t h, uint8_t spt, uint32_t type) {
    uint32_t sum = 0;
    memset(f, 0, 512);
    memcpy(f, "conectix", 8);
    wbe32(f + 8, 2);                    /* features */
    wbe32(f + 12, 0x00010000);          /* version */
    wbe32(f + 16, 0xFFFFFFFF);          /* data offset: none for fixed */
    wbe32(f + 20, 0xFFFFFFFF);
    memcpy(f + 28, "qemu", 4);
    wbe32(f + 44, data_size);           /* original size (low half of u64) */
    wbe32(f + 52, data_size);           /* current size */
    wbe16(f + 56, c);
    f[58] = h;
    f[59] = spt;
    wbe32(f + 60, type);
    for (int i = 0; i < 512; i++) sum += f[i];
    wbe32(f + 64, ~sum);
}

static void sec0_from(bd_sec0_t *s0, const uint8_t *sector) {
    memcpy(s0->head, sector, BD_SEC0_HEAD_LEN);
    memcpy(s0->ptab, sector + BD_SEC0_PTAB_OFF, BD_SEC0_PTAB_LEN);
}

static int vhd_kind(const uint8_t *footer, uint32_t file_size, bd_vhd_t *v) {
    return bd_vhd_check(footer, bd_vhd_sum(footer, 0, 512), file_size, v);
}

/* ================= part 1: pure functions ==================================== */

static void test_floppy_geometry(void) {
    static const struct { uint32_t size; uint16_t c, h, s; uint8_t type; } t[] = {
        {  163840, 40, 1,  8, 1 }, {  184320, 40, 1,  9, 1 }, {  327680, 40, 2,  8, 1 },
        {  368640, 40, 2,  9, 1 }, {  737280, 80, 2,  9, 3 }, { 1228800, 80, 2, 15, 2 },
        { 1474560, 80, 2, 18, 4 }, { 1720320, 80, 2, 21, 4 }, { 1763328, 82, 2, 21, 4 },
        { 2949120, 80, 2, 36, 6 },
    };
    bd_geom_t g;
    bd_sec0_t s0;
    uint8_t sec[512];

    SECTION("floppy geometry: size table");
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        bool ok = bd_geom_floppy(t[i].size, NULL, &g);
        CHECK(ok && g.cyl == t[i].c && g.heads == t[i].h && g.spt == t[i].s && g.drive_type == t[i].type &&
              g.total == t[i].size / 512,
              "size %u -> %d %u/%u/%u type %u total %u", t[i].size, ok, g.cyl, g.heads, g.spt, g.drive_type, g.total);
    }

    SECTION("floppy geometry: BPB fallback");
    make_bpb(sec, 18, 2, 2880, 0xF0);
    sec0_from(&s0, sec);
    CHECK(bd_bpb_valid(s0.head), "BPB valid");
    CHECK(bd_geom_floppy(1474560 + 512, &s0, &g) && g.cyl == 80 && g.heads == 2 && g.spt == 18 &&
          g.total == 2881 && g.drive_type == 4, "1.44M + trailing sector via BPB: %u/%u/%u total %u type %u",
          g.cyl, g.heads, g.spt, g.total, g.drive_type);
    make_bpb(sec, 9, 2, 720, 0xFD);
    sec0_from(&s0, sec);
    CHECK(bd_geom_floppy(360000, &s0, &g) && g.cyl == 40 && g.spt == 9 && g.drive_type == 1,
          "truncated 360K via BPB: %u/%u/%u type %u", g.cyl, g.heads, g.spt, g.drive_type);
    make_bpb(sec, 10, 2, 1600, 0xF0);
    sec0_from(&s0, sec);
    CHECK(bd_geom_floppy(819200, &s0, &g) && g.cyl == 80 && g.spt == 10 && g.drive_type == 2,
          "800K 80/2/10 via BPB: %u/%u/%u type %u", g.cyl, g.heads, g.spt, g.drive_type);
    CHECK(!bd_geom_floppy(1000000, NULL, &g), "odd size without sector 0 -> unusable");
    memset(sec, 0, 512);
    sec0_from(&s0, sec);
    CHECK(!bd_geom_floppy(1000000, &s0, &g), "odd size without BPB -> unusable");
    make_bpb(sec, 63, 16, 1000000 / 512, 0xF8);
    sec0_from(&s0, sec);
    CHECK(!bd_geom_floppy(1000000, &s0, &g), "BPB with 16 heads is no floppy");
    make_bpb(sec, 18, 2, 2880, 0xF0);
    wr16(sec + 11, 1024);
    sec0_from(&s0, sec);
    CHECK(!bd_bpb_valid(s0.head), "1024-byte sectors: BPB rejected");
    make_bpb(sec, 18, 2, 2880, 0xF0);
    sec[0] = 0;
    sec0_from(&s0, sec);
    CHECK(!bd_bpb_valid(s0.head), "no jump: BPB rejected");
    make_bpb(sec, 18, 2, 2880, 0xF0);
    sec[13] = 3;
    sec0_from(&s0, sec);
    CHECK(!bd_bpb_valid(s0.head), "3 sectors per cluster: BPB rejected");
}

static void test_harddisk_geometry(void) {
    bd_geom_t g;
    bd_sec0_t s0;
    bd_vhd_t v;
    uint8_t sec[512], foot[512];

    SECTION("hard disk geometry");
    memset(sec, 0, 512);
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 16 && g.spt == 63 && g.cyl == 40 &&
          g.total == 40960 && g.drive_type == 0, "blank 20M: default %u/%u/%u total %u", g.cyl, g.heads, g.spt, g.total);
    CHECK(bd_geom_harddisk(20u << 20, NULL, NULL, &g) && g.heads == 16 && g.spt == 63, "no sector 0: default");
    CHECK(!bd_geom_harddisk(511, NULL, NULL, &g), "less than a sector: unusable");
    CHECK(bd_geom_harddisk(4096, &s0, NULL, &g) && g.cyl == 1 && g.total == 8, "tiny image: 1 cylinder, total %u", g.total);

    make_mbr(sec, 63, 32, 32, 40928);           /* 64 heads, 32 sectors */
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 64 && g.spt == 32 && g.cyl == 20,
          "MBR 64/32: %u/%u/%u", g.cyl, g.heads, g.spt);
    make_mbr(sec, 254, 63, 63, 1000000);        /* 255/63 */
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(2000u << 20, &s0, NULL, &g) && g.heads == 255 && g.spt == 63 && g.cyl == 254,
          "MBR 255/63 2000M: %u/%u/%u", g.cyl, g.heads, g.spt);
    CHECK(bd_geom_harddisk(3900u << 20, &s0, NULL, &g) && g.cyl == 497 && g.total == (3900u << 11),
          "3900M 255/63: %u cylinders, total %u", g.cyl, g.total);
    make_mbr(sec, 255, 63, 63, 1000000);        /* end head 255 would make 256 heads */
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(2000u << 20, &s0, NULL, &g) && g.heads == 255 && g.spt == 63,
          "MBR end head 255: heads capped at %u", g.heads);
    make_mbr(sec, 15, 63, 63, 1000000);
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(2000u << 20, &s0, NULL, &g) && g.heads == 16 && g.cyl == 1024 &&
          g.total == (2000u << 11), "2000M 16/63: cylinders capped at 1024 (%u), total %u", g.cyl, g.total);
    /* a second entry with a larger end head wins */
    make_mbr(sec, 15, 63, 63, 1000);
    memcpy(sec + 446 + 16, sec + 446, 16);
    sec[446 + 16] = 0x00;
    sec[446 + 16 + 5] = 31;
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 32 && g.spt == 63, "max end head of two entries: %u", g.heads);
    /* not an MBR: bad status byte, no signature, zero LBA */
    make_mbr(sec, 63, 32, 32, 40928);
    sec[446] = 0x41;
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 16 && g.spt == 63, "bad status byte: default");
    make_mbr(sec, 63, 32, 32, 40928);
    sec[511] = 0;
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 16, "no 55AA: default");
    make_mbr(sec, 63, 32, 0, 40928);
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 16, "start LBA 0: default");
    memset(sec, 0, 512);
    sec[510] = 0x55; sec[511] = 0xAA;
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 16, "empty partition table: default");

    /* superfloppy: BPB in sector 0 */
    make_bpb(sec, 32, 8, 40960, 0xF8);
    memset(sec + 446, 0, 64);
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(20u << 20, &s0, NULL, &g) && g.heads == 8 && g.spt == 32 && g.cyl == 160,
          "superfloppy BPB 8/32: %u/%u/%u", g.cyl, g.heads, g.spt);

    SECTION("VHD footer");
    make_vhd_footer(foot, 10u << 20, 301, 4, 17, 2);
    CHECK(vhd_kind(foot, (10u << 20) + 512, &v) == BD_VHD_FIXED && v.data_size == (10u << 20) &&
          v.cyl == 301 && v.heads == 4 && v.spt == 17, "fixed VHD: size %u, %u/%u/%u", v.data_size, v.cyl, v.heads, v.spt);
    make_mbr(sec, 63, 32, 32, 20000);           /* the footer wins over the MBR */
    sec0_from(&s0, sec);
    CHECK(bd_geom_harddisk(v.data_size, &s0, &v, &g) && g.heads == 4 && g.spt == 17 && g.cyl == 301 &&
          g.total == 20480, "fixed VHD geometry: %u/%u/%u total %u", g.cyl, g.heads, g.spt, g.total);
    v.cyl = 200;                                /* footer cylinders below what the size allows */
    CHECK(bd_geom_harddisk(v.data_size, &s0, &v, &g) && g.cyl == 200, "footer cylinders are kept: %u", g.cyl);
    v.cyl = 301;
    make_vhd_footer(foot, 10u << 20, 1024, 16, 255, 2);    /* big-disk style: 255 sectors */
    CHECK(vhd_kind(foot, (10u << 20) + 512, &v) == BD_VHD_FIXED && v.spt == 0, "VHD with 255 spt: geometry unusable");
    CHECK(bd_geom_harddisk(v.data_size, &s0, &v, &g) && g.heads == 64 && g.spt == 32, "falls back to the MBR");
    make_vhd_footer(foot, 10u << 20, 304, 4, 17, 3);
    CHECK(vhd_kind(foot, (10u << 20) + 512, &v) == BD_VHD_OTHER, "dynamic VHD: other");
    make_vhd_footer(foot, 10u << 20, 304, 4, 17, 4);
    CHECK(vhd_kind(foot, (10u << 20) + 512, &v) == BD_VHD_OTHER, "differencing VHD: other");
    make_vhd_footer(foot, 10u << 20, 304, 4, 17, 2);
    foot[100] ^= 1;
    CHECK(vhd_kind(foot, (10u << 20) + 512, &v) == BD_VHD_NONE, "bad checksum: raw");
    make_vhd_footer(foot, 20u << 20, 304, 4, 17, 2);
    CHECK(vhd_kind(foot, (10u << 20) + 512, &v) == BD_VHD_FIXED && v.data_size == (10u << 20),
          "current size beyond the file is clipped: %u", v.data_size);
    make_vhd_footer(foot, 5u << 20, 304, 4, 17, 2);
    CHECK(vhd_kind(foot, (10u << 20) + 512, &v) == BD_VHD_FIXED && v.data_size == (5u << 20),
          "smaller current size is used: %u", v.data_size);
    /* checksum in pieces equals the whole */
    CHECK(bd_vhd_sum(foot, 0, 128) + bd_vhd_sum(foot + 128, 128, 384) == bd_vhd_sum(foot, 0, 512), "partial sums add up");
    memset(foot, 0, 512);
    CHECK(vhd_kind(foot, 1u << 20, &v) == BD_VHD_NONE, "zeros: raw");
}

static void test_registers(void) {
    char s[256];
    SECTION("control registers (core 0)");
    /* before bd_init nothing is bound */
    CHECK(bd_ctl_name_read(0) == 0, "unbound name reads 0");
    bd_ctl_name_write(0, 'A');
    CHECK(bd_ctl_opts_read() == 0x80, "unbound opts read 80h: %02X", bd_ctl_opts_read());

    memset(fd_name, 0xFF, sizeof(fd_name));             /* erased flash */
    memcpy(hd_name, "HD.IMG", 7);
    memset(hd_name + 7, 'x', sizeof(hd_name) - 7);      /* garbage after it is fine */
    bd_opts = 0xF3;
    bd_init(fd_name, hd_name, &bd_opts);
    CHECK(fd_name[0] == 0, "erased flash name becomes empty");
    CHECK(fd_name[BD_NAME_MAX] == 0, "name terminated");
    CHECK(bd_opts == 0x03, "opts masked at init: %02X", bd_opts);
    CHECK(bd_ctl_opts_read() == 0x83, "opts read with signature: %02X", bd_ctl_opts_read());
    CHECK((bd_ctl_opts_read() & 0xC0) == 0x80, "signature bit 7 set, bit 6 clear");
    bd_ctl_opts_write(0xFF);
    CHECK(bd_opts == 0x0F && bd_ctl_opts_read() == 0x8F, "opts write masked: %02X", bd_opts);
    bd_ctl_opts_write(0);

    read_name(1, s, sizeof(s));
    CHECK(strcmp(s, "HD.IMG") == 0, "hd name read '%s'", s);
    /* reading runs to the terminator and rewinds by itself */
    CHECK(bd_ctl_name_read(1) == 'H', "read rewound after the terminator");
    bd_ctl_name_select(1);

    bd_ctl_name_select(0);
    for (const char *p = "A:\\DISKS\\DOS622.IMG"; *p; p++) bd_ctl_name_write(0, (uint8_t)*p);
    CHECK(strcmp(fd_name, "A:\\DISKS\\DOS622.IMG") == 0, "name visible (terminated) before the commit");
    bd_ctl_name_write(0, 0);
    read_name(0, s, sizeof(s));
    CHECK(strcmp(s, "A:\\DISKS\\DOS622.IMG") == 0, "fd name read back '%s'", s);
    /* a shorter name replaces the longer one completely */
    bd_ctl_name_select(0);
    bd_ctl_name_write(0, 'X');
    bd_ctl_name_write(0, 0);
    read_name(0, s, sizeof(s));
    CHECK(strcmp(s, "X") == 0, "shorter name '%s'", s);
    /* over-long names are cut at BD_NAME_MAX */
    bd_ctl_name_select(0);
    for (int i = 0; i < 200; i++) bd_ctl_name_write(0, (uint8_t)('A' + i % 26));
    bd_ctl_name_write(0, 0);
    read_name(0, s, sizeof(s));
    CHECK(strlen(s) == BD_NAME_MAX && fd_name[BD_NAME_MAX] == 0, "over-long name cut to %zu", strlen(s));
    /* the empty string removes it */
    bd_ctl_name_select(0);
    bd_ctl_name_write(0, 0);
    CHECK(fd_name[0] == 0 && bd_ctl_name_read(0) == 0, "empty commit clears the name");
    /* out-of-range units are ignored */
    bd_ctl_name_select(2);
    bd_ctl_name_write(2, 'Q');
    CHECK(bd_ctl_name_read(2) == 0, "unit 2 reads 0");
    /* the core 1 side has not run: tasks apply the commits without a drive */
    bd_tasks();
}

/* ================= part 2: served over dfs_process() ======================== */

static void mount_drive(void) {
    FRESULT fr = f_mount(&fatfs, "", 1);
    CHECK(fr == FR_OK, "f_mount -> %d", fr);
    dfs_server_drive_mounted();         /* also bd_on_drive_mounted() */
}

static void unmount_drive(void) {
    dfs_server_drive_unmounted();       /* also bd_on_drive_unmounted() */
    f_unmount("");
}

static void test_setup(void) {
    static uint8_t work[FF_MAX_SS];
    MKFS_PARM opt = { FM_FAT32 | FM_SFD, 1, 0, 0, 512 };
    FRESULT fr;
    uint8_t s[512], foot[512];

    SECTION("setup: RAM disk and images");
    dfs_server_init();
    CHECK(ramdisk_init(64u * 1024 * 1024 / RAMDISK_SECTOR_SIZE), "ramdisk alloc");
    fr = f_mkfs("", &opt, work, sizeof(work));
    CHECK(fr == FR_OK, "f_mkfs -> %d", fr);
    fr = f_mount(&fatfs, "", 1);
    CHECK(fr == FR_OK, "f_mount -> %d", fr);
    CHECK(f_mkdir("IMAGES") == FR_OK, "mkdir");
    CHECK(make_image("IMAGES/DOS.IMG", 2880, 0x11, NULL, NULL, 0), "1.44M image");
    CHECK(make_image("IMAGES/B720.IMA", 1440, 0x22, NULL, NULL, 0), "720K image");
    make_bpb(s, 18, 2, 2880, 0xF0);
    CHECK(make_image("IMAGES/ODD.IMG", 2881, 0x33, s, NULL, 0), "1.44M + 1 sector with BPB");
    CHECK(make_image("IMAGES/JUNK.BIN", 1953, 0x44, NULL, NULL, 0), "odd size without BPB");
    make_mbr(s, 15, 63, 63, 32768 - 63);
    CHECK(make_image("HD16.IMG", 32768, 0x55, s, NULL, 0), "16M hard disk, MBR 16/63");
    make_mbr(s, 63, 32, 32, 16384 - 32);
    CHECK(make_image("HD8.IMG", 16384, 0x66, s, NULL, 0), "8M hard disk, MBR 64/32");
    CHECK(make_image("BLANK.IMG", 16384, 0x77, NULL, NULL, 0), "8M hard disk, no MBR");
    make_vhd_footer(foot, 4u << 20, 240, 2, 17, 2);
    CHECK(make_image("FIXED.VHD", 8192, 0x88, NULL, foot, 512), "4M fixed VHD");
    make_vhd_footer(foot, 4u << 20, 241, 2, 17, 3);
    CHECK(make_image("DYN.VHD", 64, 0x99, NULL, foot, 512), "dynamic VHD (footer only matters)");
    CHECK(make_image("Long Image Name.img", 1440, 0xAA, NULL, NULL, 0), "720K image with a long name");
    CHECK(f_mkdir("TREE") == FR_OK && f_mkdir("TREE/Sub Directory") == FR_OK, "tree for 8.3 aliases");
    CHECK(make_image("TREE/Sub Directory/floppy disk.img", 720, 0xBB, NULL, NULL, 0), "360K under a long directory");
    f_unmount("");

    /* fresh binding: empty names, no drive */
    memset(fd_name, 0, sizeof(fd_name));
    memset(hd_name, 0, sizeof(hd_name));
    bd_opts = 0;
    bd_init(fd_name, hd_name, &bd_opts);
}

static void test_no_drive(void) {
    info_t i;
    uint16_t ax;

    SECTION("before the drive mounts");
    ax = get_info(0, 0, &i);
    CHECK(ax == 0 && i.ver == 1 && i.state == BD_STATE_NONE && i.type == 0 && i.nlen == 0,
          "unit 0 unconfigured: ax %04X state %u type %u", ax, i.state, i.type);
    CHECK(bdread(0, 1, 0) == BD_ST_NOTREADY, "read unconfigured -> 80h");
    set_name(0, "\\IMAGES\\DOS.IMG");
    ax = get_info(0, 0, &i);
    CHECK(ax == 0 && i.state == BD_STATE_NODRIVE && i.type == BD_TYPE_FLOPPY && (i.flags & BD_FLAG_CHANGED),
          "configured, no drive: state %u type %u flags %02X", i.state, i.type, i.flags);
    CHECK(strcmp(i.name, "\\IMAGES\\DOS.IMG") == 0, "name '%s'", i.name);
    CHECK(bdread(0, 1, 0) == BD_ST_NOTREADY, "read without a drive -> 80h");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_NONE, "unit 1 OPEN without a name: none");
    /* the transport-level drive checks do not apply to PGBOOT frames */
    {
        uint8_t p[2] = { 0, 0 };
        CHECK(frame(3, BD_AL_INFO, p, 2) == 0, "drive index ignored for BDINFO");
    }
    CHECK(bdinfo(2, 0) == BD_ST_BADCMD && ans_len == 0, "unknown unit -> 1");
    CHECK(frame(0, BD_AL_INFO, NULL, 0) == BD_ST_BADCMD, "empty BDINFO -> 1");
}

static void test_floppy(void) {
    info_t i;
    uint16_t ax;
    uint8_t gen;
    static uint8_t data[8 * 512];
    uint8_t s[512];
    static const uint8_t expect[BD_INFO_LEN] = {
        1, 1, 1, 0,  80, 0, 2, 0,  18, 0, 0, 0,  0x40, 0x0B, 0, 0,
        4, 0, 15, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
    };

    SECTION("floppy image: mount, BDINFO record");
    mount_drive();
    ax = get_info(0, 0, &i);
    CHECK(ax == 0 && i.state == BD_STATE_READY && i.type == BD_TYPE_FLOPPY, "ready: state %u", i.state);
    CHECK(i.cyl == 80 && i.heads == 2 && i.spt == 18 && i.total == 2880 && i.drvtype == 4,
          "1.44M geometry %u/%u/%u total %u type %u", i.cyl, i.heads, i.spt, i.total, i.drvtype);
    CHECK(i.flags == BD_FLAG_CHANGED, "flags after mount %02X: changed", i.flags);
    gen = i.gen;
    ax = get_info(0, 0, &i);
    CHECK(ax == 0 && i.flags == 0 && i.gen == gen, "second BDINFO: changed cleared (%02X), same generation", i.flags);
    /* the record, byte for byte (generation patched in) */
    {
        uint8_t e[BD_INFO_LEN];
        memcpy(e, expect, sizeof(e));
        e[17] = gen;
        wr32(e + 20, i.token);
        CHECK(ans_len == 32 + 15 && memcmp(pl, e, BD_INFO_LEN) == 0 && memcmp(pl + 32, "\\IMAGES\\DOS.IMG", 15) == 0,
              "record bytes");
        for (int k = 0; k < BD_INFO_LEN && memcmp(pl, e, BD_INFO_LEN); k++) {
            if (pl[k] != e[k]) printf("    byte %d: %02X expected %02X\n", k, pl[k], e[k]);
        }
    }
    CHECK(bd_test_fastseek(0), "fast seek map in use");

    SECTION("floppy image: BDREAD");
    ax = bdread(0, 1, 0);
    CHECK(ax == 0 && ans_len == 512 && sector_ok(pl, 0, 0x11), "read LBA 0: ax %04X len %u", ax, ans_len);
    ax = bdread(0, 8, 100);
    {
        bool ok = (ax == 0 && ans_len == 4096);
        for (int k = 0; ok && k < 8; k++) ok = sector_ok(pl + 512 * k, 100 + k, 0x11);
        CHECK(ok, "read 8 sectors at 100: ax %04X len %u", ax, ans_len);
    }
    CHECK(bdread(0, 9, 0) == BD_ST_BADCMD && ans_len == 0, "9 sectors do not fit a 4096-byte frame -> 1");
    ax = bdread(0, 1, 2879);
    CHECK(ax == 0 && sector_ok(pl, 2879, 0x11), "last sector");
    CHECK(bdread(0, 2, 2879) == BD_ST_NOSECTOR && ans_len == 0, "past the end -> 4");
    CHECK(bdread(0, 1, 2880) == BD_ST_NOSECTOR, "LBA = total -> 4");
    CHECK(bdread(0, 1, 0xFFFFFFFF) == BD_ST_NOSECTOR, "LBA FFFFFFFF -> 4");
    CHECK(bdread(0, 0, 0) == BD_ST_BADCMD, "count 0 -> 1");
    CHECK(bdread(2, 1, 0) == BD_ST_BADCMD, "unit 2 -> 1");
    CHECK(bdread(1, 1, 0) == BD_ST_NOTREADY, "unit 1 unconfigured -> 80h");
    {
        uint8_t p[5] = { 0, 1, 0, 0, 0 };
        CHECK(frame(0, BD_AL_READ, p, 5) == BD_ST_BADCMD, "short read request -> 1");
    }

    SECTION("floppy image: BDWRITE");
    fill_sector(data, 5, 0xE1);
    ax = bdwrite(0, 1, 5, data, 512);
    CHECK(ax == 0 && ans_len == 0, "write LBA 5: ax %04X len %u", ax, ans_len);
    CHECK(bdread(0, 1, 5) == 0 && sector_ok(pl, 5, 0xE1), "read back LBA 5");
    CHECK(file_sector("IMAGES/DOS.IMG", 5, s) && sector_ok(s, 5, 0xE1), "synced: visible through another FIL");
    for (int k = 0; k < 7; k++) fill_sector(data + 512 * k, 2000 + k, 0xE2);
    ax = bdwrite(0, 7, 2000, data, 7 * 512);
    CHECK(ax == 0, "write 7 sectors: %04X", ax);
    ax = bdread(0, 8, 1999);
    {
        bool ok = ax == 0 && sector_ok(pl, 1999, 0x11);
        for (int k = 0; ok && k < 7; k++) ok = sector_ok(pl + 512 * (k + 1), 2000 + k, 0xE2);
        CHECK(ok, "7 written sectors read back with their neighbour");
    }
    CHECK(file_sector("IMAGES/DOS.IMG", 2006, s) && sector_ok(s, 2006, 0xE2), "multi-sector write synced");
    CHECK(bdwrite(0, 8, 0, data, 8 * 512) == 0xFFFF, "8 sectors cannot even be framed (6 + 4096 > 4096)");
    CHECK(bdwrite(0, 2, 0, data, 512) == BD_ST_BADCMD, "data shorter than NN*512 -> 1");
    CHECK(bdwrite(0, 1, 2880, data, 512) == BD_ST_NOSECTOR, "write past the end -> 4");
    CHECK(bdwrite(0, 2, 2879, data, 1024) == BD_ST_NOSECTOR, "write straddling the end -> 4");
    {
        FILINFO fi;
        CHECK(f_stat("IMAGES/DOS.IMG", &fi) == FR_OK && fi.fsize == 1474560, "file size unchanged: %u", (unsigned)fi.fsize);
    }
    /* restore */
    fill_sector(data, 5, 0x11);
    CHECK(bdwrite(0, 1, 5, data, 512) == 0, "restore LBA 5");

    SECTION("floppy image: read-only");
    bd_ctl_opts_write(BD_OPT_FD_RO);
    CHECK(get_info(0, 0, &i) == 0 && (i.flags & BD_FLAG_RO), "RO option -> flag %02X", i.flags);
    CHECK(bdwrite(0, 1, 5, data, 512) == BD_ST_WRPROT, "write with RO option -> 3");
    CHECK(bdread(0, 1, 5) == 0, "read still fine");
    bd_ctl_opts_write(BD_OPT_HD_RO);
    CHECK(get_info(0, 0, &i) == 0 && !(i.flags & BD_FLAG_RO), "HD RO option does not touch the floppy");
    CHECK(bdwrite(0, 1, 5, data, 512) == 0, "write allowed again");
    bd_ctl_opts_write(0);
    CHECK(f_chmod("IMAGES/B720.IMA", AM_RDO, AM_RDO) == FR_OK, "set AM_RDO on the 720K image");

    SECTION("floppy image: name commit = media change");
    get_info(0, 0, &i);
    gen = i.gen;
    set_name(0, "images/b720.ima");             /* case and separators do not matter */
    ax = get_info(0, 0, &i);
    CHECK(ax == 0 && i.state == BD_STATE_READY && (i.flags & BD_FLAG_CHANGED) && i.gen == (uint8_t)(gen + 1),
          "swap: changed flag %02X, generation %u -> %u", i.flags, gen, i.gen);
    CHECK(i.cyl == 80 && i.heads == 2 && i.spt == 9 && i.drvtype == 3 && i.total == 1440,
          "720K geometry %u/%u/%u type %u", i.cyl, i.heads, i.spt, i.drvtype);
    CHECK(strcmp(i.name, "\\images\\b720.ima") == 0, "normalised name '%s'", i.name);
    CHECK(i.flags & BD_FLAG_RO, "AM_RDO file -> read-only flag");
    CHECK(bdwrite(0, 1, 0, data, 512) == BD_ST_WRPROT, "write to AM_RDO image -> 3");
    CHECK(bdread(0, 1, 7) == 0 && sector_ok(pl, 7, 0x22), "new disk's content");
    CHECK(get_info(0, 0, &i) == 0 && !(i.flags & BD_FLAG_CHANGED), "change reported once");
    f_chmod("IMAGES/B720.IMA", 0, AM_RDO);

    SECTION("floppy image: other states");
    set_name(0, "IMAGES/NOPE.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NOTFOUND && i.type == BD_TYPE_FLOPPY && i.total == 0,
          "missing file -> state %u", i.state);
    CHECK(bdread(0, 1, 0) == BD_ST_NOTREADY, "read -> 80h");
    set_name(0, "NODIR/X.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NOTFOUND, "missing directory -> state %u", i.state);
    set_name(0, "IMAGES");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NOTFOUND, "a directory -> state %u", i.state);
    set_name(0, "IMAGES/JUNK.BIN");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_UNUSABLE, "odd size, no BPB -> state %u", i.state);
    set_name(0, "FIXED.VHD");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_UNUSABLE, "4M VHD as a floppy -> state %u", i.state);
    set_name(0, "IMAGES/ODD.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.cyl == 80 && i.spt == 18 && i.total == 2881,
          "BPB fallback: state %u %u/%u/%u total %u", i.state, i.cyl, i.heads, i.spt, i.total);
    CHECK(bdread(0, 1, 2880) == 0 && sector_ok(pl, 2880, 0x33), "the extra sector is readable by LBA");
    set_name(0, "");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NONE && i.type == 0 && i.nlen == 0 && (i.flags & BD_FLAG_CHANGED),
          "empty name -> none, changed %02X", i.flags);
    CHECK(bdread(0, 1, 0) == BD_ST_NOTREADY, "read -> 80h");
    set_name(0, "E:\\IMAGES\\DOS.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && strcmp(i.name, "\\IMAGES\\DOS.IMG") == 0,
          "drive letter dropped: '%s'", i.name);
}

static void test_harddisk(void) {
    info_t i;
    uint16_t ax;
    uint8_t gen, data[512];
    char s[256];

    SECTION("hard disk image: commit waits for OPEN");
    set_name(1, "HD16.IMG");
    CHECK(get_info(1, 0, &i) == 0 && i.state == BD_STATE_NONE, "committed but not opened: state %u", i.state);
    CHECK(bdread(1, 1, 0) == BD_ST_NOTREADY, "read before OPEN -> 80h");
    read_name(1, s, sizeof(s));
    CHECK(strcmp(s, "HD16.IMG") == 0, "register reads the new name '%s'", s);
    ax = get_info(1, BD_INFO_OPEN, &i);
    CHECK(ax == 0 && i.state == BD_STATE_READY && i.type == BD_TYPE_HARDDISK && i.drvtype == 0,
          "OPEN: state %u type %u", i.state, i.type);
    CHECK(i.heads == 16 && i.spt == 63 && i.cyl == 32 && i.total == 32768 && !(i.flags & BD_FLAG_VHD),
          "MBR geometry %u/%u/%u total %u", i.cyl, i.heads, i.spt, i.total);
    CHECK(i.flags & BD_FLAG_CHANGED, "OPEN reports a change");
    CHECK(strcmp(i.name, "\\HD16.IMG") == 0, "name '%s'", i.name);
    CHECK(bdread(1, 4, 32764) == 0 && sector_ok(pl, 32764, 0x55) && sector_ok(pl + 1536, 32767, 0x55), "read the last 4 sectors");
    gen = i.gen;
    fill_sector(data, 1000, 0xD1);
    CHECK(bdwrite(1, 1, 1000, data, 512) == 0 && bdread(1, 1, 1000) == 0 && sector_ok(pl, 1000, 0xD1), "hard disk write");

    /* a commit under a running system leaves the open disk alone */
    set_name(1, "HD8.IMG");
    ax = get_info(1, 0, &i);
    CHECK(ax == 0 && i.state == BD_STATE_READY && i.heads == 16 && i.total == 32768 && strcmp(i.name, "\\HD16.IMG") == 0 &&
          i.gen == gen && !(i.flags & BD_FLAG_CHANGED), "new name stored only: still %s", i.name);
    CHECK(bdread(1, 1, 1000) == 0 && sector_ok(pl, 1000, 0xD1), "still reading the old disk");
    /* the floppy is not affected by hard disk activity */
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.total == 2880, "floppy untouched");
    ax = get_info(1, BD_INFO_OPEN, &i);
    CHECK(ax == 0 && i.state == BD_STATE_READY && i.heads == 64 && i.spt == 32 && i.cyl == 8 && i.total == 16384 &&
          i.gen == (uint8_t)(gen + 1), "OPEN switches: %u/%u/%u total %u", i.cyl, i.heads, i.spt, i.total);
    CHECK(bdread(1, 1, 3) == 0 && sector_ok(pl, 3, 0x66), "new disk's content");
    bd_ctl_opts_write(BD_OPT_HD_RO);
    CHECK(bdwrite(1, 1, 3, data, 512) == BD_ST_WRPROT, "HD RO option -> 3");
    CHECK(get_info(1, 0, &i) == 0 && (i.flags & BD_FLAG_RO), "HD RO flag");
    bd_ctl_opts_write(0);

    SECTION("hard disk image: default geometry, VHD");
    set_name(1, "BLANK.IMG");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.heads == 16 && i.spt == 63 && i.cyl == 16 && i.total == 16384,
          "no MBR: default %u/%u/%u", i.cyl, i.heads, i.spt);
    set_name(1, "FIXED.VHD");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY && (i.flags & BD_FLAG_VHD) &&
          i.cyl == 240 && i.heads == 2 && i.spt == 17 && i.total == 8192,
          "fixed VHD: flags %02X %u/%u/%u total %u", i.flags, i.cyl, i.heads, i.spt, i.total);
    CHECK(bdread(1, 1, 8191) == 0 && sector_ok(pl, 8191, 0x88), "last data sector");
    CHECK(bdread(1, 1, 8192) == BD_ST_NOSECTOR, "the footer is not a sector -> 4");
    fill_sector(data, 8191, 0xD2);
    CHECK(bdwrite(1, 1, 8191, data, 512) == 0, "write the last data sector");
    {
        uint8_t f[512];
        CHECK(file_sector("FIXED.VHD", 8192, f) && memcmp(f, "conectix", 8) == 0, "footer intact after the write");
    }
    set_name(1, "DYN.VHD");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_UNUSABLE && i.type == BD_TYPE_HARDDISK,
          "dynamic VHD -> state %u", i.state);
    CHECK(bdread(1, 1, 0) == BD_ST_NOTREADY, "read -> 80h");
    set_name(1, "HD16.IMG");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY, "back to HD16");
}

static void test_interlock(void) {
    uint8_t req[64];
    uint16_t id;
    info_t i;

    SECTION("PGDFS refuses to change open images");
    /* open: floppy \IMAGES\DOS.IMG, hard disk \HD16.IMG */
    CHECK(frame(0, 0x13, (const uint8_t *)"\\IMAGES\\DOS.IMG", 15) == 5, "DELETE floppy image -> 5");
    CHECK(frame(0, 0x13, (const uint8_t *)"\\hd16.img", 9) == 5, "DELETE hard disk image (lower case) -> 5");
    CHECK(file_exists("IMAGES/DOS.IMG") && file_exists("HD16.IMG"), "both still there");
    CHECK(make_image("IMAGES/SCRATCH.TMP", 1, 0, NULL, NULL, 0), "scratch file");
    CHECK(frame(0, 0x13, (const uint8_t *)"\\IMAGES\\*.*", 11) == 5, "wildcard DELETE over the image -> 5");
    CHECK(file_exists("IMAGES/DOS.IMG") && !file_exists("IMAGES/SCRATCH.TMP"), "the image survived, the rest went");
    CHECK(make_image("IMAGES/SCRATCH.TMP", 1, 0, NULL, NULL, 0), "scratch file again");
    /* RENAME: L srclen, src, dst */
    req[0] = 15;
    memcpy(req + 1, "\\IMAGES\\DOS.IMG\\IMAGES\\NEW.IMG", 30);
    CHECK(frame(0, 0x11, req, 31) == 5, "RENAME the image -> 5");
    req[0] = 7;
    memcpy(req + 1, "\\IMAGES\\OTHER", 13);
    CHECK(frame(0, 0x11, req, 14) == 5, "RENAME its directory -> 5");
    req[0] = 19;
    memcpy(req + 1, "\\IMAGES\\SCRATCH.TMP\\IMAGES\\S2.TMP", 33);
    CHECK(frame(0, 0x11, req, 34) == 0, "RENAME a neighbour works");
    /* OPEN: SS stack word (open mode), CC action, MM mode, path */
    memset(req, 0, 6);
    wr16(req, 2);                                   /* read/write */
    memcpy(req + 6, "\\IMAGES\\DOS.IMG", 15);
    CHECK(frame(0, 0x16, req, 21) == 5, "OPEN for read/write -> 5");
    wr16(req, 1);
    CHECK(frame(0, 0x16, req, 21) == 5, "OPEN for write -> 5");
    wr16(req, 0);
    CHECK(frame(0, 0x16, req, 21) == 0 && ans_len == 25, "OPEN read-only works");
    id = rd16(pl + 20);
    wr32(req, 0); wr16(req + 4, id); wr16(req + 6, 512);
    CHECK(frame(0, 0x08, req, 8) == 0 && ans_len == 512 && sector_ok(pl, 0, 0x11), "READFILE of the image works");
    wr16(req, id);
    CHECK(frame(0, 0x06, req, 2) == 0, "CLOSE");
    wr16(req, 0x20);                                /* CREATE, attributes */
    wr16(req + 2, 0); wr16(req + 4, 0);
    memcpy(req + 6, "\\IMAGES\\DOS.IMG", 15);
    CHECK(frame(0, 0x17, req, 21) == 5, "CREATE over the image -> 5");
    wr16(req, 0); wr16(req + 2, 0x12); wr16(req + 4, 2);   /* SPOPNFIL: replace if exists, read/write */
    CHECK(frame(0, 0x2E, req, 21) == 5, "SPOPNFIL replace -> 5");
    wr16(req + 2, 0x01); wr16(req + 4, 0);                  /* open if exists, read-only */
    CHECK(frame(0, 0x2E, req, 21) == 0, "SPOPNFIL open read-only works");
    wr16(req, rd16(pl + 20));
    frame(0, 0x06, req, 2);
    req[0] = 0x01;                                  /* SETATTR read-only */
    memcpy(req + 1, "\\IMAGES\\DOS.IMG", 15);
    CHECK(frame(0, 0x0E, req, 16) == 5, "SETATTR -> 5");
    CHECK(frame(0, 0x0F, (const uint8_t *)"\\IMAGES\\DOS.IMG", 15) == 0 && ans_len == 9, "GETATTR works");

    SECTION("PGDFS interlock by 8.3 alias");
    set_name(0, "TREE/Sub Directory/floppy disk.img");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.total == 720 && i.spt == 9 && i.cyl == 40,
          "360K image with long names: state %u %u/%u/%u", i.state, i.cyl, i.heads, i.spt);
    CHECK(frame(0, 0x13, (const uint8_t *)"\\TREE\\SUBDIR~1\\FLOPPY~1.IMG", 27) == 5, "DELETE by 8.3 path -> 5");
    req[0] = 14;
    memcpy(req + 1, "\\TREE\\SUBDIR~1\\TREE\\X", 21);
    CHECK(frame(0, 0x11, req, 22) == 5, "RENAME the 8.3 directory -> 5");
    CHECK(frame(0, 0x13, (const uint8_t *)"\\TREE\\SUBDIR~1\\*.IMG", 20) == 5, "wildcard DELETE by 8.3 -> 5");
    CHECK(file_exists("TREE/Sub Directory/floppy disk.img"), "still there");

    SECTION("closed images are fair game again");
    set_name(0, "");
    CHECK(frame(0, 0x13, (const uint8_t *)"\\TREE\\SUBDIR~1\\FLOPPY~1.IMG", 27) == 0, "DELETE after the eject works");
    set_name(0, "IMAGES/DOS.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY, "floppy back");
    CHECK(bd_path_is_open_image("\\IMAGES\\DOS.IMG") && bd_path_is_open_image("/images//dos.img/") &&
          bd_path_is_open_image("IMAGES\\DOS.IMG"), "path forms match");
    CHECK(!bd_path_is_open_image("\\IMAGES\\DOS.IM") && !bd_path_is_open_image("\\IMAGES") &&
          !bd_path_is_open_image("\\") && !bd_path_is_open_image(""), "near misses do not");
    CHECK(bd_path_holds_open_image("\\IMAGES") && !bd_path_holds_open_image("\\IMAGE"), "directory prefix");
}

/* An image of `sectors` patterned sectors in `frags` fragments: a filler file
 * grows by one cluster (one sector on this volume) between the fragments. */
static bool make_fragmented(const char *path, const char *filler, uint32_t sectors, uint32_t frags, uint8_t tag) {
    FIL a, b;
    UINT bw;
    uint8_t s[512];
    uint32_t lba = 0;
    bool ok = f_open(&a, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    ok = ok && f_open(&b, filler, FA_WRITE | FA_OPEN_APPEND) == FR_OK;
    for (uint32_t f = 0; ok && f < frags; f++) {
        uint32_t n = (f == frags - 1) ? sectors - lba : sectors / frags;
        for (uint32_t k = 0; ok && k < n; k++, lba++) {
            fill_sector(s, lba, tag);
            ok = f_write(&a, s, 512, &bw) == FR_OK && bw == 512;
        }
        ok = ok && f_sync(&a) == FR_OK;
        ok = ok && f_write(&b, s, 512, &bw) == FR_OK && f_sync(&b) == FR_OK;
    }
    f_close(&a);
    f_close(&b);
    return ok;
}

static bool reads_ok(uint8_t unit, uint32_t total, uint8_t tag) {
    /* backwards across the fragment boundaries, 8 sectors at a time */
    for (int32_t l = (int32_t)total - 8; l >= 0; l -= 37) {
        if (bdread(unit, 8, (uint32_t)l) != 0) return false;
        for (int k = 0; k < 8; k++) if (!sector_ok(pl + 512 * k, (uint32_t)l + k, tag)) return false;
    }
    return true;
}

static void test_fastseek_and_faults(void) {
    uint8_t s[512];
    info_t i;

    SECTION("fragmented images: link map pool");
    CHECK(make_fragmented("FRAG3.IMG", "FILL.TMP", 2880, 3, 0x5A), "3-fragment floppy image");
    CHECK(make_fragmented("FRAG60.IMG", "FILL.TMP", 2880, 60, 0x5B), "60-fragment floppy image");
    CHECK(make_fragmented("FRAG90.IMG", "FILL.TMP", 2880, 90, 0x5C), "90-fragment floppy image");
    CHECK(make_fragmented("HDF150.IMG", "FILL.TMP", 8192, 150, 0x5D), "150-fragment hard disk image");
    CHECK(make_fragmented("HDF300.IMG", "FILL.TMP", 8192, 300, 0x5E), "300-fragment hard disk image");

    set_name(0, "FRAG3.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.total == 2880 && bd_test_fastseek(0),
          "FRAG3: ready with a link map (state %u)", i.state);
    CHECK(reads_ok(0, 2880, 0x5A), "FRAG3: random reads correct");
    set_name(0, "FRAG90.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && !bd_test_fastseek(0),
          "FRAG90: floppy map over its share -> documented chain-walk fallback (state %u)", i.state);
    CHECK(reads_ok(0, 2880, 0x5C), "FRAG90: random reads correct without the map");
    fill_sector(s, 1500, 0xC3);
    CHECK(bdwrite(0, 1, 1500, s, 512) == 0 && bdread(0, 1, 1500) == 0 && sector_ok(pl, 1500, 0xC3), "FRAG90: write");

    /* floppy map at the bottom, hard disk map after it; then the floppy
     * map is released and rebuilt: the hard disk map moves down under it */
    set_name(0, "FRAG60.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && bd_test_fastseek(0), "FRAG60: map (122 DWORDs)");
    set_name(1, "HDF150.IMG");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY && bd_test_fastseek(1) && i.total == 8192,
          "HDF150: map (302 DWORDs) fits next to the floppy's (state %u)", i.state);
    CHECK(reads_ok(1, 8192, 0x5D), "HDF150: random reads correct");
    set_name(0, "FRAG3.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && bd_test_fastseek(0), "floppy swapped: new map");
    CHECK(reads_ok(1, 8192, 0x5D), "HDF150: still correct after its map moved");
    CHECK(reads_ok(0, 2880, 0x5A), "FRAG3: correct");
    set_name(0, "FRAG60.IMG");
    CHECK(get_info(0, 0, &i) == 0 && bd_test_fastseek(0) && reads_ok(0, 2880, 0x5B) && reads_ok(1, 8192, 0x5D),
          "both maps correct after another swap");

    set_name(1, "HDF300.IMG");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_FRAGMENTED && i.type == BD_TYPE_HARDDISK &&
          i.token == 0 && i.total == 0, "HDF300: map does not fit -> state %u (5)", i.state);
    CHECK(bdread(1, 1, 0) == BD_ST_NOTREADY, "HDF300: read -> 80h");
    CHECK(!bd_test_fastseek(1), "no map held");
    set_name(0, "");
    get_info(0, 0, &i);
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_FRAGMENTED, "HDF300: too fragmented even for the whole pool");
    set_name(1, "HDF150.IMG");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY && reads_ok(1, 8192, 0x5D), "HDF150 again");
    set_name(1, "HD16.IMG");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY, "back to HD16");

    SECTION("disk faults -> 20h, then recovery");
    set_name(0, "IMAGES/DOS.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY, "floppy back");
    ramdisk_set_read_fault(true);
    CHECK(bdread(0, 1, 2222) == BD_ST_CTRLFAIL && ans_len == 0, "read fault -> 20h");
    ramdisk_set_read_fault(false);
    CHECK(bdread(0, 1, 2222) == 0 && sector_ok(pl, 2222, 0x11), "next read works again");
    ramdisk_set_write_fault(true);
    fill_sector(s, 2222, 0x11);
    CHECK(bdwrite(0, 1, 2222, s, 512) == BD_ST_CTRLFAIL, "write fault -> 20h");
    ramdisk_set_write_fault(false);
    CHECK(bdwrite(0, 1, 2222, s, 512) == 0, "next write works again");
    CHECK(bdread(0, 1, 2223) == 0 && sector_ok(pl, 2223, 0x11), "and reads");

    SECTION("long path: display name keeps the tail");
    {
        const char *dirs[] = { "D1234567", "D1234567/D2345678", "D1234567/D2345678/D3456789",
                               "D1234567/D2345678/D3456789/D4567890", "D1234567/D2345678/D3456789/D4567890/D5678901",
                               "D1234567/D2345678/D3456789/D4567890/D5678901/D6789012", NULL };
        const char *path = "D1234567/D2345678/D3456789/D4567890/D5678901/D6789012/LASTDISK.IMG";
        for (int k = 0; dirs[k]; k++) f_mkdir(dirs[k]);
        CHECK(make_image(path, 720, 0x61, NULL, NULL, 0), "deep image");
        set_name(0, path);
        CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.nlen == 63 &&
              strncmp(i.name, "...", 3) == 0 && strcmp(i.name + 63 - 12, "LASTDISK.IMG") == 0,
              "name %u '%s'", i.nlen, i.name);
    }
}

static void test_tokens(void) {
    info_t i;
    uint32_t t0, t1;
    uint8_t s[512];

    SECTION("image tokens");
    set_name(0, "IMAGES/DOS.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.token != 0, "floppy token %08X", i.token);
    t0 = i.token;
    CHECK(get_info(0, 0, &i) == 0 && i.token == t0, "stable across BDINFO");
    CHECK(bdread_t(0, 1, 0, t0 ^ 1) == BD_ST_CHANGED && ans_len == 0, "floppy wrong token -> 06h");
    CHECK(bdread_t(0, 1, 0, 0) == BD_ST_CHANGED, "floppy token 0 -> 06h");
    CHECK(bdread_t(0, 1, 5000, t0 ^ 1) == BD_ST_CHANGED, "token checked before the range");
    CHECK(bdread_t(0, 1, 0, t0) == 0 && sector_ok(pl, 0, 0x11), "right token reads");
    get_info(1, 0, &i);
    t1 = i.token;
    CHECK(i.state == BD_STATE_READY && t1 != 0 && t1 != t0, "hard disk token %08X differs", t1);
    CHECK(bdread_t(1, 1, 0, t1 + 1) == BD_ST_NOTREADY, "hard disk wrong token -> 80h");
    CHECK(bdread_t(1, 1, 0, t0) == BD_ST_NOTREADY, "the floppy's token on the hard disk -> 80h");
    tok[1] = t1 + 1;
    fill_sector(s, 1000, 0xEE);
    CHECK(bdwrite(1, 1, 1000, s, 512) == BD_ST_NOTREADY, "hard disk write with a wrong token -> 80h");
    tok[1] = t1;
    CHECK(bdread(1, 1, 1000) == 0 && sector_ok(pl, 1000, 0xD1), "and nothing was written");
    tok[0] = t0 ^ 1;
    CHECK(bdwrite(0, 1, 5, s, 512) == BD_ST_CHANGED, "floppy write with a wrong token -> 06h");
    bd_ctl_opts_write(BD_OPT_FD_RO);
    CHECK(bdwrite(0, 1, 5, s, 512) == BD_ST_CHANGED, "token checked before write protection");
    bd_ctl_opts_write(0);
    tok[0] = t0;

    /* a swap to the same file is still a new token (the ROM must refresh) */
    set_name(0, "IMAGES/DOS.IMG");
    CHECK(bdread_t(0, 1, 0, t0) == BD_ST_CHANGED, "same name committed again: old token -> 06h");
    CHECK(get_info(0, 0, &i) == 0 && i.token != t0 && (i.flags & BD_FLAG_CHANGED), "new token %08X, changed", i.token);
    CHECK(bdread(0, 1, 0) == 0, "new token reads");
    t0 = i.token;
    /* OPEN also gives a new token */
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.token != t1, "hard disk OPEN: new token");
    CHECK(bdread_t(1, 1, 0, t1) == BD_ST_NOTREADY, "the token from before the OPEN -> 80h");
    /* a hard disk commit does not touch the token */
    t1 = i.token;
    set_name(1, "HD8.IMG");
    CHECK(get_info(1, 0, &i) == 0 && i.token == t1 && bdread(1, 1, 0) == 0, "hard disk commit keeps the token");
    set_name(1, "HD16.IMG");
    CHECK(get_info(1, 0, &i) == 0 && i.token == t1, "and so does committing the old name back");
    CHECK(get_info(0, 0, &i) == 0 && i.token == t0, "floppy token unchanged by all that");
}

static void test_sync(void) {
    info_t i;
    uint8_t s[512];
    FILINFO fi;
    const char *img = "IMAGES/DOS.IMG";

    SECTION("write-back of the directory entry");
    set_name(0, img);                           /* fresh open: nothing synced yet */
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY, "floppy open");
    CHECK(f_chmod(img, 0, AM_ARC) == FR_OK && f_stat(img, &fi) == FR_OK && !(fi.fattrib & AM_ARC), "archive bit cleared");
    test_ms += 5000;
    fill_sector(s, 10, 0x11);
    CHECK(bdwrite(0, 1, 10, s, 512) == 0, "first write");
    CHECK(f_stat(img, &fi) == FR_OK && (fi.fattrib & AM_ARC), "first write after the open synced the entry");
    CHECK(f_chmod(img, 0, AM_ARC) == FR_OK, "clear the archive bit again");
    fill_sector(s, 11, 0xA7);
    CHECK(bdwrite(0, 1, 11, s, 512) == 0, "second write");
    CHECK(file_sector(img, 11, s) && sector_ok(s, 11, 0xA7), "the data is on the disk at once");
    CHECK(f_stat(img, &fi) == FR_OK && !(fi.fattrib & AM_ARC), "entry not synced yet");
    test_ms += 500;
    bd_tasks();
    CHECK(f_stat(img, &fi) == FR_OK && !(fi.fattrib & AM_ARC), "not after 0.5 s");
    fill_sector(s, 12, 0x11);
    CHECK(bdwrite(0, 1, 12, s, 512) == 0, "another write restarts the idle time");
    test_ms += 700;
    bd_tasks();
    CHECK(f_stat(img, &fi) == FR_OK && !(fi.fattrib & AM_ARC), "0.7 s after the last write: still pending");
    test_ms += 400;
    bd_tasks();
    CHECK(f_stat(img, &fi) == FR_OK && (fi.fattrib & AM_ARC), "synced after 1 s idle");
    /* a pending entry is written back when the image is closed */
    CHECK(f_chmod(img, 0, AM_ARC) == FR_OK, "clear the archive bit");
    fill_sector(s, 11, 0x11);
    CHECK(bdwrite(0, 1, 11, s, 512) == 0, "write, then swap at once");
    set_name(0, "IMAGES/B720.IMA");
    CHECK(f_stat(img, &fi) == FR_OK && (fi.fattrib & AM_ARC), "the swap's close synced the entry");
    CHECK(file_sector(img, 11, s) && sector_ok(s, 11, 0x11), "data restored");
    /* sync failure is retried */
    set_name(0, img);
    get_info(0, 0, &i);
    fill_sector(s, 13, 0x11);
    CHECK(bdwrite(0, 1, 13, s, 512) == 0 && bdwrite(0, 1, 13, s, 512) == 0, "two writes, the second leaves a pending sync");
    CHECK(f_chmod(img, 0, AM_ARC) == FR_OK, "clear the archive bit");
    ramdisk_set_write_fault(true);
    {
        uint32_t w0 = msc_app_get_stats()->writes, w1, fails0 = msc_app_get_stats()->write_csw_err;
        test_ms += 1500;
        bd_tasks();
        CHECK(msc_app_get_stats()->write_csw_err > fails0, "idle sync attempted and failed");
        ramdisk_set_write_fault(false);
        w0 = msc_app_get_stats()->writes;
        test_ms += 500;
        bd_tasks();
        CHECK(msc_app_get_stats()->writes == w0, "no retry before another idle second");
        test_ms += 600;
        bd_tasks();
        w1 = msc_app_get_stats()->writes;
        CHECK(w1 > w0, "retried after another idle second (%u writes)", w1 - w0);
        test_ms += 5000;
        bd_tasks();
        CHECK(msc_app_get_stats()->writes == w1, "nothing pending afterwards");
    }
    CHECK(f_stat(img, &fi) == FR_OK && (fi.fattrib & AM_ARC), "entry written back");
    CHECK(bdread(0, 1, 13) == 0 && sector_ok(pl, 13, 0x11), "the unit still serves");
}

static void test_card_reboot(void) {
    info_t i;
    uint32_t t0, t1;

    SECTION("card reboot: nothing opens until OPEN");
    set_name(0, "IMAGES/DOS.IMG");
    get_info(0, 0, &i);
    t0 = i.token;
    get_info(1, 0, &i);
    t1 = i.token;
    CHECK(t0 && t1, "both units ready before the reboot");
    /* the card reboots (mode switch): settings keep the names, the drive mounts again */
    unmount_drive();
    CHECK(strcmp(fd_name, "IMAGES/DOS.IMG") == 0 && strcmp(hd_name, "HD16.IMG") == 0, "settings hold both names");
    dfs_server_init();
    bd_init(fd_name, hd_name, &bd_opts);
    bd_set_boot_nonce(0x12345678);
    /* the ROM's wait loop asks without OPEN before the stick has mounted */
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NODRIVE && i.token == 0,
          "configured floppy before the mount after a card boot: state %u (want NODRIVE)", i.state);
    CHECK(get_info(1, 0, &i) == 0 && i.state == BD_STATE_NODRIVE, "configured hard disk before the mount: state %u", i.state);
    mount_drive();
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NONE && i.token == 0 && i.nlen == 0,
          "floppy not opened at mount: state %u", i.state);
    CHECK(get_info(1, 0, &i) == 0 && i.state == BD_STATE_NONE && i.token == 0, "hard disk not opened: state %u", i.state);
    CHECK(bdread_t(0, 1, 0, t0) == BD_ST_NOTREADY && bdread_t(1, 1, 0, t1) == BD_ST_NOTREADY,
          "the old system's requests -> 80h");
    CHECK(!bd_path_is_open_image("\\HD16.IMG"), "nothing open for the interlock");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY && i.token != 0 && i.token != t1,
          "OPEN after the reboot: ready, new token %08X", i.token);
    CHECK(bdread_t(1, 1, 0, t1) == BD_ST_NOTREADY, "old hard disk token still refused");
    CHECK(bdread(1, 1, 1000) == 0 && sector_ok(pl, 1000, 0xD1), "new token reads");
    CHECK(get_info(0, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY && i.token != t0, "floppy OPEN");
    CHECK(bdread_t(0, 1, 0, t0) == BD_ST_CHANGED, "old floppy token -> 06h");
    /* a floppy commit arms the floppy too (an explicit swap by the user) */
    unmount_drive();
    bd_init(fd_name, hd_name, &bd_opts);
    mount_drive();
    set_name(0, "IMAGES/DOS.IMG");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY, "floppy commit after a card boot opens it");
    CHECK(get_info(1, 0, &i) == 0 && i.state == BD_STATE_NONE, "the hard disk stays closed");
    CHECK(get_info(1, BD_INFO_OPEN, &i) == 0 && i.state == BD_STATE_READY, "until OPEN");
}

static void test_unmount(void) {
    info_t i;
    uint8_t g0, g1;
    uint32_t t0, t1;

    SECTION("unmount / remount");
    set_name(0, "IMAGES/DOS.IMG");
    get_info(0, 0, &i);
    g0 = i.gen;
    t0 = i.token;
    get_info(1, 0, &i);
    g1 = i.gen;
    t1 = i.token;
    unmount_drive();
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NODRIVE && i.token == 0 && i.total == 0 && i.gen == g0,
          "floppy after unmount: state %u, token 0, same generation", i.state);
    CHECK(get_info(1, 0, &i) == 0 && i.state == BD_STATE_NODRIVE && i.gen == g1, "hard disk: state %u", i.state);
    CHECK(bdread_t(0, 1, 0, t0) == BD_ST_NOTREADY && bdread_t(1, 1, 0, t1) == BD_ST_NOTREADY, "reads -> 80h");
    CHECK(!bd_path_is_open_image("\\IMAGES\\DOS.IMG"), "nothing open");
    mount_drive();
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.token == t0 && i.gen == g0 && !(i.flags & BD_FLAG_CHANGED),
          "replug: floppy reopened with the same token, no media change (flags %02X)", i.flags);
    CHECK(get_info(1, 0, &i) == 0 && i.state == BD_STATE_READY && i.token == t1 && i.gen == g1, "hard disk: same token");
    CHECK(bdread_t(1, 1, 1000, t1) == 0 && sector_ok(pl, 1000, 0xD1) && bdread_t(0, 1, 0, t0) == 0,
          "the running system goes on with its tokens");

    /* the image was replaced on the stick while it was out: new token */
    unmount_drive();
    f_mount(&fatfs, "", 1);
    CHECK(f_unlink("IMAGES/DOS.IMG") == FR_OK && make_image("FILLER.TMP", 1, 0, NULL, NULL, 0) &&
          make_image("IMAGES/DOS.IMG", 2880, 0x12, NULL, NULL, 0), "floppy image rewritten elsewhere");
    f_unmount("");
    mount_drive();
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.token != t0 && (i.flags & BD_FLAG_CHANGED) &&
          i.gen == (uint8_t)(g0 + 1), "different file: new token, media change (gen %u)", i.gen);
    CHECK(bdread_t(0, 1, 0, t0) == BD_ST_CHANGED, "old floppy token -> 06h");
    CHECK(bdread(0, 1, 7) == 0 && sector_ok(pl, 7, 0x12), "new content");

    /* a floppy swap while unplugged is remembered */
    unmount_drive();
    set_name(0, "Long Image Name.img");
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_NODRIVE && strcmp(i.name, "\\Long Image Name.img") == 0, "swap while unplugged");
    mount_drive();
    CHECK(get_info(0, 0, &i) == 0 && i.state == BD_STATE_READY && i.total == 1440,
          "remount opens the new floppy: state %u total %u flags %02X", i.state, i.total, i.flags);
    CHECK(get_info(1, 0, &i) == 0 && i.state == BD_STATE_READY && i.total == 32768 && i.token == t1, "and the hard disk");
    CHECK(bdread(1, 1, 1000) == 0 && sector_ok(pl, 1000, 0xD1), "hard disk content survived");
    /* PGDFS's own requests still behave */
    CHECK(frame(0, 0xF0, (const uint8_t *)"echo", 4) == 0 && ans_len == 4, "ECHO");
    CHECK(frame(0, 0xF2, NULL, 0) == 0 && ans_len == 60, "DIAG");
}

int main(void) {
    test_floppy_geometry();
    test_harddisk_geometry();
    test_registers();
    test_setup();
    if (n_fails == 0) {
        test_no_drive();
        test_floppy();
        test_harddisk();
        test_interlock();
        test_fastseek_and_faults();
        test_tokens();
        test_sync();
        test_unmount();
        test_card_reboot();
    }
    unmount_drive();
    ramdisk_free();
    printf("PGBOOT tests: %d checks, %d failures -> %s\n", n_checks, n_fails, n_fails ? "FAIL" : "PASS");
    return n_fails ? 1 : 0;
}
