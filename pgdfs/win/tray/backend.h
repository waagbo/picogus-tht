/* PGTRAY: the ways to the card. MIT license, see ../../LICENSE. */
#ifndef PGTRAY_BACKEND_H
#define PGTRAY_BACKEND_H

enum {
  BACKEND_NONE,     /* Windows NT without pgdfs.sys: no way to the ports */
  BACKEND_NT,       /* pgdfs.sys */
  BACKEND_VXD,      /* PGDFS.VXD */
  BACKEND_PORTS,    /* Windows 9x without the VxD: the ports themselves */
  BACKEND_DEMO      /* an emulated card (make demo) */
};

/* finds the way on first use, and again while there is none */
int backend_kind(void);
const char *backend_name(void);

#endif
