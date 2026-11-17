/*
 * PGDFS client core for the Windows drivers (PGDFS.VXD, pgdfs.sys)
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * See pgc.h for what this is and sw/dfs/PROTOCOL.md for the wire protocol.
 */

#include "pgc.h"

/* EDF5 subfunctions (AL) */
#define AL_RMDIR     0x01
#define AL_MKDIR     0x03
#define AL_CLSFIL    0x06
#define AL_READFIL   0x08
#define AL_WRITEFIL  0x09
#define AL_DISKSPACE 0x0C
#define AL_SETATTR   0x0E
#define AL_GETATTR   0x0F
#define AL_RENAME    0x11
#define AL_DELETE    0x13
#define AL_OPEN      0x16
#define AL_FINDFIRST 0x1B
#define AL_FINDNEXT  0x1C
#define AL_SKFMEND   0x21
#define AL_SETFTIME  0x24
#define AL_SPOPNFIL  0x2E
#define AL_LONGNAME  0xF1
#define AL_DIAG      0xF2

/* DIAG record fields used here (sw/dfs/dfs_server.h, DFS_DIAG_*) */
#define DIAG_OFF_VERSION  0
#define DIAG_OFF_MOUNTS   3       /* record version 3: changes with every mount, never 0 then */
#define DIAG_MOUNTS_VER   3
#define DIAG_OFF_FREECLST 4
#define DIAG_OFF_NFATENT  8
#define DIAG_OFF_CSIZE    12
#define DIAG_MIN_LEN      14

/* transport results */
#define X_OK      0
#define X_NODRIVE 1
#define X_ABORTED 2
#define X_TIMEOUT 3
#define X_BADLEN  4

/* ---- helpers -------------------------------------------------------------- */

uint16_t pgc_strlen(const char *s) {
  uint16_t n = 0;
  while (s[n] != 0) n++;
  return n;
}

void pgc_memcpy(void *dst, const void *src, uint32_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n--) *d++ = *s++;
}

void pgc_memset(void *dst, int v, uint32_t n) {
  uint8_t *d = (uint8_t *)dst;
  while (n--) *d++ = (uint8_t)v;
}

int pgc_strcmp(const char *a, const char *b) {
  while (*a != 0 && *a == *b) { a++; b++; }
  return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

void pgc_fcb_to_name(const uint8_t *fcb, char *dst) {
  int i, n = 0;
  for (i = 0; i < 8 && fcb[i] != ' '; i++) dst[n++] = (char)fcb[i];
  if (fcb[8] != ' ') {
    dst[n++] = '.';
    for (i = 8; i < 11 && fcb[i] != ' '; i++) dst[n++] = (char)fcb[i];
  }
  dst[n] = 0;
}

static char upper(char ch) {
  return (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
}

void pgc_name_to_fcb(const char *name, char *fcb) {
  int i = 0, j = 0;
  pgc_memset(fcb, ' ', 11);
  if (name[0] == '.') {                 /* "." and ".." */
    while (name[j] == '.' && i < 11) fcb[i++] = name[j++];
    return;
  }
  while (i < 8) {
    char ch = name[j];
    if (ch == 0 || ch == '.') break;
    if (ch == '*') {
      while (i < 8) fcb[i++] = '?';
      break;
    }
    fcb[i++] = upper(ch);
    j++;
  }
  while (name[j] != 0 && name[j] != '.') j++;
  if (name[j] != '.') return;
  j++;
  i = 8;
  while (i < 11) {
    char ch = name[j];
    if (ch == 0 || ch == '.') break;
    if (ch == '*') {
      while (i < 11) fcb[i++] = '?';
      break;
    }
    fcb[i++] = upper(ch);
    j++;
  }
}

int pgc_is_root(const char *path) {
  while (*path == '\\' || *path == '/') path++;
  return *path == 0;
}

int pgc_join(char *dst, const char *dir, const char *name) {
  uint16_t a = pgc_strlen(dir), b = pgc_strlen(name), n;
  while (a > 0 && (dir[a - 1] == '\\' || dir[a - 1] == '/')) a--;
  n = (uint16_t)(a + 1 + b);
  if (n >= PGC_PATH_MAX) return PGC_ETOOLONG;
  pgc_memcpy(dst, dir, a);
  dst[a] = '\\';
  pgc_memcpy(dst + a + 1, name, b);
  dst[n] = 0;
  return PGC_OK;
}

/* ---- registers ------------------------------------------------------------- */

static void sel(uint8_t reg) {
  pgc_plat_outb(CONTROL_PORT, 0xCC);  /* knock... */
  pgc_plat_outb(CONTROL_PORT, reg);   /* ...and select */
}

static uint8_t read8(uint8_t reg) {
  sel(reg);
  return pgc_plat_inb(DATA_PORT_HIGH);
}

/* 16-bit registers: low byte through DATA_PORT_LOW, high through DATA_PORT_HIGH,
 * the order pgusinit's inpw(DATA_PORT_LOW) produces on the bus */
static uint16_t read16(uint8_t reg) {
  uint8_t lo, hi;
  sel(reg);
  lo = pgc_plat_inb(DATA_PORT_LOW);
  hi = pgc_plat_inb(DATA_PORT_HIGH);
  return (uint16_t)(lo | (hi << 8));
}

static uint8_t status(pgc_card *c) {
  sel(CMD_DFSSTAT);
  c->last_status = pgc_plat_inb(DATA_PORT_HIGH);
  return c->last_status;
}

static void abort_xact(void) {
  sel(CMD_DFSSTAT);
  pgc_plat_outb(DATA_PORT_HIGH, 0);   /* any write aborts */
}

/* the data window takes words (two stream bytes each), an odd tail as a byte */
static void out_bytes(pgc_card *c, const uint8_t *src, uint16_t n) {
  if (n >= 2) pgc_plat_outsw(c->port, src, (uint16_t)(n >> 1));
  if (n & 1) pgc_plat_outb(c->port, src[n - 1]);
}

static void in_bytes(pgc_card *c, uint8_t *dst, uint16_t n) {
  if (n >= 2) pgc_plat_insw(c->port, dst, (uint16_t)(n >> 1));
  if (n & 1) dst[n - 1] = pgc_plat_inb(c->port);
}

/* one transaction on c->buf (request frame in, answer frame out) */
static int xact(pgc_card *c) {
  uint16_t len = get16(c->buf);
  uint32_t start;
  uint8_t st;
  int attempt;

  c->retried = 0;
  c->transactions++;
  for (attempt = 0;; attempt++) {
    sel(CMD_DFSREQ);
    out_bytes(c, c->buf, len);
    sel(CMD_DFSEXEC);
    pgc_plat_outb(DATA_PORT_HIGH, 1);
    sel(CMD_DFSSTAT);
    start = pgc_plat_ms();
    for (;;) {
      uint32_t waited;
      st = pgc_plat_inb(DATA_PORT_HIGH);
      if (st == DFS_STATUS_READY || st == DFS_STATUS_ABORTED || st == DFS_STATUS_NODRIVE) break;
      waited = pgc_plat_ms() - start;
      if (waited >= PGC_TIMEOUT_MS) {
        c->last_status = st;
        c->timeouts++;
        abort_xact();
        return X_TIMEOUT;
      }
      pgc_plat_poll(waited);
      sel(CMD_DFSSTAT);   /* the platform may have let others use the card */
    }
    c->last_status = st;
    if (st == DFS_STATUS_READY) break;
    if (st == DFS_STATUS_NODRIVE) return X_NODRIVE;
    /* ABORTED: a bus glitch, or the card finished a request given up on
     * earlier; the frame is still in c->buf, send it once more */
    if (attempt != 0) return X_ABORTED;
    c->retried = 1;
  }
  sel(CMD_DFSRESP);
  in_bytes(c, c->buf, PGC_HDR);
  len = get16(c->buf);
  if (len < PGC_HDR || len > PGC_FRAME) {
    abort_xact();
    return X_BADLEN;
  }
  in_bytes(c, c->buf + PGC_HDR, (uint16_t)(len - PGC_HDR));
  return X_OK;
}

/* ---- the medium ------------------------------------------------------------ */

/* the card dropped (or may have dropped) every file id */
static void lost_ids(pgc_card *c) {
  int i;
  for (i = 0; i < PGC_SLOTS; i++) {
    if (c->slot[i] != 0) c->slot[i]->id = 0;
    c->slot[i] = 0;
  }
  c->mount_gen++;
}

static void read_info(char *dst) {
  uint16_t i, n = 0;
  sel(CMD_DFSINFO);
  for (i = 0; i < 255; i++) {
    uint8_t ch = pgc_plat_inb(DATA_PORT_HIGH);
    if (ch == 0 || ch == 0xFF) break;   /* 0xFF: nobody home */
    if (n + 1 < PGC_INFO_MAX) dst[n++] = (char)ch;
  }
  dst[n] = 0;
}

/* the card's mount count, if the firmware has one (DIAG works with or
 * without a drive). Uses the frame buffer; runs the transaction itself, so
 * that a failure here does not come back into check_medium(). */
static uint8_t read_mounts(pgc_card *c) {
  uint16_t len;
  if (c->mounts_known == 2) return 0;
  put16(c->buf, PGC_HDR);
  c->buf[2] = 0;
  c->buf[3] = AL_DIAG;
  if (xact(c) != X_OK) return 0;        /* ask again at the next look */
  len = get16(c->buf);
  if (get16(c->buf + 2) != PGC_OK || len <= PGC_HDR + DIAG_OFF_MOUNTS ||
      c->buf[PGC_HDR + DIAG_OFF_VERSION] < DIAG_MOUNTS_VER) {
    c->mounts_known = 2;                /* firmware before the mount count */
    return 0;
  }
  c->mounts_known = 1;
  return c->buf[PGC_HDR + DIAG_OFF_MOUNTS];
}

/* status byte = presence, volume string = identity, mount count = whether
 * the card has started afresh since the last look. The card reports NODRIVE
 * in place of IDLE and ABORTED only, not while an answer is READY, so the
 * status is read after an abort (harmless between transactions: the next
 * CMD_DFSREQ starts afresh). The frame buffer is used: callers are done with
 * the answer in it before they get here. */
static void check_medium(pgc_card *c) {
  char info[PGC_INFO_MAX];
  uint8_t st, mounts;
  abort_xact();
  st = status(c);
  c->checked_ms = pgc_plat_ms();
  if (st == DFS_STATUS_NODRIVE) {
    if (c->present) {
      c->present = 0;
      c->change_count++;
      lost_ids(c);            /* the card forgets its files on unmount */
    }
    return;
  }
  read_info(info);
  mounts = read_mounts(c);
  if (!c->present) {
    c->present = 1;
    c->change_count++;
    lost_ids(c);              /* a fresh mount: nothing of before is open */
    if (c->seen && pgc_strcmp(info, c->info) != 0) c->media_gen++;
  } else if (c->seen && pgc_strcmp(info, c->info) != 0) {
    /* swapped between two looks: the card remounted */
    c->change_count++;
    lost_ids(c);
    c->media_gen++;
  } else if (mounts != 0 && c->mounts != 0 && mounts != c->mounts) {
    /* the same drive, pulled and plugged in again between two looks */
    c->change_count++;
    lost_ids(c);
  }
  if (mounts != 0) c->mounts = mounts;
  if (!c->seen || pgc_strcmp(info, c->info) != 0) {
    pgc_memcpy(c->info, info, sizeof(info));
    c->seen = 1;
    c->clusters = c->free_clusters = c->cluster_bytes = 0;
  }
}

int pgc_media(pgc_card *c, int force) {
  if (!c->probed) return PGC_ENOCARD;
  if (force || !c->present || (uint32_t)(pgc_plat_ms() - c->checked_ms) >= PGC_RECHECK_MS) {
    check_medium(c);
  }
  return c->present ? PGC_OK : PGC_ENOTREADY;
}

/* evidence that the card forgot its files behind our back (a replug that
 * no status read happened to see) */
static void remounted(pgc_card *c) {
  lost_ids(c);
  check_medium(c);
}

/* ---- control requests ---------------------------------------------------------- */

#define SAVE_WAIT_MS 150    /* the card writes its flash; PGUSINIT waits 100 ms */

static int ctl_allowed(uint8_t reg) {
  if (reg < CMD_DFSSTAT) return 1;      /* identification and settings */
  return reg == CMD_SAVE || reg == CMD_REBOOT || reg == CMD_HWTYPE || reg == CMD_CHIPTYPE;
}

int pgc_ctl(pgc_card *c, const pgctl_req *rq, const uint8_t *wr, uint8_t *rd, uint16_t *got) {
  uint8_t reg = rq->reg, flags = rq->flags, stop = rq->stop;
  uint16_t nwrite = rq->nwrite, nread = rq->nread;
  uint32_t skip = rq->skip, k;
  uint16_t i, n = 0;

  *got = 0;
  if (rq->version != PGCTL_VERSION || nwrite > PGCTL_MAX_DATA || nread > PGCTL_MAX_DATA ||
      skip > PGCTL_MAX_SKIP || (flags & ~(PGCTL_WORD | PGCTL_STOP)) != 0) {
    return PGC_EFUNC;
  }
  if (!ctl_allowed(reg)) return PGC_EACCESS;

  sel(reg);
  for (i = 0; i < nwrite; i++) {
    pgc_plat_outb((uint16_t)(((flags & PGCTL_WORD) && i == 0) ? DATA_PORT_LOW : DATA_PORT_HIGH), wr[i]);
  }
  for (k = 0; k < skip; k++) {
    if (pgc_plat_inb(DATA_PORT_HIGH) == stop && (flags & PGCTL_STOP)) return PGC_OK;
  }
  for (i = 0; i < nread; i++) {
    uint8_t b = pgc_plat_inb(
        (uint16_t)(((flags & PGCTL_WORD) && i == 0 && skip == 0) ? DATA_PORT_LOW : DATA_PORT_HIGH));
    rd[n++] = b;
    if (b == stop && (flags & PGCTL_STOP)) break;
  }
  *got = n;

  if (nwrite != 0 && reg == CMD_SAVE) {
    uint32_t start = pgc_plat_ms(), waited;
    while ((waited = pgc_plat_ms() - start) < SAVE_WAIT_MS) pgc_plat_poll(waited);
  } else if (nwrite != 0 && reg == CMD_REBOOT) {
    /* the card starts afresh: no drive until it has mounted one again, and
     * a mount count that starts over */
    if (c->present) {
      c->present = 0;
      c->change_count++;
    }
    lost_ids(c);
    c->mounts = 0;
  }
  return PGC_OK;
}

/* ---- requests --------------------------------------------------------------- */

static uint8_t *payload(pgc_card *c) {
  return c->buf + PGC_HDR;
}

/* runs the request in c->buf (payload already in place) and returns AX, or
 * PGC_ENOTREADY for transport failures. *alen gets the answer payload length. */
static int request(pgc_card *c, uint8_t al, uint16_t plen, uint16_t *alen) {
  int r;
  uint16_t ax;
  put16(c->buf, (uint16_t)(PGC_HDR + plen));
  c->buf[2] = 0;              /* drive 0, no flags */
  c->buf[3] = al;
  r = xact(c);
  if (alen) *alen = 0;
  if (r == X_NODRIVE) {
    check_medium(c);
    return PGC_ENOTREADY;
  }
  if (r != X_OK) return PGC_ENOTREADY;
  ax = get16(c->buf + 2);
  if (alen) *alen = (uint16_t)(get16(c->buf) - PGC_HDR);
  if (ax == PGC_ENOTREADY) check_medium(c);
  return ax;
}

/* path payload after a fixed head of n bytes */
static int put_path(pgc_card *c, uint16_t at, const char *path, uint16_t *plen) {
  uint16_t n = pgc_strlen(path);
  if (n >= PGC_PATH_MAX) return PGC_ETOOLONG;
  if (n == 0) {               /* the card spells the root "\" */
    payload(c)[at] = '\\';
    n = 1;
  } else {
    pgc_memcpy(payload(c) + at, path, n);
  }
  *plen = (uint16_t)(at + n);
  return PGC_OK;
}

/* precondition of every request that needs the drive */
static int ready(pgc_card *c) {
  return pgc_media(c, 0);
}

static int path_request(pgc_card *c, uint8_t al, const char *path, uint16_t *alen) {
  uint16_t plen;
  int r = ready(c);
  if (r != PGC_OK) return r;
  r = put_path(c, 0, path, &plen);
  if (r != PGC_OK) return r;
  return request(c, al, plen, alen);
}

/* ---- card ----------------------------------------------------------------- */

int pgc_probe(pgc_card *c) {
  uint16_t maxlen, port;
  uint16_t id;
  pgc_memset(c, 0, sizeof(*c));
  if (read8(CMD_MAGIC) != 0xDD) return PGC_ENOCARD;
  c->proto = read8(CMD_PROTOCOL);
  if (c->proto < PICOGUS_PROTOCOL_VER || c->proto == 0xFF) return PGC_ENOCARD;
  maxlen = read16(CMD_DFSMAXLEN);
  port = (uint16_t)(read16(CMD_DFSPORT) & 0xFFFEu);
  if (maxlen < 128 || maxlen > 32768u) return PGC_ENOCARD;   /* no PGDFS, or disabled */
  if (port < 0x100 || port > 0x3FE) return PGC_ENOCARD;
  c->port = port;
  c->maxpl = (maxlen < PGC_MAX_PAYLOAD) ? maxlen : PGC_MAX_PAYLOAD;
  c->probed = 1;
  /* a request left BUSY by someone else finishes first; then start clean
   * (but give up at once if the card does not answer: Windows is starting) */
  check_medium(c);
  if (c->present) {
    for (id = 1; id <= PGC_SLOTS && c->timeouts == 0; id++) {
      put16(payload(c), id);
      (void)request(c, AL_CLSFIL, 2, 0);
    }
  }
  return PGC_OK;
}

int pgc_volinfo_get(pgc_card *c, pgc_volinfo *v) {
  const char *p;
  int field = 0;
  uint16_t n = 0;
  int r = pgc_media(c, 0);
  pgc_memset(v, 0, sizeof(*v));
  if (r != PGC_OK) return r;
  for (p = c->info; *p != 0; p++) {
    char ch = *p;
    if (ch == '|') {
      field++;
      n = 0;
      continue;
    }
    switch (field) {
    case 0:
      if (n < sizeof(v->label) - 1) v->label[n++] = ch;
      break;
    case 1:
      if (n < sizeof(v->fs) - 1) v->fs[n++] = ch;
      break;
    case 2:
      if (ch >= '0' && ch <= '9') v->size_mb = v->size_mb * 10 + (uint32_t)(ch - '0');
      break;
    case 3:
      if (ch >= '0' && ch <= '9') v->serial = (v->serial << 4) | (uint32_t)(ch - '0');
      else if (ch >= 'A' && ch <= 'F') v->serial = (v->serial << 4) | (uint32_t)(ch - 'A' + 10);
      else if (ch >= 'a' && ch <= 'f') v->serial = (v->serial << 4) | (uint32_t)(ch - 'a' + 10);
      break;
    }
  }
  return PGC_OK;
}

void pgc_set_time(pgc_card *c, uint16_t dos_time, uint16_t dos_date) {
  if (!c->probed) return;
  sel(CMD_DFSTIME);
  pgc_plat_outb(DATA_PORT_HIGH, (uint8_t)dos_time);
  pgc_plat_outb(DATA_PORT_HIGH, (uint8_t)(dos_time >> 8));
  pgc_plat_outb(DATA_PORT_HIGH, (uint8_t)dos_date);
  pgc_plat_outb(DATA_PORT_HIGH, (uint8_t)(dos_date >> 8));
}

int pgc_space(pgc_card *c, uint32_t *clusters, uint32_t *free_clusters, uint32_t *cluster_bytes) {
  uint16_t alen;
  uint32_t tot_units, free_units;
  int ax, r = ready(c);
  if (r != PGC_OK) return r;
  /* DISKSPACE first: on the first call after a mount the card counts the
   * free clusters (it does not trust the FSInfo sector), which DIAG then
   * reports exactly. AX is sectors per cluster here, not an error code. */
  ax = request(c, AL_DISKSPACE, 0, &alen);
  if (alen < 6) return (ax != 0 && ax != 1) ? ax : PGC_EPROTO;
  tot_units = get16(payload(c));
  free_units = get16(payload(c) + 4);
  ax = request(c, AL_DIAG, 0, &alen);
  if (ax == PGC_OK && alen >= DIAG_MIN_LEN) {
    uint32_t fc = get32(payload(c) + DIAG_OFF_FREECLST);
    uint32_t ne = get32(payload(c) + DIAG_OFF_NFATENT);
    uint32_t cs = get16(payload(c) + DIAG_OFF_CSIZE);
    if (ne > 2 && cs != 0 && fc <= ne - 2) {
      c->clusters = ne - 2;
      c->free_clusters = fc;
      c->cluster_bytes = cs * 512u;   /* the card's FatFs uses 512-byte sectors */
      *clusters = c->clusters;
      *free_clusters = c->free_clusters;
      *cluster_bytes = c->cluster_bytes;
      return PGC_OK;
    }
  }
  /* firmware without DIAG: DOS units of 32 KB, capped at 65535 */
  *clusters = tot_units;
  *free_clusters = free_units;
  *cluster_bytes = 32768u;
  return PGC_OK;
}

/* ---- paths ---------------------------------------------------------------- */

int pgc_getattr(pgc_card *c, const char *path, uint8_t *attr, uint32_t *size,
                uint16_t *time, uint16_t *date) {
  uint16_t alen;
  int r = path_request(c, AL_GETATTR, path, &alen);
  if (r != PGC_OK) return r;
  if (alen < 9) return PGC_EPROTO;
  if (time) *time = get16(payload(c));
  if (date) *date = get16(payload(c) + 2);
  if (size) *size = get32(payload(c) + 4);
  if (attr) *attr = payload(c)[8];
  return PGC_OK;
}

int pgc_setattr(pgc_card *c, const char *path, uint8_t attr) {
  uint16_t plen;
  int r = ready(c);
  if (r != PGC_OK) return r;
  payload(c)[0] = attr;
  r = put_path(c, 1, path, &plen);
  if (r != PGC_OK) return r;
  return request(c, AL_SETATTR, plen, 0);
}

int pgc_mkdir(pgc_card *c, const char *path) {
  return path_request(c, AL_MKDIR, path, 0);
}

int pgc_rmdir(pgc_card *c, const char *path) {
  return path_request(c, AL_RMDIR, path, 0);
}

int pgc_delete(pgc_card *c, const char *path) {
  const char *p;
  for (p = path; *p != 0; p++) {
    if (*p == '?' || *p == '*') return PGC_ENOFILE;   /* the card would take it as a mask */
  }
  return path_request(c, AL_DELETE, path, 0);
}

int pgc_rename(pgc_card *c, const char *from, const char *to) {
  uint16_t a = pgc_strlen(from), b = pgc_strlen(to);
  int r = ready(c);
  if (r != PGC_OK) return r;
  if (a == 0 || b == 0) return PGC_EACCESS;
  if (a >= PGC_PATH_MAX || b >= PGC_PATH_MAX) return PGC_ETOOLONG;
  payload(c)[0] = (uint8_t)a;
  pgc_memcpy(payload(c) + 1, from, a);
  pgc_memcpy(payload(c) + 1 + a, to, b);
  return request(c, AL_RENAME, (uint16_t)(1 + a + b), 0);
}

int pgc_longname(pgc_card *c, const char *path, char *name, uint16_t max) {
  uint16_t alen;
  int r;
  if (max == 0) return PGC_EFUNC;
  name[0] = 0;
  r = path_request(c, AL_LONGNAME, path, &alen);
  if (r != PGC_OK) return r;
  if (alen >= max) alen = (uint16_t)(max - 1);
  pgc_memcpy(name, payload(c), alen);
  name[alen] = 0;
  return PGC_OK;
}

int pgc_canonical(pgc_card *c, const char *path, char *out) {
  char res[PGC_PATH_MAX], probe[PGC_PATH_MAX], name[PGC_NAME_MAX];
  uint16_t pos = 0, i = 0, n = pgc_strlen(path);
  int r = ready(c);
  if (r != PGC_OK) return r;
  if (n >= PGC_PATH_MAX) return PGC_ETOOLONG;
  while (i < n) {
    uint16_t start, len, j;
    int alias = 0;
    while (i < n && (path[i] == '\\' || path[i] == '/')) i++;
    if (i >= n) break;
    start = i;
    while (i < n && path[i] != '\\' && path[i] != '/') {
      if (path[i] == '~') alias = 1;
      i++;
    }
    len = (uint16_t)(i - start);
    /* res + "\" + component */
    if (pos + 1 + len >= PGC_PATH_MAX) return PGC_ETOOLONG;
    pgc_memcpy(probe, res, pos);
    probe[pos] = '\\';
    pgc_memcpy(probe + pos + 1, path + start, len);
    probe[pos + 1 + len] = 0;
    if (alias && pgc_longname(c, probe, name, sizeof(name)) == PGC_OK && name[0] != 0) {
      uint16_t nl = pgc_strlen(name);
      for (j = 0; j < nl && name[j] != '\\' && name[j] != '/'; j++) { }
      if (j == nl && pos + 1 + nl < PGC_PATH_MAX) {
        probe[pos + 1] = 0;
        pgc_memcpy(probe + pos + 1, name, (uint32_t)nl + 1);
        len = nl;
      }
    }
    pgc_memcpy(res, probe, (uint32_t)pos + 1 + len + 1);
    pos = (uint16_t)(pos + 1 + len);
  }
  if (pos == 0) {
    out[0] = path[0] == '\\' || path[0] == '/' ? '\\' : 0;
    if (out[0]) out[1] = 0;
    return PGC_OK;
  }
  pgc_memcpy(out, res, (uint32_t)pos + 1);
  return PGC_OK;
}

/* ---- directory search ------------------------------------------------------- */

/* AfffffffffffttddssssCCpp -> s, then the long name if wanted */
static int find_entry(pgc_card *c, pgc_find *s, uint16_t alen) {
  const uint8_t *e = payload(c);
  if (alen < 24) return PGC_EPROTO;
  s->attr = e[0];
  pgc_fcb_to_name(e + 1, s->shortname);
  s->time = get16(e + 12);
  s->date = get16(e + 14);
  s->size = get32(e + 16);
  s->dir_id = get16(e + 20);
  s->pos = get16(e + 22);
  pgc_memcpy(s->longname, s->shortname, 13);
  if (s->want_long && s->shortname[0] != '.') {
    char full[PGC_PATH_MAX];
    if (pgc_join(full, s->dir, s->shortname) == PGC_OK) {
      char name[PGC_NAME_MAX];
      int r = pgc_longname(c, full, name, sizeof(name));
      if (r == PGC_OK && name[0] != 0) pgc_memcpy(s->longname, name, (uint32_t)pgc_strlen(name) + 1);
      else if (r == PGC_ENOTREADY) return r;
    }
  }
  return PGC_OK;
}

int pgc_find_first(pgc_card *c, pgc_find *s, const char *dir, uint8_t attr, int want_long) {
  return pgc_find_mask(c, s, dir, "????????.???", attr, want_long);
}

int pgc_find_mask(pgc_card *c, pgc_find *s, const char *dir, const char *pattern, uint8_t attr,
                  int want_long) {
  char mask[PGC_PATH_MAX];
  uint16_t plen, alen, n;
  int r = ready(c);
  if (r != PGC_OK) return r;
  n = pgc_strlen(dir);
  if (n >= PGC_PATH_MAX) return PGC_ETOOLONG;
  pgc_memcpy(s->dir, dir, (uint32_t)n + 1);
  s->sattr = attr;
  s->want_long = (uint8_t)(want_long != 0);
  pgc_name_to_fcb(pattern, s->mask);
  r = pgc_join(mask, dir, pattern);
  if (r != PGC_OK) return r;
  payload(c)[0] = attr;
  r = put_path(c, 1, mask, &plen);
  if (r != PGC_OK) return r;
  r = request(c, AL_FINDFIRST, plen, &alen);
  if (r == PGC_ENOFILE) r = PGC_ENOMORE;
  if (r != PGC_OK) return r;
  return find_entry(c, s, alen);
}

int pgc_find_next(pgc_card *c, pgc_find *s) {
  uint8_t *p = payload(c);
  uint16_t alen;
  int r = ready(c);
  if (r != PGC_OK) return r;
  put16(p, s->dir_id);
  put16(p + 2, s->pos);
  p[4] = s->sattr;
  pgc_memcpy(p + 5, s->mask, 11);
  r = request(c, AL_FINDNEXT, 16, &alen);
  if (r != PGC_OK) return r;
  return find_entry(c, s, alen);
}

/* ---- card file ids ---------------------------------------------------------- */

int pgc_file_stale(const pgc_card *c, const pgc_file *f) {
  return f->media_gen != c->media_gen;
}

static void unbind_slot(pgc_card *c, pgc_file *f) {
  if (f->id >= 1 && f->id <= PGC_SLOTS && c->slot[f->id - 1] == f) c->slot[f->id - 1] = 0;
  f->id = 0;
}

static int bound(const pgc_card *c, const pgc_file *f) {
  return f->id != 0 && f->mount == c->mount_gen;
}

/* CLOSE on the card for id, whoever holds it in the table */
static int close_id(pgc_card *c, uint16_t id) {
  put16(payload(c), id);
  return request(c, AL_CLSFIL, 2, 0);
}

/* frees one card id: the least recently used binding. 0 if none is left. */
static int evict_one(pgc_card *c, const pgc_file *keep) {
  int i, victim = -1;
  for (i = 0; i < PGC_SLOTS; i++) {
    pgc_file *f = c->slot[i];
    if (f == 0 || f == keep) continue;
    if (victim < 0 || (int32_t)(f->lru - c->slot[victim]->lru) < 0) victim = i;
  }
  if (victim < 0) return 0;
  {
    pgc_file *f = c->slot[victim];
    unbind_slot(c, f);
    (void)close_id(c, (uint16_t)(victim + 1));
  }
  return 1;
}

/* closes ids the card holds but the table does not know (an answer lost to a
 * timeout, a DOS program before us) */
static void reap(pgc_card *c) {
  uint16_t id;
  for (id = 1; id <= PGC_CARD_FILES; id++) {
    if (c->slot[id - 1] == 0) (void)close_id(c, id);
  }
}

static int count_bound(const pgc_card *c) {
  int i, n = 0;
  for (i = 0; i < PGC_SLOTS; i++) {
    if (c->slot[i] != 0) n++;
  }
  return n;
}

/* the card answered with id for f: record it, unless the table says the id
 * is in use, which means the card forgot everything since. fresh: f was just
 * opened (created) on the medium that is there now; otherwise it is a file
 * re-opened by path, which keeps the medium it was opened on. */
static void take_id(pgc_card *c, pgc_file *f, uint16_t id, uint8_t mode, int fresh) {
  if (id >= 1 && id <= PGC_SLOTS && c->slot[id - 1] != 0 && c->slot[id - 1] != f) {
    remounted(c);
    if (fresh) f->media_gen = c->media_gen;
  }
  if (id < 1 || id > PGC_SLOTS) {
    /* beyond what is tracked (a future card with a bigger table): use it
     * without the remount evidence */
    f->id = id;
  } else {
    if (c->slot[id - 1] != 0 && c->slot[id - 1] != f) c->slot[id - 1]->id = 0;
    c->slot[id - 1] = f;
    f->id = id;
  }
  f->mode = mode;
  f->mount = c->mount_gen;
  f->lru = ++c->lru_clock;
}

/* opens the request for f on the card; on EHANDLES frees ids and retries */
static int open_on_card(pgc_card *c, pgc_file *f, uint8_t al, uint16_t stackw, uint16_t action,
                        uint16_t omode, uint16_t *alen) {
  int r, reaped = 0;
  for (;;) {
    uint16_t plen;
    uint8_t *p = payload(c);
    put16(p, stackw);
    put16(p + 2, action);
    put16(p + 4, omode);
    r = put_path(c, 6, f->path, &plen);
    if (r != PGC_OK) return r;
    r = request(c, al, plen, alen);
    if (r != PGC_EHANDLES) return r;
    /* full: give back the least recently used id; when the table holds
     * fewer ids than the card has, someone else's are in the way */
    if (!reaped && count_bound(c) < PGC_CARD_FILES) {
      reaped = 1;
      reap(c);
    } else if (!evict_one(c, f)) {
      if (reaped) return r;
      reaped = 1;
      reap(c);
    }
  }
}

/* makes sure f holds a card id with at least the access it needs */
static int bind(pgc_card *c, pgc_file *f) {
  uint16_t alen;
  int r;
  if (!f->opened) return PGC_EHANDLE;
  if (pgc_file_stale(c, f)) return PGC_ESTALE;
  r = ready(c);
  if (r != PGC_OK) return r;
  if (pgc_file_stale(c, f)) return PGC_ESTALE;   /* the check may have found a new medium */
  if (bound(c, f) && f->mode >= f->need) {
    f->lru = ++c->lru_clock;
    return PGC_OK;
  }
  if (bound(c, f)) {          /* upgrade read -> read/write */
    uint16_t id = f->id;
    unbind_slot(c, f);
    (void)close_id(c, id);
  } else {
    unbind_slot(c, f);
  }
  r = open_on_card(c, f, AL_OPEN, f->need, 0, 0, &alen);
  if (r != PGC_OK) return r;
  if (alen < 25) return PGC_EPROTO;
  take_id(c, f, get16(payload(c) + 20), f->need, 0);
  if (pgc_file_stale(c, f)) {
    /* a different medium happens to have a file of that name: not ours */
    pgc_unbind(c, f);
    return PGC_ESTALE;
  }
  return PGC_OK;
}

/* runs an id request built by fill(); an "invalid handle" answer means the
 * card was remounted unnoticed: rebind once (same medium) and retry */
typedef int (*fill_fn)(pgc_card *c, pgc_file *f, void *ctx, uint16_t *plen);

static int id_request(pgc_card *c, pgc_file *f, uint8_t al, fill_fn fill, void *ctx, uint16_t *alen) {
  int r, attempt;
  for (attempt = 0;; attempt++) {
    uint16_t plen;
    r = bind(c, f);
    if (r != PGC_OK) return r;
    r = fill(c, f, ctx, &plen);
    if (r != PGC_OK) return r;
    r = request(c, al, plen, alen);
    if (r != PGC_EHANDLE || attempt != 0) break;
    remounted(c);
  }
  if (r == PGC_EHANDLE) {
    unbind_slot(c, f);
  } else if (r == PGC_ENOTREADY) {
    if (pgc_file_stale(c, f)) r = PGC_ESTALE;
  }
  return r;
}

/* ---- files ---------------------------------------------------------------- */

static int set_path(pgc_file *f, const char *path) {
  uint16_t n = pgc_strlen(path);
  if (n >= PGC_PATH_MAX) return PGC_ETOOLONG;
  pgc_memcpy(f->path, path, (uint32_t)n + 1);
  return PGC_OK;
}

int pgc_open(pgc_card *c, pgc_file *f, const char *path, int disposition, uint8_t need,
             uint8_t create_attr, int *result, uint8_t *attr, uint32_t *size,
             uint16_t *time, uint16_t *date) {
  uint16_t alen;
  uint8_t *a;
  int r;
  pgc_memset(f, 0, sizeof(*f));
  r = set_path(f, path);
  if (r != PGC_OK) return r;
  if (pgc_is_root(path)) return PGC_EACCESS;
  r = ready(c);
  if (r != PGC_OK) return r;
  f->media_gen = c->media_gen;
  f->need = (need == PGC_READ) ? PGC_READ : PGC_READWRITE;
  /* SPOPNFIL: stack word = attributes of a new file, action, open mode */
  r = open_on_card(c, f, AL_SPOPNFIL, create_attr, (uint16_t)disposition, f->need, &alen);
  if (r != PGC_OK) return r;
  if (alen < 25) return PGC_EPROTO;
  a = payload(c);
  if (attr) *attr = a[0];
  if (time) *time = get16(a + 12);
  if (date) *date = get16(a + 14);
  if (size) *size = get32(a + 16);
  if (result) *result = get16(a + 22);
  f->opened = 1;
  f->media_gen = c->media_gen;
  take_id(c, f, get16(a + 20), f->need, 1);
  return PGC_OK;
}

int pgc_file_attach(pgc_card *c, pgc_file *f, const char *path, uint8_t need) {
  int r;
  pgc_memset(f, 0, sizeof(*f));
  r = set_path(f, path);
  if (r != PGC_OK) return r;
  f->media_gen = c->media_gen;
  f->need = (need == PGC_READ) ? PGC_READ : PGC_READWRITE;
  f->opened = 1;
  return PGC_OK;
}

void pgc_file_need(pgc_file *f, uint8_t need) {
  if (need != PGC_READ) f->need = PGC_READWRITE;
}

int pgc_file_moved(pgc_card *c, pgc_file *f, const char *newpath) {
  (void)c;
  return set_path(f, newpath);
}

int pgc_unbind(pgc_card *c, pgc_file *f) {
  int r = PGC_OK;
  if (bound(c, f)) {
    uint16_t id = f->id;
    unbind_slot(c, f);
    r = close_id(c, id);
    if (r == PGC_EHANDLE) r = PGC_OK;   /* the card had forgotten it already */
  } else {
    unbind_slot(c, f);
  }
  return r;
}

int pgc_close(pgc_card *c, pgc_file *f) {
  int r = PGC_OK;
  if (f->opened) r = pgc_unbind(c, f);
  f->opened = 0;
  return r;
}

/* READ: OOOO offset, SS id, LL length */
typedef struct {
  uint32_t offset;
  uint16_t len;
  const uint8_t *src;
} io_ctx;

static int fill_read(pgc_card *c, pgc_file *f, void *ctx, uint16_t *plen) {
  io_ctx *x = (io_ctx *)ctx;
  uint8_t *p = payload(c);
  put32(p, x->offset);
  put16(p + 4, f->id);
  put16(p + 6, x->len);
  *plen = 8;
  return PGC_OK;
}

int pgc_read(pgc_card *c, pgc_file *f, uint32_t offset, void *dst, uint32_t len, uint32_t *got) {
  uint8_t *d = (uint8_t *)dst;
  uint32_t total = 0;
  int r = PGC_OK;
  while (total < len) {
    io_ctx x;
    uint16_t alen;
    uint32_t want = len - total;
    if (want > c->maxpl) want = c->maxpl;
    x.offset = offset + total;
    x.len = (uint16_t)want;
    x.src = 0;
    r = id_request(c, f, AL_READFIL, fill_read, &x, &alen);
    if (r != PGC_OK) break;
    if (alen > want) {
      r = PGC_EPROTO;
      break;
    }
    pgc_memcpy(d + total, payload(c), alen);
    total += alen;
    if (alen < want) break;   /* end of file */
  }
  *got = total;
  return r;
}

/* WRITE: OOOO offset, SS id, data */
static int fill_write(pgc_card *c, pgc_file *f, void *ctx, uint16_t *plen) {
  io_ctx *x = (io_ctx *)ctx;
  uint8_t *p = payload(c);
  put32(p, x->offset);
  put16(p + 4, f->id);
  if (x->len) pgc_memcpy(p + 6, x->src, x->len);
  *plen = (uint16_t)(6 + x->len);
  return PGC_OK;
}

static int write_chunk(pgc_card *c, pgc_file *f, uint32_t offset, const uint8_t *src, uint16_t len,
                       uint16_t *done) {
  io_ctx x;
  uint16_t alen;
  int r;
  x.offset = offset;
  x.len = len;
  x.src = src;
  *done = 0;
  r = id_request(c, f, AL_WRITEFIL, fill_write, &x, &alen);
  if (r != PGC_OK) return r;
  if (alen < 2) return PGC_EPROTO;
  *done = get16(payload(c));
  return PGC_OK;
}

int pgc_write(pgc_card *c, pgc_file *f, uint32_t offset, const void *src, uint32_t len, uint32_t *done) {
  const uint8_t *s = (const uint8_t *)src;
  uint32_t total = 0;
  int r = PGC_OK;
  uint16_t room = (uint16_t)(c->maxpl - 6);
  *done = 0;
  if (f->need != PGC_READWRITE) return PGC_EACCESS;
  while (total < len) {
    uint16_t n, wrote;
    uint32_t want = len - total;
    if (want > room) want = room;
    n = (uint16_t)want;
    r = write_chunk(c, f, offset + total, s + total, n, &wrote);
    if (r != PGC_OK) break;
    total += wrote;
    if (wrote < n) break;     /* volume full */
  }
  *done = total;
  if (r == PGC_OK && total < len) r = PGC_EACCESS;   /* what DOS reports for a full disk */
  return r;
}

int pgc_set_size(pgc_card *c, pgc_file *f, uint32_t size) {
  uint16_t wrote;
  if (f->need != PGC_READWRITE) return PGC_EACCESS;
  return write_chunk(c, f, size, 0, 0, &wrote);   /* zero-length write = truncate/extend */
}

/* SEEKFROMEND: oooo offset from the end, SS id */
static int fill_size(pgc_card *c, pgc_file *f, void *ctx, uint16_t *plen) {
  uint8_t *p = payload(c);
  (void)ctx;
  put32(p, 0);
  put16(p + 4, f->id);
  *plen = 6;
  return PGC_OK;
}

int pgc_get_size(pgc_card *c, pgc_file *f, uint32_t *size) {
  uint16_t alen;
  int r = id_request(c, f, AL_SKFMEND, fill_size, 0, &alen);
  if (r != PGC_OK) return r;
  if (alen < 4) return PGC_EPROTO;
  *size = get32(payload(c));
  return PGC_OK;
}

/* SETFILETIMESTAMP: tt, dd, SS */
typedef struct {
  uint16_t t, d;
} time_ctx;

static int fill_ftime(pgc_card *c, pgc_file *f, void *ctx, uint16_t *plen) {
  time_ctx *x = (time_ctx *)ctx;
  uint8_t *p = payload(c);
  put16(p, x->t);
  put16(p + 2, x->d);
  put16(p + 4, f->id);
  *plen = 6;
  return PGC_OK;
}

int pgc_set_ftime(pgc_card *c, pgc_file *f, uint16_t dos_time, uint16_t dos_date) {
  time_ctx x;
  x.t = dos_time;
  x.d = dos_date;
  return id_request(c, f, AL_SETFTIME, fill_ftime, &x, 0);
}
