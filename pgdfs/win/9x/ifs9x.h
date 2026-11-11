/*
 * The parts of the Windows 95/98/ME installable file system interface that
 * PGDFS.VXD uses: the IFS manager's request packet and function tables, as
 * documented in the Windows 95 DDK ("Installable File System Reference"),
 * written out here for Open Watcom. Layouts are checked at compile time.
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 */
#ifndef IFS9X_H
#define IFS9X_H

#include <stdint.h>

#define IFS_VERSION       0x030A
#define IFS_REVISION      0x10
#define IFSMGRVERSION     0x22     /* what IFSMgr_Get_Version returns, for RegisterNet */
#define NUM_VOLFUNC       15
#define NUM_HNDLMISC      8
#define MAX_PATH          260

typedef unsigned short *string_t;

typedef struct PathElement {
  unsigned short pe_length;        /* bytes, including this field */
  unsigned short pe_unichars[1];
} PathElement;

typedef struct ParsedPath {
  unsigned short pp_totalLength;   /* bytes up to the terminating element */
  unsigned short pp_prefixLength;  /* offset of the last element */
  PathElement    pp_elements[1];
} ParsedPath, *path_t;

#define IFSPathSize(pp)    ((pp)->pp_totalLength + sizeof(short))
#define IFSLastElement(pp) ((PathElement *)((char *)(pp) + (pp)->pp_prefixLength))
#define IFSNextElement(pe) ((PathElement *)((char *)(pe) + (pe)->pe_length))
#define IFSIsRoot(pp)      ((pp)->pp_totalLength == 4)

typedef struct dos_time {
  unsigned short dt_time;
  unsigned short dt_date;
} dos_time;

typedef struct ioreq ioreq, *pioreq;
typedef int (__cdecl *pIFSFunc)(pioreq pir);
typedef struct hndlmisc hndlmisc;

typedef struct hndlfunc {
  pIFSFunc  hf_read;
  pIFSFunc  hf_write;
  hndlmisc *hf_misc;
} hndlfunc;

struct hndlmisc {
  short    hm_version;
  char     hm_revision;
  char     hm_size;
  pIFSFunc hm_func[NUM_HNDLMISC];
};

#define HM_SEEK         0
#define HM_CLOSE        1
#define HM_COMMIT       2
#define HM_FILELOCKS    3
#define HM_FILETIMES    4
#define HM_PIPEREQUEST  5
#define HM_HANDLEINFO   6
#define HM_ENUMHANDLE   7

typedef struct volfunc {
  short    vfn_version;
  char     vfn_revision;
  char     vfn_size;
  pIFSFunc vfn_func[NUM_VOLFUNC];
} volfunc;

#define VFN_DELETE       0
#define VFN_DIR          1
#define VFN_FILEATTRIB   2
#define VFN_FLUSH        3
#define VFN_GETDISKINFO  4
#define VFN_OPEN         5
#define VFN_RENAME       6
#define VFN_SEARCH       7
#define VFN_QUERY        8
#define VFN_DISCONNECT   9
#define VFN_UNCPIPEREQ   10
#define VFN_IOCTL16DRIVE 11
#define VFN_GETDISKPARMS 12
#define VFN_FINDOPEN     13
#define VFN_DASDIO       14

typedef union aux_t {
  void          *aux_buf;
  unsigned long  aux_ul;
  dos_time       aux_dt;
  volfunc       *aux_vf;
  hndlfunc      *aux_hf;
  void          *aux_ptr;
  string_t       aux_str;
  path_t         aux_pp;
  unsigned int   aux_ui;
} aux_t;

struct ioreq {
  unsigned int   ir_length;     /* 00 */
  unsigned char  ir_flags;      /* 04 */
  unsigned char  ir_user;       /* 05 */
  unsigned short ir_sfn;        /* 06 */
  unsigned int   ir_pid;        /* 08 */
  path_t         ir_ppath;      /* 0C */
  aux_t          ir_aux1;       /* 10 */
  void          *ir_data;       /* 14 */
  unsigned short ir_options;    /* 18 */
  short          ir_error;      /* 1A */
  void          *ir_rh;         /* 1C */
  void          *ir_fh;         /* 20 */
  unsigned long  ir_pos;        /* 24 */
  aux_t          ir_aux2;       /* 28 */
  aux_t          ir_aux3;       /* 2C */
  void          *ir_pev;        /* 30 */
  int            ir_fsd[16];    /* 34 */
};

/* fields overlaid on others */
#define ir_attr      ir_length
#define ir_pathSkip  ir_length
#define ir_size      ir_pos
#define ir_attr2     ir_pos
#define ir_sectors   ir_options
#define ir_vfunc     ir_aux1.aux_vf
#define ir_hfunc     ir_aux1.aux_hf
#define ir_ppath2    ir_aux1.aux_pp
#define ir_numfree   ir_aux2.aux_ul
#define ir_locklen   ir_aux2.aux_ul
#define ir_dostime   ir_aux2.aux_dt
#define ir_firstclus ir_aux2.aux_ul
#define ir_uFName    ir_aux2.aux_str
#define ir_upath     ir_aux3.aux_str

/* connections */
#define RESOPT_UNC_REQUEST  0x01
#define RESOPT_DEV_ATTACH   0x02
#define RESOPT_UNC_CONNECT  0x04
#define RESOPT_DISCONNECTED 0x08
#define RESOPT_NO_CREATE    0x10
#define RESOPT_STATIC       0x20
#define RESTYPE_WILD        0
#define RESTYPE_DISK        1
#define RESSTAT_OK          0

/* FS_CloseFile */
#define CLOSE_HANDLE        0
#define CLOSE_FOR_PROCESS   1
#define CLOSE_FINAL         2

/* FS_DirectoryMgmt */
#define CREATE_DIR          0
#define DELETE_DIR          1
#define CHECK_DIR           2
#define QUERY83_DIR         3
#define QUERYLONG_DIR       4

/* FS_LockFile */
#define LOCK_REGION         0
#define UNLOCK_REGION       1

/* FS_EnumerateHandle */
#define ENUMH_GETFILEINFO   0
#define ENUMH_GETFILENAME   1

/* FS_FileAttributes */
#define GET_ATTRIBUTES                  0
#define SET_ATTRIBUTES                  1
#define GET_ATTRIB_COMP_FILESIZE        2
#define SET_ATTRIB_MODIFY_DATETIME      3
#define GET_ATTRIB_MODIFY_DATETIME      4
#define SET_ATTRIB_LAST_ACCESS_DATETIME 5
#define GET_ATTRIB_LAST_ACCESS_DATETIME 6
#define SET_ATTRIB_CREATION_DATETIME    7
#define GET_ATTRIB_CREATION_DATETIME    8
#define GET_ATTRIB_FIRST_CLUST          9

/* FS_FileDateTime */
#define GET_MODIFY_DATETIME       0
#define SET_MODIFY_DATETIME       1
#define GET_LAST_ACCESS_DATETIME  4
#define SET_LAST_ACCESS_DATETIME  5
#define GET_CREATION_DATETIME     6
#define SET_CREATION_DATETIME     7

/* FS_FileSeek */
#define FILE_BEGIN          0
#define FILE_END            2

/* FS_OpenFile */
#define ACCESS_MODE_MASK    0x0007
#define ACCESS_READONLY     0x0000
#define ACCESS_WRITEONLY    0x0001
#define ACCESS_READWRITE    0x0002
#define ACCESS_EXECUTE      0x0003
#define ACTION_MASK         0xFF
#define ACTION_OPENED       1
#define ACTION_CREATED      2
#define ACTION_REPLACED     3

/* FS_SearchFile */
#define SEARCH_FIRST        0
#define SEARCH_NEXT         1

/* ring 0 callers (ir_options of reads and writes) */
#define R0_SWAPPER_CALL     0x1000
#define R0_LOADER_IO        0x2000
#define R0_MM_READ_WRITE    0x8000

#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_HIDDEN    0x02
#define FILE_ATTRIBUTE_SYSTEM    0x04
#define FILE_ATTRIBUTE_LABEL     0x08
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE   0x20
#define FILE_ATTRIBUTE_MUSTMATCH 0x00003F00

/* path parsing flags in ir_attr */
#define FILE_FLAG_WILDCARDS  0x80000000
#define FILE_FLAG_KEEP_CASE  0x10000000
#define FILE_FLAG_IS_LFN     0x04000000

/* FS_QueryResourceInfo level 2 */
#define FS_CASE_IS_PRESERVED       0x00000002
#define FS_VOL_SUPPORTS_LONG_NAMES 0x00004000

/* character sets and IFSMgr_MetaMatch */
#define BCS_WANSI  0
#define BCS_OEM    1
#define UFLG_META  0x01
#define UFLG_NT    0x02
#define UFLG_DOS   0x00

/* LAN Manager style use_info_2 for IFSMgr_InitUseAdd (packed, as IFSMgr has it) */
#define USE_DISKDEV 0
#pragma pack(push, 1)
typedef struct use_info_2 {
  char           ui2_local[9];
  char           ui2_pad_1;
  char          *ui2_remote;
  char          *ui2_password;
  unsigned short ui2_status;
  unsigned short ui2_asg_type;
  unsigned short ui2_refcount;
  unsigned short ui2_usecount;
  unsigned short ui2_res_type;
  unsigned short ui2_flags;
  unsigned short ui2_usrclass;
  void          *ui2_dirname;
  unsigned char  ui2_dfs_id[16];
} use_info_2;

/* the DOS find-first/find-next record in the caller's DTA */
typedef struct srch_key {
  unsigned char sk_drive;
  unsigned char sk_pattern[11];
  unsigned char sk_attr;
  unsigned char sk_localFSD[4];
  unsigned char sk_netFSD[2];
  unsigned char sk_ifsmgr[2];
} srch_key;

typedef struct srch_entry {
  srch_key       se_key;
  unsigned char  se_attrib;
  unsigned short se_time;
  unsigned short se_date;
  unsigned long  se_size;
  char           se_name[13];
} srch_entry;
#pragma pack(pop)

typedef struct _FILETIME {
  unsigned long dwLowDateTime;
  unsigned long dwHighDateTime;
} _FILETIME;

typedef struct _WIN32_FIND_DATA {
  unsigned long  dwFileAttributes;
  _FILETIME      ftCreationTime;
  _FILETIME      ftLastAccessTime;
  _FILETIME      ftLastWriteTime;
  unsigned long  nFileSizeHigh;
  unsigned long  nFileSizeLow;
  unsigned long  dwReserved0;
  unsigned long  dwReserved1;
  unsigned short cFileName[MAX_PATH];
  unsigned short cAlternateFileName[14];
} _WIN32_FIND_DATA;

typedef struct _BY_HANDLE_FILE_INFORMATION {
  unsigned long bhfi_dwFileAttributes;
  _FILETIME     bhfi_ftCreationTime;
  _FILETIME     bhfi_ftLastAccessTime;
  _FILETIME     bhfi_ftLastWriteTime;
  unsigned long bhfi_dwVolumeSerialNumber;
  unsigned long bhfi_nFileSizeHigh;
  unsigned long bhfi_nFileSizeLow;
  unsigned long bhfi_nNumberOfLinks;
  unsigned long bhfi_nFileIndexHigh;
  unsigned long bhfi_nFileIndexLow;
} _BY_HANDLE_FILE_INFORMATION;

/* compile-time checks against the DDK layouts */
#define IFS_CHECK(name, cond) typedef char ifs_check_##name[(cond) ? 1 : -1]
#define IFS_OFFSET(t, f) ((unsigned)(uintptr_t)&((t *)0)->f)
IFS_CHECK(ioreq_size, sizeof(ioreq) == 0x74);
IFS_CHECK(ioreq_data, IFS_OFFSET(ioreq, ir_data) == 0x14);
IFS_CHECK(ioreq_options, IFS_OFFSET(ioreq, ir_options) == 0x18);
IFS_CHECK(ioreq_rh, IFS_OFFSET(ioreq, ir_rh) == 0x1C);
IFS_CHECK(ioreq_pos, IFS_OFFSET(ioreq, ir_pos) == 0x24);
IFS_CHECK(ioreq_aux3, IFS_OFFSET(ioreq, ir_aux3) == 0x2C);
IFS_CHECK(volfunc_size, sizeof(volfunc) == 64);
IFS_CHECK(hndlmisc_size, sizeof(hndlmisc) == 36);
IFS_CHECK(srch_entry_size, sizeof(srch_entry) == 43);
IFS_CHECK(srch_entry_size_field, IFS_OFFSET(srch_entry, se_size) == 26);
IFS_CHECK(find_data_size, sizeof(_WIN32_FIND_DATA) == 592);
IFS_CHECK(find_data_name, IFS_OFFSET(_WIN32_FIND_DATA, cFileName) == 44);
IFS_CHECK(bhfi_size, sizeof(_BY_HANDLE_FILE_INFORMATION) == 52);
IFS_CHECK(use_info_2_remote, IFS_OFFSET(use_info_2, ui2_remote) == 0x0A);
IFS_CHECK(use_info_2_asg_type, IFS_OFFSET(use_info_2, ui2_asg_type) == 0x14);

#endif
