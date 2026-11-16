/*
 *  PGBOOT: disk image format and geometry detection (internal header).
 *
 *  Pure functions on the bytes of an image, no FatFs: bootdisk.c reads the
 *  pieces it needs from the file and hands them over, the host tests feed
 *  them directly. Portable C.
 *
 *  Copyright (C) 2026  PicoGUS contributors
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stdbool.h>

#define BD_SECTOR           512u

/* What is read from the image to identify it. Only these byte ranges of
 * sector 0 are looked at, so the caller never needs a whole sector buffer. */
#define BD_SEC0_HEAD_LEN    64      /* bytes 0..63: jump + FAT BPB */
#define BD_SEC0_PTAB_OFF    446     /* bytes 446..511: 4 partition entries + 55AA */
#define BD_SEC0_PTAB_LEN    66
#define BD_VHD_FOOTER_LEN   512     /* the checksum covers the whole footer */
#define BD_VHD_HEAD_LEN     68      /* footer bytes the parser looks at (through the checksum) */

typedef struct {
    uint8_t head[BD_SEC0_HEAD_LEN];
    uint8_t ptab[BD_SEC0_PTAB_LEN];
} bd_sec0_t;

/* VHD footer classification (bd_vhd_check) */
enum {
    BD_VHD_NONE = 0,    /* no valid "conectix" footer: a raw image */
    BD_VHD_FIXED,       /* fixed VHD: data = file minus the 512-byte footer */
    BD_VHD_OTHER        /* dynamic, differencing or unknown disk type: unusable */
};

typedef struct {
    uint32_t data_size;     /* bytes of disk data (fixed VHD: current size, clipped) */
    uint16_t cyl;           /* footer geometry, 0 when unusable for INT 13h */
    uint8_t  heads;
    uint8_t  spt;
} bd_vhd_t;

/* Geometry result */
typedef struct {
    uint16_t cyl;
    uint16_t heads;
    uint16_t spt;
    uint32_t total;         /* sectors, the LBA size */
    uint8_t  drive_type;    /* floppy: INT 13h AH=08h BL; 0 for hard disks */
} bd_geom_t;

/* Classifies the last 512 bytes of a file of file_size bytes. head: the first
 * BD_VHD_HEAD_LEN bytes of them; sum: the byte sum of all 512 except the
 * checksum field (offsets 64..67), so the caller can read the footer in
 * pieces. bd_vhd_sum() computes it from a whole footer. */
int      bd_vhd_check(const uint8_t *head, uint32_t sum, uint32_t file_size, bd_vhd_t *vhd);
uint32_t bd_vhd_sum(const uint8_t *footer, uint32_t off, uint32_t len);   /* partial sum, footer offsets off.. */

/* A plausible FAT BPB (DOS 2.0+ boot sector) in the first bytes of sector 0. */
bool bd_bpb_valid(const uint8_t *head);

/* Floppy: the standard size table first, then the BPB. sec0 may be NULL when
 * the image is too small to hold one. False: not a recognisable floppy image. */
bool bd_geom_floppy(uint32_t data_size, const bd_sec0_t *sec0, bd_geom_t *g);

/* Hard disk: VHD footer geometry (vhd may be NULL), MBR partition table, BPB,
 * else 16 heads / 63 sectors. Cylinders = min(1024, total / (heads * spt)).
 * False only when the image holds no whole sector. */
bool bd_geom_harddisk(uint32_t data_size, const bd_sec0_t *sec0, const bd_vhd_t *vhd, bd_geom_t *g);

#ifdef __cplusplus
}
#endif
