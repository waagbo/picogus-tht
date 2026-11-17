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
#include "bd_tune.h"

#define BD_UNITS        2       /* 0 = floppy (BIOS 00h), 1 = hard disk (BIOS 80h) */
#define BD_UNIT_FD      0
#define BD_UNIT_HD      1
#define BD_NAME_MAX     127     /* path chars, excluding the terminator */
#define BD_NAME_BUF     (BD_NAME_MAX + 1)

/* PGDFS frame subfunctions (AL) */
#define BD_AL_INFO      0xF3
#define BD_AL_READ      0xF4
#define BD_AL_WRITE     0xF5
#define BD_AL_LIST      0xF6    /* directory listing for the ROM's boot menu */

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

/* BDLIST (PROTOCOL.md). Request: start index u16, max entries u8, list id u8,
 * then the directory path (no terminator, empty = root; folder lists only).
 * Answer: total entries u16, entries in this answer u8, reserved u8, then
 * the entries. */
#define BD_LIST_REQ_LEN     4
#define BD_LIST_HDR_LEN     4
#define BD_LIST_ENTRY_LEN   48
#define BD_LIST_OFF_KIND     0  /* u8  BD_KIND_* */
#define BD_LIST_OFF_FLAGS    1  /* u8  bit 0 read-only file */
#define BD_LIST_OFF_SECTORS  2  /* u32 size in 512-byte sectors (0 for directories) */
#define BD_LIST_OFF_DISPLEN  6  /* u8  length of the display name */
#define BD_LIST_OFF_SFN      8  /* 13 bytes: 8.3 name, zero padded (what a path is built from) */
#define BD_LIST_OFF_DISP    21  /* 27 bytes: long name for display, zero padded */
#define BD_LIST_SFN_LEN     13
#define BD_LIST_DISP_LEN    27

#define BD_LIST_OFF_ARG      2  /* menu entries, in place of the size: u8 a, u8 b (see the kinds) */

#define BD_KIND_DIR         0   /* directory (".." first, below the root) */
#define BD_KIND_FLOPPY      1   /* image the size of a floppy disk: goes to A: */
#define BD_KIND_HARDDISK    2   /* any larger image, and every .VHD/.HDD: goes to C: */
#define BD_KIND_MENU        3   /* opens another list: a = its id; the entry's name is its title */
#define BD_KIND_CHOICE      4   /* a setting: the value b is written to control register a
                                 * (a = CMD_BOOTMODE: and the card is told to save and restart) */
#define BD_KIND_BOOT        5   /* leaves the menu and boots */

#define BD_LIST_FLAG_RO      0x01   /* the image file is read-only */
#define BD_LIST_FLAG_CURRENT 0x02   /* the choice that is in effect */

/* The lists: a folder of the USB drive (the request carries its path), or
 * one of the boot menu's own */
#define BD_LIST_ALL         0x00    /* a folder: its folders and disk images */
#define BD_LIST_FLOPPY      0x01    /* its folders and floppy images */
#define BD_LIST_HARDDISK    0x02    /* its folders and hard disk images */
#define BD_LIST_MAIN        0x10    /* the main menu */
#define BD_LIST_TUNES       0x11    /* the start-up tunes */
#define BD_LIST_MODES       0x12    /* the card's modes */
#define BD_LIST_THEMES      0x13    /* the colours of the menu */
#define BD_LIST_ORDER       0x14    /* which image boots when both drives hold one */

/* Largest file the boot menu offers as a floppy: 2.88 MB */
#define BD_FLOPPY_MAX_BYTES 2949120UL

/* CMD_BDMENU: bits 0-1 the mode, bits 2-5 the seconds the ROM waits for the
 * menu key; reads return the value | 40h (bit 7 clear: firmware without the
 * boot menu reads FFh). CMD_BDTUNE: the start-up tune, same signature. */
#define BD_MENU_OFF         0   /* no boot menu: boot what is configured */
#define BD_MENU_KEY         1   /* offer the menu for a few seconds at boot */
#define BD_MENU_ALWAYS      2   /* open the menu at every boot */
#define BD_MENU_MODE_MASK   0x03
#define BD_MENU_WAIT_SHIFT  2
#define BD_MENU_WAIT_MAX    15
#define BD_MENU_WAIT_DEFAULT 3
#define BD_MENU_DEFAULT     ((BD_MENU_WAIT_DEFAULT << BD_MENU_WAIT_SHIFT) | BD_MENU_KEY)
#define BD_MENU_MASK        0x3F
#define BD_TUNE_CUSTOM      62  /* CMD_BDTUNE: 0 = silent, 1..BD_TUNE_BUILTIN = the firmware's
                                 * melodies (bd_tune.c), 62 = the user's (CMD_BDMELODY) */
#define BD_TUNE_DEFAULT     1
#define BD_THEME_SETUP      0   /* CMD_BDTHEME: the colours of the boot menu, */
#define BD_THEME_COMMANDER  1   /* named as on the web page */
#define BD_THEME_MONO       2
#define BD_THEME_MAX        2
#define BD_THEME_DEFAULT    BD_THEME_SETUP
#define BD_UI_NEXT          0x3F    /* written to CMD_BDTUNE / CMD_BDTHEME: the next one */
#define BD_UI_SIGNATURE     0x40
#define BD_MELODY_MAX       255     /* RTTTL text of the user's melody, excluding the terminator */
#define BD_MELODY_BUF       (BD_MELODY_MAX + 1)

/* INT 13h style status codes returned in AX */
#define BD_ST_OK            0x00
#define BD_ST_BADCMD        0x01
#define BD_ST_NODIR         0x02    /* BDLIST: no such directory */
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

/* Binds the boot menu, start-up tune and colour settings (one byte each) and
 * the text of the user's melody (BD_MELODY_BUF bytes, zero-terminated). NULL:
 * that register reads its default and ignores writes. Call after bd_init();
 * compiles the selected tune. */
void bd_init_ui(uint8_t *menu, uint8_t *tune, uint8_t *theme, char *melody);

/* The modes the card can be switched to from the boot menu (bit n: mode n of
 * card_mode_t; one bit or none: no "Card Mode" in the menu) and the one that
 * is running. The ROM switches like pgusinit /mode does: CMD_BOOTMODE,
 * CMD_SAVE, CMD_REBOOT. */
void bd_init_modes(uint8_t available, uint8_t current);

/* ---- core 0: control register handlers (O(1), no FatFs, ISR-safe) ----- */
void    bd_ctl_name_select(uint8_t unit);           /* register selected: rewind */
void    bd_ctl_name_write(uint8_t unit, uint8_t c); /* append; 0 commits */
uint8_t bd_ctl_name_read(uint8_t unit);             /* next char; 0 at the end, then rewinds */
uint8_t bd_ctl_opts_read(void);                     /* opts | BD_OPTS_SIGNATURE */
void    bd_ctl_opts_write(uint8_t v);
uint8_t bd_ctl_menu_read(void);                     /* menu | BD_UI_SIGNATURE */
void    bd_ctl_menu_write(uint8_t v);
uint8_t bd_ctl_tune_read(void);                     /* tune | BD_UI_SIGNATURE */
void    bd_ctl_tune_write(uint8_t v);               /* BD_UI_NEXT: the next tune there is */
uint8_t bd_ctl_theme_read(void);                    /* theme | BD_UI_SIGNATURE */
void    bd_ctl_theme_write(uint8_t v);              /* BD_UI_NEXT: the next theme */
void    bd_ctl_melody_select(void);                 /* register selected: rewind */
void    bd_ctl_melody_write(uint8_t c);             /* append; 0 commits */
uint8_t bd_ctl_melody_read(void);                   /* next char; 0 at the end, then rewinds */
void    bd_ctl_tunedata_select(void);               /* register selected: rewind */
uint8_t bd_ctl_tunedata_read(void);                 /* next byte of the tune's stream; FFh past
                                                     * its end and while it is being compiled */

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
