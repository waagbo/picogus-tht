/*
 * The setup registers of the emulated PicoGUS (CD images, card mode, WiFi),
 * as sw/picogus.cpp, sw/cdrom/cdrom.c and sw/web/web_main.c serve them.
 *
 * Copyright (C) 2026 PicoGUS contributors. MIT license, see ../../LICENSE.
 */
#ifndef EMUCTL_H
#define EMUCTL_H

#include <stdint.h>
#include <stdbool.h>

/* the register file: true when the card knows reg */
bool    emuctl_select(uint8_t reg);
void    emuctl_write_low(uint8_t reg, uint8_t v);
void    emuctl_write_high(uint8_t reg, uint8_t v);
uint8_t emuctl_read_low(uint8_t reg);
uint8_t emuctl_read_high(uint8_t reg);
/* "core 1": serves a pending CD command, once busy_polls calls have passed */
void    emuctl_task(void);

#define EMUCTL_MAX_IMAGES 300

typedef struct {
    /* the card */
    const char *fw;                 /* CMD_FWSTRING */
    uint8_t  mode;                  /* card_mode_t the firmware runs as */
    bool     has_cd;                /* firmware with CD-ROM emulation (SB, USB mode) */
    bool     has_wifi;              /* firmware with the web server */
    uint16_t cd_port;
    uint8_t  autoadvance;
    /* the USB drive's CD images */
    bool     drive;
    const char *image[EMUCTL_MAX_IMAGES];
    int      images;
    const char *bad_image;          /* loading this one fails */
    int      busy_polls;            /* emuctl_task() calls a CD command takes */
    /* WiFi */
    char     ssid[33], pass[64];
    const char *wifi_status;        /* what CMD_WIFISTAT answers */
    int      wifi_busy_reads;       /* FFh this often first (the NE2000 firmware's "not ready") */
    /* what happened */
    int      saves, reboots, applies, list_scans;
    uint8_t  boot_request;          /* CMD_BOOTMODE as written */
    int      list_overruns;         /* reads of the CD list past its end, or without a list */
    void   (*on_reboot)(void);
} emuctl_t;

extern emuctl_t emuctl;

void emuctl_reset(void);

#endif
