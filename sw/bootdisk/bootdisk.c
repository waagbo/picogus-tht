/*
 *  PGBOOT: floppy / hard disk image server for the boot ROM.
 *
 *  See PROTOCOL.md for the wire protocol and bootdisk.h for the API.
 *
 *  Threading (same model as PGDFS, sw/dfs/dfs.h):
 *    - bd_ctl_* run on core 0 inside an ISA bus cycle: O(1), no FatFs, no
 *      loops. They read and write the settings storage bound by bd_init()
 *      (the name buffers and the options byte) and bump a per-unit commit
 *      sequence number when a name is committed.
 *    - Everything else runs on core 1 (bd_tasks() from dfs_tasks(), the
 *      mount hooks from the USB glue, bd_process() from dfs_process()).
 *      Core 1 copies a name out of the settings buffer only between two
 *      reads of the commit sequence that agree, so a name core 0 is still
 *      writing is never half-applied for good: the commit that ends the
 *      write bumps the sequence and core 1 copies again.
 *
 *  Each unit keeps its image open in its own FIL with a fast seek cluster
 *  link map, so a random sector read costs one seek in the map instead of a
 *  walk down the FAT chain. Written sectors reach the drive before the
 *  answer; the directory entry follows when the unit goes idle.
 *
 *  Safety against serving the wrong disk: after a card boot no unit opens
 *  until the ROM asks (BDINFO OPEN), and every BDREAD/BDWRITE carries the
 *  image token from BDINFO, which covers the card boot (nonce), the explicit
 *  open and the file itself.
 *
 *  Portable C: no Pico SDK headers, so the host tests and the emulator's
 *  card simulator build the same file.
 *
 *  Copyright (C) 2026  PicoGUS contributors
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */
#include <string.h>
#include "ff.h"
#include "bootdisk.h"
#include "bd_geometry.h"
#include "../dfs/dfs_server.h"         /* dfs_platform_millis() */
#ifndef PICOGUS_NO_MODENAMES
#define PICOGUS_NO_MODENAMES            /* only the register numbers are wanted */
#endif
#include "../../common/picogus.h"       /* CMD_* for the menu's choices */

#if FF_FS_READONLY || FF_MAX_SS != 512
#error "PGBOOT needs FatFs with write support and 512-byte sectors"
#endif

#define BD_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* Normalised path: a leading '\' is added to the configured name. The same
 * path spelled with 8.3 aliases can be longer ("a b" -> "AB~1"). */
#define BD_PATH_BUF     (BD_NAME_BUF + 1)
#define BD_SFN_BUF      (BD_NAME_BUF + 48)

/* Fast seek cluster link maps (CLMT) come from one pool shared by the two
 * units, each map sized to what its file needs: 2 DWORDs per fragment + 2.
 * The floppy may take at most BD_CLMT_FD_MAX of it; a floppy image whose map
 * does not fit is served with plain seeks, which walk its short FAT chain
 * (at most 2.88 MB). A hard disk image whose map does not fit the rest of
 * the pool is refused (BD_STATE_FRAGMENTED): walking the chain of a large
 * image on every seek would stall core 1 for too long. */
#define BD_CLMT_POOL    512             /* DWORDs: 2 KB, about 250 fragments */
#define BD_CLMT_FD_MAX  128             /* DWORDs: 63 fragments */

/* Write-back of the directory entry (time stamp, archive bit): data sectors
 * go straight to the drive in f_write(); the entry is synced on the first
 * write after an open, then once the unit has been idle this long. */
#define BD_SYNC_IDLE_MS 1000

/* FatFs private FIL.flag bits (ff.c). FA_DIRTY: the FIL's sector buffer
 * holds data not yet written; whole-sector aligned writes never set it,
 * checked anyway. FA_MODIFIED: the directory entry needs a write-back. */
#ifndef FA_DIRTY
#define FA_DIRTY        0x80
#endif
#ifndef FA_MODIFIED
#define FA_MODIFIED     0x40            /* FatFs private: the entry needs a write-back */
#endif

/* ---- core 0 state (register handlers) ------------------------------------- */

static char    *cfg_name[BD_UNITS];         /* settings storage, BD_NAME_BUF each */
static uint8_t *cfg_opts;
static uint8_t *cfg_menu;                   /* CMD_BDMENU, CMD_BDTUNE storage */
static uint8_t *cfg_tune;
static uint8_t *cfg_theme;
static uint8_t  modes_available;            /* bit n: the card can be switched to mode n */
static uint8_t  mode_current;
static char    *cfg_melody;                 /* BD_MELODY_BUF, the user's melody as RTTTL */
static uint16_t melody_wr, melody_rd;       /* core 0 only */
static uint16_t tunedata_rd;                /* core 0 only */
static volatile uint32_t tune_seq;          /* written by core 0 only: tune or melody changed */

/* The selected tune compiled for the ROM (bd_tune.h). Core 1 writes it (and
 * the main thread before core 1 starts), core 0 reads it byte by byte: while
 * it is not ready, and past its end, CMD_BDTUNEDATA reads FFh. */
static uint8_t  tune_stream[BD_TUNE_STREAM_MAX];
static volatile uint16_t tune_len;
static volatile bool     tune_ready;
static uint32_t tune_applied_seq;           /* core 1 */
static uint8_t  wr_pos[BD_UNITS];           /* core 0 only */
static uint8_t  rd_pos[BD_UNITS];           /* core 0 only */
static volatile uint32_t commit_seq[BD_UNITS];  /* written by core 0 only */

/* ---- core 1 state ---------------------------------------------------------- */

typedef struct {
    FIL       fil;
    bool      open;             /* fil is open on the mounted volume */
    bool      file_ro;          /* opened read-only (AM_RDO attribute or write-protected) */
    bool      vhd;              /* fixed VHD: the footer is not part of the disk */
    bool      changed;          /* media changed since the last BDINFO */
    bool      sync_pending;     /* written since the last f_sync() */
    bool      synced_once;      /* f_sync() done since the open */
    uint8_t   state;            /* BD_STATE_* */
    uint8_t   gen;              /* media generation */
    uint16_t  clmt_off;         /* the unit's link map in clmt_pool, 0 DWORDs = none */
    uint16_t  clmt_len;
    uint32_t  epoch;            /* explicit (re)opens: BDINFO OPEN, floppy commit */
    uint32_t  token;            /* image token of the last open image (kept across an unplug) */
    uint32_t  last_write_ms;
    uint32_t  applied_seq;      /* commit_seq value the path was copied at */
    bd_geom_t geom;
    char      path[BD_PATH_BUF];/* active image path, normalised; "" = none */
    char      sfn[BD_SFN_BUF];  /* the same by 8.3 aliases while open; "" = unknown */
} bd_unit_t;

static bd_unit_t units[BD_UNITS];
static bool      mounted;
static uint32_t  boot_nonce;
#if FF_USE_FASTSEEK
static DWORD     clmt_pool[BD_CLMT_POOL];
#endif
static FILINFO   fno;               /* 8.3 alias lookup, BDLIST */
static DIR       list_dir;          /* BDLIST */
static char      list_path[BD_PATH_BUF];
static bd_sec0_t sec0;              /* identification scratch */
static uint8_t   scratch[128];
_Static_assert(sizeof(scratch) >= BD_NAME_BUF, "scratch holds a path for BDLIST");
static char      list_have[BD_PATH_BUF];    /* BDLIST: an image name as configured, normalised */
_Static_assert(BD_LIST_FLOPPY == BD_KIND_FLOPPY && BD_LIST_HARDDISK == BD_KIND_HARDDISK,
               "the list of one kind of image has that kind's number");

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static bool is_sep(char c) { return c == '\\' || c == '/'; }

/* ================= core 0: control registers ================================ */

void bd_ctl_name_select(uint8_t unit) {
    if (unit >= BD_UNITS) return;
    wr_pos[unit] = 0;
    rd_pos[unit] = 0;
}

void bd_ctl_name_write(uint8_t unit, uint8_t c) {
    char *n;
    if (unit >= BD_UNITS || (n = cfg_name[unit]) == NULL) return;
    if (c == 0) {
        n[wr_pos[unit]] = 0;        /* wr_pos <= BD_NAME_MAX */
        wr_pos[unit] = 0;
        BD_FENCE();                 /* the name is complete before the commit shows */
        commit_seq[unit] = commit_seq[unit] + 1;
        return;
    }
    if (wr_pos[unit] < BD_NAME_MAX) {   /* longer names are cut, the ROM shows what stuck */
        n[wr_pos[unit]++] = (char)c;
        n[wr_pos[unit]] = 0;        /* always terminated, even before the commit */
    }
}

uint8_t bd_ctl_name_read(uint8_t unit) {
    const char *n;
    uint8_t p, c;
    if (unit >= BD_UNITS || (n = cfg_name[unit]) == NULL) return 0;
    p = rd_pos[unit];
    c = (p < BD_NAME_MAX) ? (uint8_t)n[p] : 0;
    rd_pos[unit] = c ? (uint8_t)(p + 1) : 0;    /* the terminator rewinds */
    return c;
}

uint8_t bd_ctl_opts_read(void) {
    return (uint8_t)((cfg_opts ? (*cfg_opts & BD_OPT_MASK) : 0) | BD_OPTS_SIGNATURE);
}

void bd_ctl_opts_write(uint8_t v) {
    if (cfg_opts) *cfg_opts = (uint8_t)(v & BD_OPT_MASK);
}

/* Unusable values never get stored, so the ROM need not check them. */
static uint8_t menu_clean(uint8_t v) {
    uint8_t mode = v & BD_MENU_MODE_MASK;
    uint8_t wait = (uint8_t)((v >> BD_MENU_WAIT_SHIFT) & BD_MENU_WAIT_MAX);
    if (mode > BD_MENU_ALWAYS) mode = BD_MENU_KEY;
    if (wait == 0) wait = BD_MENU_WAIT_DEFAULT;
    return (uint8_t)((wait << BD_MENU_WAIT_SHIFT) | mode);
}

uint8_t bd_ctl_menu_read(void) {
    return (uint8_t)(menu_clean(cfg_menu ? *cfg_menu : BD_MENU_DEFAULT) | BD_UI_SIGNATURE);
}

void bd_ctl_menu_write(uint8_t v) {
    if (cfg_menu) *cfg_menu = menu_clean(v);
}

static bool tune_valid(uint8_t t) {
    return t <= BD_TUNE_BUILTIN || (t == BD_TUNE_CUSTOM && cfg_melody && cfg_melody[0]);
}

uint8_t bd_ctl_tune_read(void) {
    uint8_t t = cfg_tune ? *cfg_tune : BD_TUNE_DEFAULT;
    if (t > BD_TUNE_BUILTIN && t != BD_TUNE_CUSTOM) t = 0;
    return (uint8_t)(t | BD_UI_SIGNATURE);
}

void bd_ctl_tune_write(uint8_t v) {
    if (!cfg_tune) return;
    if (v == BD_UI_NEXT) {                  /* off, 1 .. the last built-in, the user's if there is one */
        v = *cfg_tune;
        v = (v == BD_TUNE_CUSTOM) ? 0 : (v < BD_TUNE_BUILTIN) ? (uint8_t)(v + 1) : BD_TUNE_CUSTOM;
        if (!tune_valid(v)) v = 0;
    } else if (v > BD_TUNE_BUILTIN && v != BD_TUNE_CUSTOM) {
        return;                             /* no such tune: not stored */
    }
    *cfg_tune = v;
    BD_FENCE();
    tune_seq = tune_seq + 1;
}

uint8_t bd_ctl_theme_read(void) {
    uint8_t t = cfg_theme ? *cfg_theme : BD_THEME_DEFAULT;
    if (t > BD_THEME_MAX) t = BD_THEME_DEFAULT;
    return (uint8_t)(t | BD_UI_SIGNATURE);
}

void bd_ctl_theme_write(uint8_t v) {
    if (!cfg_theme) return;
    if (v == BD_UI_NEXT) v = (*cfg_theme >= BD_THEME_MAX) ? 0 : (uint8_t)(*cfg_theme + 1);
    if (v <= BD_THEME_MAX) *cfg_theme = v;
}

void bd_ctl_melody_select(void) {
    melody_wr = 0;
    melody_rd = 0;
}

void bd_ctl_melody_write(uint8_t c) {
    if (!cfg_melody) return;
    if (c == 0) {
        cfg_melody[melody_wr] = 0;          /* melody_wr <= BD_MELODY_MAX */
        melody_wr = 0;
        BD_FENCE();                         /* the text is complete before core 1 compiles it */
        tune_seq = tune_seq + 1;
        return;
    }
    if (melody_wr < BD_MELODY_MAX) {
        cfg_melody[melody_wr++] = (char)c;
        cfg_melody[melody_wr] = 0;
    }
}

uint8_t bd_ctl_melody_read(void) {
    uint8_t c;
    if (!cfg_melody) return 0;
    c = (melody_rd < BD_MELODY_MAX) ? (uint8_t)cfg_melody[melody_rd] : 0;
    melody_rd = c ? (uint16_t)(melody_rd + 1) : 0;
    return c;
}

void bd_ctl_tunedata_select(void) {
    tunedata_rd = 0;
}

uint8_t bd_ctl_tunedata_read(void) {
    if (!tune_ready || tunedata_rd >= tune_len) return 0xFF;
    return tune_stream[tunedata_rd++];
}

/* The stream of the tune the setting names. Runs on core 1 (bd_tasks) and,
 * before core 1 exists, in bd_init_ui(). */
static void tune_compile(void) {
    uint8_t t;
    size_t n;
    tune_ready = false;
    BD_FENCE();
    tune_applied_seq = tune_seq;
    BD_FENCE();
    t = cfg_tune ? *(volatile uint8_t *)cfg_tune : BD_TUNE_DEFAULT;
    if (t >= 1 && t <= BD_TUNE_BUILTIN) {
        n = bd_tune_compile(bd_tune_builtin[t - 1], NULL, tune_stream);
    } else if (t == BD_TUNE_CUSTOM && cfg_melody) {
        n = bd_tune_compile(cfg_melody, "Custom", tune_stream);
    } else {
        n = bd_tune_compile("", "Off", tune_stream);
    }
    tune_len = (uint16_t)n;
    BD_FENCE();
    tune_ready = true;
}

/* ================= core 1 ==================================================== */

static uint8_t opts_now(void) {
    return cfg_opts ? (uint8_t)(*(volatile uint8_t *)cfg_opts & BD_OPT_MASK) : 0;
}

static bool unit_ro(int u) {
    uint8_t bit = (u == BD_UNIT_FD) ? BD_OPT_FD_RO : BD_OPT_HD_RO;
    return units[u].file_ro || (opts_now() & bit);
}

/* "E:/Dir//img.IMA" -> "\Dir\img.IMA": drive letter dropped, one kind of
 * separator, no doubles, a leading one, no trailing one. "" stays "". */
static void norm_path(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (src[0] == 0) {
        dst[0] = 0;
        return;
    }
    if (((src[0] >= 'A' && src[0] <= 'Z') || (src[0] >= 'a' && src[0] <= 'z')) && src[1] == ':') src += 2;
    dst[n++] = '\\';
    for (; *src && n < cap - 1; src++) {
        char c = is_sep(*src) ? '\\' : *src;
        if (c == '\\' && dst[n - 1] == '\\') continue;
        dst[n++] = c;
    }
    while (n > 1 && dst[n - 1] == '\\') n--;
    dst[n] = 0;
}

/* Copy unit u's configured name into its active path. */
static void fetch_name(int u) {
    bd_unit_t *un = &units[u];
    uint32_t s;
    do {
        s = commit_seq[u];
        BD_FENCE();
        if (cfg_name[u]) {
            norm_path(un->path, sizeof(un->path), cfg_name[u]);
        } else {
            un->path[0] = 0;
        }
        BD_FENCE();
    } while (s != commit_seq[u]);
    un->applied_seq = s;
}

/* ---- fast seek link map pool -------------------------------------------------- */

/* Invariant: a unit holding a map has it at clmt_off with clmt_len > 0, and
 * when only one map exists it starts at 0; a new map goes right after the
 * existing one. Releasing the lower map moves the upper one down (the maps
 * hold cluster numbers only, and core 1 is between FatFs calls here). */
static void clmt_release(int u) {
#if FF_USE_FASTSEEK
    bd_unit_t *un = &units[u], *ot = &units[1 - u];
    if (un->clmt_len && ot->clmt_len && ot->clmt_off > un->clmt_off) {
        memmove(&clmt_pool[un->clmt_off], &clmt_pool[ot->clmt_off], (size_t)ot->clmt_len * sizeof(DWORD));
        ot->clmt_off = un->clmt_off;
        ot->fil.cltbl = &clmt_pool[ot->clmt_off];
    }
    un->clmt_off = 0;
    un->clmt_len = 0;
    un->fil.cltbl = NULL;
#else
    (void)u;
#endif
}

/* Build unit u's map. BD_STATE_READY, BD_STATE_FRAGMENTED (hard disk map
 * does not fit) or BD_STATE_UNUSABLE (disk error). */
static uint8_t clmt_build(int u) {
#if FF_USE_FASTSEEK
    bd_unit_t *un = &units[u], *ot = &units[1 - u];
    uint32_t off = ot->clmt_len ? (uint32_t)ot->clmt_off + ot->clmt_len : 0;
    uint32_t room = BD_CLMT_POOL - off;
    DWORD *tbl = &clmt_pool[off];
    FRESULT fr;

    if (u == BD_UNIT_FD && room > BD_CLMT_FD_MAX) room = BD_CLMT_FD_MAX;
    if (room < 4) {                         /* not even one fragment */
        un->fil.cltbl = NULL;
        return (u == BD_UNIT_FD) ? BD_STATE_READY : BD_STATE_FRAGMENTED;
    }
    tbl[0] = room;
    un->fil.cltbl = tbl;
    fr = f_lseek(&un->fil, CREATE_LINKMAP);
    if (fr == FR_OK) {
        un->clmt_off = (uint16_t)off;
        un->clmt_len = (uint16_t)tbl[0];    /* DWORDs used, terminator included */
        return BD_STATE_READY;
    }
    un->fil.cltbl = NULL;
    if (fr == FR_NOT_ENOUGH_CORE) {         /* tbl[0] = what it would need */
        return (u == BD_UNIT_FD) ? BD_STATE_READY : BD_STATE_FRAGMENTED;
    }
    return BD_STATE_UNUSABLE;
#else
    (void)u;
    return BD_STATE_READY;
#endif
}

/* ---- open / close -------------------------------------------------------------- */

static void forget_image(int u) {
    bd_unit_t *un = &units[u];
    clmt_release(u);
    un->open = false;
    un->file_ro = false;
    un->vhd = false;
    un->sync_pending = false;
    un->synced_once = false;
    un->sfn[0] = 0;
    memset(&un->geom, 0, sizeof(un->geom));
}

static void close_image(int u) {
    bd_unit_t *un = &units[u];
    /* f_close() writes back the directory entry of a written image */
    if (un->open && mounted) f_close(&un->fil);
    forget_image(u);
}

static void media_changed(bd_unit_t *un) {
    un->gen++;
    un->changed = true;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t fnv32(uint32_t h, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    return fnv(h, b, 4);
}

/* Identifies this card boot, this unit, this explicit open and this file. */
static uint32_t make_token(int u) {
    const bd_unit_t *un = &units[u];
    uint32_t h = 2166136261u;
    h = fnv32(h, boot_nonce);
    h = fnv32(h, (uint32_t)u);
    h = fnv32(h, un->epoch);
    for (const char *p = un->path; *p; p++) {
        char c = upper(*p);
        h = fnv(h, &c, 1);
    }
    h = fnv32(h, (uint32_t)un->fil.obj.sclust);
    h = fnv32(h, (uint32_t)f_size(&un->fil));
    return h ? h : 1;
}

static bool read_at(FIL *fp, uint32_t ofs, void *dst, UINT len) {
    UINT br = 0;
    if (f_lseek(fp, ofs) != FR_OK) return false;
    return f_read(fp, dst, len, &br) == FR_OK && br == len;
}

/* Recognise the open image of unit u: format, data size and geometry.
 * The link map is already built, so the reads here seek through it. */
static bool identify(int u) {
    bd_unit_t *un = &units[u];
    FIL *fp = &un->fil;
    uint32_t size = (uint32_t)f_size(fp), data = size, sum = 0;
    bd_vhd_t vhd;
    int kind = BD_VHD_NONE;
    bool have_sec0 = false, ok;

    memset(&vhd, 0, sizeof(vhd));
    if (size >= 2 * BD_SECTOR && (size % BD_SECTOR) == 0) {
        /* the footer in pieces: bytes 128..511 for the checksum, then the head */
        uint32_t base = size - BD_SECTOR;
        for (uint32_t off = sizeof(scratch); off < BD_SECTOR; off += sizeof(scratch)) {
            if (!read_at(fp, base + off, scratch, sizeof(scratch))) return false;
            sum += bd_vhd_sum(scratch, off, sizeof(scratch));
        }
        if (!read_at(fp, base, scratch, sizeof(scratch))) return false;
        sum += bd_vhd_sum(scratch, 0, sizeof(scratch));
        kind = bd_vhd_check(scratch, sum, size, &vhd);
        if (kind == BD_VHD_OTHER) return false;         /* dynamic / differencing */
        if (kind == BD_VHD_FIXED) data = vhd.data_size;
    }
    if (data >= BD_SECTOR) {
        if (!read_at(fp, 0, sec0.head, BD_SEC0_HEAD_LEN)) return false;
        if (!read_at(fp, BD_SEC0_PTAB_OFF, sec0.ptab, BD_SEC0_PTAB_LEN)) return false;
        have_sec0 = true;
    }
    if (u == BD_UNIT_FD) {
        ok = bd_geom_floppy(data, have_sec0 ? &sec0 : NULL, &un->geom);
    } else {
        ok = bd_geom_harddisk(data, have_sec0 ? &sec0 : NULL, kind == BD_VHD_FIXED ? &vhd : NULL, &un->geom);
    }
    un->vhd = (kind == BD_VHD_FIXED);
    return ok;
}

/* The open image's path by 8.3 aliases, the form PGDFS paths arrive in. */
static void build_sfn(bd_unit_t *un) {
    char tmp[BD_PATH_BUF];
    size_t n = 0, i;

    un->sfn[0] = 0;
    strcpy(tmp, un->path);                          /* "\A\B.IMG" */
    for (i = 1; ; i++) {
        char c = tmp[i];
        const char *alias;
        size_t len;
        if (c != '\\' && c != 0) continue;
        tmp[i] = 0;
        if (f_stat(tmp, &fno) != FR_OK) {
            un->sfn[0] = 0;
            return;
        }
        tmp[i] = c;
        alias = fno.altname[0] ? fno.altname : fno.fname;
        len = strlen(alias);
        if (n + 1 + len >= sizeof(un->sfn)) {
            un->sfn[0] = 0;
            return;
        }
        un->sfn[n++] = '\\';
        memcpy(un->sfn + n, alias, len);
        n += len;
        un->sfn[n] = 0;
        if (c == 0) return;
    }
}

/* Open unit u's active path (closed before). Sets state and token. */
static void open_image(int u) {
    bd_unit_t *un = &units[u];
    FRESULT fr;
    uint8_t st;

    un->token = 0;
    if (un->path[0] == 0) {
        un->state = BD_STATE_NONE;
        return;
    }
    if (!mounted) {
        un->state = BD_STATE_NODRIVE;
        return;
    }
    fr = f_open(&un->fil, un->path, FA_READ | FA_WRITE);
    if (fr == FR_DENIED || fr == FR_WRITE_PROTECTED) {  /* AM_RDO file or write-protected drive */
        fr = f_open(&un->fil, un->path, FA_READ);
        un->file_ro = true;
    }
    if (fr != FR_OK) {
        un->file_ro = false;
        un->state = (fr == FR_NO_FILE || fr == FR_NO_PATH || fr == FR_INVALID_NAME || fr == FR_INVALID_DRIVE)
                  ? BD_STATE_NOTFOUND : BD_STATE_UNUSABLE;
        return;
    }
    un->open = true;
    if (un->fil.obj.attr & AM_RDO) un->file_ro = true;
    st = clmt_build(u);
    if (st == BD_STATE_READY && !identify(u)) st = BD_STATE_UNUSABLE;
    if (st != BD_STATE_READY) {
        close_image(u);
        un->state = st;
        return;
    }
    build_sfn(un);
    un->token = make_token(u);
    un->state = BD_STATE_READY;
}

/* Close unit u and open its active path again. explicit: a BDINFO OPEN or a
 * floppy commit, always a media change and a new token; otherwise a USB
 * replug, a change only when the image differs from the one before. */
static void reopen(int u, bool explicit) {
    bd_unit_t *un = &units[u];
    uint32_t before = un->token;

    close_image(u);
    if (explicit) un->epoch++;
    open_image(u);
    if (explicit || un->token != before) media_changed(un);
}

/* ---- setup ------------------------------------------------------------------ */

void bd_init(char *fd_name, char *hd_name, uint8_t *opts) {
    cfg_name[BD_UNIT_FD] = fd_name;
    cfg_name[BD_UNIT_HD] = hd_name;
    cfg_opts = opts;
    for (int u = 0; u < BD_UNITS; u++) {
        char *n = cfg_name[u];
        if (n) {
            n[BD_NAME_MAX] = 0;                     /* whatever flash held */
            if ((uint8_t)n[0] == 0xFF) n[0] = 0;    /* erased flash */
        }
        wr_pos[u] = 0;
        rd_pos[u] = 0;
        commit_seq[u] = 0;
        /* Nothing is armed after a card boot: a running system whose card
         * rebooted must not be handed an image it did not boot with, so a
         * unit only opens at the ROM's BDINFO with OPEN (or a floppy commit). */
        memset(&units[u], 0, sizeof(units[u]));
        units[u].state = BD_STATE_NONE;
    }
    if (opts) *opts &= BD_OPT_MASK;
    mounted = false;
}

void bd_init_ui(uint8_t *menu, uint8_t *tune, uint8_t *theme, char *melody) {
    cfg_menu = menu;
    cfg_tune = tune;
    cfg_theme = theme;
    cfg_melody = melody;
    if (menu) *menu = menu_clean(*menu);            /* erased flash included */
    if (melody) {
        melody[BD_MELODY_MAX] = 0;                  /* whatever flash held */
        if ((uint8_t)melody[0] == 0xFF) melody[0] = 0;
    }
    if (tune && !tune_valid(*tune)) *tune = 0;
    if (theme && *theme > BD_THEME_MAX) *theme = BD_THEME_DEFAULT;
    melody_wr = melody_rd = 0;
    tunedata_rd = 0;
    tune_compile();
}

void bd_init_modes(uint8_t available, uint8_t current) {
    modes_available = available;
    mode_current = current;
}

void bd_set_boot_nonce(uint32_t nonce) {
    boot_nonce = nonce;
}

/* ---- core 1 hooks ------------------------------------------------------------- */

void bd_tasks(void) {
    if (tune_seq != tune_applied_seq) tune_compile();   /* another tune, or the user's melody changed */
    /* A floppy name commit swaps the disk at once; a hard disk name waits
     * for the next BDINFO with OPEN (the next boot). */
    if (commit_seq[BD_UNIT_FD] != units[BD_UNIT_FD].applied_seq) {
        fetch_name(BD_UNIT_FD);
        reopen(BD_UNIT_FD, true);
    }
    for (int u = 0; u < BD_UNITS; u++) {
        bd_unit_t *un = &units[u];
        uint32_t now;
        if (!un->open || !un->sync_pending) continue;
        now = dfs_platform_millis();
        if ((uint32_t)(now - un->last_write_ms) < BD_SYNC_IDLE_MS) continue;
        if (f_sync(&un->fil) == FR_OK) {
            un->sync_pending = false;
        } else {
            /* try again after another idle period; FatFs dropped its
             * "entry modified" mark although the write-back failed */
            un->fil.err = 0;
            un->fil.flag |= FA_MODIFIED;
            un->last_write_ms = now;
        }
    }
}

void bd_on_drive_mounted(void) {
    mounted = true;
    /* reopen exactly the units that were in use before the unplug */
    for (int u = 0; u < BD_UNITS; u++) {
        if (units[u].path[0]) reopen(u, false);
    }
}

void bd_on_drive_unmounted(void) {
    /* The volume is gone: forget the FILs, no FatFs call. Only marks state,
     * so it is safe even if it ever runs underneath bd_process(). The token
     * is kept: a replug of the same stick reopens the same image. */
    mounted = false;
    for (int u = 0; u < BD_UNITS; u++) {
        bd_unit_t *un = &units[u];
        forget_image(u);
        un->state = un->path[0] ? BD_STATE_NODRIVE : BD_STATE_NONE;
    }
}

/* ---- requests ------------------------------------------------------------------ */

/* The display name for the BDINFO record: the active path, or its tail with
 * a "..." in front when it is longer than BD_INFO_NAME_MAX. */
static size_t display_name(const bd_unit_t *un, char *out) {
    size_t n = strlen(un->path);
    if (n <= BD_INFO_NAME_MAX) {
        memcpy(out, un->path, n);
        return n;
    }
    memcpy(out, "...", 3);
    memcpy(out + 3, un->path + n - (BD_INFO_NAME_MAX - 3), BD_INFO_NAME_MAX - 3);
    return BD_INFO_NAME_MAX;
}

static uint16_t do_info(const uint8_t *req, uint16_t req_len, uint8_t *answ, uint16_t maxpl, uint16_t *ax) {
    uint8_t rec[BD_INFO_LEN];
    char name[BD_INFO_NAME_MAX];
    uint8_t u, flags = 0;
    bd_unit_t *un;
    size_t n;

    if (req_len < 1 || req[0] >= BD_UNITS || maxpl < BD_INFO_LEN) {
        *ax = BD_ST_BADCMD;
        return 0;
    }
    u = req[0];
    if (req_len >= 2 && (req[1] & BD_INFO_OPEN)) {
        fetch_name(u);
        reopen(u, true);
    }
    un = &units[u];

    memset(rec, 0, sizeof(rec));
    rec[BD_INFO_OFF_VERSION] = BD_INFO_VERSION;
    rec[BD_INFO_OFF_STATE] = un->state;
    /* Before the first OPEN after a card boot the unit reads NONE; while no
     * USB drive is mounted and an image is configured, say NODRIVE instead,
     * so the ROM's wait loop (BDINFO without OPEN) keeps waiting. */
    if (!mounted && un->state == BD_STATE_NONE && cfg_name[u] && cfg_name[u][0])
        rec[BD_INFO_OFF_STATE] = BD_STATE_NODRIVE;
    if (un->state != BD_STATE_NONE) {
        rec[BD_INFO_OFF_TYPE] = (u == BD_UNIT_FD) ? BD_TYPE_FLOPPY : BD_TYPE_HARDDISK;
        if (unit_ro(u)) flags |= BD_FLAG_RO;
    }
    if (un->vhd) flags |= BD_FLAG_VHD;
    if (un->changed) flags |= BD_FLAG_CHANGED;
    rec[BD_INFO_OFF_FLAGS] = flags;
    put16(rec + BD_INFO_OFF_CYL, un->geom.cyl);
    put16(rec + BD_INFO_OFF_HEADS, un->geom.heads);
    put16(rec + BD_INFO_OFF_SPT, un->geom.spt);
    put32(rec + BD_INFO_OFF_TOTAL, un->geom.total);
    rec[BD_INFO_OFF_DRVTYPE] = un->geom.drive_type;
    rec[BD_INFO_OFF_GEN] = un->gen;
    if (un->state == BD_STATE_READY) put32(rec + BD_INFO_OFF_TOKEN, un->token);
    un->changed = false;

    n = display_name(un, name);
    if (n > (size_t)(maxpl - BD_INFO_LEN)) n = (size_t)(maxpl - BD_INFO_LEN);
    rec[BD_INFO_OFF_NAMELEN] = (uint8_t)n;
    memcpy(answ, rec, BD_INFO_LEN);                 /* req is consumed: answ may alias it */
    memcpy(answ + BD_INFO_LEN, name, n);
    *ax = BD_ST_OK;
    return (uint16_t)(BD_INFO_LEN + n);
}

/* Common checks of BDREAD/BDWRITE. Returns a status; *un_out, *ofs, *len on OK. */
static uint16_t check_io(const uint8_t *req, uint16_t req_len, bool write, uint16_t maxpl,
                         bd_unit_t **un_out, uint32_t *ofs, uint32_t *len) {
    uint8_t u, nn;
    uint32_t lba, total, token;
    bd_unit_t *un;

    if (req_len < BD_IO_HDR_LEN) return BD_ST_BADCMD;
    u = req[0];
    nn = req[1];
    lba = get32(req + 2);
    token = get32(req + 6);
    if (u >= BD_UNITS || nn == 0) return BD_ST_BADCMD;
    *len = (uint32_t)nn * BD_SECTOR;
    if (write) {
        if ((uint32_t)req_len < BD_IO_HDR_LEN + *len) return BD_ST_BADCMD;   /* data missing */
    } else {
        if (*len > maxpl) return BD_ST_BADCMD;      /* answer would not fit the frame */
    }
    un = &units[u];
    if (un->state != BD_STATE_READY || !un->open) return BD_ST_NOTREADY;
    /* not the image the caller got from BDINFO: a floppy swap is a media
     * change the ROM reports to DOS; a hard disk must never change under a
     * running system */
    if (token != un->token) return (u == BD_UNIT_FD) ? BD_ST_CHANGED : BD_ST_NOTREADY;
    if (write && unit_ro(u)) return BD_ST_WRPROT;
    total = un->geom.total;
    if (lba >= total || nn > total - lba) return BD_ST_NOSECTOR;
    *un_out = un;
    *ofs = lba * BD_SECTOR;                         /* < 4 GB: total came from a FatFs file size */
    return BD_ST_OK;
}

static uint16_t io_failed(bd_unit_t *un) {
    if (!un->open) return BD_ST_NOTREADY;           /* the drive went away underneath */
    /* FatFs keeps a hard error sticky in the FIL; clear it so the next
     * request tries the drive again instead of failing forever. The FIL
     * stays consistent: FatFs only records a sector as cached after reading
     * it, and a failed write-back leaves the sector marked dirty. */
    un->fil.err = 0;
    return BD_ST_CTRLFAIL;
}

static uint16_t do_read(const uint8_t *req, uint16_t req_len, uint8_t *answ, uint16_t maxpl, uint16_t *ax) {
    bd_unit_t *un = NULL;
    uint32_t ofs = 0, len = 0;
    UINT br = 0;
    FRESULT fr;

    *ax = check_io(req, req_len, false, maxpl, &un, &ofs, &len);
    if (*ax != BD_ST_OK) return 0;
    fr = f_lseek(&un->fil, ofs);
    if (fr == FR_OK) fr = f_read(&un->fil, answ, (UINT)len, &br);     /* answ may alias req: parsed already */
    if (fr != FR_OK || br != len || !un->open) {
        *ax = io_failed(un);
        return 0;
    }
    return (uint16_t)len;
}

static uint16_t do_write(const uint8_t *req, uint16_t req_len, uint16_t *ax) {
    bd_unit_t *un = NULL;
    uint32_t ofs = 0, len = 0;
    UINT bw = 0;
    FRESULT fr;

    *ax = check_io(req, req_len, true, 0, &un, &ofs, &len);
    if (*ax != BD_ST_OK) return 0;
    fr = f_lseek(&un->fil, ofs);
    if (fr == FR_OK) fr = f_write(&un->fil, req + BD_IO_HDR_LEN, (UINT)len, &bw);
    if (fr == FR_OK && bw != len) fr = FR_DISK_ERR;     /* never extends: cannot run out of space */
    /* Whole aligned sectors go straight to the drive (FatFs's direct path
     * refreshes, never dirties, the FIL's sector buffer), so what is left is
     * the directory entry: synced on the first write, then when idle. */
    if (fr == FR_OK && (!un->synced_once || (un->fil.flag & FA_DIRTY))) {
        fr = f_sync(&un->fil);
        if (fr == FR_OK) un->synced_once = true;
    }
    if (fr != FR_OK || !un->open) {
        *ax = io_failed(un);
        if (un->open) {                     /* the entry is written back later */
            un->fil.flag |= FA_MODIFIED;
            un->sync_pending = true;
        }
    } else if (un->fil.flag & FA_MODIFIED) {
        un->sync_pending = true;            /* bd_tasks() syncs when idle */
    }
    un->last_write_ms = dfs_platform_millis();
    return 0;
}

/* ---- BDLIST: what the boot menu shows ----------------------------------------- */

/* File name extensions offered as disk images, upper case */
static const char list_ext[][4] = { "IMG", "IMA", "VFD", "FLP", "DSK", "VHD", "HDD" };
#define LIST_EXT_HD     5       /* from here on: always a hard disk */

/* What the entry in fno is for the menu: BD_KIND_*, or -1 for "not listed"
 * (hidden and system entries, the volume label, other file types). */
static int list_kind(void) {
    const char *dot = NULL, *p;
    char ext[4];
    size_t i;

    if (fno.fattrib & (AM_HID | AM_SYS)) return -1;
    if (fno.fattrib & AM_DIR) return BD_KIND_DIR;
    for (p = fno.fname; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (dot == NULL || strlen(dot + 1) != 3) return -1;
    for (i = 0; i < 3; i++) ext[i] = upper(dot[1 + i]);
    ext[3] = 0;
    for (i = 0; i < sizeof(list_ext) / sizeof(list_ext[0]); i++) {
        if (memcmp(ext, list_ext[i], 4) != 0) continue;
        if (i >= LIST_EXT_HD) return BD_KIND_HARDDISK;
        return (fno.fsize <= BD_FLOPPY_MAX_BYTES) ? BD_KIND_FLOPPY : BD_KIND_HARDDISK;
    }
    return -1;
}

/* One 48-byte entry: the 8.3 name the ROM builds paths from, and the long
 * name it shows (cut with "..." when it does not fit). */
static void list_put(uint8_t *e, int kind, uint8_t flags, uint32_t sectors, const char *sfn, const char *disp) {
    size_t n = strlen(disp), k = strlen(sfn);
    if (k > BD_LIST_SFN_LEN - 1) k = BD_LIST_SFN_LEN - 1;
    memset(e, 0, BD_LIST_ENTRY_LEN);
    e[BD_LIST_OFF_KIND] = (uint8_t)kind;
    e[BD_LIST_OFF_FLAGS] = flags;
    put32(e + BD_LIST_OFF_SECTORS, sectors);
    memcpy(e + BD_LIST_OFF_SFN, sfn, k);
    if (n > BD_LIST_DISP_LEN) {
        memcpy(e + BD_LIST_OFF_DISP, disp, BD_LIST_DISP_LEN - 3);
        memcpy(e + BD_LIST_OFF_DISP + BD_LIST_DISP_LEN - 3, "...", 3);
        n = BD_LIST_DISP_LEN;
    } else {
        memcpy(e + BD_LIST_OFF_DISP, disp, n);
    }
    e[BD_LIST_OFF_DISPLEN] = (uint8_t)n;
}

/* ---- the boot menu's own lists ---------------------------------------------------
 * The ROM is a list browser: what the menu offers is decided here, so that
 * new firmware can add to it without a new ROM. An entry opens another list
 * (BD_KIND_MENU), writes a value to a control register (BD_KIND_CHOICE; the
 * one in effect is flagged), or boots (BD_KIND_BOOT). */

static const char *const mode_name[] = {    /* by card_mode_t */
    NULL, "Gravis UltraSound", "AdLib", "MPU-401", "Tandy / CMS", "Sound Blaster", "USB only", NULL
};
static const char *const theme_name[BD_THEME_MAX + 1] = { "Setup", "Commander", "Mono" };

static unsigned mode_count(void) {
    unsigned n = 0;
    for (unsigned m = 1; m < 7; m++) n += (modes_available >> m) & 1;
    return n;
}

/* The tune with index i of the tunes list: 0 off, then the firmware's, then
 * the user's. False past the end. */
static bool tune_at(unsigned i, uint8_t *value, char *name) {
    const char *text, *colon;
    size_t n;
    if (i == 0) {
        *value = 0;
        strcpy(name, "Off");
        return true;
    }
    if (i <= BD_TUNE_BUILTIN) {
        *value = (uint8_t)i;
        text = bd_tune_builtin[i - 1];
    } else if (i == BD_TUNE_BUILTIN + 1 && tune_valid(BD_TUNE_CUSTOM)) {
        *value = BD_TUNE_CUSTOM;
        text = cfg_melody;
    } else {
        return false;
    }
    colon = strchr(text, ':');
    n = (colon && strchr(colon + 1, ':')) ? (size_t)(colon - text) : 0;     /* as bd_tune_compile() names it */
    if (n > BD_TUNE_NAME_MAX) n = BD_TUNE_NAME_MAX;
    memcpy(name, text, n);
    name[n] = 0;
    if (n == 0) strcpy(name, "Custom");
    return true;
}

/* Entry i of a menu list into e (e may be NULL: only whether there is one) */
static bool menu_entry(uint8_t list, unsigned i, uint8_t *e) {
    char name[BD_TUNE_NAME_MAX + 1];
    const char *disp = name;
    int kind = BD_KIND_CHOICE;
    uint8_t a = 0, b = 0, flags = 0;

    switch (list) {
    case BD_LIST_MAIN: {
        static const struct { const char *name; uint8_t kind, a, b; } item[] = {
            { "Floppy Image",  BD_KIND_MENU,   BD_LIST_FLOPPY,   0 },
            { "HDD Image",     BD_KIND_MENU,   BD_LIST_HARDDISK, 0 },
            { "Boot Order",    BD_KIND_MENU,   BD_LIST_ORDER,    0 },
            { "Boot Melody",   BD_KIND_MENU,   BD_LIST_TUNES,    0 },
            { "Card Mode",     BD_KIND_MENU,   BD_LIST_MODES,    0 },
            { "Colours",       BD_KIND_MENU,   BD_LIST_THEMES,   0 },
            { "Save Settings", BD_KIND_CHOICE, CMD_SAVE,         1 },
            { "Boot",          BD_KIND_BOOT,   0,                0 },
        };
        if (mode_count() < 2 && i >= 4) i++;        /* nothing to switch to: no "Card Mode" */
        if (i >= sizeof(item) / sizeof(item[0])) return false;
        disp = item[i].name;
        kind = item[i].kind;
        a = item[i].a;
        b = item[i].b;
        break;
    }
    case BD_LIST_TUNES:
        if (!tune_at(i, &b, name)) return false;
        a = CMD_BDTUNE;
        if (b == (bd_ctl_tune_read() & BD_MENU_MASK)) flags = BD_LIST_FLAG_CURRENT;
        break;
    case BD_LIST_MODES: {
        unsigned m;
        for (m = 1; m < 7; m++) {
            if (!((modes_available >> m) & 1)) continue;
            if (i == 0) break;
            i--;
        }
        if (m >= 7) return false;
        disp = mode_name[m];
        a = CMD_BOOTMODE;
        b = (uint8_t)m;
        if (m == mode_current) flags = BD_LIST_FLAG_CURRENT;
        break;
    }
    case BD_LIST_ORDER: {
        /* option bit 3, the other options as they are */
        uint8_t opts = opts_now();
        if (i > 1) return false;
        disp = i ? "Hard disk C: first" : "Floppy A: first";
        a = CMD_BDOPTS;
        b = i ? (uint8_t)(opts | BD_OPT_BOOT_HD) : (uint8_t)(opts & ~BD_OPT_BOOT_HD);
        if (b == opts) flags = BD_LIST_FLAG_CURRENT;
        break;
    }
    case BD_LIST_THEMES:
        if (i > BD_THEME_MAX) return false;
        disp = theme_name[i];
        a = CMD_BDTHEME;
        b = (uint8_t)i;
        if (b == (bd_ctl_theme_read() & BD_MENU_MASK)) flags = BD_LIST_FLAG_CURRENT;
        break;
    default:
        return false;
    }
    if (e) list_put(e, kind, flags, (uint32_t)a | ((uint32_t)b << 8), "", disp);
    return true;
}

/* Whether unit's configured image is the file sfn of the folder list_path
 * (sfn NULL: whether the unit has no image). The boot menu writes the path
 * it built from the folder and the 8.3 name, which is what is compared;
 * an image set by another path to the same file (a long name) is not marked. */
static bool list_current(int unit, const char *sfn) {
    char *have = list_have;
    size_t n = strlen(list_path), i;
    if (!cfg_name[unit]) return false;
    norm_path(have, sizeof(list_have), cfg_name[unit]);
    if (sfn == NULL) return have[0] == 0;
    for (i = 0; i < n; i++) {
        if (upper(have[i]) != upper(list_path[i])) return false;
    }
    if (have[n] != '\\') return false;
    for (i = 0; sfn[i]; i++) {
        if (upper(have[n + 1 + i]) != upper(sfn[i])) return false;
    }
    return have[n + 1 + i] == 0;
}

/* A folder: its directories (".." first, below the root), then its disk
 * images (all, or of one kind), each group in directory order. The ROM asks
 * for a screenful at a time by index, so every request walks the directory
 * twice; nothing is kept between requests and the listing follows what is on
 * the drive. The boot menu's own lists are served from the same request and
 * need no drive. */
static uint16_t do_list(const uint8_t *req, uint16_t req_len, uint8_t *answ, uint16_t maxpl, uint16_t *ax) {
    char *raw = (char *)scratch;    /* not on core 1's small stack */
    uint16_t start, total = 0;
    unsigned want, room, count = 0;
    uint8_t list;
    size_t plen;
    int pass, unit;

    if (req_len < BD_LIST_REQ_LEN || maxpl < BD_LIST_HDR_LEN) {
        *ax = BD_ST_BADCMD;
        return 0;
    }
    start = get16(req);
    want = req[2];
    list = req[3];
    plen = (size_t)(req_len - BD_LIST_REQ_LEN);
    if (plen > BD_NAME_MAX) {
        *ax = BD_ST_BADCMD;
        return 0;
    }
    room = (unsigned)(maxpl - BD_LIST_HDR_LEN) / BD_LIST_ENTRY_LEN;
    if (want > room) want = room;

    if (list >= BD_LIST_MAIN) {
        while (menu_entry(list, total, NULL)) total++;
        if (total == 0) {
            *ax = BD_ST_BADCMD;                     /* no such list */
            return 0;
        }
        while (count < want && menu_entry(list, start + count, answ + BD_LIST_HDR_LEN + count * BD_LIST_ENTRY_LEN)) count++;
        goto done;
    }
    if (list > BD_LIST_HARDDISK) {
        *ax = BD_ST_BADCMD;
        return 0;
    }
    memcpy(raw, req + BD_LIST_REQ_LEN, plen);       /* answ may alias req: take the path first */
    raw[plen] = 0;
    norm_path(list_path, sizeof(list_path), raw);
    if (list_path[0] && list_path[1] == 0) list_path[0] = 0;        /* "\\" alone is the root, like "" */
    if (!mounted) {
        *ax = BD_ST_NOTREADY;
        return 0;
    }

    switch (f_opendir(&list_dir, list_path[0] ? list_path : "/")) {
    case FR_OK:
        break;
    case FR_NO_PATH: case FR_NO_FILE: case FR_INVALID_NAME:
        *ax = BD_ST_NODIR;
        return 0;
    default:
        *ax = BD_ST_CTRLFAIL;
        return 0;
    }
    if (list_path[0]) {                             /* below the root: the way back up */
        if (start == 0 && want > 0) {
            list_put(answ + BD_LIST_HDR_LEN, BD_KIND_DIR, 0, 0, "..", "..");
            count = 1;
        }
        total = 1;
    } else if (list != BD_LIST_ALL) {
        /* at the top of the list of one kind of image: the drive without
         * one, as a choice that writes an empty name to its register */
        unit = (list == BD_LIST_FLOPPY) ? BD_UNIT_FD : BD_UNIT_HD;
        if (start == 0 && want > 0) {
            list_put(answ + BD_LIST_HDR_LEN, BD_KIND_CHOICE, list_current(unit, NULL) ? BD_LIST_FLAG_CURRENT : 0,
                     (unit == BD_UNIT_FD) ? CMD_BDFDNAME : CMD_BDHDNAME, "", "(none)");
            count = 1;
        }
        total = 1;
    }
    for (pass = 0; pass < 2; pass++) {
        if (pass && f_readdir(&list_dir, NULL) != FR_OK) goto fail;     /* rewind */
        for (;;) {
            int kind;
            if (f_readdir(&list_dir, &fno) != FR_OK) goto fail;
            if (fno.fname[0] == 0) break;
            kind = list_kind();
            if (kind < 0 || (kind == BD_KIND_DIR) != (pass == 0)) continue;
            if (list != BD_LIST_ALL && kind != BD_KIND_DIR && kind != (int)list) continue;  /* the other kind of image */
            if (total >= start && count < want) {
                const char *sfn = fno.altname[0] ? fno.altname : fno.fname;
                uint8_t flags = (fno.fattrib & AM_RDO) ? BD_LIST_FLAG_RO : 0;
                if (kind != BD_KIND_DIR && list_current(kind == BD_KIND_FLOPPY ? BD_UNIT_FD : BD_UNIT_HD, sfn))
                    flags |= BD_LIST_FLAG_CURRENT;
                list_put(answ + BD_LIST_HDR_LEN + count * BD_LIST_ENTRY_LEN, kind, flags,
                         (uint32_t)(fno.fsize / BD_SECTOR), sfn, fno.fname);
                count++;
            }
            if (total < 0xFFFF) total++;
        }
    }
    f_closedir(&list_dir);
done:
    put16(answ, total);
    answ[2] = (uint8_t)count;
    answ[3] = 0;
    *ax = BD_ST_OK;
    return (uint16_t)(BD_LIST_HDR_LEN + count * BD_LIST_ENTRY_LEN);
fail:
    f_closedir(&list_dir);
    *ax = BD_ST_CTRLFAIL;
    return 0;
}

uint16_t bd_process(uint8_t al, const uint8_t *req, uint16_t req_len,
                    uint8_t *answ, uint16_t maxpl, uint16_t *ax) {
    bd_tasks();                     /* a floppy commit applies before anything is served */
    switch (al) {
    case BD_AL_INFO:  return do_info(req, req_len, answ, maxpl, ax);
    case BD_AL_READ:  return do_read(req, req_len, answ, maxpl, ax);
    case BD_AL_WRITE: return do_write(req, req_len, ax);
    case BD_AL_LIST:  return do_list(req, req_len, answ, maxpl, ax);
    default:
        *ax = BD_ST_BADCMD;
        return 0;
    }
}

/* ---- PGDFS interlock ------------------------------------------------------------- */

/* Case-insensitive compare of two normalised paths; prefix: a may also name
 * a directory on b's path. */
static bool path_match(const char *a, const char *b, bool prefix) {
    while (*a && *b) {
        if (upper(*a) != upper(*b)) return false;
        a++;
        b++;
    }
    if (*a) return false;
    return *b == 0 || (prefix && *b == '\\');
}

static bool open_image_match(const char *path, bool prefix) {
    char norm[BD_SFN_BUF];
    bool any = false;
    for (int u = 0; u < BD_UNITS; u++) any |= units[u].open;
    if (!any || path == NULL || path[0] == 0) return false;
    if (strlen(path) >= sizeof(norm)) return false;     /* longer than any image path we hold */
    norm_path(norm, sizeof(norm), path);
    if (norm[1] == 0) return false;                     /* the root */
    for (int u = 0; u < BD_UNITS; u++) {
        const bd_unit_t *un = &units[u];
        if (!un->open) continue;
        if (path_match(norm, un->path, prefix)) return true;
        if (un->sfn[0] && path_match(norm, un->sfn, prefix)) return true;
    }
    return false;
}

bool bd_path_is_open_image(const char *path) {
    return open_image_match(path, false);
}

bool bd_path_holds_open_image(const char *path) {
    return open_image_match(path, true);
}

#ifdef BD_HOST_TEST
/* Host tests only: whether unit u serves through a fast seek link map. */
bool bd_test_fastseek(int u) {
#if FF_USE_FASTSEEK
    return u >= 0 && u < BD_UNITS && units[u].open && units[u].fil.cltbl != NULL;
#else
    return false;
#endif
}
#endif
