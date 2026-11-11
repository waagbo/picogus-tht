/*
 * PGDFS.VXD - the PicoGUS USB drive as a drive letter on Windows 95, 98 and ME
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * A static VxD that registers with the IFS manager as a network file system
 * driver and connects itself to a drive letter at boot (IFSMgr_InitUseAdd
 * with the pseudo-UNC name \\.\PGDFS, P: unless the registry says otherwise),
 * the way the FSINFILE sample of "Inside the Windows 95 File System" does it.
 * IFSMgr then sends every file operation on that letter (from Win32
 * programs, Win16 programs and DOS boxes alike) to the volume and handle
 * functions below, which turn them into PGDFS requests to the card through
 * the shared client core (../common/pgc.c). Long file names work both ways:
 * Win32 searches return the long names (one LONGNAME request per entry),
 * DOS searches the 8.3 aliases, and the card's FatFs resolves either form in
 * a path.
 *
 * The removable drive:
 *  - no USB drive: requests fail with ERROR_NOT_READY (21), which DOS
 *    programs see as "Not ready reading drive P";
 *  - the same drive plugged back in: open files carry on;
 *  - a different drive: files opened on the previous one fail with
 *    ERROR_WRONG_DISK (34), new opens see the new drive.
 *
 * Every entry point takes one mutex: the card does one request at a time.
 * While the card works on a long request the thread sleeps in 1 ms steps,
 * except for paging I/O (program loader, memory-mapped files), which spins.
 */

#include "ifs9x.h"
#include "../common/pgc.h"

#define PGDFS_NETID     0x00FE0000ul      /* network type id for RegisterNet */
#define REMOTE_NAME     "\\\\.\\PGDFS"    /* what the drive letter is connected to */
#define REG_KEY         "System\\CurrentControlSet\\Services\\VxD\\PGDFS"
#define HKEY_LOCAL_MACHINE 0x80000002ul
#define REG_SZ          1
#define TIME_PUSH_MS    60000
#define DOS_SEARCHES    16
#define CACHE_BLOCK     4096

/* Win32 / DOS error codes for ir_error */
#define ERROR_INVALID_FUNCTION     1
#define ERROR_FILE_NOT_FOUND       2
#define ERROR_PATH_NOT_FOUND       3
#define ERROR_TOO_MANY_OPEN_FILES  4
#define ERROR_ACCESS_DENIED        5
#define ERROR_INVALID_HANDLE       6
#define ERROR_NOT_ENOUGH_MEMORY    8
#define ERROR_INVALID_DRIVE        15
#define ERROR_NO_MORE_FILES        18
#define ERROR_WRITE_PROTECT        19
#define ERROR_NOT_READY            21
#define ERROR_WRITE_FAULT          29
#define ERROR_READ_FAULT           30
#define ERROR_GEN_FAILURE          31
#define ERROR_SHARING_VIOLATION    32
#define ERROR_WRONG_DISK           34
#define ERROR_BAD_NETPATH          53
#define ERROR_BAD_NET_NAME         67
#define ERROR_FILE_EXISTS          80
#define ERROR_DISK_FULL            112
#define ERROR_INVALID_NAME         123
#define ERROR_NEGATIVE_SEEK        131
#define ERROR_FILENAME_EXCED_RANGE 206

/* ---- services (vxd.asm) ------------------------------------------------------ */

void *   __cdecl vx_heap_alloc(unsigned size, unsigned flags);
unsigned __cdecl vx_heap_free(void *p, unsigned flags);
void *   __cdecl vx_create_mutex(int boost, unsigned flags);
void     __cdecl vx_enter_mutex(void *mutex, unsigned flags);
void     __cdecl vx_leave_mutex(void *mutex);
long     __cdecl vx_reg_open_key(unsigned long hkey, const char *subkey, unsigned long *out);
long     __cdecl vx_reg_close_key(unsigned long hkey);
long     __cdecl vx_reg_query_value(unsigned long hkey, const char *name, unsigned long *reserved,
                                    unsigned long *type, void *data, unsigned long *size);
int      __cdecl vx_ifs_get_version(void);
int      __cdecl vx_ifs_register_net(pIFSFunc connect, unsigned long version, unsigned long netid);
unsigned long __cdecl vx_ifs_get_dostime(void);
int      __cdecl vx_ifs_init_use_add(use_info_2 *ui, int proid, int fstatic, int lana);
unsigned long long __cdecl vx_ifs_dos_to_win32(unsigned long dostime);
unsigned __cdecl vx_ifs_metamatch(unsigned short *pattern, unsigned short *name, int flags);
unsigned long long __cdecl vx_uni_to_bcs(unsigned char *dst, const unsigned short *src, unsigned bytes,
                                         unsigned max, unsigned charset);
unsigned long long __cdecl vx_bcs_to_uni(unsigned short *dst, const unsigned char *src, unsigned bytes,
                                         int charset);
unsigned __cdecl vx_uni_to_upper(unsigned short *dst, const unsigned short *src, unsigned bytes);
unsigned __cdecl vx_get_system_time(void);
void     __cdecl vx_time_slice_sleep(unsigned ms);
int      __cdecl vx_drive_is_ifs(int drive);
unsigned __cdecl vx_inb(unsigned port);
void     __cdecl vx_outb(unsigned port, unsigned value);
void     __cdecl vx_insw(unsigned port, void *dst, unsigned words);
void     __cdecl vx_outsw(unsigned port, const void *src, unsigned words);

/* called from the control procedure */
int  __cdecl pg_device_init(void);
void __cdecl pg_init_complete(void);

/* ---- state ------------------------------------------------------------------- */

/* A file open on the card: one per path, shared by every Windows handle of
 * that file. The card's FatFs must not have the same file open twice (it
 * keeps no locks: two FILs would each keep their own size and clusters). */
typedef struct pgnode {
  struct pgnode *next;          /* nodes */
  int refs;                     /* Windows handles */
  pgc_file f;                   /* path: the canonical (long name) form */
} pgnode;

/* a Windows handle (ir_fh of an open file) */
typedef struct pgfile {
  pgnode *node;
  ParsedPath *ppath;            /* for ENUMH_GETFILENAME */
} pgfile;

typedef struct {
  pgc_find s;
  unsigned long attr;           /* search attributes, must-match bits in 8-15 */
  int sem;                      /* IFSMgr_MetaMatch semantics */
  int pattern_dot;              /* the pattern has a dot (DOS rule for dotless 8.3 names) */
  int fresh;                    /* s holds an entry not looked at yet */
  int label;                    /* a volume label search, answered once */
  _WIN32_FIND_DATA fd;          /* filled under the mutex, copied out after it */
  unsigned short pattern[PGC_NAME_MAX];
} pgfind;

typedef struct {
  unsigned char used, gen;
  pgc_find s;
} dossearch;

static pgc_card card;
static void *mutex;
static int proid = -1;
static void *ifs_resource;      /* IFSMgr's handle of our connection */
static char resource;           /* its address is our resource handle */
static pgnode *nodes;
static dossearch dos_search[DOS_SEARCHES];
static unsigned dos_search_next;
static unsigned time_pushed;
static int time_valid;
static int no_sleep;            /* paging I/O, initialization: do not sleep */
static char letter;

/* ---- platform layer of the client core ---------------------------------------- */

uint8_t pgc_plat_inb(uint16_t port) { return (uint8_t)vx_inb(port); }
void pgc_plat_outb(uint16_t port, uint8_t value) { vx_outb(port, value); }
void pgc_plat_insw(uint16_t port, void *dst, uint16_t words) { vx_insw(port, dst, words); }
void pgc_plat_outsw(uint16_t port, const void *src, uint16_t words) { vx_outsw(port, src, words); }
uint32_t pgc_plat_ms(void) { return vx_get_system_time(); }

void pgc_plat_poll(uint32_t waited_ms) {
  if (waited_ms >= 2 && !no_sleep) vx_time_slice_sleep(1);
}

/* ---- helpers ------------------------------------------------------------------- */

/* the two halves of an EDX:EAX result, without 64-bit shifts (no runtime library) */
typedef union {
  unsigned long long q;
  struct {
    unsigned long lo, hi;
  } d;
} qword;

#define ENTER() vx_enter_mutex(mutex, 0)
#define LEAVE() vx_leave_mutex(mutex)

static int ret(pioreq pir, int err) {
  pir->ir_error = (short)err;
  return err;
}

static int w32(int r) {
  switch (r) {
  case PGC_OK:        return 0;
  case PGC_EFUNC:     return ERROR_INVALID_FUNCTION;
  case PGC_ENOFILE:   return ERROR_FILE_NOT_FOUND;
  case PGC_ENOPATH:   return ERROR_PATH_NOT_FOUND;
  case PGC_EHANDLES:  return ERROR_TOO_MANY_OPEN_FILES;
  case PGC_EACCESS:   return ERROR_ACCESS_DENIED;
  case PGC_EHANDLE:   return ERROR_INVALID_HANDLE;
  case PGC_ENOMEM:    return ERROR_NOT_ENOUGH_MEMORY;
  case PGC_EDRIVE:    return ERROR_INVALID_DRIVE;
  case PGC_ENOMORE:   return ERROR_NO_MORE_FILES;
  case PGC_EWRPROT:   return ERROR_WRITE_PROTECT;
  case PGC_ENOTREADY: return ERROR_NOT_READY;
  case PGC_EWRFAULT:  return ERROR_WRITE_FAULT;
  case PGC_ERDFAULT:  return ERROR_READ_FAULT;
  case PGC_ESHARING:  return ERROR_SHARING_VIOLATION;
  case PGC_EEXISTS:   return ERROR_FILE_EXISTS;
  case PGC_ESTALE:    return ERROR_WRONG_DISK;
  case PGC_ETOOLONG:  return ERROR_FILENAME_EXCED_RANGE;
  case PGC_ENOCARD:   return ERROR_NOT_READY;
  default:            return ERROR_GEN_FAILURE;
  }
}

static void *alloc(unsigned n) {
  void *p = vx_heap_alloc(n, 0);
  if (p) pgc_memset(p, 0, n);
  return p;
}

static unsigned wlen(const unsigned short *s) {
  unsigned n = 0;
  while (s[n]) n++;
  return n;
}

/* not from paging I/O: IFSMgr_Get_DOSTime may be pageable */
static void push_time(int force) {
  unsigned now = vx_get_system_time();
  unsigned long dt;
  if (!force && time_valid && now - time_pushed < TIME_PUSH_MS) return;
  dt = vx_ifs_get_dostime();
  pgc_set_time(&card, (uint16_t)dt, (uint16_t)(dt >> 16));
  time_pushed = now;
  time_valid = 1;
}

/* Unicode name -> the card's code page; 0 or an error code */
static int to_oem(const unsigned short *u, unsigned chars, char *dst, unsigned room, unsigned *len) {
  qword q;
  if (room == 0) return ERROR_FILENAME_EXCED_RANGE;
  q.q = vx_uni_to_bcs((unsigned char *)dst, u, chars * 2, room, BCS_OEM);
  *len = (unsigned)q.d.lo;
  if (q.d.hi != 0) return ERROR_INVALID_NAME;     /* a character the code page lacks */
  if (*len >= room) return ERROR_FILENAME_EXCED_RANGE;
  return 0;
}

/* OEM name -> Unicode, NUL terminated; returns the characters */
static unsigned to_uni(const char *oem, unsigned short *dst, unsigned max_chars) {
  unsigned n = pgc_strlen(oem), chars;
  qword q;
  if (n > max_chars - 1) n = max_chars - 1;
  q.q = vx_bcs_to_uni(dst, (const unsigned char *)oem, n, BCS_OEM);
  chars = (unsigned)q.d.lo / 2;
  if (chars > max_chars - 1) chars = max_chars - 1;
  dst[chars] = 0;
  return chars;
}

static unsigned elements(path_t pp) {
  PathElement *pe = pp->pp_elements;
  char *end = (char *)pp + pp->pp_totalLength;
  unsigned n = 0;
  while ((char *)pe < end && pe->pe_length > 2) {
    n++;
    pe = IFSNextElement(pe);
  }
  return n;
}

/* parsed path (relative to the connection) -> card path "\A\B" ("" for the
 * root). skip_last leaves out the last element (a search pattern);
 * case_name is the last element as the program spelled it (ir_uFName),
 * or NULL. 0 or an error code. */
static int card_path(path_t pp, int skip_last, const unsigned short *case_name, char *out) {
  PathElement *pe = pp->pp_elements;
  unsigned n = elements(pp), i, pos = 0;
  if (skip_last && n) n--;
  for (i = 0; i < n; i++, pe = IFSNextElement(pe)) {
    const unsigned short *src = pe->pe_unichars;
    unsigned chars = (pe->pe_length - 2) / 2, len;
    int r;
    if (case_name && i == n - 1 && wlen(case_name) == chars) src = case_name;
    if (pos + 2 >= PGC_PATH_MAX) return ERROR_FILENAME_EXCED_RANGE;
    out[pos++] = '\\';
    r = to_oem(src, chars, out + pos, PGC_PATH_MAX - 1 - pos, &len);
    if (r) return r;
    pos += len;
  }
  out[pos] = 0;
  return 0;
}

/* the last element as OEM (a search pattern) */
static int last_oem(path_t pp, char *out, unsigned room) {
  PathElement *pe = IFSLastElement(pp);
  unsigned len;
  int r;
  if (IFSIsRoot(pp)) {
    out[0] = 0;
    return 0;
  }
  r = to_oem(pe->pe_unichars, (pe->pe_length - 2) / 2, out, room, &len);
  if (r) return r;
  out[len] = 0;
  return 0;
}

static int path_eq(const char *a, const char *b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
    if (x == 0) return 1;
  }
}

/* a is b or a directory above it */
static int path_within(const char *a, const char *b) {
  unsigned n = pgc_strlen(b);
  char head[PGC_PATH_MAX];
  if (pgc_strlen(a) < n) return 0;
  if (a[n] != 0 && a[n] != '\\') return 0;
  pgc_memcpy(head, a, n);
  head[n] = 0;
  return path_eq(head, b);
}

static unsigned long hash(const char *s) {
  unsigned long h = 2166136261ul;
  for (; *s; s++) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    h = (h ^ (unsigned char)ch) * 16777619ul;
  }
  return h;
}

static _FILETIME filetime(uint16_t time, uint16_t date) {
  _FILETIME ft;
  qword t;
  t.q = 0;
  if (date) t.q = vx_ifs_dos_to_win32((unsigned long)time | ((unsigned long)date << 16));
  ft.dwLowDateTime = t.d.lo;
  ft.dwHighDateTime = t.d.hi;
  return ft;
}

/* ---- the function tables --------------------------------------------------------- */

static int __cdecl fs_bad(pioreq pir);
static int __cdecl fs_delete(pioreq pir);
static int __cdecl fs_dir(pioreq pir);
static int __cdecl fs_fileattrib(pioreq pir);
static int __cdecl fs_flush(pioreq pir);
static int __cdecl fs_getdiskinfo(pioreq pir);
static int __cdecl fs_open(pioreq pir);
static int __cdecl fs_rename(pioreq pir);
static int __cdecl fs_search(pioreq pir);
static int __cdecl fs_query(pioreq pir);
static int __cdecl fs_disconnect(pioreq pir);
static int __cdecl fs_findopen(pioreq pir);
static int __cdecl fs_connect(pioreq pir);

static int __cdecl file_read(pioreq pir);
static int __cdecl file_write(pioreq pir);
static int __cdecl file_seek(pioreq pir);
static int __cdecl file_close(pioreq pir);
static int __cdecl file_commit(pioreq pir);
static int __cdecl file_locks(pioreq pir);
static int __cdecl file_times(pioreq pir);
static int __cdecl file_enum(pioreq pir);
static int __cdecl find_next(pioreq pir);
static int __cdecl find_close(pioreq pir);

static volfunc volfuncs = {
  IFS_VERSION, IFS_REVISION, NUM_VOLFUNC,
  {
    fs_delete,          /* VFN_DELETE */
    fs_dir,             /* VFN_DIR */
    fs_fileattrib,      /* VFN_FILEATTRIB */
    fs_flush,           /* VFN_FLUSH */
    fs_getdiskinfo,     /* VFN_GETDISKINFO */
    fs_open,            /* VFN_OPEN */
    fs_rename,          /* VFN_RENAME */
    fs_search,          /* VFN_SEARCH */
    fs_query,           /* VFN_QUERY */
    fs_disconnect,      /* VFN_DISCONNECT */
    fs_bad,             /* VFN_UNCPIPEREQ */
    fs_bad,             /* VFN_IOCTL16DRIVE */
    fs_bad,             /* VFN_GETDISKPARMS */
    fs_findopen,        /* VFN_FINDOPEN */
    fs_bad              /* VFN_DASDIO */
  }
};

static hndlmisc filemisc = {
  IFS_VERSION, IFS_REVISION, NUM_HNDLMISC,
  {
    file_seek,          /* HM_SEEK */
    file_close,         /* HM_CLOSE */
    file_commit,        /* HM_COMMIT */
    file_locks,         /* HM_FILELOCKS */
    file_times,         /* HM_FILETIMES */
    fs_bad,             /* HM_PIPEREQUEST */
    fs_bad,             /* HM_HANDLEINFO */
    file_enum           /* HM_ENUMHANDLE */
  }
};

static hndlmisc findmisc = {
  IFS_VERSION, IFS_REVISION, NUM_HNDLMISC,
  {
    fs_bad,             /* HM_SEEK */
    find_close,         /* HM_CLOSE */
    fs_bad, fs_bad, fs_bad, fs_bad, fs_bad, fs_bad
  }
};

static int __cdecl fs_bad(pioreq pir) {
  return ret(pir, ERROR_INVALID_FUNCTION);
}

/* ---- connection ---------------------------------------------------------------------- */

static int element_is(PathElement *pe, const char *name) {
  unsigned chars = (pe->pe_length - 2) / 2, i;
  if (chars != pgc_strlen(name)) return 0;
  for (i = 0; i < chars; i++) {
    unsigned short c = pe->pe_unichars[i];
    if (c >= 'a' && c <= 'z') c = (unsigned short)(c - 32);
    if (c != (unsigned char)name[i]) return 0;
  }
  return 1;
}

/* FS_ConnectNetResource: \\.\PGDFS, the use made at Init_Complete (and UNC
 * paths that name it) */
static int __cdecl fs_connect(pioreq pir) {
  path_t pp = pir->ir_ppath;
  PathElement *server, *share;
  if (!pp || IFSIsRoot(pp)) return ret(pir, ERROR_BAD_NETPATH);
  server = pp->pp_elements;
  share = IFSNextElement(server);
  if ((char *)share >= (char *)pp + pp->pp_totalLength || share->pe_length <= 2) return ret(pir, ERROR_BAD_NETPATH);
  if (!element_is(server, "\\.") && !element_is(server, ".")) return ret(pir, ERROR_BAD_NETPATH);
  if (!element_is(share, "PGDFS")) return ret(pir, ERROR_BAD_NET_NAME);
  if (pir->ir_flags != RESTYPE_DISK && pir->ir_flags != RESTYPE_WILD) return ret(pir, ERROR_BAD_NET_NAME);
  ENTER();
  if (!ifs_resource) ifs_resource = pir->ir_fh;
  else pir->ir_fh = ifs_resource;     /* the connection IFSMgr has already */
  LEAVE();
  pir->ir_rh = &resource;
  pir->ir_pathSkip = 2;               /* \.\PGDFS */
  pir->ir_vfunc = &volfuncs;
  if (pir->ir_flags == RESTYPE_WILD) pir->ir_flags = RESTYPE_DISK;
  return ret(pir, 0);
}

static int __cdecl fs_disconnect(pioreq pir) {
  ENTER();
  ifs_resource = 0;
  LEAVE();
  return ret(pir, 0);
}

static void put_element(PathElement *pe, const char *ascii) {
  unsigned i, n = pgc_strlen(ascii);
  pe->pe_length = (unsigned short)(2 + 2 * n);
  for (i = 0; i < n; i++) pe->pe_unichars[i] = (unsigned char)ascii[i];
}

/* FS_QueryResourceInfo: the name of the connection (levels 0, 1) and the file
 * system (level 2, GetVolumeInformation) */
static int __cdecl fs_query(pioreq pir) {
  if (pir->ir_options == 0 || pir->ir_options == 1) {
    path_t pp = pir->ir_ppath;
    if (pp) {
      PathElement *pe = pp->pp_elements;
      put_element(pe, "\\.");
      pe = IFSNextElement(pe);
      put_element(pe, "PGDFS");
      pp->pp_prefixLength = (unsigned short)((char *)pe - (char *)pp);
      pe = IFSNextElement(pe);
      pp->pp_totalLength = (unsigned short)((char *)pe - (char *)pp);
      pe->pe_length = 0;
    }
    if (pir->ir_options == 1) {
      pir->ir_options = RESSTAT_OK;
      pir->ir_flags = RESTYPE_DISK;
    }
    return ret(pir, 0);
  }
  if (pir->ir_options == 2) {
    pgc_volinfo v;
    const char *fs = "FAT";
    unsigned n;
    ENTER();
    if (pgc_volinfo_get(&card, &v) == PGC_OK && pgc_strcmp(v.fs, "FAT32") == 0) fs = "FAT32";
    LEAVE();
    n = pgc_strlen(fs) + 1;
    if (pir->ir_data && pir->ir_length >= n) pgc_memcpy(pir->ir_data, fs, n);
    pir->ir_length = ((unsigned)MAX_PATH << 16) | 127;   /* FatFs FF_MAX_LFN on the card */
    pir->ir_options = FS_CASE_IS_PRESERVED | FS_VOL_SUPPORTS_LONG_NAMES;
    pir->ir_pos = CACHE_BLOCK;
    return ret(pir, 0);
  }
  return ret(pir, ERROR_INVALID_FUNCTION);
}

/* ---- volume functions -------------------------------------------------------------------
 * Programs' buffers (ir_data) can be pageable, and a page fault can come back
 * into this driver (a program or a mapped file on this drive). They are
 * therefore only touched outside the mutex: requests go through a bounce
 * buffer or a scratch record, never straight into the caller's memory while
 * the card's frame buffer is in use. */

/* path in its canonical form (aliases replaced by long names); 0 or an error */
static int canonical(char *path) {
  int r = pgc_canonical(&card, path, path);
  return (r == PGC_OK) ? 0 : w32(r);
}

/* the shared card file of a path, if one is open (on the current medium) */
static pgnode *find_node(const char *path) {
  pgnode *n;
  for (n = nodes; n; n = n->next) {
    if (!pgc_file_stale(&card, &n->f) && path_eq(n->f.path, path)) return n;
  }
  return 0;
}

/* gives the card id back of every open file at path or below it (the card's
 * FatFs must not have a file open that is renamed or deleted under it) */
static void unbind_within(const char *path) {
  pgnode *n;
  for (n = nodes; n; n = n->next) {
    if (path_within(n->f.path, path)) (void)pgc_unbind(&card, &n->f);
  }
}

/* FS_DeleteFile. Wildcards are left to IFSMgr (ERROR_INVALID_FUNCTION makes it
 * search and delete the files one by one, with the DOS attribute rules). */
static int __cdecl fs_delete(pioreq pir) {
  char path[PGC_PATH_MAX];
  uint8_t attr;
  int r;
  if (pir->ir_attr & FILE_FLAG_WILDCARDS) return ret(pir, ERROR_INVALID_FUNCTION);
  r = card_path(pir->ir_ppath, 0, 0, path);
  if (r) return ret(pir, r);
  ENTER();
  push_time(0);
  r = canonical(path);
  if (!r && find_node(path)) r = ERROR_ACCESS_DENIED;     /* open: FAT says no */
  if (!r) r = w32(pgc_getattr(&card, path, &attr, 0, 0, 0));
  if (!r && (attr & FILE_ATTRIBUTE_DIRECTORY)) r = ERROR_ACCESS_DENIED;
  if (!r) r = w32(pgc_delete(&card, path));
  LEAVE();
  return ret(pir, r);
}

/* one element of an 8.3 / long path translation: the entry of dir whose long
 * or short name is the element, its other name into name. Under the mutex
 * (the buffers are static, the ring 0 stack is small). */
static int translate_element(const char *dir, PathElement *pe, int want_short, char *next, char *name) {
  static pgc_find s;
  static unsigned short want[PGC_NAME_MAX], got[PGC_NAME_MAX], raw[PGC_NAME_MAX];
  unsigned chars = (pe->pe_length - 2) / 2, i;
  int r;
  if (chars >= PGC_NAME_MAX) return ERROR_FILENAME_EXCED_RANGE;
  vx_uni_to_upper(want, pe->pe_unichars, chars * 2);
  want[chars] = 0;
  for (r = pgc_find_first(&card, &s, dir, 0x16, 1); r == PGC_OK; r = pgc_find_next(&card, &s)) {
    int k;
    for (k = 0; k < 2; k++) {
      unsigned n = to_uni(k ? s.shortname : s.longname, raw, PGC_NAME_MAX);
      vx_uni_to_upper(got, raw, n * 2);
      if (n != chars) continue;
      for (i = 0; i < n && got[i] == want[i]; i++) { }
      if (i == n) break;
    }
    if (k < 2) {
      const char *other = want_short ? s.shortname : s.longname;
      pgc_memcpy(name, other, pgc_strlen(other) + 1);
      return w32(pgc_join(next, dir, s.shortname));
    }
  }
  return (r == PGC_ENOMORE) ? ERROR_PATH_NOT_FOUND : w32(r);
}

/* QUERY83_DIR / QUERYLONG_DIR: ir_ppath with every element in its 8.3 or long
 * form, into ir_ppath2 (IFSMgr's buffer) */
static int translate_path(pioreq pir, int want_short) {
  static char dir[PGC_PATH_MAX], next[PGC_PATH_MAX], name[PGC_NAME_MAX];
  static unsigned short u[PGC_NAME_MAX];
  path_t in = pir->ir_ppath, out = pir->ir_ppath2;
  PathElement *pe = in->pp_elements, *po = out->pp_elements, *last = po;
  unsigned n = elements(in), i;
  dir[0] = 0;
  for (i = 0; i < n; i++, pe = IFSNextElement(pe)) {
    unsigned chars, k;
    int r = translate_element(dir, pe, want_short, next, name);
    if (r) return r;
    chars = to_uni(name, u, PGC_NAME_MAX);
    if ((char *)po - (char *)out + 2 + 2 * chars + 2 > 2 * MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
    po->pe_length = (unsigned short)(2 + 2 * chars);
    for (k = 0; k < chars; k++) po->pe_unichars[k] = u[k];
    last = po;
    po = IFSNextElement(po);
    pgc_memcpy(dir, next, pgc_strlen(next) + 1);
  }
  out->pp_prefixLength = (unsigned short)((char *)last - (char *)out);
  out->pp_totalLength = (unsigned short)((char *)po - (char *)out);
  po->pe_length = 0;
  return 0;
}

/* FS_DirectoryMgmt */
static int __cdecl fs_dir(pioreq pir) {
  char path[PGC_PATH_MAX];
  uint8_t attr;
  int r;
  if (pir->ir_flags == QUERY83_DIR || pir->ir_flags == QUERYLONG_DIR) {
    ENTER();
    r = translate_path(pir, pir->ir_flags == QUERY83_DIR);
    LEAVE();
    return ret(pir, r);
  }
  r = card_path(pir->ir_ppath, 0, (pir->ir_flags == CREATE_DIR && (pir->ir_attr & FILE_FLAG_KEEP_CASE)) ?
                pir->ir_uFName : 0, path);
  if (r) return ret(pir, r);
  ENTER();
  switch (pir->ir_flags) {
  case CHECK_DIR:
    r = w32(pgc_getattr(&card, path, &attr, 0, 0, 0));
    if (r == ERROR_FILE_NOT_FOUND || (!r && !(attr & FILE_ATTRIBUTE_DIRECTORY))) r = ERROR_PATH_NOT_FOUND;
    break;
  case CREATE_DIR:
    push_time(0);
    r = w32(pgc_mkdir(&card, path));
    break;
  case DELETE_DIR:
    r = canonical(path);
    if (!r) {
      unbind_within(path);
      r = w32(pgc_rmdir(&card, path));
    }
    break;
  default:
    r = ERROR_INVALID_FUNCTION;
    break;
  }
  LEAVE();
  return ret(pir, r);
}

/* FS_FileAttributes */
static int __cdecl fs_fileattrib(pioreq pir) {
  static pgc_file f;            /* under the mutex */
  char path[PGC_PATH_MAX];
  uint8_t attr;
  uint32_t size;
  uint16_t t, d;
  int r = card_path(pir->ir_ppath, 0, 0, path);
  if (r) return ret(pir, r);
  ENTER();
  switch (pir->ir_flags) {
  case SET_ATTRIBUTES:
    r = w32(pgc_setattr(&card, path, (uint8_t)(pir->ir_attr & 0x27)));
    break;
  case SET_ATTRIB_MODIFY_DATETIME:
    /* the card sets the time through an open file: the shared one when the
     * file is open, a short-lived one otherwise */
    r = canonical(path);
    if (!r) r = w32(pgc_getattr(&card, path, &attr, 0, 0, 0));
    if (!r && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      pgnode *n = find_node(path);
      if (n) {
        r = w32(pgc_set_ftime(&card, &n->f, pir->ir_dostime.dt_time, pir->ir_dostime.dt_date));
      } else {
        r = w32(pgc_open(&card, &f, path, PGC_OPEN_EXISTING, PGC_READ, 0, 0, 0, 0, 0, 0));
        if (!r) {
          r = w32(pgc_set_ftime(&card, &f, pir->ir_dostime.dt_time, pir->ir_dostime.dt_date));
          (void)pgc_close(&card, &f);
        }
      }
    }
    break;
  case SET_ATTRIB_LAST_ACCESS_DATETIME:
  case SET_ATTRIB_CREATION_DATETIME:
    r = w32(pgc_getattr(&card, path, &attr, 0, 0, 0));   /* FAT on the card keeps one time */
    break;
  default:
    r = w32(pgc_getattr(&card, path, &attr, &size, &t, &d));
    if (r) break;
    switch (pir->ir_flags) {
    case GET_ATTRIBUTES:
      pir->ir_attr = attr;
      pir->ir_size = size;
      break;
    case GET_ATTRIB_COMP_FILESIZE:
      pir->ir_size = size;
      break;
    case GET_ATTRIB_MODIFY_DATETIME:
    case GET_ATTRIB_LAST_ACCESS_DATETIME:
    case GET_ATTRIB_CREATION_DATETIME:
      pir->ir_dostime.dt_time = t;
      pir->ir_dostime.dt_date = d;
      if (pir->ir_flags == GET_ATTRIB_CREATION_DATETIME) pir->ir_options = 0;
      break;
    case GET_ATTRIB_FIRST_CLUST:
      pir->ir_pos = 0;
      break;
    default:
      r = ERROR_INVALID_FUNCTION;
      break;
    }
    break;
  }
  LEAVE();
  return ret(pir, r);
}

static int __cdecl fs_flush(pioreq pir) {
  return ret(pir, 0);               /* the card writes every request through */
}

/* FS_GetDiskInfo: INT 21h 36h wants at most 65535 clusters (about 2 GB in
 * 32 KB clusters, as VFAT reports big drives); the extended call (bit 0 of
 * ir_options, GetDiskFreeSpaceEx) the real geometry */
static int __cdecl fs_getdiskinfo(pioreq pir) {
  uint32_t clusters, freec, bytes;
  int r;
  ENTER();
  r = w32(pgc_space(&card, &clusters, &freec, &bytes));
  LEAVE();
  if (r) return ret(pir, r);
  if (bytes < 512) bytes = 512;
  if (!(pir->ir_options & 1)) {
    if (bytes >= 32768) {
      clusters *= bytes / 32768;
      freec *= bytes / 32768;
    } else {
      clusters /= 32768 / bytes;
      freec /= 32768 / bytes;
    }
    bytes = 32768;
    if (clusters > 0xFFFF) clusters = 0xFFFF;
    if (freec > 0xFFFF) freec = 0xFFFF;
  }
  pir->ir_length = 512;
  pir->ir_sectors = (unsigned short)(bytes / 512);
  pir->ir_size = clusters;
  pir->ir_numfree = freec;
  pir->ir_data = 0;
  return ret(pir, 0);
}

/* the open of an existing shared file: open (raise the access it needs) or
 * truncate it, as the card would open the file itself */
static int open_existing(pgnode *n, const char *path, unsigned action, uint8_t need, uint8_t create_attr,
                         int *res, uint8_t *attr, uint32_t *size, uint16_t *t, uint16_t *d) {
  int r;
  if (action == PGC_CREATE_NEW) return PGC_EEXISTS;
  if (action & 0x02) {          /* truncate: the shared file is opened anew */
    uint8_t keep = (n->f.need == PGC_READWRITE) ? PGC_READWRITE : need;
    (void)pgc_close(&card, &n->f);
    r = pgc_open(&card, &n->f, path, PGC_TRUNCATE_EXISTING, PGC_READWRITE, create_attr, res, attr, size, t, d);
    if (r != PGC_OK) {
      (void)pgc_file_attach(&card, &n->f, path, keep);   /* the other handles carry on */
      return r;
    }
    if (keep != PGC_READWRITE) pgc_file_need(&n->f, keep);
    return PGC_OK;
  }
  r = pgc_getattr(&card, path, attr, size, t, d);
  if (r != PGC_OK) return r;
  if (*attr & FILE_ATTRIBUTE_DIRECTORY) return PGC_EACCESS;
  if (need == PGC_READWRITE && (*attr & FILE_ATTRIBUTE_READONLY)) return PGC_EACCESS;
  pgc_file_need(&n->f, need);
  *res = PGC_OPENED;
  return PGC_OK;
}

/* FS_OpenFile */
static int __cdecl fs_open(pioreq pir) {
  char path[PGC_PATH_MAX];
  unsigned access = pir->ir_flags & ACCESS_MODE_MASK;
  /* the action bits only: ir_options also carries open flags (no-inherit) */
  unsigned action = pir->ir_options & (ACTION_EXISTS_OPEN | ACTION_TRUNCATE | ACTION_NEXISTS_CREATE);
  uint8_t need = (access == ACCESS_WRITEONLY || access == ACCESS_READWRITE) ? PGC_READWRITE : PGC_READ;
  uint8_t create_attr = (uint8_t)(pir->ir_attr & 0x27);
  pgfile *pf;
  pgnode *n, *fresh = 0;
  int r, res = 0;
  uint8_t attr = 0;
  uint32_t size = 0;
  uint16_t t = 0, d = 0;

  if (!pir->ir_ppath || (pir->ir_attr & 0xFF) == FILE_ATTRIBUTE_LABEL) return ret(pir, ERROR_ACCESS_DENIED);
  if (action != PGC_OPEN_EXISTING && action != PGC_TRUNCATE_EXISTING && action != PGC_CREATE_NEW &&
      action != PGC_OPEN_ALWAYS && action != PGC_CREATE_ALWAYS) return ret(pir, ERROR_INVALID_FUNCTION);
  r = card_path(pir->ir_ppath, 0, (pir->ir_attr & FILE_FLAG_KEEP_CASE) ? pir->ir_uFName : 0, path);
  if (r) return ret(pir, r);
  pf = (pgfile *)alloc(sizeof(pgfile));
  if (pf) pf->ppath = (ParsedPath *)alloc(IFSPathSize(pir->ir_ppath));
  if (pf && pf->ppath) fresh = (pgnode *)alloc(sizeof(pgnode));
  if (!pf || !pf->ppath || !fresh) {
    if (fresh) vx_heap_free(fresh, 0);
    if (pf && pf->ppath) vx_heap_free(pf->ppath, 0);
    if (pf) vx_heap_free(pf, 0);
    return ret(pir, ERROR_NOT_ENOUGH_MEMORY);
  }
  pgc_memcpy(pf->ppath, pir->ir_ppath, IFSPathSize(pir->ir_ppath));
  ENTER();
  if (action != PGC_OPEN_EXISTING) push_time(0);
  r = pgc_canonical(&card, path, path);
  n = (r == PGC_OK) ? find_node(path) : 0;
  if (r != PGC_OK) {
    /* not ready */
  } else if (n) {
    r = open_existing(n, path, action, need, create_attr, &res, &attr, &size, &t, &d);
    if (r == PGC_OK) n->refs++;
  } else {
    r = pgc_open(&card, &fresh->f, path, (int)action, need, create_attr, &res, &attr, &size, &t, &d);
    if (r == PGC_OK) {
      n = fresh;
      fresh = 0;
      n->refs = 1;
      n->next = nodes;
      nodes = n;
    }
  }
  LEAVE();
  if (fresh) vx_heap_free(fresh, 0);
  if (r != PGC_OK) {
    vx_heap_free(pf->ppath, 0);
    vx_heap_free(pf, 0);
    return ret(pir, w32(r));
  }
  pf->node = n;
  pir->ir_fh = pf;
  pir->ir_hfunc->hf_read = file_read;
  pir->ir_hfunc->hf_write = file_write;
  pir->ir_hfunc->hf_misc = &filemisc;
  pir->ir_options = (unsigned short)(res == PGC_CREATED ? ACTION_CREATED : res == PGC_TRUNCATED ? ACTION_REPLACED : ACTION_OPENED);
  pir->ir_size = size;
  pir->ir_attr = attr;
  pir->ir_dostime.dt_time = t;
  pir->ir_dostime.dt_date = d;
  return ret(pir, 0);
}

/* FS_RenameFile (files and directories, also across directories). Renames
 * with wildcards (DOS) are left to IFSMgr. */
static int __cdecl fs_rename(pioreq pir) {
  char from[PGC_PATH_MAX], to[PGC_PATH_MAX], canon_to[PGC_PATH_MAX];
  pgnode *n;
  int r;
  if ((pir->ir_attr & FILE_FLAG_WILDCARDS) || (pir->ir_attr2 & FILE_FLAG_WILDCARDS)) return ret(pir, ERROR_INVALID_FUNCTION);
  r = card_path(pir->ir_ppath, 0, 0, from);
  if (!r) r = card_path(pir->ir_ppath2, 0, (pir->ir_attr2 & FILE_FLAG_KEEP_CASE) ? pir->ir_uFName : 0, to);
  if (r) return ret(pir, r);
  ENTER();
  r = canonical(from);
  if (!r) {
    pgc_memcpy(canon_to, to, pgc_strlen(to) + 1);
    r = canonical(canon_to);
  }
  /* a directory into its own subtree would cut it off */
  if (!r && !path_eq(canon_to, from) && path_within(canon_to, from)) r = ERROR_ACCESS_DENIED;
  if (!r) {
    unbind_within(from);
    r = w32(pgc_rename(&card, from, to));
  }
  if (!r) {
    /* open files at the old name or below it follow */
    unsigned fl = pgc_strlen(from), tl = pgc_strlen(to);
    for (n = nodes; n; n = n->next) {
      const char *rest;
      unsigned rl;
      char moved[PGC_PATH_MAX];
      if (!path_within(n->f.path, from)) continue;
      rest = n->f.path + fl;        /* "" or "\\SUB\\FILE" */
      rl = pgc_strlen(rest);
      if (tl + rl >= PGC_PATH_MAX) continue;
      pgc_memcpy(moved, to, tl);
      pgc_memcpy(moved + tl, rest, rl + 1);
      (void)pgc_file_moved(&card, &n->f, moved);
    }
  }
  LEAVE();
  return ret(pir, r);
}

/* ---- DOS searches (INT 21h 4Eh/4Fh, FCB searches) -------------------------------------
 * The card's FINDFIRST/FINDNEXT do the DOS matching; the DTA keeps a slot of
 * a small table (sk_netFSD), whose entry has the card's search state. */

static void fill_srch(srch_entry *se, const pgc_find *s) {
  unsigned n = pgc_strlen(s->shortname);
  se->se_attrib = s->attr;
  se->se_time = s->time;
  se->se_date = s->date;
  se->se_size = s->size;
  pgc_memset(se->se_name, 0, sizeof(se->se_name));
  pgc_memcpy(se->se_name, s->shortname, n);
}

static int __cdecl fs_search(pioreq pir) {
  srch_entry *user = (srch_entry *)pir->ir_data;
  srch_entry se;                /* the DTA is the program's memory: copied outside the mutex */
  dossearch *ds = 0;
  unsigned slot;
  int r;
  pgc_memcpy(&se, user, sizeof(se));
  if (pir->ir_flags == SEARCH_FIRST) {
    char dir[PGC_PATH_MAX], pattern[PGC_PATH_MAX];
    r = card_path(pir->ir_ppath, 1, 0, dir);
    if (!r) r = last_oem(pir->ir_ppath, pattern, sizeof(pattern));
    if (!r && pattern[0] == 0) r = ERROR_NO_MORE_FILES;
    if (r) return ret(pir, r);
    ENTER();
    for (slot = 0; slot < DOS_SEARCHES && dos_search[slot].used; slot++) { }
    if (slot == DOS_SEARCHES) slot = dos_search_next++ % DOS_SEARCHES;
    ds = &dos_search[slot];
    ds->used = 1;
    ds->gen++;
    r = pgc_find_mask(&card, &ds->s, dir, pattern, (uint8_t)(pir->ir_attr & 0x3F), 0);
    if (r == PGC_OK) {
      pgc_memcpy(se.se_key.sk_pattern, ds->s.mask, 11);
      se.se_key.sk_attr = (unsigned char)(pir->ir_attr & 0x3F);
      se.se_key.sk_netFSD[0] = (unsigned char)slot;
      se.se_key.sk_netFSD[1] = ds->gen;
    }
  } else {
    slot = se.se_key.sk_netFSD[0];
    ENTER();
    if (slot >= DOS_SEARCHES || !dos_search[slot].used || dos_search[slot].gen != se.se_key.sk_netFSD[1]) {
      LEAVE();
      return ret(pir, ERROR_NO_MORE_FILES);
    }
    ds = &dos_search[slot];
    r = pgc_find_next(&card, &ds->s);
  }
  if (r == PGC_OK) fill_srch(&se, &ds->s);
  else ds->used = 0;
  LEAVE();
  if (r == PGC_OK) pgc_memcpy(user, &se, sizeof(se));
  pir->ir_firstclus = 0;
  return ret(pir, w32(r));
}

/* ---- Win32 searches (FindFirstFile, INT 21h 714Eh) ---------------------------------------
 * Long-name API matching with IFSMgr_MetaMatch (UFLG_NT): against the long
 * name, and against the 8.3 name with a dot appended when it has none and
 * the pattern has one ("README" matches "README.*"), as the DDK asks. */

static int meta(pgfind *f, const char *oem, int dot) {
  static unsigned short name[PGC_NAME_MAX + 1], raw[PGC_NAME_MAX];
  unsigned n = to_uni(oem, raw, PGC_NAME_MAX), i;
  vx_uni_to_upper(name, raw, n * 2);
  if (dot) {
    for (i = 0; i < n && name[i] != '.'; i++) { }
    if (i == n) name[n++] = '.';
  }
  name[n] = 0;
  return vx_ifs_metamatch(f->pattern, name, f->sem) != 0;
}

static int find_match(pgfind *f) {
  unsigned attr = f->s.attr;
  /* DOS rules: hidden, system and directories only when asked for, the
   * must-match bits (8-13) all present */
  if (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY) & ~(f->attr & 0xFF)) return 0;
  if (((f->attr & ((unsigned long)attr << 8)) ^ f->attr) & FILE_ATTRIBUTE_MUSTMATCH) return 0;
  return meta(f, f->s.longname, 0) || meta(f, f->s.shortname, f->pattern_dot);
}

static void fill_find(_WIN32_FIND_DATA *fd, const pgc_find *s) {
  static unsigned short lu[PGC_NAME_MAX], su[16];
  unsigned ln, sn, i;
  int same;
  pgc_memset(fd, 0, sizeof(*fd));
  fd->dwFileAttributes = s->attr;
  fd->ftLastWriteTime = filetime(s->time, s->date);
  fd->ftCreationTime = fd->ftLastWriteTime;
  fd->ftLastAccessTime = fd->ftLastWriteTime;
  fd->nFileSizeLow = (s->attr & FILE_ATTRIBUTE_DIRECTORY) ? 0 : s->size;
  ln = to_uni(s->longname, lu, PGC_NAME_MAX);
  if (ln > MAX_PATH - 1) ln = MAX_PATH - 1;
  for (i = 0; i < ln; i++) fd->cFileName[i] = lu[i];
  /* the 8.3 name, when the entry has a long one that is not just it */
  sn = to_uni(s->shortname, su, 16);
  same = (sn == ln);
  for (i = 0; same && i < sn; i++) {
    unsigned short a = lu[i], b = su[i];
    if (a >= 'a' && a <= 'z') a = (unsigned short)(a - 32);
    if (a != b) same = 0;
  }
  if (!same && sn < 14) {
    for (i = 0; i < sn; i++) fd->cAlternateFileName[i] = su[i];
  }
}

/* the next entry that matches, into f->fd */
static int find_fill(pgfind *f) {
  int r = PGC_OK;
  for (;;) {
    if (!f->fresh) r = pgc_find_next(&card, &f->s);
    f->fresh = 0;
    if (r != PGC_OK) return (r == PGC_ENOMORE) ? ERROR_NO_MORE_FILES : w32(r);
    if (find_match(f)) {
      fill_find(&f->fd, &f->s);
      return 0;
    }
  }
}

static int __cdecl fs_findopen(pioreq pir) {
  char dir[PGC_PATH_MAX];
  PathElement *pe;
  pgfind *f;
  unsigned chars;
  int r;

  if (IFSIsRoot(pir->ir_ppath)) return ret(pir, ERROR_FILE_NOT_FOUND);
  f = (pgfind *)alloc(sizeof(pgfind));
  if (!f) return ret(pir, ERROR_NOT_ENOUGH_MEMORY);
  f->attr = pir->ir_attr & 0xFFFF;
  pe = IFSLastElement(pir->ir_ppath);
  chars = (pe->pe_length - 2) / 2;
  ENTER();
  if ((f->attr & 0xFF) == FILE_ATTRIBUTE_LABEL || (f->attr & (FILE_ATTRIBUTE_LABEL << 8))) {
    /* the volume label (GetVolumeInformation) */
    pgc_volinfo v;
    r = w32(pgc_volinfo_get(&card, &v));
    if (!r && v.label[0] == 0) r = ERROR_FILE_NOT_FOUND;
    if (!r) {
      pgc_memset(&f->fd, 0, sizeof(f->fd));
      f->fd.dwFileAttributes = FILE_ATTRIBUTE_LABEL;
      to_uni(v.label, f->fd.cFileName, MAX_PATH);
      f->label = 1;
    }
  } else {
    r = (chars >= PGC_NAME_MAX) ? ERROR_INVALID_NAME : card_path(pir->ir_ppath, 1, 0, dir);
    if (!r) {
      unsigned i;
      vx_uni_to_upper(f->pattern, pe->pe_unichars, chars * 2);
      f->pattern[chars] = 0;
      for (i = 0; i < chars && f->pattern[i] != '.'; i++) { }
      f->pattern_dot = (i < chars);
      f->sem = UFLG_NT;
      if (pir->ir_attr & FILE_FLAG_WILDCARDS) f->sem |= UFLG_META;
      r = pgc_find_first(&card, &f->s, dir, 0x16, 1);
      if (r == PGC_OK) {
        f->fresh = 1;
        r = find_fill(f);
        if (r == ERROR_NO_MORE_FILES) r = ERROR_FILE_NOT_FOUND;
      } else {
        r = (r == PGC_ENOMORE) ? ERROR_FILE_NOT_FOUND : w32(r);
      }
    }
  }
  LEAVE();
  if (r) {
    vx_heap_free(f, 0);
    return ret(pir, r);
  }
  pgc_memcpy(pir->ir_data, &f->fd, sizeof(f->fd));
  pir->ir_fh = f;
  pir->ir_hfunc->hf_read = find_next;
  pir->ir_hfunc->hf_write = fs_bad;
  pir->ir_hfunc->hf_misc = &findmisc;
  pir->ir_pos = 0;
  return ret(pir, 0);
}

static int __cdecl find_next(pioreq pir) {
  pgfind *f = (pgfind *)pir->ir_fh;
  int r;
  if (f->label) return ret(pir, ERROR_NO_MORE_FILES);
  ENTER();
  r = find_fill(f);
  LEAVE();
  if (!r) pgc_memcpy(pir->ir_data, &f->fd, sizeof(f->fd));
  pir->ir_pos = 0;
  return ret(pir, r);
}

static int __cdecl find_close(pioreq pir) {
  vx_heap_free(pir->ir_fh, 0);
  return ret(pir, 0);
}

/* ---- open files ----------------------------------------------------------------------- */

#define BOUNCE_MAX 32768u       /* data moved per turn of the mutex */

static int paging(pioreq pir) {
  return (pir->ir_options & (R0_SWAPPER_CALL | R0_LOADER_IO | R0_MM_READ_WRITE)) != 0;
}

/* FS_ReadFile. Paging reads (the program loader, memory-mapped files) come
 * with locked buffers and are served straight into them, without sleeping;
 * everything else through a bounce buffer, copied out with the mutex free. */
static int __cdecl file_read(pioreq pir) {
  pgfile *pf = (pgfile *)pir->ir_fh;
  uint32_t len = pir->ir_length, total = 0, got = 0;
  int r = PGC_OK;
  if (paging(pir)) {
    ENTER();
    no_sleep++;
    r = pgc_read(&card, &pf->node->f, pir->ir_pos, pir->ir_data, len, &total);
    no_sleep--;
    LEAVE();
  } else if (len) {
    uint32_t chunk = (len < BOUNCE_MAX) ? len : BOUNCE_MAX;
    unsigned char *bounce = (unsigned char *)vx_heap_alloc(chunk, 0);
    if (!bounce) return ret(pir, ERROR_NOT_ENOUGH_MEMORY);
    while (total < len) {
      uint32_t n = len - total;
      if (n > chunk) n = chunk;
      ENTER();
      r = pgc_read(&card, &pf->node->f, pir->ir_pos + total, bounce, n, &got);
      LEAVE();
      if (r != PGC_OK) break;
      pgc_memcpy((unsigned char *)pir->ir_data + total, bounce, got);
      total += got;
      if (got < n) break;       /* end of file */
    }
    vx_heap_free(bounce, 0);
  }
  if (r != PGC_OK) {            /* cut short by the card: an error, not a short file */
    pir->ir_length = 0;
    return ret(pir, w32(r));
  }
  pir->ir_length = total;
  pir->ir_pos += total;
  return ret(pir, 0);
}

/* FS_WriteFile; a zero-length write sets the size to the position */
static int __cdecl file_write(pioreq pir) {
  pgfile *pf = (pgfile *)pir->ir_fh;
  uint32_t len = pir->ir_length, total = 0, done = 0;
  int r = PGC_OK;
  if (pf->node->f.need != PGC_READWRITE) return ret(pir, ERROR_ACCESS_DENIED);
  if (paging(pir)) {
    ENTER();
    no_sleep++;
    r = pgc_write(&card, &pf->node->f, pir->ir_pos, pir->ir_data, len, &total);
    no_sleep--;
    LEAVE();
  } else if (len == 0) {
    ENTER();
    push_time(0);
    r = pgc_set_size(&card, &pf->node->f, pir->ir_pos);
    LEAVE();
  } else {
    uint32_t chunk = (len < BOUNCE_MAX) ? len : BOUNCE_MAX;
    unsigned char *bounce = (unsigned char *)vx_heap_alloc(chunk, 0);
    if (!bounce) return ret(pir, ERROR_NOT_ENOUGH_MEMORY);
    while (total < len) {
      uint32_t n = len - total;
      if (n > chunk) n = chunk;
      pgc_memcpy(bounce, (unsigned char *)pir->ir_data + total, n);
      ENTER();
      push_time(0);
      r = pgc_write(&card, &pf->node->f, pir->ir_pos + total, bounce, n, &done);
      LEAVE();
      total += done;
      if (r != PGC_OK) break;
    }
    vx_heap_free(bounce, 0);
  }
  if (r == PGC_EACCESS && len != 0) {
    if (total == 0) return ret(pir, ERROR_DISK_FULL);
    r = PGC_OK;                 /* a short write: DOS's way of saying the disk is full */
  }
  if (r != PGC_OK) {
    pir->ir_length = 0;
    return ret(pir, w32(r));
  }
  pir->ir_length = total;
  pir->ir_pos += total;
  return ret(pir, 0);
}

/* FS_FileSeek: IFSMgr keeps the position; only the end of file needs the card
 * (GetFileSize: files up to 4 GB on FAT32) */
static int __cdecl file_seek(pioreq pir) {
  pgfile *pf = (pgfile *)pir->ir_fh;
  uint32_t size;
  long delta = (long)pir->ir_pos;
  int r;
  if (pir->ir_flags != FILE_END) return ret(pir, 0);
  ENTER();
  r = pgc_get_size(&card, &pf->node->f, &size);
  LEAVE();
  if (r != PGC_OK) return ret(pir, w32(r));
  if (delta < 0 && (unsigned long)(-delta) > size) return ret(pir, ERROR_NEGATIVE_SEEK);
  pir->ir_pos = size + (unsigned long)delta;
  return ret(pir, 0);
}

static int __cdecl file_close(pioreq pir) {
  pgfile *pf = (pgfile *)pir->ir_fh;
  int r = 0;
  if (pir->ir_flags == CLOSE_FINAL) {
    pgnode *n = pf->node, **pp;
    ENTER();
    if (--n->refs == 0) {
      for (pp = &nodes; *pp; pp = &(*pp)->next) {
        if (*pp == n) {
          *pp = n->next;
          break;
        }
      }
      r = pgc_close(&card, &n->f);
      if (r == PGC_ESTALE || r == PGC_ENOTREADY) r = PGC_OK;   /* nothing left to save */
    } else {
      n = 0;
    }
    LEAVE();
    if (n) vx_heap_free(n, 0);
    vx_heap_free(pf->ppath, 0);
    vx_heap_free(pf, 0);
  }
  pir->ir_pos = 0;
  return ret(pir, w32(r));
}

static int __cdecl file_commit(pioreq pir) {
  return ret(pir, 0);               /* every WRITE is synced on the card */
}

static int __cdecl file_locks(pioreq pir) {
  if (pir->ir_flags != LOCK_REGION && pir->ir_flags != UNLOCK_REGION) return ret(pir, ERROR_INVALID_FUNCTION);
  return ret(pir, 0);               /* no locking, as in EtherDFS */
}

/* FS_FileDateTime */
static int __cdecl file_times(pioreq pir) {
  pgfile *pf = (pgfile *)pir->ir_fh;
  pgc_file *f = &pf->node->f;
  uint16_t t, d;
  int r;
  ENTER();
  switch (pir->ir_flags) {
  case SET_MODIFY_DATETIME:
    r = w32(pgc_set_ftime(&card, f, pir->ir_dostime.dt_time, pir->ir_dostime.dt_date));
    break;
  case SET_LAST_ACCESS_DATETIME:
  case SET_CREATION_DATETIME:
    r = 0;
    break;
  case GET_MODIFY_DATETIME:
  case GET_LAST_ACCESS_DATETIME:
  case GET_CREATION_DATETIME:
    r = pgc_file_stale(&card, f) ? ERROR_WRONG_DISK : w32(pgc_getattr(&card, f->path, 0, 0, &t, &d));
    if (!r) {
      pir->ir_dostime.dt_time = t;
      pir->ir_dostime.dt_date = d;
      if (pir->ir_flags == GET_CREATION_DATETIME) pir->ir_options = 0;
    }
    break;
  default:
    r = ERROR_INVALID_FUNCTION;
    break;
  }
  LEAVE();
  return ret(pir, r);
}

/* FS_EnumerateHandle: GetFileInformationByHandle and the name of a handle */
static int __cdecl file_enum(pioreq pir) {
  pgfile *pf = (pgfile *)pir->ir_fh;
  pgc_file *f = &pf->node->f;
  int r = 0;
  if (pir->ir_flags == ENUMH_GETFILEINFO) {
    _BY_HANDLE_FILE_INFORMATION bi;
    uint8_t attr;
    uint32_t size;
    uint16_t t, d;
    pgc_volinfo v;
    ENTER();
    r = pgc_file_stale(&card, f) ? ERROR_WRONG_DISK : w32(pgc_getattr(&card, f->path, &attr, &size, &t, &d));
    if (!r && pgc_volinfo_get(&card, &v) != PGC_OK) v.serial = 0;
    if (!r) {
      pgc_memset(&bi, 0, sizeof(bi));
      bi.bhfi_dwFileAttributes = attr;
      bi.bhfi_ftLastWriteTime = filetime(t, d);
      bi.bhfi_ftCreationTime = bi.bhfi_ftLastWriteTime;
      bi.bhfi_ftLastAccessTime = bi.bhfi_ftLastWriteTime;
      bi.bhfi_dwVolumeSerialNumber = v.serial;
      bi.bhfi_nFileSizeLow = size;
      bi.bhfi_nNumberOfLinks = 1;
      bi.bhfi_nFileIndexLow = hash(f->path);
    }
    LEAVE();
    if (!r) pgc_memcpy(pir->ir_data, &bi, sizeof(bi));
    return ret(pir, r);
  }
  if (pir->ir_flags == ENUMH_GETFILENAME) {
    pgc_memcpy(pir->ir_ppath, pf->ppath, IFSPathSize(pf->ppath));
    return ret(pir, 0);
  }
  return ret(pir, ERROR_INVALID_FUNCTION);
}

/* ---- initialization -------------------------------------------------------------------- */

/* Device_Init: the card must be there and PGDFS enabled on it, or the VxD
 * does not load */
int __cdecl pg_device_init(void) {
  int ok = 0;
  no_sleep = 1;                 /* no thread to put to sleep while Windows starts */
  if (vx_ifs_get_version() >= IFSMGRVERSION && pgc_probe(&card) == PGC_OK) {
    mutex = vx_create_mutex(0, 0);
    if (mutex) proid = vx_ifs_register_net(fs_connect, IFSMGRVERSION, PGDFS_NETID);
    if (mutex && proid != -1) {
      push_time(1);
      ok = 1;
    }
  }
  no_sleep = 0;
  return ok;
}

static char read_letter(void) {
  unsigned long key, type = 0, size;
  char value[8];
  char l = 'P';
  if (vx_reg_open_key(HKEY_LOCAL_MACHINE, REG_KEY, &key) == 0) {
    size = sizeof(value);
    pgc_memset(value, 0, sizeof(value));
    if (vx_reg_query_value(key, "DriveLetter", 0, &type, value, &size) == 0 && type == REG_SZ) {
      char c = value[0];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      if (c >= 'D' && c <= 'Z') l = c;
    }
    vx_reg_close_key(key);
  }
  return l;
}

/* Init_Complete: every local and CD-ROM drive has its letter by now; take the
 * configured one, or the next one IFSMgr does not manage yet */
void __cdecl pg_init_complete(void) {
  static use_info_2 ui;
  static char remote[] = REMOTE_NAME;
  int i;
  char l = read_letter();
  for (i = 0; i < 23; i++, l = (char)(l == 'Z' ? 'D' : l + 1)) {
    if (vx_drive_is_ifs(l - 'A')) continue;
    pgc_memset(&ui, 0, sizeof(ui));
    ui.ui2_local[0] = l;
    ui.ui2_local[1] = ':';
    ui.ui2_remote = remote;
    ui.ui2_password = 0;
    ui.ui2_asg_type = USE_DISKDEV;
    if (vx_ifs_init_use_add(&ui, proid, 1, 0xFF) == 0) {
      letter = l;
      return;
    }
  }
}
