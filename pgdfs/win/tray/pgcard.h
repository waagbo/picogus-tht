/*
 * PGTRAY: what the program does with the card, as control requests (pgctl.h)
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * The sequences are those of PGUSINIT (pgusinit/pgusinit.c). Plain C without
 * library calls: the Windows program links no C runtime, and the host test in
 * ../test runs this file against the emulated card.
 *
 * Strings from the card are in its code page (the OEM code page for file
 * names), NUL-terminated and cut to the buffer.
 */

#ifndef PGCARD_H
#define PGCARD_H

#include "../common/pgctl.h"

#define PGCARD_OK        0
#define PGCARD_EIO       1   /* the request did not reach the card (driver, access) */
#define PGCARD_ENOCARD   2   /* no PicoGUS answers */
#define PGCARD_ETIMEOUT  3   /* the card did not finish in time */
#define PGCARD_ECARD     4   /* the card reported an error (its text in err) */
#define PGCARD_ENOTSUP   5   /* not in this firmware, or not in this card mode */
#define PGCARD_EINVAL    6   /* a value the card cannot take */

#define PGCARD_TEXT      256 /* names, error and status texts incl. NUL */
#define PGCARD_SSID_MAX  32
#define PGCARD_PASS_MAX  63

/* ---- provided by the program (backend.c) or the test -------------------------- */

/* runs one control request; PGCARD_OK or PGCARD_EIO. *got: bytes read */
int  pgcard_plat_ctl(const pgctl_req *rq, const unsigned char *wr, unsigned char *rd, unsigned *got);
void pgcard_plat_sleep(unsigned ms);

/* ---- the card ----------------------------------------------------------------- */

typedef struct {
  char          fw[PGCARD_TEXT];  /* firmware string */
  unsigned char protocol;
  unsigned char mode;             /* card_mode_t */
  unsigned char has_cd;           /* this mode has CD-ROM emulation */
  unsigned short cd_port;         /* 0 = CD-ROM emulation switched off */
} pgcard_info;

int pgcard_get_info(pgcard_info *info);

/* "GUS", "Sound Blaster", ... for a card_mode_t; "?" for others */
const char *pgcard_mode_name(unsigned char mode);

/* writes the mode, saves the settings if asked to, restarts the card and
 * waits for it. *now: the mode it runs in afterwards */
int pgcard_set_mode(unsigned char mode, int save, unsigned char *now);

/* ---- CD images ---------------------------------------------------------------- */

/* index of the loaded image in the list (1..), 0 = none, and its name */
int pgcard_cd_current(unsigned char *index, char *name, unsigned max);

/* the images on the USB drive, in the card's order: names packed one after
 * another into buf, each NUL-terminated; *count how many (those that fit).
 * PGCARD_ECARD: no USB drive, no images (err says which). */
int pgcard_cd_list(char *buf, unsigned size, unsigned *count, char *err, unsigned errmax);

/* loads image index (1..) of the list, 0 unloads */
int pgcard_cd_load(unsigned char index, char *err, unsigned errmax);

/* ---- WiFi (firmware with the web server) ----------------------------------------- */

/* the card's status line, e.g. "Connected to SSID x, Signal -55 dB, web UI:
 * http://192.168.1.42/". PGCARD_ENOTSUP: firmware without WiFi */
int pgcard_wifi_status(char *text, unsigned max);

/* the address of the web page in a status line, 0 if it has none */
int pgcard_wifi_url(const char *status, char *url, unsigned max);

/* sets the network and connects; pass "" for an open network */
int pgcard_wifi_set(const char *ssid, const char *pass, int save);

#endif
