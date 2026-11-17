/*
 * PGTRAY: what the program does with the card, see pgcard.h
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 */

#ifndef PICOGUS_NO_MODENAMES
#define PICOGUS_NO_MODENAMES
#endif
#include "../../../common/picogus.h"
#include "pgcard.h"

#define CD_EOT          4       /* ends the CD image list */
#define CD_POLL_MS      20
#define CD_TIMEOUT_MS   25600   /* PGUSINIT: 256 polls of 100 ms */
#define CD_LIST_MAX     (PGCTL_MAX_SKIP - PGCTL_MAX_DATA) /* more than 255 names of 255 bytes */
#define REBOOT_FIRST_MS 250
#define REBOOT_POLL_MS  100
#define REBOOT_TIMEOUT_MS 10000
#define WIFI_WAIT_MS    100
#define WIFI_RETRIES    20
#define NO_ANSWER       0xFF    /* a register the firmware does not have, an empty bus */

/* ---- requests ------------------------------------------------------------------- */

static int request(unsigned char reg, unsigned char flags, unsigned char stop, const unsigned char *wr,
                   unsigned nwrite, unsigned long skip, unsigned char *rd, unsigned nread, unsigned *got) {
  pgctl_req rq;
  unsigned n = 0;
  int r;
  rq.version = PGCTL_VERSION;
  rq.reg = reg;
  rq.flags = flags;
  rq.stop = stop;
  rq.nwrite = (uint16_t)nwrite;
  rq.nread = (uint16_t)nread;
  rq.skip = skip;
  r = pgcard_plat_ctl(&rq, wr, rd, &n);
  if (got) *got = n;
  return r;
}

static int read8(unsigned char reg, unsigned char *v) {
  unsigned got;
  int r = request(reg, 0, 0, 0, 0, 0, v, 1, &got);
  if (r == PGCARD_OK && got != 1) r = PGCARD_EIO;
  return r;
}

static int write8(unsigned char reg, unsigned char v) {
  return request(reg, 0, 0, &v, 1, 0, 0, 0, 0);
}

/* a string register, read up to its NUL */
static int read_string(unsigned char reg, char *dst, unsigned max) {
  unsigned char buf[PGCARD_TEXT];
  unsigned got, i;
  int r = request(reg, PGCTL_STOP, 0, 0, 0, 0, buf, sizeof(buf) - 1, &got);
  if (max) dst[0] = 0;
  if (r != PGCARD_OK) return r;
  buf[got] = 0;
  for (i = 0; i + 1 < max && buf[i] != 0; i++) dst[i] = (char)buf[i];
  if (max) dst[i] = 0;
  return PGCARD_OK;
}

static unsigned length(const char *s) {
  unsigned n = 0;
  while (s[n]) n++;
  return n;
}

static void copy(char *dst, const char *src, unsigned max) {
  unsigned i;
  if (!max) return;
  for (i = 0; i + 1 < max && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

static int alive(void) {
  unsigned char magic;
  int r = read8(CMD_MAGIC, &magic);
  if (r != PGCARD_OK) return r;
  return magic == 0xDD ? PGCARD_OK : PGCARD_ENOCARD;
}

/* ---- the card ------------------------------------------------------------------- */

int pgcard_get_info(pgcard_info *info) {
  unsigned char w[2], idx;
  unsigned got;
  int r;
  info->fw[0] = 0;
  info->protocol = info->mode = info->has_cd = 0;
  info->cd_port = 0;
  if ((r = alive()) != PGCARD_OK) return r;
  if ((r = read8(CMD_PROTOCOL, &info->protocol)) != PGCARD_OK) return r;
  if ((r = read_string(CMD_FWSTRING, info->fw, sizeof(info->fw))) != PGCARD_OK) return r;
  if ((r = read8(CMD_BOOTMODE, &info->mode)) != PGCARD_OK) return r;
  /* firmware without CD-ROM emulation answers FFh for its registers */
  if ((r = read8(CMD_CDLOAD, &idx)) != PGCARD_OK) return r;
  info->has_cd = idx != NO_ANSWER;
  if ((r = request(CMD_CDPORT, PGCTL_WORD, 0, 0, 0, 0, w, 2, &got)) != PGCARD_OK) return r;
  if (got == 2) info->cd_port = (unsigned short)(w[0] | (w[1] << 8));
  return PGCARD_OK;
}

const char *pgcard_mode_name(unsigned char mode) {
  switch (mode) {
  case GUS_MODE:    return "GUS";
  case ADLIB_MODE:  return "AdLib";
  case MPU_MODE:    return "MPU-401";
  case PSG_MODE:    return "Tandy / CMS";
  case SB_MODE:     return "Sound Blaster";
  case USB_MODE:    return "USB";
  case NE2000_MODE: return "NE2000";
  default:          return "?";
  }
}

int pgcard_set_mode(unsigned char mode, int save, unsigned char *now) {
  unsigned waited;
  int r;
  if (mode < GUS_MODE || mode > USB_MODE) return PGCARD_EINVAL;
  if ((r = alive()) != PGCARD_OK) return r;
  if ((r = write8(CMD_BOOTMODE, mode)) != PGCARD_OK) return r;
  if (save && (r = write8(CMD_SAVE, 0xFF)) != PGCARD_OK) return r;
  if ((r = write8(CMD_REBOOT, 0xFF)) != PGCARD_OK) return r;
  pgcard_plat_sleep(REBOOT_FIRST_MS);
  for (waited = REBOOT_FIRST_MS;; waited += REBOOT_POLL_MS) {
    r = alive();
    if (r == PGCARD_OK) break;
    if (r != PGCARD_ENOCARD) return r;
    if (waited >= REBOOT_TIMEOUT_MS) return PGCARD_ETIMEOUT;
    pgcard_plat_sleep(REBOOT_POLL_MS);
  }
  return read8(CMD_BOOTMODE, now);
}

/* ---- CD images ------------------------------------------------------------------ */

static int cd_present(void) {
  unsigned char idx;
  int r = read8(CMD_CDLOAD, &idx);
  if (r != PGCARD_OK) return r;
  return idx == NO_ANSWER ? PGCARD_ENOTSUP : PGCARD_OK;
}

/* waits for the card's CD command; PGCARD_ECARD with the card's text */
static int cd_wait(char *err, unsigned errmax) {
  unsigned waited;
  unsigned char st;
  int r;
  if (errmax) err[0] = 0;
  for (waited = 0;; waited += CD_POLL_MS) {
    if ((r = read8(CMD_CDSTATUS, &st)) != PGCARD_OK) return r;
    if (st != CD_STATUS_BUSY) break;
    if (waited >= CD_TIMEOUT_MS) return PGCARD_ETIMEOUT;
    pgcard_plat_sleep(CD_POLL_MS);
  }
  if (st == (unsigned char)CD_STATUS_ERROR) {
    r = read_string(CMD_CDERROR, err, errmax);
    return r != PGCARD_OK ? r : PGCARD_ECARD;
  }
  return PGCARD_OK;
}

int pgcard_cd_current(unsigned char *index, char *name, unsigned max) {
  int r;
  *index = 0;
  if (max) name[0] = 0;
  if ((r = read8(CMD_CDLOAD, index)) != PGCARD_OK) return r;
  if (*index == NO_ANSWER) {
    *index = 0;
    return PGCARD_ENOTSUP;
  }
  if (*index == 0) return PGCARD_OK;
  return read_string(CMD_CDNAME, name, max);
}

int pgcard_cd_list(char *buf, unsigned size, unsigned *count, char *err, unsigned errmax) {
  unsigned char chunk[PGCTL_MAX_DATA];
  unsigned long total = 0;
  unsigned used = 0, start = 0, got, i;
  int full = 0, r;

  *count = 0;
  if (errmax) err[0] = 0;
  if ((r = cd_present()) != PGCARD_OK) return r;
  /* selecting the register starts the listing; the list may only be read
   * once the card says it has one */
  if ((r = request(CMD_CDLIST, 0, 0, 0, 0, 0, 0, 0, 0)) != PGCARD_OK) return r;
  if ((r = cd_wait(err, errmax)) != PGCARD_OK) return r;

  /* the list is read to its end even when buf is full: the card keeps it
   * until then */
  for (;;) {
    r = request(CMD_CDLIST, PGCTL_STOP, CD_EOT, 0, 0, total, chunk, sizeof(chunk), &got);
    if (r != PGCARD_OK) return r;
    if (got == 0) break;
    for (i = 0; i < got; i++) {
      unsigned char ch = chunk[i];
      if (ch == CD_EOT) return PGCARD_OK;
      if (full) continue;
      if (used >= size) {         /* this name does not fit: drop its start */
        used = start;
        full = 1;
        continue;
      }
      buf[used++] = (char)ch;
      if (ch == 0) {
        (*count)++;
        start = used;
      }
    }
    total += got;
    if (total > CD_LIST_MAX) return PGCARD_EIO;   /* not a list: no end in sight */
  }
  return PGCARD_OK;
}

int pgcard_cd_load(unsigned char index, char *err, unsigned errmax) {
  int r;
  if (errmax) err[0] = 0;
  if ((r = cd_present()) != PGCARD_OK) return r;
  if ((r = write8(CMD_CDLOAD, index)) != PGCARD_OK) return r;
  return cd_wait(err, errmax);
}

/* ---- WiFi ----------------------------------------------------------------------- */

int pgcard_wifi_status(char *text, unsigned max) {
  unsigned char buf[PGCARD_TEXT], zero = 0;
  unsigned got = 0, i, tries;
  int r;
  if (max) text[0] = 0;
  if ((r = alive()) != PGCARD_OK) return r;
  /* a write asks the card for a fresh status; the web server answers the
   * line it had until it has made the new one, so ask twice */
  if ((r = request(CMD_WIFISTAT, 0, 0, &zero, 1, 0, 0, 0, 0)) != PGCARD_OK) return r;
  pgcard_plat_sleep(WIFI_WAIT_MS);
  r = request(CMD_WIFISTAT, PGCTL_STOP, 0, &zero, 1, 0, buf, sizeof(buf) - 1, &got);
  /* the NE2000 firmware answers FFh while it collects the status */
  for (tries = 0; r == PGCARD_OK && got != 0 && buf[0] == NO_ANSWER; tries++) {
    if (tries == WIFI_RETRIES) return PGCARD_ETIMEOUT;
    pgcard_plat_sleep(WIFI_WAIT_MS / 2);
    r = request(CMD_WIFISTAT, PGCTL_STOP, 0, 0, 0, 0, buf, sizeof(buf) - 1, &got);
  }
  if (r != PGCARD_OK) return r;
  buf[got] = 0;
  if (buf[0] == 0) return PGCARD_ENOTSUP;
  for (i = 0; i + 1 < max && buf[i] != 0; i++) text[i] = (char)buf[i];
  if (max) text[i] = 0;
  return PGCARD_OK;
}

int pgcard_wifi_url(const char *status, char *url, unsigned max) {
  static const char scheme[] = "http://";
  unsigned i, j, n;
  if (max) url[0] = 0;
  for (i = 0; status[i]; i++) {
    for (j = 0; scheme[j] && status[i + j] == scheme[j]; j++) {}
    if (scheme[j]) continue;
    for (n = i; status[n] && status[n] != ' ' && status[n] != ','; n++) {}
    if (n - i + 1 > max) return 0;
    for (j = 0; j < n - i; j++) url[j] = status[i + j];
    url[j] = 0;
    return 1;
  }
  return 0;
}

int pgcard_wifi_set(const char *ssid, const char *pass, int save) {
  unsigned ls = length(ssid), lp = length(pass);
  char buf[PGCARD_PASS_MAX + 1];
  int r;
  if (ls == 0 || ls > PGCARD_SSID_MAX || lp > PGCARD_PASS_MAX) return PGCARD_EINVAL;
  if ((r = alive()) != PGCARD_OK) return r;
  copy(buf, ssid, sizeof(buf));
  if ((r = request(CMD_WIFISSID, 0, 0, (const unsigned char *)buf, ls + 1, 0, 0, 0, 0)) != PGCARD_OK) return r;
  copy(buf, pass, sizeof(buf));
  if ((r = request(CMD_WIFIPASS, 0, 0, (const unsigned char *)buf, lp + 1, 0, 0, 0, 0)) != PGCARD_OK) return r;
  if ((r = write8(CMD_WIFIAPPLY, 0)) != PGCARD_OK) return r;
  if (save && (r = write8(CMD_SAVE, 0xFF)) != PGCARD_OK) return r;
  return PGCARD_OK;
}
