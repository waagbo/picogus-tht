/*
 * Control requests to the PicoGUS through the PGDFS drivers
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * What PGUSINIT does with the card's control registers (common/picogus.h),
 * for a Windows program: the CD images, the card mode, WiFi. The drivers own
 * the control port while Windows runs: a program that used the ports itself
 * would get between the register select and the data access of a PGDFS
 * transaction (and the driver between the program's), and under Windows NT a
 * program cannot reach the ports at all.
 *
 * One request is one uninterrupted sequence on the bus:
 *
 *   select `reg` (knock, register number on CONTROL_PORT)
 *   write `nwrite` bytes to DATA_PORT_HIGH
 *   read `skip` bytes from DATA_PORT_HIGH and drop them
 *   read up to `nread` bytes from DATA_PORT_HIGH; with PGCTL_STOP the byte
 *   equal to `stop` is the last one (a string's NUL, the CD list's EOT)
 *
 * The select rewinds the card's string registers, so a long answer is read in
 * pieces by asking for it again with a larger `skip`. With PGCTL_STOP the
 * request also ends, with nothing read, when the stop byte is among the
 * skipped ones: the card must not be read past the end of its CD list.
 *
 * PGCTL_WORD is for the 16-bit registers (the port settings): the first byte
 * written and the first byte read go through DATA_PORT_LOW, the second
 * through DATA_PORT_HIGH.
 *
 * The drivers refuse the registers of PGDFS itself (80h-87h), firmware
 * programming (CMD_FLASH) and CMD_DEFAULTS, which would move the PGDFS data
 * port from under them. After a write to CMD_SAVE the driver waits for the
 * card to write its flash; after a write to CMD_REBOOT it takes the USB drive
 * for unplugged until the card is back.
 *
 * Windows NT: open \\.\PgDfs, DeviceIoControl(PGCTL_IOCTL).
 * Windows 9x: open \\.\PGDFS (the VxD), DeviceIoControl(PGCTL_IOCTL).
 * In: a pgctl_req followed by the bytes to write. Out: the bytes read.
 */

#ifndef PGCTL_H
#define PGCTL_H

#include <stdint.h>

#define PGCTL_VERSION   1
#define PGCTL_MAX_DATA  1024    /* most bytes written, and most read, by one request */
#define PGCTL_MAX_SKIP  0x20000UL /* most bytes skipped: the card's longest answer, its CD list */

#define PGCTL_WORD      0x01
#define PGCTL_STOP      0x02

typedef struct {
  uint8_t  version;     /* PGCTL_VERSION */
  uint8_t  reg;         /* CMD_* register */
  uint8_t  flags;       /* PGCTL_* */
  uint8_t  stop;
  uint16_t nwrite;
  uint16_t nread;
  uint32_t skip;
} pgctl_req;            /* 12 bytes, no padding */

/* CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define PGCTL_IOCTL     0x80002000UL

#define PGCTL_DEVICE_NT "\\\\.\\PgDfs"
#define PGCTL_DEVICE_9X "\\\\.\\PGDFS"

#endif
