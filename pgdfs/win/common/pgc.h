/*
 * PGDFS client core for the Windows drivers (PGDFS.VXD, pgdfs.sys)
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * Everything the Windows 9x and Windows NT drivers have in common: the ISA
 * transport of sw/dfs/PROTOCOL.md, the EDF5 requests, and the bookkeeping
 * that makes the card's small file table and its removable USB drive look
 * like an ordinary file system to the operating system:
 *
 *  - Card file ids. The card keeps 8 open files (DFS_MAX_FILES). Windows
 *    keeps far more handles open than that, so a pgc_file is only bound to a
 *    card file id while it needs one; the least recently used binding is
 *    closed on the card when an id is needed for another file, and a file
 *    that lost its id opens itself again by path on its next read or write.
 *    The card's READ and WRITE take an explicit offset, so nothing but the id
 *    is lost when that happens.
 *
 *  - The medium. The USB drive can be unplugged and plugged in again at any
 *    time, and the card then forgets every open file. The core tracks this
 *    with two counters: mount_gen changes whenever the card may have dropped
 *    its file ids (every bound id becomes invalid and is re-opened by path on
 *    demand), media_gen changes when a *different* medium appears (its volume
 *    string, label|fs|size|serial, differs from the one seen before). Files
 *    opened on an older medium are stale and fail with PGC_ESTALE; files of
 *    the same medium carry on, the way FAT on a floppy keeps its handles when
 *    the same disk goes back into the drive.
 *
 *    A replug of the same drive shows in neither the status byte nor the
 *    volume string. The card counts its mounts (DIAG record, version 3), and
 *    the count is read with every look at the medium: at most once in
 *    PGC_RECHECK_MS, and whenever a request says the drive is gone. Between
 *    two looks, and with firmware that has no mount count, a replug is
 *    inferred: only this driver opens files on the card, so the card's
 *    open-file table mirrors pgc_card.slot[] exactly until the card forgets
 *    it; afterwards, either a request on a stale id answers "invalid handle"
 *    (6), or the card hands out an id the table still holds. Both are taken
 *    as proof of a remount.
 *
 * The core is plain C (C99 without its library), uses no C library function and
 * does no memory allocation; it is compiled into ring 0 code by OpenWatcom
 * (VxD) and by MinGW gcc (NT kernel driver), and on the host for the tests in
 * ../test. It is not reentrant: the driver serializes every call on one
 * pgc_card with a lock of its own.
 *
 * Paths and names are 8-bit strings in the OEM code page of the card's FatFs
 * (FATFS_CODE_PAGE, 437 by default), drive-relative with backslashes:
 * "\DIR\FILE.TXT", "" or "\" for the root. Long names are fine (FatFs
 * resolves them), but a path may not exceed PGC_PATH_MAX - 1 bytes.
 */

#ifndef PGC_H
#define PGC_H

#include <stdint.h>

#ifndef PICOGUS_NO_MODENAMES
#define PICOGUS_NO_MODENAMES
#endif
#include "../../../common/picogus.h"
#include "pgctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits ---------------------------------------------------------------- */

#define PGC_HDR          DFS_HDR_LEN              /* frame header             */
#define PGC_MAX_PAYLOAD  DFS_DEFAULT_MAX_PAYLOAD  /* largest frame payload    */
#define PGC_FRAME        (PGC_HDR + PGC_MAX_PAYLOAD)
#define PGC_PATH_MAX     128   /* card path buffer incl. NUL (DFS_PATH_MAX)   */
#define PGC_NAME_MAX     256   /* long file name incl. NUL (FatFs FF_LFN_BUF) */
#define PGC_SLOTS        16    /* card file ids tracked (the card uses 1..8)  */
#define PGC_CARD_FILES   8     /* the card's open-file table (DFS_MAX_FILES)  */
#define PGC_INFO_MAX     64    /* CMD_DFSINFO string incl. NUL                */
#define PGC_TIMEOUT_MS   30000 /* no answer within this time: abort, not ready */
#define PGC_RECHECK_MS   1000  /* re-read the medium state at most this often */

/* ---- results ----------------------------------------------------------------
 * 0 or a DOS error code as the card answers it (AX), plus the core's own codes
 * from 100h up. */
#define PGC_OK           0x00
#define PGC_EFUNC        0x01  /* invalid function / malformed request      */
#define PGC_ENOFILE      0x02  /* file not found                            */
#define PGC_ENOPATH      0x03  /* path not found                            */
#define PGC_EHANDLES     0x04  /* too many open files                       */
#define PGC_EACCESS      0x05  /* access denied (also: read-only, volume full, not empty) */
#define PGC_EHANDLE      0x06  /* invalid handle                            */
#define PGC_ENOMEM       0x08  /* insufficient memory                       */
#define PGC_EDRIVE       0x0F  /* invalid drive                             */
#define PGC_ENOMORE      0x12  /* no more files                             */
#define PGC_EWRPROT      0x13  /* write-protected                           */
#define PGC_ENOTREADY    0x15  /* no USB drive, or the card did not answer  */
#define PGC_EWRFAULT     0x1D  /* write fault                               */
#define PGC_ERDFAULT     0x1E  /* read fault                                */
#define PGC_ESHARING     0x20  /* sharing violation                         */
#define PGC_EEXISTS      0x50  /* file exists                               */
#define PGC_ESTALE       0x100 /* the file belongs to a medium that was removed */
#define PGC_ETOOLONG     0x101 /* path longer than the card accepts         */
#define PGC_EPROTO       0x102 /* malformed answer from the card            */
#define PGC_ENOCARD      0x103 /* no PicoGUS, no PGDFS, or PGDFS disabled   */

/* DOS / FAT attributes */
#define PGC_ATTR_RDONLY  0x01
#define PGC_ATTR_HIDDEN  0x02
#define PGC_ATTR_SYSTEM  0x04
#define PGC_ATTR_VOLUME  0x08
#define PGC_ATTR_DIR     0x10
#define PGC_ATTR_ARCHIVE 0x20

/* access a file needs on the card (DOS open modes) */
#define PGC_READ         0
#define PGC_READWRITE    2

/* pgc_open() dispositions (INT 21h/6Ch action codes) */
#define PGC_OPEN_EXISTING    0x01  /* open, fail if missing                  */
#define PGC_TRUNCATE_EXISTING 0x02 /* truncate, fail if missing              */
#define PGC_CREATE_NEW       0x10  /* create, fail if it exists              */
#define PGC_OPEN_ALWAYS      0x11  /* open, create if missing                */
#define PGC_CREATE_ALWAYS    0x12  /* truncate, create if missing            */

/* pgc_open() *result */
#define PGC_OPENED       1
#define PGC_CREATED      2
#define PGC_TRUNCATED    3

/* ---- state ----------------------------------------------------------------- */

typedef struct pgc_file pgc_file;

typedef struct {
  uint16_t port;            /* data window base (CMD_DFSPORT)               */
  uint16_t maxpl;           /* frame payload limit in use                   */
  uint8_t  proto;           /* CMD_PROTOCOL                                 */
  uint8_t  probed;          /* pgc_probe() succeeded                        */

  /* the medium */
  uint8_t  present;         /* a USB drive is mounted, as far as known      */
  uint8_t  seen;            /* a medium has been seen since the probe       */
  uint8_t  mounts;          /* the card's mount count at the last look, 0 = not known */
  uint8_t  mounts_known;    /* 0 not asked yet, 1 the firmware counts, 2 it does not */
  uint32_t media_gen;       /* bumped when a different medium appears       */
  uint32_t mount_gen;       /* bumped whenever the card may have lost its ids */
  uint32_t change_count;    /* bumped on every arrival/removal (for the OS) */
  uint32_t checked_ms;      /* when the medium state was last read          */
  char     info[PGC_INFO_MAX]; /* CMD_DFSINFO of the current medium         */

  /* card file ids: slot[id - 1] is the file bound to id (NULL = free) */
  pgc_file *slot[PGC_SLOTS];
  uint32_t lru_clock;

  /* disk geometry from the last pgc_space() (DIAG), 0 = unknown */
  uint32_t clusters, free_clusters, cluster_bytes;

  /* diagnostics of the last transaction */
  uint8_t  last_status;     /* CMD_DFSSTAT at the end                       */
  uint8_t  retried;         /* the request went out twice (ABORTED once)    */
  uint32_t transactions;    /* count, for the curious                       */
  uint32_t timeouts;

  uint8_t  buf[PGC_FRAME];  /* the one frame buffer (request, then answer)  */
} pgc_card;

struct pgc_file {
  char     path[PGC_PATH_MAX];  /* where it lives on the card               */
  uint32_t media_gen;       /* medium it was opened on                      */
  uint32_t mount;           /* mount_gen the card id belongs to             */
  uint32_t lru;             /* last use of the card id                      */
  uint16_t id;              /* card file id, 0 = not bound                  */
  uint8_t  mode;            /* PGC_READ / PGC_READWRITE the id was opened with */
  uint8_t  need;            /* PGC_READ / PGC_READWRITE the file needs      */
  uint8_t  opened;          /* pgc_open()/pgc_file_attach() done, not closed */
};

/* one directory entry from FINDFIRST/FINDNEXT (+ LONGNAME) */
typedef struct {
  uint8_t  attr;
  uint16_t time, date;      /* FAT packed, local time                       */
  uint32_t size;
  char     shortname[13];   /* "NAME.EXT"                                   */
  char     longname[PGC_NAME_MAX]; /* long name, or the short name (in the
                             * case Windows recorded) when it has none      */
  /* search state */
  uint16_t dir_id, pos;
  uint8_t  sattr;
  uint8_t  want_long;
  char     mask[11];        /* FCB form of the pattern                      */
  char     dir[PGC_PATH_MAX];
} pgc_find;

/* parsed CMD_DFSINFO */
typedef struct {
  char     label[12];       /* volume label, may be empty                   */
  char     fs[8];           /* "FAT12", "FAT16", "FAT32", "EXFAT"           */
  uint32_t size_mb;
  uint32_t serial;
} pgc_volinfo;

/* ---- platform layer: provided by the driver (and by the host test) ---------- */

uint8_t  pgc_plat_inb(uint16_t port);
void     pgc_plat_outb(uint16_t port, uint8_t value);
/* 16-bit string I/O at an 8-bit port: the bus splits each word into a byte
 * at port and one at port + 1 (rep insw / rep outsw). */
void     pgc_plat_insw(uint16_t port, void *dst, uint16_t words);
void     pgc_plat_outsw(uint16_t port, const void *src, uint16_t words);
/* a millisecond clock that may wrap */
uint32_t pgc_plat_ms(void);
/* called on every poll of the status byte while the card works on a request,
 * waited_ms after the request went out: spin for short waits, sleep or yield
 * for long ones */
void     pgc_plat_poll(uint32_t waited_ms);

/* ---- card ----------------------------------------------------------------- */

/* resets c and looks for a PicoGUS with PGDFS: magic byte, protocol >= 5,
 * CMD_DFSMAXLEN and CMD_DFSPORT. PGC_OK, or PGC_ENOCARD. Closes every file id
 * the card may still hold from an earlier session (a DOS program before
 * Windows started, a warm reboot) and reads the medium state. */
int      pgc_probe(pgc_card *c);

/* reads the medium state when the last check is older than PGC_RECHECK_MS
 * (always with force), updating present/media_gen/mount_gen. PGC_OK when a
 * drive is mounted, PGC_ENOTREADY when not, PGC_ENOCARD without a card. */
int      pgc_media(pgc_card *c, int force);

/* parsed volume string of the current medium (PGC_ENOTREADY without one) */
int      pgc_volinfo_get(pgc_card *c, pgc_volinfo *v);

/* sends the local time (FAT packed) for the timestamps the card writes */
void     pgc_set_time(pgc_card *c, uint16_t dos_time, uint16_t dos_date);

/* size and free space in clusters of cluster_bytes. Exact (from the DIAG
 * record) when the firmware has it, else DOS's 32 KB units capped at 2 GB */
int      pgc_space(pgc_card *c, uint32_t *clusters, uint32_t *free_clusters, uint32_t *cluster_bytes);

/* ---- control requests (pgctl.h) ----------------------------------------------
 * Runs one request of a setup program on the control port: wr are the
 * rq->nwrite bytes to write, rd takes up to rq->nread bytes (rd may be wr:
 * everything is written before anything is read), *got how many were read.
 * PGC_OK, PGC_EFUNC for a malformed request, PGC_EACCESS for a register that
 * is not a setup program's. */
int      pgc_ctl(pgc_card *c, const pgctl_req *rq, const uint8_t *wr, uint8_t *rd, uint16_t *got);

/* ---- paths ---------------------------------------------------------------- */

int      pgc_getattr(pgc_card *c, const char *path, uint8_t *attr, uint32_t *size,
                     uint16_t *time, uint16_t *date);
int      pgc_setattr(pgc_card *c, const char *path, uint8_t attr);
int      pgc_mkdir(pgc_card *c, const char *path);
int      pgc_rmdir(pgc_card *c, const char *path);
int      pgc_delete(pgc_card *c, const char *path);     /* one file, no wildcards */
int      pgc_rename(pgc_card *c, const char *from, const char *to);
/* the long name of the last component of an 8.3 path */
int      pgc_longname(pgc_card *c, const char *path, char *name, uint16_t max);
/* path with every component that is a generated 8.3 alias ("MYDOCU~1.TXT",
 * anything with a '~') replaced by the long name of the entry it names, so
 * that one file has one spelling whichever name a program used. Components
 * that name nothing, or a path that would get too long, stay as they are.
 * out may be path. PGC_ENOTREADY without a drive, else PGC_OK. */
int      pgc_canonical(pgc_card *c, const char *path, char *out);

/* ---- directory search -------------------------------------------------------
 * pgc_find_first() lists dir ("" or "\" = root) entry by entry, with "." and
 * ".." first in a subdirectory. attr is the DOS search mask (16h: hidden,
 * system and directories as well). want_long: fetch every entry's long name
 * (one more request per entry). PGC_ENOMORE at the end. */
int      pgc_find_first(pgc_card *c, pgc_find *s, const char *dir, uint8_t attr, int want_long);
int      pgc_find_next(pgc_card *c, pgc_find *s);
/* the same with a DOS pattern ("*.TXT", "FILE????.???", "NAME.EXT") that the
 * card matches against the 8.3 names, with the DOS search rules for attr */
int      pgc_find_mask(pgc_card *c, pgc_find *s, const char *dir, const char *pattern, uint8_t attr,
                       int want_long);

/* ---- files ---------------------------------------------------------------- */

/* opens or creates path (one of the PGC_* dispositions) for need access;
 * create_attr is applied to a file that gets created. On PGC_OK f is open
 * (and bound to a card id), *result says what happened (may be NULL) and
 * attr/size/time/date describe the file (each may be NULL). */
int      pgc_open(pgc_card *c, pgc_file *f, const char *path, int disposition, uint8_t need,
                  uint8_t create_attr, int *result, uint8_t *attr, uint32_t *size,
                  uint16_t *time, uint16_t *date);

/* makes f an open file for an existing path without touching the card (the
 * caller has seen it with pgc_getattr()); the card id is taken on first use */
int      pgc_file_attach(pgc_card *c, pgc_file *f, const char *path, uint8_t need);

/* raises the access f needs (a later handle wants to write) */
void     pgc_file_need(pgc_file *f, uint8_t need);

/* the file was renamed on the card: f follows it */
int      pgc_file_moved(pgc_card *c, pgc_file *f, const char *newpath);

/* closes the card id (if any) and forgets f. Returns the card's CLOSE result
 * (a write fault surfaces here), PGC_OK when nothing was bound. */
int      pgc_close(pgc_card *c, pgc_file *f);

/* drops the card id (if any) without forgetting f; PGC_OK if nothing bound */
int      pgc_unbind(pgc_card *c, pgc_file *f);

/* non-zero when f was opened on a medium that is gone */
int      pgc_file_stale(const pgc_card *c, const pgc_file *f);

int      pgc_read(pgc_card *c, pgc_file *f, uint32_t offset, void *dst, uint32_t len, uint32_t *got);
int      pgc_write(pgc_card *c, pgc_file *f, uint32_t offset, const void *src, uint32_t len, uint32_t *done);
/* sets the file size (shrinks or grows) */
int      pgc_set_size(pgc_card *c, pgc_file *f, uint32_t size);
/* current size as the card's open file sees it */
int      pgc_get_size(pgc_card *c, pgc_file *f, uint32_t *size);
/* sets the modification time (FAT packed local time) */
int      pgc_set_ftime(pgc_card *c, pgc_file *f, uint16_t dos_time, uint16_t dos_date);

/* ---- helpers ---------------------------------------------------------------- */

uint16_t pgc_strlen(const char *s);
void     pgc_memcpy(void *dst, const void *src, uint32_t n);
void     pgc_memset(void *dst, int v, uint32_t n);
int      pgc_strcmp(const char *a, const char *b);
/* "FILE    TXT" -> "FILE.TXT" (dst at least 13 bytes) */
void     pgc_fcb_to_name(const uint8_t *fcb, char *dst);
/* "FILE.TXT" / "*.*" -> 11-byte FCB form, upper case, '*' as '?' (what the
 * card does with a search pattern) */
void     pgc_name_to_fcb(const char *name, char *fcb);
/* joins dir and name with one backslash into dst (PGC_PATH_MAX); PGC_ETOOLONG */
int      pgc_join(char *dst, const char *dir, const char *name);
/* true for "", "\" */
int      pgc_is_root(const char *path);

#ifdef __cplusplus
}
#endif

#endif
