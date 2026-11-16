/*
 *  PGBOOT: disk image format and geometry detection.
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
#include "bd_geometry.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)be16(p) << 16) | be16(p + 2); }

/* ---- VHD ------------------------------------------------------------------ */

/* Footer layout (Microsoft Virtual Hard Disk Image Format Specification),
 * big-endian: 0 cookie "conectix", 8 features, 12 version, 16 data offset,
 * 24 timestamp, 28 creator app, 32 creator version, 36 creator OS,
 * 40 original size (u64), 48 current size (u64), 56 geometry (u16 cylinders,
 * u8 heads, u8 sectors), 60 disk type (2 fixed, 3 dynamic, 4 differencing),
 * 64 checksum (one's complement of the byte sum of the footer without it). */
#define VHD_OFF_CURSIZE  48
#define VHD_OFF_GEOM     56
#define VHD_OFF_TYPE     60
#define VHD_OFF_CSUM     64
#define VHD_TYPE_FIXED   2

uint32_t bd_vhd_sum(const uint8_t *footer, uint32_t off, uint32_t len) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t o = off + i;
        if (o >= VHD_OFF_CSUM && o < VHD_OFF_CSUM + 4) continue;
        s += footer[i];
    }
    return s;
}

int bd_vhd_check(const uint8_t *head, uint32_t sum, uint32_t file_size, bd_vhd_t *vhd) {
    uint32_t csum, cur_hi, cur_lo, avail;

    memset(vhd, 0, sizeof(*vhd));
    if (file_size < 2 * BD_SECTOR || memcmp(head, "conectix", 8) != 0) return BD_VHD_NONE;
    csum = be32(head + VHD_OFF_CSUM);
    if (csum != (uint32_t)~sum) return BD_VHD_NONE;     /* coincidence in a raw image's last sector */
    if (be32(head + VHD_OFF_TYPE) != VHD_TYPE_FIXED) return BD_VHD_OTHER;
    /* A fixed VHD is the raw disk followed by the footer. Trust the current
     * size only as far as the file backs it. */
    avail = file_size - BD_SECTOR;
    cur_hi = be32(head + VHD_OFF_CURSIZE);
    cur_lo = be32(head + VHD_OFF_CURSIZE + 4);
    vhd->data_size = (cur_hi == 0 && cur_lo != 0 && cur_lo < avail) ? cur_lo : avail;
    vhd->data_size &= ~(BD_SECTOR - 1);
    vhd->cyl = be16(head + VHD_OFF_GEOM);
    vhd->heads = head[VHD_OFF_GEOM + 2];
    vhd->spt = head[VHD_OFF_GEOM + 3];
    /* Large VHDs carry 255 sectors per track (beyond the CHS the spec's
     * algorithm can express in INT 13h): not usable as a BIOS geometry. */
    if (vhd->cyl == 0 || vhd->heads == 0 || vhd->spt == 0 || vhd->spt > 63) {
        vhd->cyl = 0;
        vhd->heads = 0;
        vhd->spt = 0;
    }
    return BD_VHD_FIXED;
}

/* ---- BPB ------------------------------------------------------------------ */

bool bd_bpb_valid(const uint8_t *s) {
    uint8_t spc = s[13];
    uint16_t spt = le16(s + 24), heads = le16(s + 26);
    uint32_t total = le16(s + 19) ? le16(s + 19) : le32(s + 32);

    if (s[0] != 0xEB && s[0] != 0xE9) return false;         /* x86 jump to the boot code */
    if (le16(s + 11) != BD_SECTOR) return false;            /* bytes per sector */
    if (spc == 0 || (spc & (spc - 1)) != 0) return false;   /* sectors per cluster: power of 2 */
    if (le16(s + 14) == 0) return false;                    /* reserved sectors */
    if (s[16] < 1 || s[16] > 2) return false;               /* number of FATs */
    if (s[21] < 0xF0) return false;                         /* media descriptor */
    if (spt < 1 || spt > 63 || heads < 1 || heads > 255) return false;
    return total != 0;
}

static uint32_t bpb_total(const uint8_t *s) {
    return le16(s + 19) ? le16(s + 19) : le32(s + 32);
}

/* ---- floppy --------------------------------------------------------------- */

static const struct {
    uint32_t size;
    uint8_t  cyl, heads, spt, type;
} floppies[] = {
    {  163840, 40, 1,  8, 0x01 },  /* 160K, DOS 1.0 single sided */
    {  184320, 40, 1,  9, 0x01 },  /* 180K */
    {  327680, 40, 2,  8, 0x01 },  /* 320K */
    {  368640, 40, 2,  9, 0x01 },  /* 360K */
    {  737280, 80, 2,  9, 0x03 },  /* 720K */
    { 1228800, 80, 2, 15, 0x02 },  /* 1.2M */
    { 1474560, 80, 2, 18, 0x04 },  /* 1.44M */
    { 1720320, 80, 2, 21, 0x04 },  /* 1.68M DMF, read by a 1.44M drive */
    { 1763328, 82, 2, 21, 0x04 },  /* 1.72M, 82 tracks */
    { 2949120, 80, 2, 36, 0x06 },  /* 2.88M */
};

bool bd_geom_floppy(uint32_t data_size, const bd_sec0_t *sec0, bd_geom_t *g) {
    uint32_t total = data_size / BD_SECTOR, cyl;
    uint16_t heads, spt;

    memset(g, 0, sizeof(*g));
    for (unsigned i = 0; i < sizeof(floppies) / sizeof(floppies[0]); i++) {
        if (floppies[i].size == data_size) {
            g->cyl = floppies[i].cyl;
            g->heads = floppies[i].heads;
            g->spt = floppies[i].spt;
            g->total = total;
            g->drive_type = floppies[i].type;
            return true;
        }
    }
    /* odd size (truncated, trailing data, unusual format): the boot sector */
    if (total == 0 || sec0 == NULL || !bd_bpb_valid(sec0->head)) return false;
    heads = le16(sec0->head + 26);
    spt = le16(sec0->head + 24);
    if (heads > 2) return false;
    cyl = bpb_total(sec0->head) / ((uint32_t)heads * spt);
    if (cyl == 0 || cyl > 255) return false;
    g->cyl = (uint16_t)cyl;
    g->heads = heads;
    g->spt = spt;
    g->total = total;
    if (spt <= 9)       g->drive_type = (cyl <= 42) ? 0x01 : 0x03;
    else if (spt <= 15) g->drive_type = 0x02;
    else if (spt <= 21) g->drive_type = 0x04;
    else                g->drive_type = 0x06;
    return true;
}

/* ---- hard disk ------------------------------------------------------------ */

/* An MBR whose non-empty entries all look sane: status 00h/80h, a type, a
 * start LBA past the MBR, a length, and an end sector 1..63. The ranges of
 * a FAT boot sector's code where the table would sit fail this quickly. */
static bool mbr_geometry(const uint8_t *ptab, uint16_t *heads, uint16_t *spt) {
    uint16_t h = 0, s = 0;
    int used = 0;

    if (ptab[64] != 0x55 || ptab[65] != 0xAA) return false;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = ptab + 16 * i;
        if (e[4] == 0) continue;                            /* empty entry */
        if (e[0] != 0x00 && e[0] != 0x80) return false;
        if (le32(e + 8) == 0 || le32(e + 12) == 0) return false;
        if ((e[6] & 0x3F) == 0) return false;
        if (e[5] + 1u > h) h = (uint16_t)(e[5] + 1u);       /* end head */
        if ((e[6] & 0x3F) > s) s = e[6] & 0x3F;             /* end sector */
        used++;
    }
    if (used == 0) return false;
    if (h > 255) h = 255;                                   /* end head 255: INT 13h counts 255 at most */
    *heads = h;
    *spt = s;
    return true;
}

bool bd_geom_harddisk(uint32_t data_size, const bd_sec0_t *sec0, const bd_vhd_t *vhd, bd_geom_t *g) {
    uint32_t total = data_size / BD_SECTOR, cyl;
    uint16_t heads = 0, spt = 0, maxcyl = 1024;

    memset(g, 0, sizeof(*g));
    if (total == 0) return false;
    if (vhd && vhd->spt) {
        heads = vhd->heads;
        spt = vhd->spt;
        if (vhd->cyl < maxcyl) maxcyl = vhd->cyl;
    } else if (sec0 && mbr_geometry(sec0->ptab, &heads, &spt)) {
        /* heads and spt from the partition table */
    } else if (sec0 && bd_bpb_valid(sec0->head)) {
        heads = le16(sec0->head + 26);                      /* superfloppy */
        spt = le16(sec0->head + 24);
    } else {
        heads = 16;
        spt = 63;
    }
    cyl = total / ((uint32_t)heads * spt);
    if (cyl > maxcyl) cyl = maxcyl;
    if (cyl == 0) cyl = 1;                                  /* smaller than one cylinder */
    g->cyl = (uint16_t)cyl;
    g->heads = heads;
    g->spt = spt;
    g->total = total;
    return true;
}
