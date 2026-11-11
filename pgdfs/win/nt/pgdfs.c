/*
 * pgdfs.sys - the PicoGUS USB drive as a drive letter on Windows NT 4.0 and
 * Windows 2000 (and XP)
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * A small file system driver. It owns one device object,
 * \Device\PgDfs, of type FILE_DEVICE_DISK_FILE_SYSTEM with the
 * FILE_REMOVABLE_MEDIA characteristic and no VPB, and a drive letter that
 * points at it (\DosDevices\P: by default). The I/O manager hands every
 * create on that letter straight to this driver with the path in the file
 * object, and Windows reports the letter as a removable drive. Every request
 * is translated into PGDFS (EDF5) requests to the card by the shared client
 * core in ../common/pgc.c, which also deals with the card's eight file ids
 * and with the USB drive coming and going.
 *
 * The removable drive:
 *  - no USB drive: creates fail with STATUS_NO_MEDIA_IN_DEVICE ("Please
 *    insert a disk into drive P:" in Explorer), a volume handle (\\.\P:)
 *    answers IOCTL_STORAGE_CHECK_VERIFY with the same, as a floppy does;
 *  - the same drive plugged back in: open files carry on (FastFat does the
 *    same after verifying a floppy's serial number and label);
 *  - a different drive: files opened on the previous one fail with
 *    STATUS_FILE_INVALID, new opens see the new drive;
 *  - a thread looks at the card every two seconds and completes pending
 *    directory change notifications when the drive came or went, so an
 *    Explorer window on the drive refreshes by itself.
 *
 * Design limits: no cache manager (reads and writes go to the card as they
 * come; memory-mapped files and programs run from the drive work through
 * paging I/O), no file locking (lock requests succeed, as in EtherDFS), no
 * security or EAs (as FAT), long names limited to what the card's FatFs and
 * its 127-byte paths allow. All requests are serialized by one mutex: there
 * is one card and it does one request at a time.
 *
 * Only kernel exports that Windows NT 4.0 already has are used.
 */

#include <ntifs.h>
#include "../common/pgc.h"

/* storage IOCTLs a removable drive answers (ntddstor.h / ntdddisk.h values) */
#define PG_IOCTL_STORAGE_CHECK_VERIFY  CTL_CODE(FILE_DEVICE_MASS_STORAGE, 0x0200, METHOD_BUFFERED, FILE_READ_ACCESS)
#define PG_IOCTL_STORAGE_CHECK_VERIFY2 CTL_CODE(FILE_DEVICE_MASS_STORAGE, 0x0200, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define PG_IOCTL_DISK_CHECK_VERIFY     CTL_CODE(FILE_DEVICE_DISK, 0x0200, METHOD_BUFFERED, FILE_READ_ACCESS)
#define PG_IOCTL_DISK_IS_WRITABLE      CTL_CODE(FILE_DEVICE_DISK, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#ifndef FILE_DEVICE_IS_MOUNTED
#define FILE_DEVICE_IS_MOUNTED       0x00000020
#endif

#define PGDFS_TAG       0x73666750  /* 'Pgfs' */
#define PGDFS_VERSION   "0.3.0"
#define DEVICE_NAME     L"\\Device\\PgDfs"
#define POLL_MS         2000        /* media check interval */
#define TIME_PUSH_MS    60000       /* how often the clock goes to the card */
#define LIST_BLOCK      16384       /* directory snapshot chunk */

#define DBG_PRINT(x)    DbgPrint x

/* ---- data structures ------------------------------------------------------- */

enum { FCB_VOLUME, FCB_ROOT, FCB_DIR, FCB_FILE };

typedef struct _PGFCB {
  FSRTL_COMMON_FCB_HEADER Header;     /* first: FsContext points here */
  SECTION_OBJECT_POINTERS Section;
  ERESOURCE MainResource;
  ERESOURCE PagingResource;
  LIST_ENTRY Link;                    /* Vcb.FcbList (unless orphaned) */
  LONG RefCount;                      /* file objects (IRP_MJ_CLOSE pending) */
  LONG OpenCount;                     /* handles (IRP_MJ_CLEANUP pending)    */
  UNICODE_STRING Name;                /* "\DIR\NAME", "\" for the root, "" volume */
  char Oem[PGC_PATH_MAX];             /* the same in the card's code page    */
  UCHAR Kind;
  UCHAR Attr;                         /* FAT attributes                      */
  USHORT Time, Date;                  /* FAT modification time, local        */
  BOOLEAN Listed;                     /* in Vcb.FcbList                      */
  BOOLEAN DeletePending;
  BOOLEAN Deleted;
  BOOLEAN Modified;                   /* written through: notify at cleanup  */
  BOOLEAN TimeStale;                  /* the card changed the time (writes)  */
  ULONG MediaGen;                     /* medium the FCB belongs to           */
  SHARE_ACCESS Share;
  pgc_file File;                      /* the card side of a file             */
} PGFCB;

/* one directory entry of a listing snapshot, followed by the long name and
 * the short name (WCHARs), padded to 4 bytes */
typedef struct {
  USHORT Bytes;                       /* record size                         */
  UCHAR Attr;
  UCHAR Pad;
  ULONG Size;
  USHORT Time, Date;
  USHORT LongLen, ShortLen;           /* bytes                               */
} DENT;

typedef struct _LISTBLK {
  struct _LISTBLK *Next;
  ULONG Used;
  UCHAR Data[LIST_BLOCK];
} LISTBLK;

typedef struct _PGCCB {
  LIST_ENTRY NotifyLink;              /* Vcb.NotifyList while registered     */
  LIST_ENTRY NotifyIrps;              /* pending IRP_MN_NOTIFY_CHANGE_DIRECTORY */
  BOOLEAN NotifyRegistered;
  BOOLEAN NotifyTree;
  BOOLEAN NotifyChanged;              /* a change came while no IRP waited   */
  ULONG NotifyFilter;
  PFILE_OBJECT FileObject;
  /* directory listing */
  LISTBLK *List;
  ULONG ListCount;
  ULONG Index;                        /* next entry to return                */
  LISTBLK *CurBlk;                    /* cursor at Index                     */
  ULONG CurOff;
  UNICODE_STRING Pattern;             /* upcased, allocated by Rtl           */
  BOOLEAN PatternWild;
  BOOLEAN Started;
  BOOLEAN ReturnedAny;
  BOOLEAN DeleteOnClose;
} PGCCB;

typedef struct _VCB {
  KMUTEX Lock;                        /* everything below and the card       */
  LIST_ENTRY FcbList;
  KSPIN_LOCK NotifyLock;              /* NotifyList and the CCB IRP queues   */
  LIST_ENTRY NotifyList;
  PDEVICE_OBJECT Device;
  WCHAR Letter;
  ULONG ChangeCount;                  /* Card.change_count last seen         */
  ULONG TimePushed;                   /* pgc_plat_ms() of the last clock push */
  BOOLEAN TimeValid;
  LARGE_INTEGER PerfFreq;
} VCB;

static VCB Vcb;
static pgc_card Card;

/* ---- platform layer for the client core ------------------------------------- */

uint8_t pgc_plat_inb(uint16_t port) {
  uint8_t v;
  __asm__ __volatile__("inb %w1, %b0" : "=a"(v) : "Nd"(port));
  return v;
}

void pgc_plat_outb(uint16_t port, uint8_t value) {
  __asm__ __volatile__("outb %b0, %w1" : : "a"(value), "Nd"(port));
}

void pgc_plat_insw(uint16_t port, void *dst, uint16_t words) {
  ULONG n = words;
  __asm__ __volatile__("cld; rep insw" : "+D"(dst), "+c"(n) : "d"(port) : "memory");
}

void pgc_plat_outsw(uint16_t port, const void *src, uint16_t words) {
  ULONG n = words;
  __asm__ __volatile__("cld; rep outsw" : "+S"(src), "+c"(n) : "d"(port) : "memory");
}

/* 64 / 32 bit division without libgcc: two DIVs, each with a quotient that fits */
static ULONGLONG UDiv64(ULONGLONG n, ULONG d, ULONG *rem) {
  ULONG hi = (ULONG)(n >> 32), lo = (ULONG)n, qhi, qlo, r;
  qhi = hi / d;
  r = hi % d;
  __asm__("divl %4" : "=a"(qlo), "=d"(r) : "a"(lo), "d"(r), "rm"(d));
  if (rem) *rem = r;
  return ((ULONGLONG)qhi << 32) | qlo;
}

uint32_t pgc_plat_ms(void) {
  ULONGLONG t = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
  ULONGLONG f = (ULONGLONG)Vcb.PerfFreq.QuadPart;
  ULONG r, shift = 0, ms;
  if (f == 0) return 0;
  while ((f >> shift) > 0xFFFFFFFFull) shift++;     /* a counter faster than 4 GHz */
  t >>= shift;
  f >>= shift;
  ms = (ULONG)UDiv64(t, (ULONG)f, &r) * 1000;       /* whole seconds; wraps like a ms counter should */
  return ms + (ULONG)UDiv64((ULONGLONG)r * 1000, (ULONG)f, NULL);
}

/* the card answers most requests within a millisecond or two: spin that
 * long, then give the processor away in 1 ms naps (paging I/O arrives at
 * APC_LEVEL, where KeDelayExecutionThread is still allowed) */
void pgc_plat_poll(uint32_t waited_ms) {
  if (waited_ms >= 2 && KeGetCurrentIrql() <= APC_LEVEL) {
    LARGE_INTEGER d;
    d.QuadPart = -10000;    /* 1 ms, relative */
    KeDelayExecutionThread(KernelMode, FALSE, &d);
  }
}

/* ---- small helpers ------------------------------------------------------------ */

static void VcbLock(void) {
  KeWaitForSingleObject(&Vcb.Lock, Executive, KernelMode, FALSE, NULL);
}

static void VcbUnlock(void) {
  KeReleaseMutex(&Vcb.Lock, FALSE);
}

static PVOID AllocNp(ULONG n) {
  PVOID p = ExAllocatePoolWithTag(NonPagedPool, n, PGDFS_TAG);
  if (p) RtlZeroMemory(p, n);
  return p;
}

static PVOID AllocPaged(ULONG n) {
  return ExAllocatePoolWithTag(PagedPool, n, PGDFS_TAG);
}

static void Complete(PIRP irp, NTSTATUS st, ULONG_PTR info) {
  irp->IoStatus.Status = st;
  irp->IoStatus.Information = info;
  IoCompleteRequest(irp, NT_SUCCESS(st) ? IO_DISK_INCREMENT : IO_NO_INCREMENT);
}

/* MmGetSystemAddressForMdl as NT 4.0 has it */
static PVOID MdlAddress(PMDL mdl) {
  if (mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) return mdl->MappedSystemVa;
  return MmMapLockedPages(mdl, KernelMode);
}

static NTSTATUS PgcStatus(int r) {
  switch (r) {
  case PGC_OK:        return STATUS_SUCCESS;
  case PGC_ENOFILE:   return STATUS_OBJECT_NAME_NOT_FOUND;
  case PGC_ENOPATH:   return STATUS_OBJECT_PATH_NOT_FOUND;
  case PGC_EHANDLES:  return STATUS_TOO_MANY_OPENED_FILES;
  case PGC_EACCESS:   return STATUS_ACCESS_DENIED;
  case PGC_ENOMEM:    return STATUS_INSUFFICIENT_RESOURCES;
  case PGC_ENOMORE:   return STATUS_NO_MORE_FILES;
  case PGC_EWRPROT:   return STATUS_MEDIA_WRITE_PROTECTED;
  case PGC_ENOTREADY: return Card.present ? STATUS_DEVICE_NOT_READY : STATUS_NO_MEDIA_IN_DEVICE;
  case PGC_EWRFAULT:
  case PGC_ERDFAULT:
  case PGC_EPROTO:    return STATUS_IO_DEVICE_ERROR;
  case PGC_ESHARING:  return STATUS_SHARING_VIOLATION;
  case PGC_EEXISTS:   return STATUS_OBJECT_NAME_COLLISION;
  case PGC_ESTALE:    return STATUS_FILE_INVALID;
  case PGC_ETOOLONG:  return STATUS_NAME_TOO_LONG;
  case PGC_ENOCARD:
  case PGC_EDRIVE:    return STATUS_NO_SUCH_DEVICE;
  default:            return STATUS_UNEXPECTED_IO_ERROR;
  }
}

static ULONG NtAttr(UCHAR a) {
  ULONG r = a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                 FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE);
  return r ? r : FILE_ATTRIBUTE_NORMAL;
}

static UCHAR FatAttr(ULONG a) {
  return (UCHAR)(a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE));
}

/* FAT local time -> NT system time (UTC); 0 when there is none */
static LARGE_INTEGER NtTime(USHORT time, USHORT date) {
  TIME_FIELDS tf;
  LARGE_INTEGER local, sys;
  sys.QuadPart = 0;
  if (date == 0) return sys;
  tf.Year = (CSHORT)(1980 + (date >> 9));
  tf.Month = (CSHORT)((date >> 5) & 15);
  tf.Day = (CSHORT)(date & 31);
  tf.Hour = (CSHORT)(time >> 11);
  tf.Minute = (CSHORT)((time >> 5) & 63);
  tf.Second = (CSHORT)((time & 31) * 2);
  tf.Milliseconds = 0;
  tf.Weekday = 0;
  if (!RtlTimeFieldsToTime(&tf, &local)) return sys;
  ExLocalTimeToSystemTime(&local, &sys);
  return sys;
}

/* NT system time -> FAT local time; FALSE when it does not fit FAT */
static BOOLEAN FatTime(LARGE_INTEGER sys, USHORT *time, USHORT *date) {
  TIME_FIELDS tf;
  LARGE_INTEGER local;
  ExSystemTimeToLocalTime(&sys, &local);
  RtlTimeToTimeFields(&local, &tf);
  if (tf.Year < 1980 || tf.Year > 2107) return FALSE;
  *time = (USHORT)((tf.Hour << 11) | (tf.Minute << 5) | (tf.Second / 2));
  *date = (USHORT)(((tf.Year - 1980) << 9) | (tf.Month << 5) | tf.Day);
  return TRUE;
}

/* the card stamps the files it creates and modifies with the clock it was given */
static void PushTime(BOOLEAN force) {
  LARGE_INTEGER now;
  USHORT t, d;
  ULONG ms = pgc_plat_ms();
  if (!force && Vcb.TimeValid && (ULONG)(ms - Vcb.TimePushed) < TIME_PUSH_MS) return;
  KeQuerySystemTime(&now);
  if (FatTime(now, &t, &d)) {
    pgc_set_time(&Card, t, d);
    Vcb.TimePushed = ms;
    Vcb.TimeValid = TRUE;
  }
}

/* the card's FatFs uses 512-byte sectors and reports its cluster size in the
 * DIAG record; until it has been asked, assume 4 KB */
static ULONG ClusterBytes(void) {
  return Card.cluster_bytes ? Card.cluster_bytes : 4096;
}

/* cluster sizes are powers of two */
static LONGLONG AllocSize(ULONG size) {
  ULONGLONG c = ClusterBytes();
  return (LONGLONG)(((ULONGLONG)size + c - 1) & ~(c - 1));
}

static BOOLEAN UpcaseEqual(PCUNICODE_STRING a, PCUNICODE_STRING b) {
  return RtlEqualUnicodeString(a, b, TRUE);
}

/* a is a strict ancestor directory of b ("\A" of "\A\B", "\" of anything) */
static BOOLEAN IsAncestor(PCUNICODE_STRING a, PCUNICODE_STRING b) {
  UNICODE_STRING head;
  USHORT al = a->Length;
  if (al == 2 && a->Buffer[0] == L'\\') return b->Length > 2;   /* the root */
  if (b->Length <= al || b->Buffer[al / 2] != L'\\') return FALSE;
  head.Buffer = b->Buffer;
  head.Length = head.MaximumLength = al;
  return UpcaseEqual(a, &head);
}

/* "\A\B" -> parent "\A", "\A" -> "\"; last component in *last */
static void SplitPath(PCUNICODE_STRING full, PUNICODE_STRING parent, PUNICODE_STRING last) {
  USHORT n = full->Length / 2, i = n;
  while (i > 0 && full->Buffer[i - 1] != L'\\') i--;
  parent->Buffer = full->Buffer;
  parent->Length = parent->MaximumLength = (USHORT)((i > 1 ? i - 1 : 1) * 2);
  if (last) {
    last->Buffer = full->Buffer + i;
    last->Length = last->MaximumLength = (USHORT)((n - i) * 2);
  }
}

/* Unicode path -> the card's code page; refuses names that do not survive the
 * round trip (characters the OEM code page does not have) */
static NTSTATUS ToOem(PCUNICODE_STRING u, char *oem) {
  char tmp[2 * PGC_PATH_MAX];
  WCHAR back[2 * PGC_PATH_MAX];
  ULONG n = 0, m = 0;
  NTSTATUS st;
  if (u->Length / 2 >= PGC_PATH_MAX) return STATUS_NAME_TOO_LONG;
  st = RtlUnicodeToOemN(tmp, sizeof(tmp), &n, u->Buffer, u->Length);
  if (!NT_SUCCESS(st)) return STATUS_OBJECT_NAME_INVALID;
  if (n >= PGC_PATH_MAX) return STATUS_NAME_TOO_LONG;
  st = RtlOemToUnicodeN(back, sizeof(back), &m, tmp, n);
  if (!NT_SUCCESS(st) || m != u->Length || RtlCompareMemory(back, u->Buffer, m) != m) return STATUS_OBJECT_NAME_INVALID;
  RtlCopyMemory(oem, tmp, n);
  oem[n] = 0;
  return STATUS_SUCCESS;
}

/* OEM -> Unicode into dst (max bytes); returns the byte length */
static USHORT FromOem(const char *oem, WCHAR *dst, ULONG max) {
  ULONG n = 0;
  if (!NT_SUCCESS(RtlOemToUnicodeN(dst, max, &n, (PCHAR)oem, pgc_strlen(oem)))) return 0;
  return (USHORT)n;
}

/* ---- change notification ----------------------------------------------------------
 * Pending IRP_MN_NOTIFY_CHANGE_DIRECTORY requests are completed with
 * STATUS_NOTIFY_ENUM_DIR ("something changed, list it again") when a change
 * made through this driver touches the watched directory, and all of them
 * when the medium comes or goes. Changes made while no request is pending
 * are remembered per handle, so none is lost between two requests. */

static VOID NTAPI NotifyCancel(PDEVICE_OBJECT dev, PIRP irp) {
  KIRQL old;
  (void)dev;
  IoReleaseCancelSpinLock(irp->CancelIrql);
  KeAcquireSpinLock(&Vcb.NotifyLock, &old);
  RemoveEntryList(&irp->Tail.Overlay.ListEntry);
  KeReleaseSpinLock(&Vcb.NotifyLock, old);
  Complete(irp, STATUS_CANCELLED, 0);
}

/* takes the CCB's pending IRPs (those whose cancel routine has not started)
 * onto out. NotifyLock held. */
static void TakeIrps(PGCCB *ccb, PLIST_ENTRY out) {
  PLIST_ENTRY e = ccb->NotifyIrps.Flink;
  while (e != &ccb->NotifyIrps) {
    PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
    e = e->Flink;
    if (IoSetCancelRoutine(irp, NULL) != NULL) {
      RemoveEntryList(&irp->Tail.Overlay.ListEntry);
      InsertTailList(out, &irp->Tail.Overlay.ListEntry);
    }
  }
}

static void CompleteList(PLIST_ENTRY list, NTSTATUS st) {
  while (!IsListEmpty(list)) {
    PLIST_ENTRY e = RemoveHeadList(list);
    Complete(CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry), st, 0);
  }
}

/* something in directory dir changed (filter: FILE_NOTIFY_CHANGE_*) */
static void NotifyDir(PCUNICODE_STRING dir, ULONG filter) {
  LIST_ENTRY done;
  PLIST_ENTRY e;
  KIRQL old;
  InitializeListHead(&done);
  KeAcquireSpinLock(&Vcb.NotifyLock, &old);
  for (e = Vcb.NotifyList.Flink; e != &Vcb.NotifyList; e = e->Flink) {
    PGCCB *ccb = CONTAINING_RECORD(e, PGCCB, NotifyLink);
    PGFCB *fcb = (PGFCB *)ccb->FileObject->FsContext;
    if (!(ccb->NotifyFilter & filter)) continue;
    if (!UpcaseEqual(&fcb->Name, dir) && !(ccb->NotifyTree && IsAncestor(&fcb->Name, dir))) continue;
    if (IsListEmpty(&ccb->NotifyIrps)) ccb->NotifyChanged = TRUE;
    else TakeIrps(ccb, &done);
  }
  KeReleaseSpinLock(&Vcb.NotifyLock, old);
  CompleteList(&done, STATUS_NOTIFY_ENUM_DIR);
}

/* the item at path changed: notify its directory */
static void NotifyItem(PCUNICODE_STRING path, ULONG filter) {
  UNICODE_STRING parent;
  SplitPath(path, &parent, NULL);
  NotifyDir(&parent, filter);
}

/* the medium came or went: every watcher lists again */
static void NotifyAll(void) {
  LIST_ENTRY done;
  PLIST_ENTRY e;
  KIRQL old;
  InitializeListHead(&done);
  KeAcquireSpinLock(&Vcb.NotifyLock, &old);
  for (e = Vcb.NotifyList.Flink; e != &Vcb.NotifyList; e = e->Flink) {
    PGCCB *ccb = CONTAINING_RECORD(e, PGCCB, NotifyLink);
    if (IsListEmpty(&ccb->NotifyIrps)) ccb->NotifyChanged = TRUE;
    else TakeIrps(ccb, &done);
  }
  KeReleaseSpinLock(&Vcb.NotifyLock, old);
  CompleteList(&done, STATUS_NOTIFY_ENUM_DIR);
}

/* called with Vcb.Lock held after anything that may have looked at the medium */
static void MediaChanged(void) {
  if (Card.change_count != Vcb.ChangeCount) {
    Vcb.ChangeCount = Card.change_count;
    NotifyAll();
  }
}

static NTSTATUS NotifyChangeDirectory(PIRP irp, PIO_STACK_LOCATION sp, PGFCB *fcb, PGCCB *ccb) {
  KIRQL old;
  BOOLEAN now = FALSE;
  if (fcb->Kind != FCB_DIR && fcb->Kind != FCB_ROOT) return STATUS_INVALID_PARAMETER;
  KeAcquireSpinLock(&Vcb.NotifyLock, &old);
  ccb->NotifyFilter = sp->Parameters.NotifyDirectory.CompletionFilter;
  ccb->NotifyTree = (sp->Flags & SL_WATCH_TREE) != 0;
  if (!ccb->NotifyRegistered) {
    ccb->NotifyRegistered = TRUE;
    InsertTailList(&Vcb.NotifyList, &ccb->NotifyLink);
  }
  if (ccb->NotifyChanged) {
    ccb->NotifyChanged = FALSE;
    now = TRUE;
  } else {
    IoMarkIrpPending(irp);
    InsertTailList(&ccb->NotifyIrps, &irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(irp, NotifyCancel);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL) != NULL) {
      RemoveEntryList(&irp->Tail.Overlay.ListEntry);
      KeReleaseSpinLock(&Vcb.NotifyLock, old);
      Complete(irp, STATUS_CANCELLED, 0);
      return STATUS_PENDING;
    }
  }
  KeReleaseSpinLock(&Vcb.NotifyLock, old);
  if (now) {
    Complete(irp, STATUS_NOTIFY_ENUM_DIR, 0);
    return STATUS_NOTIFY_ENUM_DIR;
  }
  return STATUS_PENDING;
}

static void NotifyCleanup(PGCCB *ccb) {
  LIST_ENTRY done;
  KIRQL old;
  InitializeListHead(&done);
  KeAcquireSpinLock(&Vcb.NotifyLock, &old);
  if (ccb->NotifyRegistered) {
    TakeIrps(ccb, &done);
    RemoveEntryList(&ccb->NotifyLink);
    ccb->NotifyRegistered = FALSE;
  }
  KeReleaseSpinLock(&Vcb.NotifyLock, old);
  CompleteList(&done, STATUS_NOTIFY_CLEANUP);
}

/* ---- FCBs ------------------------------------------------------------------------- */

static PGFCB *NewFcb(PCUNICODE_STRING name, const char *oem, UCHAR kind) {
  PGFCB *f = (PGFCB *)AllocNp(sizeof(PGFCB));
  if (!f) return NULL;
  f->Name.Buffer = (PWCH)AllocNp(name->Length + sizeof(WCHAR));
  if (!f->Name.Buffer && name->Length) {
    ExFreePool(f);
    return NULL;
  }
  if (name->Length) RtlCopyMemory(f->Name.Buffer, name->Buffer, name->Length);
  f->Name.Length = name->Length;
  f->Name.MaximumLength = (USHORT)(name->Length + sizeof(WCHAR));
  RtlCopyMemory(f->Oem, oem, pgc_strlen(oem) + 1);
  f->Kind = kind;
  f->MediaGen = Card.media_gen;
  ExInitializeResourceLite(&f->MainResource);
  ExInitializeResourceLite(&f->PagingResource);
  f->Header.NodeTypeCode = 0x0F01;
  f->Header.NodeByteSize = sizeof(PGFCB);
  f->Header.IsFastIoPossible = FastIoIsNotPossible;
  f->Header.Resource = &f->MainResource;
  f->Header.PagingIoResource = &f->PagingResource;
  InsertTailList(&Vcb.FcbList, &f->Link);
  f->Listed = TRUE;
  return f;
}

static void SetSize(PGFCB *f, ULONG size) {
  f->Header.FileSize.QuadPart = size;
  f->Header.ValidDataLength.QuadPart = size;
  f->Header.AllocationSize.QuadPart = AllocSize(size);
}

static void Unlist(PGFCB *f) {
  if (f->Listed) {
    RemoveEntryList(&f->Link);
    f->Listed = FALSE;
  }
}

static void FreeFcb(PGFCB *f) {
  Unlist(f);
  if (f->Kind == FCB_FILE) (void)pgc_close(&Card, &f->File);
  ExDeleteResourceLite(&f->MainResource);
  ExDeleteResourceLite(&f->PagingResource);
  if (f->Name.Buffer) ExFreePool(f->Name.Buffer);
  ExFreePool(f);
}

/* the live FCB for path, if any. FCBs of a medium that is gone are taken
 * out of the table: their handles fail, new opens get a new FCB. */
static PGFCB *FindFcb(PCUNICODE_STRING path) {
  PLIST_ENTRY e = Vcb.FcbList.Flink;
  while (e != &Vcb.FcbList) {
    PGFCB *f = CONTAINING_RECORD(e, PGFCB, Link);
    e = e->Flink;
    if (f->MediaGen != Card.media_gen) {
      Unlist(f);
      continue;
    }
    if (!f->Deleted && UpcaseEqual(&f->Name, path)) return f;
  }
  return NULL;
}

/* after a rename of a directory: open FCBs below it follow */
static NTSTATUS ToOem(PCUNICODE_STRING u, char *oem);

static void RenameChildren(PCUNICODE_STRING from, PCUNICODE_STRING to) {
  PLIST_ENTRY e;
  USHORT fl = from->Length;
  for (e = Vcb.FcbList.Flink; e != &Vcb.FcbList; e = e->Flink) {
    PGFCB *f = CONTAINING_RECORD(e, PGFCB, Link);
    USHORT rest, nl;
    PWCH nb;
    char oem[PGC_PATH_MAX];
    if (!IsAncestor(from, &f->Name)) continue;
    rest = (USHORT)(f->Name.Length - fl);
    nl = (USHORT)(to->Length + rest);
    nb = (PWCH)AllocNp(nl + sizeof(WCHAR));
    if (!nb) {
      Unlist(f);              /* cannot follow: let it go stale */
      continue;
    }
    RtlCopyMemory(nb, to->Buffer, to->Length);
    RtlCopyMemory((PUCHAR)nb + to->Length, (PUCHAR)f->Name.Buffer + fl, rest);
    ExFreePool(f->Name.Buffer);
    f->Name.Buffer = nb;
    f->Name.Length = nl;
    f->Name.MaximumLength = (USHORT)(nl + sizeof(WCHAR));
    /* the card path follows the Unicode one */
    if (NT_SUCCESS(ToOem(&f->Name, oem))) {
      RtlCopyMemory(f->Oem, oem, pgc_strlen(oem) + 1);
      if (f->Kind == FCB_FILE) (void)pgc_file_moved(&Card, &f->File, oem);
    } else {
      Unlist(f);
    }
  }
}

/* ---- create ------------------------------------------------------------------------- */

static BOOLEAN BadChar(WCHAR c) {
  return c < 0x20 || c == L'"' || c == L'*' || c == L'/' || c == L':' || c == L'<' || c == L'>' ||
         c == L'?' || c == L'|';
}

/* checks a path that starts with "\" (not the root): no empty components, no
 * "." / ".." and nothing FAT cannot store */
static NTSTATUS CheckPath(PCUNICODE_STRING p) {
  USHORT n = p->Length / 2, i, start = 1;
  for (i = 1; i <= n; i++) {
    if (i == n || p->Buffer[i] == L'\\') {
      USHORT len = (USHORT)(i - start);
      if (len == 0) return STATUS_OBJECT_NAME_INVALID;
      if (len > 255) return STATUS_OBJECT_NAME_INVALID;
      if (p->Buffer[start] == L'.' && (len == 1 || (len == 2 && p->Buffer[start + 1] == L'.')))
        return STATUS_OBJECT_NAME_INVALID;
      start = (USHORT)(i + 1);
    } else if (BadChar(p->Buffer[i])) {
      return STATUS_OBJECT_NAME_INVALID;
    }
  }
  return STATUS_SUCCESS;
}

/* full path of a create: related object's path + the name. Allocates out.
 * *dironly: the name ended with a backslash. */
static NTSTATUS CreatePath(PFILE_OBJECT fo, PUNICODE_STRING out, BOOLEAN *dironly) {
  UNICODE_STRING name = fo->FileName, base;
  PGFCB *rel = NULL;
  ULONG len;
  PWCH b;
  NTSTATUS st;
  static const WCHAR rootname[] = L"\\";

  *dironly = FALSE;
  RtlZeroMemory(out, sizeof(*out));
  if (fo->RelatedFileObject) rel = (PGFCB *)fo->RelatedFileObject->FsContext;
  /* a stream suffix: only the unnamed data stream exists */
  {
    USHORT n = name.Length / 2, i;
    for (i = 0; i < n; i++) {
      if (name.Buffer[i] == L':') {
        UNICODE_STRING suffix, d1, d2;
        suffix.Buffer = name.Buffer + i;
        suffix.Length = suffix.MaximumLength = (USHORT)((n - i) * 2);
        RtlInitUnicodeString(&d1, L"::$DATA");
        RtlInitUnicodeString(&d2, L":$DATA");
        if (!UpcaseEqual(&suffix, &d1) && !UpcaseEqual(&suffix, &d2)) return STATUS_OBJECT_NAME_INVALID;
        name.Length = (USHORT)(i * 2);
        break;
      }
    }
  }
  while (name.Length > 2 && name.Buffer[name.Length / 2 - 1] == L'\\') {
    name.Length -= 2;
    *dironly = TRUE;
  }
  if (rel && rel->Kind != FCB_VOLUME) {
    if (rel->Kind == FCB_FILE) return STATUS_INVALID_PARAMETER;
    if (name.Length && name.Buffer[0] == L'\\') return STATUS_OBJECT_NAME_INVALID;
    base = rel->Name;
  } else {
    if (name.Length == 0 || name.Buffer[0] != L'\\') return STATUS_OBJECT_NAME_INVALID;
    base.Buffer = (PWCH)rootname;
    base.Length = 0;
  }
  len = (ULONG)base.Length + 2 + name.Length;
  if (len > 0xFFF0) return STATUS_NAME_TOO_LONG;
  b = (PWCH)AllocNp(len + 2);
  if (!b) return STATUS_INSUFFICIENT_RESOURCES;
  out->Buffer = b;
  out->MaximumLength = (USHORT)(len + 2);
  if (rel && rel->Kind != FCB_VOLUME) {
    RtlCopyMemory(b, base.Buffer, base.Length);
    out->Length = base.Length;
    if (name.Length) {
      if (out->Length != 2) b[out->Length / 2] = L'\\', out->Length += 2;   /* not after the root "\" */
      RtlCopyMemory((PUCHAR)b + out->Length, name.Buffer, name.Length);
      out->Length += name.Length;
    }
  } else {
    RtlCopyMemory(b, name.Buffer, name.Length);
    out->Length = name.Length;
  }
  if (out->Length == 2) return STATUS_SUCCESS;     /* "\" */
  st = CheckPath(out);
  if (!NT_SUCCESS(st)) {
    ExFreePool(out->Buffer);
    RtlZeroMemory(out, sizeof(*out));
  }
  return st;
}

/* FAT attributes etc. of path: from the FCB when one is open, else the card */
static int Stat(PGFCB *f, const char *oem, UCHAR *attr, ULONG *size, USHORT *time, USHORT *date) {
  uint8_t a;
  uint32_t s;
  uint16_t t, d;
  int r;
  if (f && !f->TimeStale) {
    *attr = f->Attr;
    *size = (ULONG)f->Header.FileSize.QuadPart;
    *time = f->Time;
    *date = f->Date;
    return PGC_OK;
  }
  r = pgc_getattr(&Card, oem, &a, &s, &t, &d);
  if (r != PGC_OK) return r;
  *attr = a;
  *size = s;
  *time = t;
  *date = d;
  if (f) {
    f->Attr = a;
    f->Time = t;
    f->Date = d;
    f->TimeStale = FALSE;
    if (f->Kind == FCB_FILE) SetSize(f, s);
  }
  return PGC_OK;
}

static PGFCB VolumeFcb;          /* \\.\P: handles, never freed */

static NTSTATUS AttachFile(PFILE_OBJECT fo, PGFCB *fcb, PIO_STACK_LOCATION sp, BOOLEAN delete_on_close) {
  PGCCB *ccb = (PGCCB *)AllocNp(sizeof(PGCCB));
  if (!ccb) return STATUS_INSUFFICIENT_RESOURCES;
  InitializeListHead(&ccb->NotifyIrps);
  ccb->FileObject = fo;
  ccb->DeleteOnClose = delete_on_close;
  if (fcb->Kind != FCB_VOLUME) {
    ACCESS_MASK access = sp->Parameters.Create.SecurityContext->DesiredAccess;
    ULONG share = sp->Parameters.Create.ShareAccess;
    if (fcb->OpenCount == 0) {
      IoSetShareAccess(access, share, fo, &fcb->Share);
    } else {
      NTSTATUS st = IoCheckShareAccess(access, share, fo, &fcb->Share, TRUE);
      if (!NT_SUCCESS(st)) {
        ExFreePool(ccb);
        return st;
      }
    }
  }
  fo->FsContext = fcb;
  fo->FsContext2 = ccb;
  fo->SectionObjectPointer = &fcb->Section;
  fo->Vpb = NULL;
  fcb->RefCount++;
  fcb->OpenCount++;
  return STATUS_SUCCESS;
}

/* MmFlushImageSection and friends may wait for paging I/O, which needs the
 * lock: call them with the lock dropped (the caller's FCB reference keeps
 * the FCB alive) */
static BOOLEAN FlushImage(PGFCB *f, MMFLUSH_TYPE how) {
  BOOLEAN ok;
  if (!f || f->Section.ImageSectionObject == NULL) return TRUE;
  f->RefCount++;
  VcbUnlock();
  ok = MmFlushImageSection(&f->Section, how);
  VcbLock();
  f->RefCount--;
  return ok;
}

static BOOLEAN CanTruncate(PGFCB *f, LONGLONG size) {
  LARGE_INTEGER s;
  if (!f || f->Section.DataSectionObject == NULL) return TRUE;
  s.QuadPart = size;
  return MmCanFileBeTruncated(&f->Section, &s);
}

static NTSTATUS Create(PIRP irp, PIO_STACK_LOCATION sp) {
  PFILE_OBJECT fo = sp->FileObject;
  ULONG options = sp->Parameters.Create.Options;
  ULONG disp = (options >> 24) & 0xFF;
  ULONG copt = options & 0x00FFFFFF;
  ACCESS_MASK access = sp->Parameters.Create.SecurityContext->DesiredAccess;
  ULONG fattr = sp->Parameters.Create.FileAttributes;
  BOOLEAN dironly, want_dir, want_file, delete_on_close, writer, exists;
  UNICODE_STRING path;
  char oem[PGC_PATH_MAX];
  PGFCB *fcb;
  UCHAR attr = 0;
  ULONG size = 0;
  USHORT t = 0, d = 0;
  NTSTATUS st;
  int r;

  irp->IoStatus.Information = 0;
  if (sp->Parameters.Create.EaLength != 0) return STATUS_EAS_NOT_SUPPORTED;
  if (copt & FILE_OPEN_BY_FILE_ID) return STATUS_INVALID_PARAMETER;

  /* the volume itself: \\.\P: (no media needed, so CHECK_VERIFY can be asked) */
  if (fo->FileName.Length == 0 && (fo->RelatedFileObject == NULL ||
      ((PGFCB *)fo->RelatedFileObject->FsContext)->Kind == FCB_VOLUME)) {
    if (copt & FILE_DIRECTORY_FILE) return STATUS_NOT_A_DIRECTORY;
    st = AttachFile(fo, &VolumeFcb, sp, FALSE);
    if (NT_SUCCESS(st)) irp->IoStatus.Information = FILE_OPENED;
    return st;
  }

  st = CreatePath(fo, &path, &dironly);
  if (!NT_SUCCESS(st)) return st;

  r = pgc_media(&Card, 0);
  MediaChanged();
  if (r != PGC_OK) {
    st = PgcStatus(r);
    goto out;
  }
  PushTime(FALSE);

  want_dir = (copt & FILE_DIRECTORY_FILE) != 0 || dironly;
  want_file = (copt & FILE_NON_DIRECTORY_FILE) != 0;
  delete_on_close = (copt & FILE_DELETE_ON_CLOSE) != 0;
  writer = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0 ||
           disp == FILE_SUPERSEDE || disp == FILE_OVERWRITE || disp == FILE_OVERWRITE_IF;

  /* rename and link targets: open the parent, say whether the name exists */
  if (sp->Flags & SL_OPEN_TARGET_DIRECTORY) {
    UNICODE_STRING parent, last;
    char poem[PGC_PATH_MAX];
    if (path.Length == 2) {
      st = STATUS_OBJECT_NAME_INVALID;
      goto out;
    }
    SplitPath(&path, &parent, &last);
    st = ToOem(&path, oem);
    if (NT_SUCCESS(st)) st = ToOem(&parent, poem);
    if (!NT_SUCCESS(st)) goto out;
    fcb = FindFcb(&parent);
    if (parent.Length == 2) {
      attr = PGC_ATTR_DIR;
      r = PGC_OK;
    } else {
      r = Stat(fcb, poem, &attr, &size, &t, &d);
    }
    if (r != PGC_OK) {
      st = (r == PGC_ENOFILE) ? STATUS_OBJECT_PATH_NOT_FOUND : PgcStatus(r);
      goto out;
    }
    if (!(attr & PGC_ATTR_DIR)) {
      st = STATUS_OBJECT_PATH_NOT_FOUND;
      goto out;
    }
    exists = (FindFcb(&path) != NULL) || pgc_getattr(&Card, oem, NULL, NULL, NULL, NULL) == PGC_OK;
    if (!fcb) fcb = NewFcb(&parent, poem, parent.Length == 2 ? FCB_ROOT : FCB_DIR);
    if (!fcb) {
      st = STATUS_INSUFFICIENT_RESOURCES;
      goto out;
    }
    fcb->Attr = attr;
    st = AttachFile(fo, fcb, sp, FALSE);
    if (!NT_SUCCESS(st)) {
      if (fcb->RefCount == 0) FreeFcb(fcb);
      goto out;
    }
    /* the I/O manager wants the final component in the file object's name */
    {
      UNICODE_STRING *fn = &fo->FileName;
      USHORT n = fn->Length / 2, i = n;
      while (i > 0 && fn->Buffer[i - 1] != L'\\') i--;
      RtlMoveMemory(fn->Buffer, fn->Buffer + i, (n - i) * 2);
      fn->Length = (USHORT)((n - i) * 2);
    }
    irp->IoStatus.Information = exists ? FILE_EXISTS : FILE_DOES_NOT_EXIST;
    st = STATUS_SUCCESS;
    goto out;
  }

  /* the root */
  if (path.Length == 2) {
    if (want_file) {
      st = STATUS_FILE_IS_A_DIRECTORY;
      goto out;
    }
    if (disp != FILE_OPEN && disp != FILE_OPEN_IF) {
      st = STATUS_ACCESS_DENIED;
      goto out;
    }
    if (delete_on_close) {
      st = STATUS_CANNOT_DELETE;
      goto out;
    }
    fcb = FindFcb(&path);
    if (!fcb) fcb = NewFcb(&path, "\\", FCB_ROOT);
    if (!fcb) {
      st = STATUS_INSUFFICIENT_RESOURCES;
      goto out;
    }
    fcb->Attr = PGC_ATTR_DIR;
    st = AttachFile(fo, fcb, sp, FALSE);
    if (!NT_SUCCESS(st) && fcb->RefCount == 0) FreeFcb(fcb);
    if (NT_SUCCESS(st)) irp->IoStatus.Information = FILE_OPENED;
    goto out;
  }

  st = ToOem(&path, oem);
  if (!NT_SUCCESS(st)) goto out;
  fcb = FindFcb(&path);
  r = Stat(fcb, oem, &attr, &size, &t, &d);
  if (r != PGC_OK && r != PGC_ENOFILE) {
    st = PgcStatus(r);
    goto out;
  }
  exists = (r == PGC_OK);

  if (exists) {
    BOOLEAN isdir = (attr & PGC_ATTR_DIR) != 0;
    if (disp == FILE_CREATE) {
      st = STATUS_OBJECT_NAME_COLLISION;
      goto out;
    }
    if (isdir && want_file) {
      st = STATUS_FILE_IS_A_DIRECTORY;
      goto out;
    }
    if (!isdir && want_dir) {
      st = STATUS_NOT_A_DIRECTORY;
      goto out;
    }
    if (fcb && fcb->DeletePending) {
      st = STATUS_DELETE_PENDING;
      goto out;
    }
    if (isdir) {
      if (disp != FILE_OPEN && disp != FILE_OPEN_IF) {
        st = STATUS_OBJECT_NAME_COLLISION;
        goto out;
      }
      if (!fcb) fcb = NewFcb(&path, oem, FCB_DIR);
      if (!fcb) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
      }
      fcb->Attr = attr;
      fcb->Time = t;
      fcb->Date = d;
      st = AttachFile(fo, fcb, sp, delete_on_close);
      if (!NT_SUCCESS(st) && fcb->RefCount == 0) FreeFcb(fcb);
      if (NT_SUCCESS(st)) irp->IoStatus.Information = FILE_OPENED;
      goto out;
    }
    /* an existing file */
    if ((attr & PGC_ATTR_RDONLY) && (writer || delete_on_close)) {
      st = delete_on_close ? STATUS_CANNOT_DELETE : STATUS_ACCESS_DENIED;
      goto out;
    }
    if (writer || delete_on_close) {
      if (!FlushImage(fcb, delete_on_close ? MmFlushForDelete : MmFlushForWrite)) {
        st = delete_on_close ? STATUS_CANNOT_DELETE : STATUS_SHARING_VIOLATION;
        goto out;
      }
      fcb = FindFcb(&path);   /* the lock was dropped */
    }
    if (disp == FILE_OPEN || disp == FILE_OPEN_IF) {
      if (!fcb) {
        fcb = NewFcb(&path, oem, FCB_FILE);
        if (!fcb) {
          st = STATUS_INSUFFICIENT_RESOURCES;
          goto out;
        }
        r = pgc_file_attach(&Card, &fcb->File, oem, writer ? PGC_READWRITE : PGC_READ);
        if (r != PGC_OK) {
          FreeFcb(fcb);
          st = PgcStatus(r);
          goto out;
        }
        fcb->Attr = attr;
        fcb->Time = t;
        fcb->Date = d;
        SetSize(fcb, size);
      } else if (writer) {
        pgc_file_need(&fcb->File, PGC_READWRITE);
      }
      st = AttachFile(fo, fcb, sp, delete_on_close);
      if (!NT_SUCCESS(st) && fcb->RefCount == 0) FreeFcb(fcb);
      if (NT_SUCCESS(st)) irp->IoStatus.Information = FILE_OPENED;
      goto out;
    }
    /* FILE_SUPERSEDE, FILE_OVERWRITE, FILE_OVERWRITE_IF: truncate */
    if (!CanTruncate(fcb, 0)) {
      st = STATUS_USER_MAPPED_FILE;
      goto out;
    }
    if (!fcb) {
      fcb = NewFcb(&path, oem, FCB_FILE);
      if (!fcb) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
      }
    } else if (fcb->OpenCount > 0) {
      /* others have it open: their share access must allow the write */
      st = IoCheckShareAccess(access, sp->Parameters.Create.ShareAccess, fo, &fcb->Share, FALSE);
      if (!NT_SUCCESS(st)) goto out;
    }
    (void)pgc_close(&Card, &fcb->File);
    {
      uint8_t na;
      uint32_t ns;
      uint16_t nt, nd;
      r = pgc_open(&Card, &fcb->File, oem, PGC_TRUNCATE_EXISTING, PGC_READWRITE,
                   (uint8_t)(FatAttr(fattr) | PGC_ATTR_ARCHIVE), NULL, &na, &ns, &nt, &nd);
      if (r != PGC_OK) {
        if (fcb->RefCount == 0) FreeFcb(fcb);
        st = PgcStatus(r);
        goto out;
      }
      if (disp == FILE_SUPERSEDE || fattr) {
        /* overwrite replaces the attributes too */
        uint8_t keep = (uint8_t)(FatAttr(fattr) | PGC_ATTR_ARCHIVE);
        if (keep != na && pgc_setattr(&Card, oem, keep) == PGC_OK) na = keep;
      }
      fcb->Attr = na;
      fcb->Time = nt;
      fcb->Date = nd;
      SetSize(fcb, ns);
    }
    st = AttachFile(fo, fcb, sp, delete_on_close);
    if (!NT_SUCCESS(st)) {
      if (fcb->RefCount == 0) FreeFcb(fcb);
      goto out;
    }
    irp->IoStatus.Information = (disp == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
    NotifyItem(&path, FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    goto out;
  }

  /* does not exist */
  if (disp == FILE_OPEN || disp == FILE_OVERWRITE) {
    st = STATUS_OBJECT_NAME_NOT_FOUND;
    goto out;
  }
  if (fcb) {          /* gone from the card while open here: a new file */
    fcb->Deleted = TRUE;
    Unlist(fcb);
    fcb = NULL;
  }
  if (want_dir) {
    uint8_t na;
    uint32_t ns;
    uint16_t nt, nd;
    r = pgc_mkdir(&Card, oem);
    if (r == PGC_OK) r = pgc_getattr(&Card, oem, &na, &ns, &nt, &nd);
    attr = na;
    t = nt;
    d = nd;
    if (r != PGC_OK) {
      st = (r == PGC_EACCESS) ? STATUS_ACCESS_DENIED : PgcStatus(r);
      goto out;
    }
    fcb = NewFcb(&path, oem, FCB_DIR);
    if (!fcb) {
      st = STATUS_INSUFFICIENT_RESOURCES;
      goto out;
    }
    fcb->Attr = attr;
    fcb->Time = t;
    fcb->Date = d;
  } else {
    uint8_t na;
    uint32_t ns;
    uint16_t nt, nd;
    fcb = NewFcb(&path, oem, FCB_FILE);
    if (!fcb) {
      st = STATUS_INSUFFICIENT_RESOURCES;
      goto out;
    }
    r = pgc_open(&Card, &fcb->File, oem, PGC_CREATE_NEW, writer ? PGC_READWRITE : PGC_READ,
                 (uint8_t)(FatAttr(fattr) | PGC_ATTR_ARCHIVE), NULL, &na, &ns, &nt, &nd);
    if (r != PGC_OK) {
      FreeFcb(fcb);
      st = (r == PGC_EEXISTS) ? STATUS_OBJECT_NAME_COLLISION : PgcStatus(r);
      goto out;
    }
    fcb->Attr = na;
    fcb->Time = nt;
    fcb->Date = nd;
    SetSize(fcb, ns);
  }
  st = AttachFile(fo, fcb, sp, delete_on_close);
  if (!NT_SUCCESS(st)) {
    if (fcb->RefCount == 0) FreeFcb(fcb);   /* stays on the card, as with any failed create */
    goto out;
  }
  irp->IoStatus.Information = FILE_CREATED;
  NotifyItem(&path, want_dir ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME);

out:
  MediaChanged();
  if (path.Buffer) ExFreePool(path.Buffer);
  return st;
}

/* ---- cleanup and close ------------------------------------------------------------- */

static NTSTATUS Cleanup(PIO_STACK_LOCATION sp) {
  PFILE_OBJECT fo = sp->FileObject;
  PGFCB *fcb = (PGFCB *)fo->FsContext;
  PGCCB *ccb = (PGCCB *)fo->FsContext2;
  if (!fcb || !ccb) return STATUS_SUCCESS;
  NotifyCleanup(ccb);
  if (fcb->Kind == FCB_VOLUME) {
    fcb->OpenCount--;
    fo->Flags |= FO_CLEANUP_COMPLETE;
    return STATUS_SUCCESS;
  }
  IoRemoveShareAccess(fo, &fcb->Share);
  if (ccb->DeleteOnClose) fcb->DeletePending = TRUE;
  fcb->OpenCount--;
  if (fcb->OpenCount == 0) {
    if (fcb->DeletePending && !fcb->Deleted && fcb->MediaGen == Card.media_gen) {
      int r;
      if (fcb->Kind == FCB_FILE) {
        (void)pgc_unbind(&Card, &fcb->File);
        r = pgc_delete(&Card, fcb->Oem);
      } else {
        r = pgc_rmdir(&Card, fcb->Oem);
      }
      if (r == PGC_OK || r == PGC_ENOFILE) {
        fcb->Deleted = TRUE;
        Unlist(fcb);
        NotifyItem(&fcb->Name, fcb->Kind == FCB_FILE ? FILE_NOTIFY_CHANGE_FILE_NAME : FILE_NOTIFY_CHANGE_DIR_NAME);
      }
      fcb->DeletePending = FALSE;
    } else if (fcb->Kind == FCB_FILE) {
      /* give the card id back; paging I/O after this re-opens by path */
      (void)pgc_unbind(&Card, &fcb->File);
      if (fcb->Modified) {
        fcb->Modified = FALSE;
        NotifyItem(&fcb->Name, FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
      }
    }
  }
  fo->Flags |= FO_CLEANUP_COMPLETE;
  MediaChanged();
  return STATUS_SUCCESS;
}

static void FreeListing(PGCCB *ccb) {
  while (ccb->List) {
    LISTBLK *b = ccb->List;
    ccb->List = b->Next;
    ExFreePool(b);
  }
  ccb->ListCount = 0;
  ccb->CurBlk = NULL;
}

static NTSTATUS Close(PIO_STACK_LOCATION sp) {
  PFILE_OBJECT fo = sp->FileObject;
  PGFCB *fcb = (PGFCB *)fo->FsContext;
  PGCCB *ccb = (PGCCB *)fo->FsContext2;
  if (ccb) {
    FreeListing(ccb);
    if (ccb->Pattern.Buffer) RtlFreeUnicodeString(&ccb->Pattern);
    ExFreePool(ccb);
  }
  if (fcb) {
    fcb->RefCount--;
    if (fcb->RefCount == 0 && fcb != &VolumeFcb) FreeFcb(fcb);
  }
  return STATUS_SUCCESS;
}

/* ---- read and write ------------------------------------------------------------------ */

static PVOID IrpBuffer(PIRP irp) {
  if (irp->MdlAddress) return MdlAddress(irp->MdlAddress);
  return irp->UserBuffer;     /* kernel callers without an MDL */
}

static NTSTATUS Read(PIRP irp, PIO_STACK_LOCATION sp) {
  PFILE_OBJECT fo = sp->FileObject;
  PGFCB *fcb = (PGFCB *)fo->FsContext;
  ULONG len = sp->Parameters.Read.Length;
  LARGE_INTEGER off = sp->Parameters.Read.ByteOffset;
  BOOLEAN paging = (irp->Flags & IRP_PAGING_IO) != 0;
  uint32_t got = 0;
  PUCHAR buf;
  int r;

  if (!fcb || fcb->Kind != FCB_FILE) return STATUS_INVALID_DEVICE_REQUEST;
  if (sp->MinorFunction & IRP_MN_MDL) return STATUS_INVALID_DEVICE_REQUEST;
  if (off.LowPart == FILE_USE_FILE_POINTER_POSITION && off.HighPart == -1) off = fo->CurrentByteOffset;
  if (len == 0) return STATUS_SUCCESS;
  buf = (PUCHAR)IrpBuffer(irp);
  if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
  if (off.HighPart != 0) {
    r = PGC_OK;
  } else {
    ULONG want = len;
    if ((ULONGLONG)off.LowPart + want > 0xFFFFFFFFull) want = 0xFFFFFFFF - off.LowPart;
    r = pgc_read(&Card, &fcb->File, off.LowPart, buf, want, &got);
  }
  MediaChanged();
  if (r != PGC_OK && got == 0) return PgcStatus(r);
  if (paging) {
    /* a page past the end of the file reads as zeros */
    if (got < len) RtlZeroMemory(buf + got, len - got);
    irp->IoStatus.Information = len;
    return STATUS_SUCCESS;
  }
  if (got == 0) return STATUS_END_OF_FILE;
  irp->IoStatus.Information = got;
  if (fo->Flags & FO_SYNCHRONOUS_IO) fo->CurrentByteOffset.QuadPart = off.QuadPart + got;
  return STATUS_SUCCESS;
}

static NTSTATUS Write(PIRP irp, PIO_STACK_LOCATION sp) {
  PFILE_OBJECT fo = sp->FileObject;
  PGFCB *fcb = (PGFCB *)fo->FsContext;
  ULONG len = sp->Parameters.Write.Length;
  LARGE_INTEGER off = sp->Parameters.Write.ByteOffset;
  BOOLEAN paging = (irp->Flags & IRP_PAGING_IO) != 0;
  ULONG fsize;
  uint32_t done = 0;
  PUCHAR buf;
  int r;

  if (!fcb || fcb->Kind != FCB_FILE) return STATUS_INVALID_DEVICE_REQUEST;
  if (sp->MinorFunction & IRP_MN_MDL) return STATUS_INVALID_DEVICE_REQUEST;
  fsize = (ULONG)fcb->Header.FileSize.QuadPart;
  if (off.LowPart == FILE_WRITE_TO_END_OF_FILE && off.HighPart == -1) {
    off.QuadPart = fsize;
  } else if (off.LowPart == FILE_USE_FILE_POINTER_POSITION && off.HighPart == -1) {
    off = fo->CurrentByteOffset;
  }
  if (len == 0) return STATUS_SUCCESS;
  if (paging) {
    /* the memory manager writes whole pages; never past the end of file */
    if (off.QuadPart >= fsize) return STATUS_SUCCESS;
    if (off.QuadPart + len > fsize) len = (ULONG)(fsize - off.QuadPart);
  }
  if ((ULONGLONG)off.QuadPart + len > 0xFFFFFFFFull) return STATUS_DISK_FULL;   /* FAT: 4 GB - 1 */
  buf = (PUCHAR)IrpBuffer(irp);
  if (!buf) return STATUS_INSUFFICIENT_RESOURCES;
  pgc_file_need(&fcb->File, PGC_READWRITE);
  PushTime(FALSE);
  r = pgc_write(&Card, &fcb->File, off.LowPart, buf, len, &done);
  MediaChanged();
  if (done) {
    if (off.LowPart + done > fsize) SetSize(fcb, off.LowPart + done);
    fcb->Modified = TRUE;
    fcb->TimeStale = TRUE;
    fcb->Attr |= PGC_ATTR_ARCHIVE;
  }
  if (r != PGC_OK && done == 0) return (r == PGC_EACCESS) ? STATUS_DISK_FULL : PgcStatus(r);
  irp->IoStatus.Information = done;
  if (!paging && (fo->Flags & FO_SYNCHRONOUS_IO)) fo->CurrentByteOffset.QuadPart = off.QuadPart + done;
  /* mapped views of the file are now out of date */
  if (!paging && fcb->Section.DataSectionObject) {
    LARGE_INTEGER o;
    o.QuadPart = off.QuadPart;
    (void)CcPurgeCacheSection(&fcb->Section, &o, done, FALSE);
  }
  if (r != PGC_OK) return (r == PGC_EACCESS) ? STATUS_DISK_FULL : STATUS_SUCCESS;
  return STATUS_SUCCESS;
}

/* ---- query and set information --------------------------------------------------------- */

static void FillBasic(PGFCB *f, PFILE_BASIC_INFORMATION b) {
  LARGE_INTEGER t = NtTime(f->Time, f->Date);
  b->CreationTime = t;
  b->LastAccessTime = t;
  b->LastWriteTime = t;
  b->ChangeTime = t;
  if (f->Kind == FCB_ROOT || f->Kind == FCB_DIR) b->FileAttributes = NtAttr((UCHAR)(f->Attr | PGC_ATTR_DIR));
  else if (f->Kind == FCB_FILE) b->FileAttributes = NtAttr(f->Attr);
  else b->FileAttributes = FILE_ATTRIBUTE_NORMAL;
}

static void FillStandard(PGFCB *f, PFILE_STANDARD_INFORMATION s) {
  BOOLEAN dir = (f->Kind == FCB_ROOT || f->Kind == FCB_DIR);
  s->AllocationSize.QuadPart = dir ? 0 : f->Header.AllocationSize.QuadPart;
  s->EndOfFile.QuadPart = dir ? 0 : f->Header.FileSize.QuadPart;
  s->NumberOfLinks = 1;
  s->DeletePending = f->DeletePending;
  s->Directory = dir;
}

static ULONG NameHash(PCUNICODE_STRING n) {
  ULONG h = 2166136261u, i;
  for (i = 0; i < n->Length / 2; i++) {
    WCHAR c = RtlUpcaseUnicodeChar(n->Buffer[i]);
    h = (h ^ c) * 16777619u;
  }
  return h;
}

/* FILE_NAME_INFORMATION-style name into buf at off, clipped to len */
static NTSTATUS FillName(PGFCB *f, PUCHAR buf, ULONG len, ULONG off, ULONG *used) {
  PFILE_NAME_INFORMATION n = (PFILE_NAME_INFORMATION)(buf + off);
  ULONG fixed = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
  ULONG room, copy;
  if (len < off + fixed) return STATUS_BUFFER_OVERFLOW;
  room = len - off - fixed;
  n->FileNameLength = f->Name.Length;
  copy = f->Name.Length;
  if (copy > room) copy = room;
  RtlCopyMemory(n->FileName, f->Name.Buffer, copy);
  *used = off + fixed + copy;
  return (copy < f->Name.Length) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* the 8.3 name of the file: looked up in its directory listing */
static NTSTATUS AltName(PGFCB *f, PUCHAR buf, ULONG len, ULONG *used) {
  PFILE_NAME_INFORMATION n = (PFILE_NAME_INFORMATION)buf;
  ULONG fixed = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
  UNICODE_STRING parent, last;
  char poem[PGC_PATH_MAX];
  static pgc_find s;          /* under the lock */
  WCHAR w[PGC_NAME_MAX];
  int r;
  if (f->Kind == FCB_ROOT || f->Kind == FCB_VOLUME) return STATUS_OBJECT_NAME_NOT_FOUND;
  SplitPath(&f->Name, &parent, &last);
  if (!NT_SUCCESS(ToOem(&parent, poem))) return STATUS_OBJECT_NAME_NOT_FOUND;
  for (r = pgc_find_first(&Card, &s, poem, 0x16, 1); r == PGC_OK; r = pgc_find_next(&Card, &s)) {
    UNICODE_STRING ln, sn;
    WCHAR sw[13];
    ln.Buffer = w;
    ln.Length = ln.MaximumLength = FromOem(s.longname, w, sizeof(w));
    sn.Buffer = sw;
    sn.Length = sn.MaximumLength = FromOem(s.shortname, sw, sizeof(sw));
    if (UpcaseEqual(&ln, &last) || UpcaseEqual(&sn, &last)) {
      ULONG copy = sn.Length;
      if (len < fixed) return STATUS_BUFFER_OVERFLOW;
      n->FileNameLength = sn.Length;
      if (copy > len - fixed) copy = len - fixed;
      RtlCopyMemory(n->FileName, sw, copy);
      *used = fixed + copy;
      return (copy < sn.Length) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
  }
  return (r == PGC_ENOMORE) ? STATUS_OBJECT_NAME_NOT_FOUND : PgcStatus(r);
}

/* attributes and times of an open file or directory, from the card when the
 * card changed them (a write) */
static int Refresh(PGFCB *f) {
  UCHAR a;
  ULONG s;
  USHORT t, d;
  if (f->Kind != FCB_FILE && f->Kind != FCB_DIR) return PGC_OK;
  if (!f->TimeStale || f->Deleted || f->MediaGen != Card.media_gen) return PGC_OK;
  return Stat(f, f->Oem, &a, &s, &t, &d);
}

static NTSTATUS QueryInfo(PIRP irp, PIO_STACK_LOCATION sp) {
  PFILE_OBJECT fo = sp->FileObject;
  PGFCB *f = (PGFCB *)fo->FsContext;
  PUCHAR buf = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
  ULONG len = sp->Parameters.QueryFile.Length, used = 0;
  NTSTATUS st = STATUS_SUCCESS;
  if (!f) return STATUS_INVALID_PARAMETER;
  if (f->Kind == FCB_FILE && pgc_file_stale(&Card, &f->File)) return STATUS_FILE_INVALID;
  (void)Refresh(f);
  switch (sp->Parameters.QueryFile.FileInformationClass) {
  case FileBasicInformation:
    if (len < sizeof(FILE_BASIC_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    FillBasic(f, (PFILE_BASIC_INFORMATION)buf);
    used = sizeof(FILE_BASIC_INFORMATION);
    break;
  case FileStandardInformation:
    if (len < sizeof(FILE_STANDARD_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    FillStandard(f, (PFILE_STANDARD_INFORMATION)buf);
    used = sizeof(FILE_STANDARD_INFORMATION);
    break;
  case FileInternalInformation:
    if (len < sizeof(FILE_INTERNAL_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    ((PFILE_INTERNAL_INFORMATION)buf)->IndexNumber.QuadPart = NameHash(&f->Name);
    used = sizeof(FILE_INTERNAL_INFORMATION);
    break;
  case FileEaInformation:
    if (len < sizeof(FILE_EA_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    ((PFILE_EA_INFORMATION)buf)->EaSize = 0;
    used = sizeof(FILE_EA_INFORMATION);
    break;
  case FilePositionInformation:
    if (len < sizeof(FILE_POSITION_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    ((PFILE_POSITION_INFORMATION)buf)->CurrentByteOffset = fo->CurrentByteOffset;
    used = sizeof(FILE_POSITION_INFORMATION);
    break;
  case FileNameInformation:
    st = FillName(f, buf, len, 0, &used);
    break;
  case FileAlternateNameInformation:
    st = AltName(f, buf, len, &used);
    break;
  case FileAllInformation: {
    PFILE_ALL_INFORMATION a = (PFILE_ALL_INFORMATION)buf;
    /* the I/O manager fills in access, mode and alignment */
    if (len < FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName)) return STATUS_INFO_LENGTH_MISMATCH;
    FillBasic(f, &a->BasicInformation);
    FillStandard(f, &a->StandardInformation);
    a->InternalInformation.IndexNumber.QuadPart = NameHash(&f->Name);
    a->EaInformation.EaSize = 0;
    a->PositionInformation.CurrentByteOffset = fo->CurrentByteOffset;
    st = FillName(f, buf, len, FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation), &used);
    break;
  }
  case FileNetworkOpenInformation: {
    PFILE_NETWORK_OPEN_INFORMATION n = (PFILE_NETWORK_OPEN_INFORMATION)buf;
    FILE_BASIC_INFORMATION b;
    FILE_STANDARD_INFORMATION s;
    if (len < sizeof(*n)) return STATUS_INFO_LENGTH_MISMATCH;
    FillBasic(f, &b);
    FillStandard(f, &s);
    n->CreationTime = b.CreationTime;
    n->LastAccessTime = b.LastAccessTime;
    n->LastWriteTime = b.LastWriteTime;
    n->ChangeTime = b.ChangeTime;
    n->AllocationSize = s.AllocationSize;
    n->EndOfFile = s.EndOfFile;
    n->FileAttributes = b.FileAttributes;
    used = sizeof(*n);
    break;
  }
  case FileAttributeTagInformation: {
    PFILE_ATTRIBUTE_TAG_INFORMATION a = (PFILE_ATTRIBUTE_TAG_INFORMATION)buf;
    FILE_BASIC_INFORMATION b;
    if (len < sizeof(*a)) return STATUS_INFO_LENGTH_MISMATCH;
    FillBasic(f, &b);
    a->FileAttributes = b.FileAttributes;
    a->ReparseTag = 0;
    used = sizeof(*a);
    break;
  }
  default:
    return STATUS_INVALID_PARAMETER;   /* streams, compression, ...: what FAT says */
  }
  irp->IoStatus.Information = used;
  return st;
}

static BOOLEAN DirEmpty(const char *oem, int *err) {
  static pgc_find s;          /* under the lock */
  int r;
  for (r = pgc_find_first(&Card, &s, oem, 0x16, 0); r == PGC_OK; r = pgc_find_next(&Card, &s)) {
    if (pgc_strcmp(s.shortname, ".") != 0 && pgc_strcmp(s.shortname, "..") != 0) {
      *err = PGC_OK;
      return FALSE;
    }
  }
  *err = (r == PGC_ENOMORE) ? PGC_OK : r;
  return TRUE;
}

static NTSTATUS SetBasic(PGFCB *f, PFILE_BASIC_INFORMATION b) {
  int r;
  if (f->Kind == FCB_ROOT || f->Kind == FCB_VOLUME) return STATUS_SUCCESS;
  if (b->FileAttributes != 0) {
    /* FILE_ATTRIBUTE_NORMAL alone clears them all */
    UCHAR want = FatAttr(b->FileAttributes);
    UCHAR have = FatAttr(f->Attr);
    if (want != have) {
      r = pgc_setattr(&Card, f->Oem, want);
      if (r != PGC_OK) return PgcStatus(r);
      f->Attr = (UCHAR)((f->Attr & PGC_ATTR_DIR) | want);
      NotifyItem(&f->Name, FILE_NOTIFY_CHANGE_ATTRIBUTES);
    }
  }
  if (f->Kind == FCB_FILE && b->LastWriteTime.QuadPart != 0 && b->LastWriteTime.QuadPart != -1) {
    USHORT t, d;
    if (FatTime(b->LastWriteTime, &t, &d)) {
      r = pgc_set_ftime(&Card, &f->File, t, d);
      if (r != PGC_OK) return PgcStatus(r);
      f->Time = t;
      f->Date = d;
      f->TimeStale = FALSE;
      NotifyItem(&f->Name, FILE_NOTIFY_CHANGE_LAST_WRITE);
    }
  }
  /* directories keep their time; creation and access times are not stored */
  return STATUS_SUCCESS;
}

static NTSTATUS SetSizeInfo(PGFCB *f, LONGLONG size) {
  int r;
  if (f->Kind != FCB_FILE) return STATUS_INVALID_DEVICE_REQUEST;
  if (size < 0 || size > 0xFFFFFFFFll) return STATUS_DISK_FULL;
  if (size < f->Header.FileSize.QuadPart && !CanTruncate(f, size)) return STATUS_USER_MAPPED_FILE;
  pgc_file_need(&f->File, PGC_READWRITE);
  PushTime(FALSE);
  r = pgc_set_size(&Card, &f->File, (uint32_t)size);
  if (r != PGC_OK) return (r == PGC_EACCESS) ? STATUS_DISK_FULL : PgcStatus(r);
  SetSize(f, (ULONG)size);
  f->Modified = TRUE;
  f->TimeStale = TRUE;
  return STATUS_SUCCESS;
}

static NTSTATUS Rename(PGFCB *f, PIO_STACK_LOCATION sp, PFILE_RENAME_INFORMATION ri, ULONG len) {
  PFILE_OBJECT target = sp->Parameters.SetFile.FileObject;
  UNICODE_STRING parent, newpath, newname;
  char oemto[PGC_PATH_MAX];
  PGFCB *other;
  NTSTATUS st;
  int r;
  ULONG nl;

  if (f->Kind != FCB_FILE && f->Kind != FCB_DIR) return STATUS_INVALID_PARAMETER;
  if (len < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) ||
      len < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + ri->FileNameLength) return STATUS_INVALID_PARAMETER;
  if (target) {
    /* the I/O manager opened the target directory; its name is the final component */
    PGFCB *tdir = (PGFCB *)target->FsContext;
    parent = tdir->Name;
    newname = target->FileName;
  } else {
    USHORT i;
    SplitPath(&f->Name, &parent, NULL);
    newname.Buffer = ri->FileName;
    newname.Length = newname.MaximumLength = (USHORT)ri->FileNameLength;
    for (i = 0; i < newname.Length / 2; i++) {
      if (newname.Buffer[i] == L'\\') return STATUS_INVALID_PARAMETER;   /* relative renames stay in the directory */
    }
  }
  if (newname.Length == 0) return STATUS_OBJECT_NAME_INVALID;
  nl = (ULONG)parent.Length + 2 + newname.Length;
  if (nl > 0xFFF0) return STATUS_NAME_TOO_LONG;
  newpath.Buffer = (PWCH)AllocNp(nl + 2);
  if (!newpath.Buffer) return STATUS_INSUFFICIENT_RESOURCES;
  newpath.MaximumLength = (USHORT)(nl + 2);
  RtlCopyMemory(newpath.Buffer, parent.Buffer, parent.Length);
  newpath.Length = parent.Length;
  if (newpath.Length != 2) newpath.Buffer[newpath.Length / 2] = L'\\', newpath.Length += 2;
  RtlCopyMemory((PUCHAR)newpath.Buffer + newpath.Length, newname.Buffer, newname.Length);
  newpath.Length += newname.Length;

  st = CheckPath(&newpath);
  if (NT_SUCCESS(st)) st = ToOem(&newpath, oemto);
  if (!NT_SUCCESS(st)) goto out;
  if (f->Kind == FCB_DIR && IsAncestor(&f->Name, &newpath)) {
    st = STATUS_INVALID_PARAMETER;     /* into itself */
    goto out;
  }
  if (UpcaseEqual(&f->Name, &newpath) && pgc_strcmp(f->Oem, oemto) != 0) {
    /* only the case changes: FatFs renames onto itself fine */
  } else if (!UpcaseEqual(&f->Name, &newpath)) {
    uint8_t ta;
    r = pgc_getattr(&Card, oemto, &ta, NULL, NULL, NULL);
    if (r == PGC_OK) {
      if (!ri->ReplaceIfExists) {
        st = STATUS_OBJECT_NAME_COLLISION;
        goto out;
      }
      other = FindFcb(&newpath);
      if ((ta & PGC_ATTR_DIR) || (other && other->OpenCount > 0)) {
        st = STATUS_ACCESS_DENIED;
        goto out;
      }
      r = pgc_delete(&Card, oemto);
      if (r != PGC_OK) {
        st = PgcStatus(r);
        goto out;
      }
      if (other) {
        other->Deleted = TRUE;
        Unlist(other);
      }
    } else if (r != PGC_ENOFILE) {
      st = PgcStatus(r);
      goto out;
    }
  }
  /* an open card file must not be renamed under FatFs: its directory entry moves */
  if (f->Kind == FCB_FILE) (void)pgc_unbind(&Card, &f->File);
  r = pgc_rename(&Card, f->Oem, oemto);
  if (r != PGC_OK) {
    st = (r == PGC_ENOPATH) ? STATUS_OBJECT_PATH_NOT_FOUND : PgcStatus(r);
    goto out;
  }
  NotifyItem(&f->Name, f->Kind == FCB_DIR ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME);
  if (f->Kind == FCB_DIR) RenameChildren(&f->Name, &newpath);
  ExFreePool(f->Name.Buffer);
  f->Name = newpath;
  newpath.Buffer = NULL;
  RtlCopyMemory(f->Oem, oemto, pgc_strlen(oemto) + 1);
  if (f->Kind == FCB_FILE) (void)pgc_file_moved(&Card, &f->File, oemto);
  NotifyItem(&f->Name, f->Kind == FCB_DIR ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME);
  st = STATUS_SUCCESS;
out:
  if (newpath.Buffer) ExFreePool(newpath.Buffer);
  return st;
}

static NTSTATUS SetInfo(PIRP irp, PIO_STACK_LOCATION sp) {
  PFILE_OBJECT fo = sp->FileObject;
  PGFCB *f = (PGFCB *)fo->FsContext;
  PVOID buf = irp->AssociatedIrp.SystemBuffer;
  ULONG len = sp->Parameters.SetFile.Length;
  if (!f || f->Kind == FCB_VOLUME) return STATUS_INVALID_PARAMETER;
  if (f->MediaGen != Card.media_gen || (f->Kind == FCB_FILE && pgc_file_stale(&Card, &f->File))) return STATUS_FILE_INVALID;
  if (f->Deleted) return STATUS_FILE_DELETED;
  switch (sp->Parameters.SetFile.FileInformationClass) {
  case FileBasicInformation:
    if (len < sizeof(FILE_BASIC_INFORMATION)) return STATUS_INVALID_PARAMETER;
    return SetBasic(f, (PFILE_BASIC_INFORMATION)buf);
  case FileDispositionInformation:
    if (len < sizeof(FILE_DISPOSITION_INFORMATION)) return STATUS_INVALID_PARAMETER;
    if (((PFILE_DISPOSITION_INFORMATION)buf)->DeleteFile) {
      if (f->Kind == FCB_ROOT) return STATUS_CANNOT_DELETE;
      if (f->Attr & PGC_ATTR_RDONLY) return STATUS_CANNOT_DELETE;
      if (f->Kind == FCB_DIR) {
        int err;
        if (!DirEmpty(f->Oem, &err)) return STATUS_DIRECTORY_NOT_EMPTY;
        if (err != PGC_OK) return PgcStatus(err);
      } else if (!FlushImage(f, MmFlushForDelete)) {
        return STATUS_CANNOT_DELETE;
      }
      f->DeletePending = TRUE;
      fo->DeletePending = TRUE;
    } else {
      f->DeletePending = FALSE;
      fo->DeletePending = FALSE;
    }
    return STATUS_SUCCESS;
  case FileRenameInformation:
    if (f->Kind == FCB_ROOT) return STATUS_ACCESS_DENIED;
    return Rename(f, sp, (PFILE_RENAME_INFORMATION)buf, len);
  case FileEndOfFileInformation:
    if (len < sizeof(FILE_END_OF_FILE_INFORMATION)) return STATUS_INVALID_PARAMETER;
    if (f->Kind == FCB_FILE && sp->Parameters.SetFile.AdvanceOnly) {
      /* from a cache manager: only ever moves the valid data length up */
      return STATUS_SUCCESS;
    }
    return SetSizeInfo(f, ((PFILE_END_OF_FILE_INFORMATION)buf)->EndOfFile.QuadPart);
  case FileAllocationInformation:
    if (len < sizeof(FILE_ALLOCATION_INFORMATION)) return STATUS_INVALID_PARAMETER;
    if (f->Kind != FCB_FILE) return STATUS_INVALID_DEVICE_REQUEST;
    /* FAT allocates as it writes; only a cut below the size matters */
    if (((PFILE_ALLOCATION_INFORMATION)buf)->AllocationSize.QuadPart < f->Header.FileSize.QuadPart)
      return SetSizeInfo(f, ((PFILE_ALLOCATION_INFORMATION)buf)->AllocationSize.QuadPart);
    return STATUS_SUCCESS;
  case FilePositionInformation:
    if (len < sizeof(FILE_POSITION_INFORMATION)) return STATUS_INVALID_PARAMETER;
    fo->CurrentByteOffset = ((PFILE_POSITION_INFORMATION)buf)->CurrentByteOffset;
    return STATUS_SUCCESS;
  default:
    return STATUS_INVALID_PARAMETER;
  }
}

/* ---- volume information ------------------------------------------------------------ */

static NTSTATUS QueryVolume(PIRP irp, PIO_STACK_LOCATION sp) {
  PUCHAR buf = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
  ULONG len = sp->Parameters.QueryVolume.Length, used = 0;
  NTSTATUS st = STATUS_SUCCESS;
  pgc_volinfo v;
  int r;
  FS_INFORMATION_CLASS cls = sp->Parameters.QueryVolume.FsInformationClass;

  if (cls == FileFsDeviceInformation) {
    PFILE_FS_DEVICE_INFORMATION d = (PFILE_FS_DEVICE_INFORMATION)buf;
    if (len < sizeof(*d)) return STATUS_INFO_LENGTH_MISMATCH;
    d->DeviceType = FILE_DEVICE_DISK;
    d->Characteristics = FILE_REMOVABLE_MEDIA | (Card.present ? FILE_DEVICE_IS_MOUNTED : 0);
    irp->IoStatus.Information = sizeof(*d);
    return STATUS_SUCCESS;
  }
  r = pgc_volinfo_get(&Card, &v);
  MediaChanged();
  if (r != PGC_OK) return PgcStatus(r);
  switch (cls) {
  case FileFsVolumeInformation: {
    PFILE_FS_VOLUME_INFORMATION vi = (PFILE_FS_VOLUME_INFORMATION)buf;
    ULONG fixed = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
    WCHAR w[16];
    USHORT n = FromOem(v.label, w, sizeof(w));
    ULONG copy = n;
    if (len < fixed) return STATUS_INFO_LENGTH_MISMATCH;
    vi->VolumeCreationTime.QuadPart = 0;
    vi->VolumeSerialNumber = v.serial;
    vi->VolumeLabelLength = n;
    vi->SupportsObjects = FALSE;
    if (copy > len - fixed) copy = len - fixed, st = STATUS_BUFFER_OVERFLOW;
    RtlCopyMemory(vi->VolumeLabel, w, copy);
    used = fixed + copy;
    break;
  }
  case FileFsSizeInformation:
  case FileFsFullSizeInformation: {
    uint32_t total, freec, cb;
    r = pgc_space(&Card, &total, &freec, &cb);
    MediaChanged();
    if (r != PGC_OK) return PgcStatus(r);
    if (cls == FileFsSizeInformation) {
      PFILE_FS_SIZE_INFORMATION s = (PFILE_FS_SIZE_INFORMATION)buf;
      if (len < sizeof(*s)) return STATUS_INFO_LENGTH_MISMATCH;
      s->TotalAllocationUnits.QuadPart = total;
      s->AvailableAllocationUnits.QuadPart = freec;
      s->SectorsPerAllocationUnit = cb / 512;
      s->BytesPerSector = 512;
      used = sizeof(*s);
    } else {
      PFILE_FS_FULL_SIZE_INFORMATION s = (PFILE_FS_FULL_SIZE_INFORMATION)buf;
      if (len < sizeof(*s)) return STATUS_INFO_LENGTH_MISMATCH;
      s->TotalAllocationUnits.QuadPart = total;
      s->CallerAvailableAllocationUnits.QuadPart = freec;
      s->ActualAvailableAllocationUnits.QuadPart = freec;
      s->SectorsPerAllocationUnit = cb / 512;
      s->BytesPerSector = 512;
      used = sizeof(*s);
    }
    break;
  }
  case FileFsAttributeInformation: {
    PFILE_FS_ATTRIBUTE_INFORMATION a = (PFILE_FS_ATTRIBUTE_INFORMATION)buf;
    ULONG fixed = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
    const WCHAR *name = L"FAT";
    ULONG n, copy;
    if (pgc_strcmp(v.fs, "FAT32") == 0) name = L"FAT32";
    else if (pgc_strcmp(v.fs, "EXFAT") == 0) name = L"exFAT";
    for (n = 0; name[n]; n++) { }
    n *= 2;
    if (len < fixed) return STATUS_INFO_LENGTH_MISMATCH;
    a->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES;
    a->MaximumComponentNameLength = 127;    /* the card's FatFs (FF_MAX_LFN) */
    a->FileSystemNameLength = n;
    copy = n;
    if (copy > len - fixed) copy = len - fixed, st = STATUS_BUFFER_OVERFLOW;
    RtlCopyMemory(a->FileSystemName, name, copy);
    used = fixed + copy;
    break;
  }
  default:
    return STATUS_INVALID_PARAMETER;
  }
  irp->IoStatus.Information = used;
  return st;
}

/* ---- directory listing ------------------------------------------------------------- */

static NTSTATUS AddEntry(PGCCB *ccb, const pgc_find *s) {
  WCHAR lw[PGC_NAME_MAX], sw[13];
  USHORT ll = FromOem(s->longname, lw, sizeof(lw));
  USHORT sl = FromOem(s->shortname, sw, sizeof(sw));
  ULONG bytes = (sizeof(DENT) + ll + sl + 3) & ~3u;
  LISTBLK *b = ccb->List;
  DENT *e;
  if (ll == 0) return STATUS_SUCCESS;    /* a name the code page cannot show */
  while (b && b->Next) b = b->Next;
  if (!b || b->Used + bytes > LIST_BLOCK) {
    LISTBLK *nb = (LISTBLK *)AllocPaged(sizeof(LISTBLK));
    if (!nb) return STATUS_INSUFFICIENT_RESOURCES;
    nb->Next = NULL;
    nb->Used = 0;
    if (b) b->Next = nb;
    else ccb->List = nb;
    b = nb;
  }
  e = (DENT *)(b->Data + b->Used);
  e->Bytes = (USHORT)bytes;
  e->Attr = s->attr;
  e->Pad = 0;
  e->Size = s->size;
  e->Time = s->time;
  e->Date = s->date;
  e->LongLen = ll;
  /* a short name only when it differs from the long one, as FAT reports it */
  e->ShortLen = (sl == ll && RtlCompareMemory(lw, sw, ll) == ll) ? 0 : sl;
  RtlCopyMemory((PUCHAR)(e + 1), lw, ll);
  RtlCopyMemory((PUCHAR)(e + 1) + ll, sw, e->ShortLen);
  b->Used += bytes;
  ccb->ListCount++;
  return STATUS_SUCCESS;
}

static NTSTATUS LoadListing(PGFCB *fcb, PGCCB *ccb) {
  static pgc_find s;          /* under the lock */
  NTSTATUS st = STATUS_SUCCESS;
  int r;
  FreeListing(ccb);
  for (r = pgc_find_first(&Card, &s, fcb->Kind == FCB_ROOT ? "" : fcb->Oem, 0x16, 1); r == PGC_OK;
       r = pgc_find_next(&Card, &s)) {
    st = AddEntry(ccb, &s);
    if (!NT_SUCCESS(st)) break;
  }
  MediaChanged();
  if (NT_SUCCESS(st) && r != PGC_ENOMORE) st = PgcStatus(r);
  if (!NT_SUCCESS(st)) FreeListing(ccb);
  ccb->CurBlk = ccb->List;
  ccb->CurOff = 0;
  ccb->Index = 0;
  return st;
}

static DENT *CurEntry(PGCCB *ccb) {
  while (ccb->CurBlk && ccb->CurOff >= ccb->CurBlk->Used) {
    ccb->CurBlk = ccb->CurBlk->Next;
    ccb->CurOff = 0;
  }
  return ccb->CurBlk ? (DENT *)(ccb->CurBlk->Data + ccb->CurOff) : NULL;
}

static void NextEntry(PGCCB *ccb, DENT *e) {
  ccb->CurOff += e->Bytes;
  ccb->Index++;
}

static void SeekEntry(PGCCB *ccb, ULONG index) {
  DENT *e;
  ccb->CurBlk = ccb->List;
  ccb->CurOff = 0;
  ccb->Index = 0;
  while (ccb->Index < index && (e = CurEntry(ccb)) != NULL) NextEntry(ccb, e);
}

static BOOLEAN EntryMatches(PGCCB *ccb, PUNICODE_STRING ln, PUNICODE_STRING sn) {
  if (ccb->Pattern.Length == 0) return TRUE;
  if (ccb->PatternWild) {
    return FsRtlIsNameInExpression(&ccb->Pattern, ln, TRUE, NULL) ||
           (sn->Length && FsRtlIsNameInExpression(&ccb->Pattern, sn, TRUE, NULL));
  }
  return UpcaseEqual(&ccb->Pattern, ln) || (sn->Length && UpcaseEqual(&ccb->Pattern, sn));
}

/* fixed part size and name offset of the directory information classes */
static BOOLEAN ClassLayout(FILE_INFORMATION_CLASS cls, ULONG *fixed) {
  switch (cls) {
  case FileDirectoryInformation:     *fixed = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName); return TRUE;
  case FileFullDirectoryInformation: *fixed = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName); return TRUE;
  case FileBothDirectoryInformation: *fixed = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName); return TRUE;
  case FileNamesInformation:         *fixed = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName); return TRUE;
  case FileIdBothDirectoryInformation: *fixed = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName); return TRUE;
  case FileIdFullDirectoryInformation: *fixed = FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName); return TRUE;
  default: return FALSE;
  }
}

static void FillEntry(FILE_INFORMATION_CLASS cls, PUCHAR p, DENT *e, PUNICODE_STRING ln, PUNICODE_STRING sn,
                      ULONG index, ULONG namebytes, PGFCB *dir) {
  LARGE_INTEGER t = NtTime(e->Time, e->Date);
  LARGE_INTEGER eof, alloc, id;
  ULONG attr = NtAttr(e->Attr);
  BOOLEAN isdir = (e->Attr & PGC_ATTR_DIR) != 0;
  eof.QuadPart = isdir ? 0 : e->Size;
  alloc.QuadPart = isdir ? 0 : AllocSize(e->Size);
  {
    /* the same number FileInternalInformation gives for the path */
    UNICODE_STRING full;
    WCHAR tmp[PGC_PATH_MAX + PGC_NAME_MAX];
    ULONG n = 0;
    if ((ULONG)dir->Name.Length + 2 + ln->Length <= sizeof(tmp)) {
      RtlCopyMemory(tmp, dir->Name.Buffer, dir->Name.Length);
      n = dir->Name.Length;
      if (n != 2) tmp[n / 2] = L'\\', n += 2;
      RtlCopyMemory((PUCHAR)tmp + n, ln->Buffer, ln->Length);
      n += ln->Length;
    }
    full.Buffer = tmp;
    full.Length = full.MaximumLength = (USHORT)n;
    id.QuadPart = NameHash(&full);
  }
  switch (cls) {
  case FileNamesInformation: {
    PFILE_NAMES_INFORMATION i = (PFILE_NAMES_INFORMATION)p;
    i->FileIndex = index;
    i->FileNameLength = ln->Length;
    RtlCopyMemory(i->FileName, ln->Buffer, namebytes);
    return;
  }
  case FileDirectoryInformation: {
    PFILE_DIRECTORY_INFORMATION i = (PFILE_DIRECTORY_INFORMATION)p;
    i->FileIndex = index;
    i->CreationTime = i->LastAccessTime = i->LastWriteTime = i->ChangeTime = t;
    i->EndOfFile = eof;
    i->AllocationSize = alloc;
    i->FileAttributes = attr;
    i->FileNameLength = ln->Length;
    RtlCopyMemory(i->FileName, ln->Buffer, namebytes);
    return;
  }
  case FileFullDirectoryInformation:
  case FileIdFullDirectoryInformation: {
    PFILE_FULL_DIR_INFORMATION i = (PFILE_FULL_DIR_INFORMATION)p;
    i->FileIndex = index;
    i->CreationTime = i->LastAccessTime = i->LastWriteTime = i->ChangeTime = t;
    i->EndOfFile = eof;
    i->AllocationSize = alloc;
    i->FileAttributes = attr;
    i->FileNameLength = ln->Length;
    i->EaSize = 0;
    if (cls == FileIdFullDirectoryInformation) {
      PFILE_ID_FULL_DIR_INFORMATION j = (PFILE_ID_FULL_DIR_INFORMATION)p;
      j->FileId = id;
      RtlCopyMemory(j->FileName, ln->Buffer, namebytes);
    } else {
      RtlCopyMemory(i->FileName, ln->Buffer, namebytes);
    }
    return;
  }
  case FileBothDirectoryInformation:
  case FileIdBothDirectoryInformation: {
    PFILE_BOTH_DIR_INFORMATION i = (PFILE_BOTH_DIR_INFORMATION)p;
    i->FileIndex = index;
    i->CreationTime = i->LastAccessTime = i->LastWriteTime = i->ChangeTime = t;
    i->EndOfFile = eof;
    i->AllocationSize = alloc;
    i->FileAttributes = attr;
    i->FileNameLength = ln->Length;
    i->EaSize = 0;
    i->ShortNameLength = (CCHAR)(sn->Length > 24 ? 24 : sn->Length);
    RtlZeroMemory(i->ShortName, sizeof(i->ShortName));
    RtlCopyMemory(i->ShortName, sn->Buffer, i->ShortNameLength);
    if (cls == FileIdBothDirectoryInformation) {
      PFILE_ID_BOTH_DIR_INFORMATION j = (PFILE_ID_BOTH_DIR_INFORMATION)p;
      j->FileId = id;
      RtlCopyMemory(j->FileName, ln->Buffer, namebytes);
    } else {
      RtlCopyMemory(i->FileName, ln->Buffer, namebytes);
    }
    return;
  }
  default:
    return;
  }
}

static NTSTATUS QueryDirectory(PIRP irp, PIO_STACK_LOCATION sp, PGFCB *fcb, PGCCB *ccb) {
  ULONG len = sp->Parameters.QueryDirectory.Length;
  FILE_INFORMATION_CLASS cls = sp->Parameters.QueryDirectory.FileInformationClass;
  PUNICODE_STRING pattern = sp->Parameters.QueryDirectory.FileName;
  BOOLEAN restart = (sp->Flags & SL_RESTART_SCAN) != 0;
  BOOLEAN single = (sp->Flags & SL_RETURN_SINGLE_ENTRY) != 0;
  ULONG fixed, off = 0, last = 0;
  BOOLEAN any = FALSE;
  NTSTATUS st = STATUS_SUCCESS;
  PUCHAR buf;
  DENT *e;

  if (fcb->Kind != FCB_DIR && fcb->Kind != FCB_ROOT) return STATUS_INVALID_PARAMETER;
  if (!ClassLayout(cls, &fixed)) return STATUS_INVALID_INFO_CLASS;
  buf = (PUCHAR)IrpBuffer(irp);
  if (!buf) return STATUS_INSUFFICIENT_RESOURCES;

  if (!ccb->Started || restart) {
    if (pattern && pattern->Length && (!ccb->Started || restart)) {
      if (ccb->Pattern.Buffer) RtlFreeUnicodeString(&ccb->Pattern);
      RtlZeroMemory(&ccb->Pattern, sizeof(ccb->Pattern));
      st = RtlUpcaseUnicodeString(&ccb->Pattern, pattern, TRUE);
      if (!NT_SUCCESS(st)) return st;
      ccb->PatternWild = FsRtlDoesNameContainWildCards(&ccb->Pattern);
      if (ccb->Pattern.Length == 2 && ccb->Pattern.Buffer[0] == L'*') {
        RtlFreeUnicodeString(&ccb->Pattern);   /* "*": everything */
        RtlZeroMemory(&ccb->Pattern, sizeof(ccb->Pattern));
      }
    }
    st = LoadListing(fcb, ccb);
    if (!NT_SUCCESS(st)) return st;
    ccb->Started = TRUE;
    ccb->ReturnedAny = FALSE;
  }
  if (sp->Flags & SL_INDEX_SPECIFIED) SeekEntry(ccb, sp->Parameters.QueryDirectory.FileIndex + 1);

  while ((e = CurEntry(ccb)) != NULL) {
    UNICODE_STRING ln, sn;
    ULONG need, at;
    ln.Buffer = (PWCH)(e + 1);
    ln.Length = ln.MaximumLength = e->LongLen;
    sn.Buffer = (PWCH)((PUCHAR)(e + 1) + e->LongLen);
    sn.Length = sn.MaximumLength = e->ShortLen;
    if (!EntryMatches(ccb, &ln, &sn)) {
      NextEntry(ccb, e);
      continue;
    }
    at = any ? ((off + 7) & ~7u) : 0;
    need = fixed + ln.Length;
    if (at + need > len) {
      if (!any) {
        /* not even one fits: as much as there is room for */
        if (len < fixed) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(buf, fixed);
        FillEntry(cls, buf, e, &ln, &sn, ccb->Index, len - fixed, fcb);
        irp->IoStatus.Information = len;
        return STATUS_BUFFER_OVERFLOW;
      }
      break;
    }
    if (any) *(PULONG)(buf + last) = at - last;      /* NextEntryOffset of the previous one */
    RtlZeroMemory(buf + at, fixed);
    FillEntry(cls, buf + at, e, &ln, &sn, ccb->Index, ln.Length, fcb);
    last = at;
    off = at + need;
    any = TRUE;
    NextEntry(ccb, e);
    if (single) break;
  }
  if (!any) {
    st = ccb->ReturnedAny ? STATUS_NO_MORE_FILES : STATUS_NO_SUCH_FILE;
    irp->IoStatus.Information = 0;
    return st;
  }
  ccb->ReturnedAny = TRUE;
  irp->IoStatus.Information = off;
  return STATUS_SUCCESS;
}

/* ---- file system and device control --------------------------------------------------- */

static NTSTATUS FsControl(PIRP irp, PIO_STACK_LOCATION sp) {
  (void)irp;
  if (sp->MinorFunction != IRP_MN_USER_FS_REQUEST) return STATUS_INVALID_DEVICE_REQUEST;
  switch (sp->Parameters.FileSystemControl.FsControlCode) {
  case FSCTL_IS_VOLUME_MOUNTED: {
    int r = pgc_media(&Card, 1);
    MediaChanged();
    return (r == PGC_OK) ? STATUS_SUCCESS : PgcStatus(r);
  }
  case FSCTL_LOCK_VOLUME:
  case FSCTL_UNLOCK_VOLUME:
  case FSCTL_DISMOUNT_VOLUME:
    return STATUS_SUCCESS;        /* nothing is cached here */
  default:
    return STATUS_INVALID_DEVICE_REQUEST;
  }
}

static NTSTATUS DeviceControl(PIRP irp, PIO_STACK_LOCATION sp) {
  (void)irp;
  switch (sp->Parameters.DeviceIoControl.IoControlCode) {
  case PG_IOCTL_STORAGE_CHECK_VERIFY:
  case PG_IOCTL_STORAGE_CHECK_VERIFY2:
  case PG_IOCTL_DISK_CHECK_VERIFY: {
    int r = pgc_media(&Card, 1);
    MediaChanged();
    return (r == PGC_OK) ? STATUS_SUCCESS : STATUS_NO_MEDIA_IN_DEVICE;
  }
  case PG_IOCTL_DISK_IS_WRITABLE: {
    int r = pgc_media(&Card, 0);
    return (r == PGC_OK) ? STATUS_SUCCESS : STATUS_NO_MEDIA_IN_DEVICE;
  }
  default:
    return STATUS_INVALID_DEVICE_REQUEST;
  }
}

/* ---- dispatch ------------------------------------------------------------------------- */

static NTSTATUS NTAPI Dispatch(PDEVICE_OBJECT dev, PIRP irp) {
  PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
  PFILE_OBJECT fo = sp->FileObject;
  PGFCB *fcb = fo ? (PGFCB *)fo->FsContext : NULL;
  PGCCB *ccb = fo ? (PGCCB *)fo->FsContext2 : NULL;
  NTSTATUS st;
  (void)dev;

  irp->IoStatus.Information = 0;
  VcbLock();
  switch (sp->MajorFunction) {
  case IRP_MJ_CREATE:
    st = Create(irp, sp);
    break;
  case IRP_MJ_CLEANUP:
    st = Cleanup(sp);
    break;
  case IRP_MJ_CLOSE:
    st = Close(sp);
    break;
  case IRP_MJ_READ:
    st = Read(irp, sp);
    break;
  case IRP_MJ_WRITE:
    st = Write(irp, sp);
    break;
  case IRP_MJ_QUERY_INFORMATION:
    st = QueryInfo(irp, sp);
    break;
  case IRP_MJ_SET_INFORMATION:
    PushTime(FALSE);
    st = SetInfo(irp, sp);
    break;
  case IRP_MJ_QUERY_VOLUME_INFORMATION:
    st = QueryVolume(irp, sp);
    break;
  case IRP_MJ_DIRECTORY_CONTROL:
    if (!fcb || !ccb) {
      st = STATUS_INVALID_PARAMETER;
    } else if (sp->MinorFunction == IRP_MN_QUERY_DIRECTORY) {
      st = QueryDirectory(irp, sp, fcb, ccb);
    } else if (sp->MinorFunction == IRP_MN_NOTIFY_CHANGE_DIRECTORY) {
      st = NotifyChangeDirectory(irp, sp, fcb, ccb);
      VcbUnlock();
      return st;                  /* pending, or completed already */
    } else {
      st = STATUS_INVALID_DEVICE_REQUEST;
    }
    break;
  case IRP_MJ_FILE_SYSTEM_CONTROL:
    st = FsControl(irp, sp);
    break;
  case IRP_MJ_DEVICE_CONTROL:
    st = DeviceControl(irp, sp);
    break;
  case IRP_MJ_FLUSH_BUFFERS:
  case IRP_MJ_LOCK_CONTROL:       /* no locking, as in EtherDFS */
  case IRP_MJ_SHUTDOWN:
    st = STATUS_SUCCESS;          /* the card writes every request through */
    break;
  case IRP_MJ_QUERY_EA:
  case IRP_MJ_SET_EA:
    st = STATUS_EAS_NOT_SUPPORTED;
    break;
  case IRP_MJ_SET_VOLUME_INFORMATION:
    st = STATUS_MEDIA_WRITE_PROTECTED;   /* the label is the card's */
    break;
  default:
    st = STATUS_INVALID_DEVICE_REQUEST;
    break;
  }
  VcbUnlock();
  irp->IoStatus.Status = st;
  IoCompleteRequest(irp, NT_SUCCESS(st) ? IO_DISK_INCREMENT : IO_NO_INCREMENT);
  return st;
}

/* ---- media poll thread ----------------------------------------------------------------- */

static VOID NTAPI PollThread(PVOID ctx) {
  LARGE_INTEGER d;
  (void)ctx;
  d.QuadPart = -10000LL * POLL_MS;
  for (;;) {
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    VcbLock();
    (void)pgc_media(&Card, 1);
    if (Card.present) PushTime(FALSE);
    MediaChanged();
    VcbUnlock();
  }
}

/* ---- driver entry ---------------------------------------------------------------------- */

static WCHAR ReadLetter(PUNICODE_STRING regpath) {
  RTL_QUERY_REGISTRY_TABLE q[2];
  UNICODE_STRING val, path;
  WCHAR vbuf[8], letter = L'P';
  NTSTATUS st;
  static const WCHAR sub[] = L"\\Parameters";
  path.MaximumLength = (USHORT)(regpath->Length + sizeof(sub));
  path.Buffer = (PWCH)AllocNp(path.MaximumLength);
  if (!path.Buffer) return letter;
  RtlCopyMemory(path.Buffer, regpath->Buffer, regpath->Length);
  RtlCopyMemory((PUCHAR)path.Buffer + regpath->Length, sub, sizeof(sub));
  path.Length = (USHORT)(regpath->Length + sizeof(sub) - sizeof(WCHAR));
  RtlZeroMemory(q, sizeof(q));
  RtlZeroMemory(vbuf, sizeof(vbuf));
  val.Buffer = vbuf;
  val.Length = 0;
  val.MaximumLength = sizeof(vbuf) - sizeof(WCHAR);
  q[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
  q[0].Name = (PWSTR)L"DriveLetter";
  q[0].EntryContext = &val;
  st = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path.Buffer, q, NULL, NULL);
  if (NT_SUCCESS(st) && val.Length >= 2) {
    WCHAR c = vbuf[0];
    if (c >= L'a' && c <= L'z') c = (WCHAR)(c - 32);
    if (c >= L'C' && c <= L'Z') letter = c;
  }
  ExFreePool(path.Buffer);
  return letter;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regpath) {
  UNICODE_STRING devname, linkname;
  WCHAR link[] = L"\\DosDevices\\P:";
  PDEVICE_OBJECT dev;
  HANDLE th;
  NTSTATUS st;
  int i, r;

  DBG_PRINT(("pgdfs: PicoGUS PGDFS driver " PGDFS_VERSION "\n"));
  RtlZeroMemory(&Vcb, sizeof(Vcb));
  KeInitializeMutex(&Vcb.Lock, 0);
  InitializeListHead(&Vcb.FcbList);
  KeInitializeSpinLock(&Vcb.NotifyLock);
  InitializeListHead(&Vcb.NotifyList);
  KeQueryPerformanceCounter(&Vcb.PerfFreq);
  VolumeFcb.Kind = FCB_VOLUME;

  r = pgc_probe(&Card);
  if (r != PGC_OK) {
    DBG_PRINT(("pgdfs: no PicoGUS with PGDFS enabled found\n"));
    return STATUS_NO_SUCH_DEVICE;
  }
  DBG_PRINT(("pgdfs: card protocol %u, data port %Xh, frame payload %u, drive %s\n", Card.proto, Card.port,
             Card.maxpl, Card.present ? Card.info : "(none)"));
  PushTime(TRUE);
  Vcb.ChangeCount = Card.change_count;

  RtlInitUnicodeString(&devname, DEVICE_NAME);
  st = IoCreateDevice(drv, 0, &devname, FILE_DEVICE_DISK_FILE_SYSTEM, FILE_REMOVABLE_MEDIA, FALSE, &dev);
  if (!NT_SUCCESS(st)) return st;
  dev->Flags |= DO_DIRECT_IO;
  Vcb.Device = dev;
  Vcb.Letter = ReadLetter(regpath);
  link[12] = Vcb.Letter;
  RtlInitUnicodeString(&linkname, link);
  st = IoCreateSymbolicLink(&linkname, &devname);
  if (!NT_SUCCESS(st)) {
    DBG_PRINT(("pgdfs: drive letter %c: is taken (%08lX)\n", (char)Vcb.Letter, st));
    IoDeleteDevice(dev);
    return st;
  }
  for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = Dispatch;
  drv->DriverUnload = NULL;       /* a file system stays */
  dev->Flags &= ~DO_DEVICE_INITIALIZING;

  st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, NULL);
  if (NT_SUCCESS(st)) ZwClose(th);
  DBG_PRINT(("pgdfs: %c: ready\n", (char)Vcb.Letter));
  return STATUS_SUCCESS;
}
