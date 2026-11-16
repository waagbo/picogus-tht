/*
 *  PGBOOT: floppy / hard disk image server for the boot ROM (see PROTOCOL.md).
 *
 *  Portable C: no Pico SDK dependencies, so the same code runs in the
 *  firmware (core 0 register handlers + core 1 server) and in the host-side
 *  card simulator used by the emulator tests (tools/emu).
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stdbool.h>

#define BD_UNITS        2       /* 0 = floppy (BIOS 00h), 1 = hard disk (BIOS 80h) */
#define BD_UNIT_FD      0
#define BD_UNIT_HD      1
#define BD_NAME_MAX     127     /* path chars, excluding the terminator */
#define BD_NAME_BUF     (BD_NAME_MAX + 1)

/* PGDFS frame subfunctions (AL) */
#define BD_AL_INFO      0xF3
#define BD_AL_READ      0xF4
#define BD_AL_WRITE     0xF5

/* BDINFO request flags */
#define BD_INFO_OPEN    0x01

/* BDINFO answer record (PROTOCOL.md): BD_INFO_LEN bytes, then the name */
#define BD_INFO_VERSION     1
#define BD_INFO_LEN         32
#define BD_INFO_NAME_MAX    63
#define BD_INFO_OFF_VERSION  0  /* u8  BD_INFO_VERSION */
#define BD_INFO_OFF_STATE    1  /* u8  BD_STATE_* */
#define BD_INFO_OFF_TYPE     2  /* u8  BD_TYPE_* */
#define BD_INFO_OFF_FLAGS    3  /* u8  BD_FLAG_* */
#define BD_INFO_OFF_CYL      4  /* u16 */
#define BD_INFO_OFF_HEADS    6  /* u16 */
#define BD_INFO_OFF_SPT      8  /* u16 */
#define BD_INFO_OFF_TOTAL   12  /* u32 sectors */
#define BD_INFO_OFF_DRVTYPE 16  /* u8  floppy drive type (INT 13h AH=08h BL), 0 for disks */
#define BD_INFO_OFF_GEN     17  /* u8  media generation */
#define BD_INFO_OFF_NAMELEN 18  /* u8  display name length, the name follows the record */
#define BD_INFO_OFF_TOKEN   20  /* u32 image token, echoed in BDREAD/BDWRITE; 0 when not ready */

#define BD_STATE_NONE       0   /* no image configured, or none opened since the card booted */
#define BD_STATE_READY      1
#define BD_STATE_NOTFOUND   2   /* file not found */
#define BD_STATE_UNUSABLE   3   /* size/format not recognised, dynamic VHD, read error */
#define BD_STATE_NODRIVE    4   /* USB drive not mounted */
#define BD_STATE_FRAGMENTED 5   /* image file too fragmented for the fast-seek map */

#define BD_TYPE_NONE        0
#define BD_TYPE_FLOPPY      1
#define BD_TYPE_HARDDISK    2

#define BD_FLAG_RO          0x01
#define BD_FLAG_VHD         0x02
#define BD_FLAG_CHANGED     0x04

/* BDREAD / BDWRITE request header: UU NN LL LL LL LL TT TT TT TT (unit, count, LBA, token) */
#define BD_IO_HDR_LEN       10

/* CMD_BDOPTS bits */
#define BD_OPT_FD_RO        0x01
#define BD_OPT_HD_RO        0x02
#define BD_OPT_ROM_OFF      0x04
#define BD_OPT_BOOT_HD      0x08
#define BD_OPT_MASK         0x0F
#define BD_OPTS_SIGNATURE   0x80    /* read value = opts | 80h (bit 6 clear) */

/* INT 13h style status codes returned in AX */
#define BD_ST_OK            0x00
#define BD_ST_BADCMD        0x01
#define BD_ST_WRPROT        0x03
#define BD_ST_NOSECTOR      0x04
#define BD_ST_CHANGED       0x06    /* floppy: token mismatch = media changed */
#define BD_ST_CTRLFAIL      0x20
#define BD_ST_NOTREADY      0x80

/* ---- setup (before core 1 starts) --------------------------------------
 * Binds the server to the persisted settings storage: the two name buffers
 * (BD_NAME_BUF bytes each, zero-terminated) and the options byte. */
void bd_init(char *fd_name, char *hd_name, uint8_t *opts);
/* A value that differs on every card boot (the Pico: get_rand_32()), mixed
 * into the image tokens so a system that booted before a card reboot never
 * matches an image opened after it. Call after bd_init(); 0 if never called. */
void bd_set_boot_nonce(uint32_t nonce);

/* ---- core 0: control register handlers (O(1), no FatFs, ISR-safe) ----- */
void    bd_ctl_name_select(uint8_t unit);           /* register selected: rewind */
void    bd_ctl_name_write(uint8_t unit, uint8_t c); /* append; 0 commits */
uint8_t bd_ctl_name_read(uint8_t unit);             /* next char; 0 at the end, then rewinds */
uint8_t bd_ctl_opts_read(void);                     /* opts | BD_OPTS_SIGNATURE */
void    bd_ctl_opts_write(uint8_t v);

/* ---- core 1 ------------------------------------------------------------- */
void bd_tasks(void);                /* from dfs_tasks(): applies committed names */
void bd_on_drive_mounted(void);     /* the USB volume was mounted */
void bd_on_drive_unmounted(void);   /* the USB volume went away */

/* Serves one BD_AL_* request. req/req_len: the frame payload after the 4-byte
 * header; answ: where the answer payload goes (may alias req), maxpl its
 * capacity. Sets *ax and returns the answer payload length. */
uint16_t bd_process(uint8_t al, const uint8_t *req, uint16_t req_len,
                    uint8_t *answ, uint16_t maxpl, uint16_t *ax);

/* True when path (FatFs form, as the PGDFS server builds it) names an image
 * that is currently open; PGDFS then refuses to modify, rename or delete it. */
bool bd_path_is_open_image(const char *path);
/* Same, or path is a directory an open image lives under (PGDFS RENAME source). */
bool bd_path_holds_open_image(const char *path);

#ifdef __cplusplus
}
#endif
