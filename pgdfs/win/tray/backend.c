/*
 * PGTRAY: the ways to the card
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * With a PGDFS driver loaded the control requests go through it: it owns the
 * control port, and a request of ours must not get between the register
 * select and the data access of one of its transactions (pgctl.h). Without
 * the driver, Windows 9x lets a program use I/O ports no VxD has claimed, so
 * the request is run here; Windows NT does not, and there is no way then.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PICOGUS_NO_MODENAMES
#include "../../../common/picogus.h"
#include "pgcard.h"
#include "backend.h"

static HANDLE device = INVALID_HANDLE_VALUE;
static int kind = BACKEND_NONE;

static int is_9x(void) {
  return (GetVersion() & 0x80000000UL) != 0;
}

int backend_kind(void) {
  if (kind != BACKEND_NONE) return kind;
  if (is_9x()) {
    /* a static VxD is opened by its name; the old PGDFS.VXD without control
     * requests refuses the open */
    device = CreateFileA(PGCTL_DEVICE_9X, 0, 0, NULL, 0, 0, NULL);
    kind = device != INVALID_HANDLE_VALUE ? BACKEND_VXD : BACKEND_PORTS;
  } else {
    device = CreateFileA(PGCTL_DEVICE_NT, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (device != INVALID_HANDLE_VALUE) kind = BACKEND_NT;
  }
  return kind;
}

const char *backend_name(void) {
  switch (backend_kind()) {
  case BACKEND_NT:    return "the PGDFS driver (pgdfs.sys)";
  case BACKEND_VXD:   return "the PGDFS driver (PGDFS.VXD)";
  case BACKEND_PORTS: return "its I/O ports (no PGDFS driver loaded)";
  default:            return "nothing: pgdfs.sys is not running";
  }
}

/* ---- the request on the ports themselves (pgc_ctl() of ../common/pgc.c) ----------- */

static void outb(unsigned short port, unsigned char v) {
  __asm__ volatile("outb %b0, %w1" : : "a"(v), "d"(port));
}

static unsigned char inb(unsigned short port) {
  unsigned char v;
  __asm__ volatile("inb %w1, %b0" : "=a"(v) : "d"(port));
  return v;
}

static void on_ports(const pgctl_req *rq, const unsigned char *wr, unsigned char *rd, unsigned *got) {
  unsigned long k;
  unsigned i, n = 0;
  outb(CONTROL_PORT, 0xCC);
  outb(CONTROL_PORT, rq->reg);
  for (i = 0; i < rq->nwrite; i++) {
    outb(((rq->flags & PGCTL_WORD) && i == 0) ? DATA_PORT_LOW : DATA_PORT_HIGH, wr[i]);
  }
  for (k = 0; k < rq->skip; k++) {
    if (inb(DATA_PORT_HIGH) == rq->stop && (rq->flags & PGCTL_STOP)) {
      *got = 0;
      return;
    }
  }
  for (i = 0; i < rq->nread; i++) {
    unsigned char b = inb(((rq->flags & PGCTL_WORD) && i == 0 && rq->skip == 0) ? DATA_PORT_LOW : DATA_PORT_HIGH);
    rd[n++] = b;
    if (b == rq->stop && (rq->flags & PGCTL_STOP)) break;
  }
  *got = n;
  if (rq->nwrite != 0 && rq->reg == CMD_SAVE) Sleep(150);   /* the card writes its flash */
}

/* ---- pgcard.h --------------------------------------------------------------------- */

int pgcard_plat_ctl(const pgctl_req *rq, const unsigned char *wr, unsigned char *rd, unsigned *got) {
  static unsigned char in[sizeof(pgctl_req) + PGCTL_MAX_DATA];
  static unsigned char out[PGCTL_MAX_DATA];
  DWORD n = 0;
  unsigned i;

  *got = 0;
  if (rq->nwrite > PGCTL_MAX_DATA || rq->nread > PGCTL_MAX_DATA) return PGCARD_EIO;
  switch (backend_kind()) {
  case BACKEND_PORTS:
    on_ports(rq, wr, rd, got);
    return PGCARD_OK;
  case BACKEND_NT:
  case BACKEND_VXD:
    for (i = 0; i < sizeof(*rq); i++) in[i] = ((const unsigned char *)rq)[i];
    for (i = 0; i < rq->nwrite; i++) in[sizeof(*rq) + i] = wr[i];
    if (!DeviceIoControl(device, PGCTL_IOCTL, in, sizeof(*rq) + rq->nwrite, out, rq->nread, &n, NULL)) {
      return PGCARD_EIO;
    }
    if (n > rq->nread) n = rq->nread;
    for (i = 0; i < n; i++) rd[i] = out[i];
    *got = n;
    return PGCARD_OK;
  default:
    return PGCARD_EIO;
  }
}

void pgcard_plat_sleep(unsigned ms) {
  Sleep(ms);
}
