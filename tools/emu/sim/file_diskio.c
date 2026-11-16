/*
 * pgcard-sim: the "USB stick" behind FatFs diskio.h, a raw image file
 * (superfloppy or MBR partitioned; FatFs finds the volume either way).
 *
 * Also the host stand-ins for what usb_msc/msc_app.c provides on the card:
 * msc_app_get_stats() (telemetry for the PGDFS DIAG request) and
 * dfs_platform_fatfs() (the mounted FATFS object).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ff.h"
#include "diskio.h"
#include "dfs_fs.h"
#include "usb_msc/msc_app.h"
#include "sim.h"

#define SECTOR 512

static int img_fd = -1;
static uint32_t n_sectors;
static bool present;            /* the "drive" is plugged in (after --mount-delay-ms) */
static msc_stats_t stats;
static FATFS fatfs;

bool stick_open(const char *path) {
    struct stat st;
    img_fd = open(path, O_RDWR);
    if (img_fd < 0) {
        fprintf(stderr, "pgcard-sim: cannot open stick image %s: %s\n", path, strerror(errno));
        return false;
    }
    if (fstat(img_fd, &st) != 0 || st.st_size < SECTOR) {
        fprintf(stderr, "pgcard-sim: stick image %s is too small\n", path);
        return false;
    }
    n_sectors = (uint32_t)(st.st_size / SECTOR);
    return true;
}

void stick_close(void) {
    if (img_fd >= 0) {
        fsync(img_fd);
        close(img_fd);
    }
    img_fd = -1;
}

void stick_set_present(bool on) {
    present = on;
}

FATFS *sim_fatfs(void) {
    return &fatfs;
}

FATFS *dfs_platform_fatfs(void) {
    return &fatfs;
}

const msc_stats_t *msc_app_get_stats(void) {
    return &stats;
}

static void count_sat(uint32_t *c) {
    if (*c != 0xFFFFFFFFu) (*c)++;
}

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == 0 && present && img_fd >= 0) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    DRESULT res = RES_OK;
    uint8_t cause = MSC_IO_OK;
    count_sat(&stats.reads);
    if (disk_status(pdrv)) {
        res = RES_NOTRDY;
        cause = MSC_IO_NODEV;
    } else if ((uint64_t)sector + count > n_sectors) {
        res = RES_PARERR;
        cause = MSC_IO_REFUSED;
        count_sat(&stats.read_refused);
    } else {
        size_t len = (size_t)count * SECTOR;
        ssize_t n = pread(img_fd, buff, len, (off_t)sector * SECTOR);
        if (n != (ssize_t)len) {
            res = RES_ERROR;
            cause = MSC_IO_CSW;
            count_sat(&stats.read_csw_err);
        }
    }
    sim_trace_disk('R', (uint32_t)sector, count, res);
    stats.last_read_res = (uint8_t)res;
    stats.last_read_cause = cause;
    return res;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    DRESULT res = RES_OK;
    uint8_t cause = MSC_IO_OK;
    count_sat(&stats.writes);
    stats.last_write_lba = (uint32_t)sector;
    stats.last_write_count = (uint16_t)count;
    stats.last_write_us = 0;
    if (disk_status(pdrv)) {
        res = RES_NOTRDY;
        cause = MSC_IO_NODEV;
    } else if ((uint64_t)sector + count > n_sectors) {
        res = RES_PARERR;
        cause = MSC_IO_REFUSED;
        count_sat(&stats.write_refused);
    } else {
        size_t len = (size_t)count * SECTOR;
        ssize_t n = pwrite(img_fd, buff, len, (off_t)sector * SECTOR);
        if (n != (ssize_t)len) {
            res = RES_ERROR;
            cause = MSC_IO_CSW;
            count_sat(&stats.write_csw_err);
        }
        stats.last_write_us = 100u * count;
    }
    sim_trace_disk('W', (uint32_t)sector, count, res);
    stats.last_write_res = (uint8_t)res;
    stats.last_write_cause = cause;
    return res;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (disk_status(pdrv)) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:        return fsync(img_fd) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = n_sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
    default:               return RES_PARERR;
    }
}
