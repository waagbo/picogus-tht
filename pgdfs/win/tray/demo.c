/*
 * PGTRAY demo: the way to an emulated card (make demo)
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * Takes the place of backend.c: the control requests run against the setup
 * registers of the host tests (../test/emuctl.c), a Sound Blaster mode card
 * with a few CD images and a web server. For working on the window on any
 * Windows, or under Wine, without a PicoGUS.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PICOGUS_NO_MODENAMES
#include "../../../common/picogus.h"
#include "pgcard.h"
#include "backend.h"
#include "emuctl.h"

static int started;

/* the little of the C library the emulated card uses */
char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++) != 0) {}
  return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
  char *d = dst + lstrlenA(dst);
  while (n-- && *src) *d++ = *src++;
  *d = 0;
  return dst;
}

char *strcat(char *dst, const char *src) {
  return strncat(dst, src, (size_t)-1);
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

static void start(void) {
  static const char *const images[] = {
    "Doom II.cue", "Monkey Island 2.iso", "QUAKE.CUE", "Tomb Raider (Disc 1).cue", "Tomb Raider (Disc 2).cue",
    "Windows 98 SE.iso",
  };
  unsigned i;
  if (started) return;
  started = 1;
  emuctl_reset();
  emuctl.fw = "PicoGUS v4.2.0+cdrom-subdir+pgdfs+webserver";
  for (i = 0; i < sizeof(images) / sizeof(images[0]); i++) emuctl.image[i] = images[i];
  emuctl.images = (int)i;
  emuctl.busy_polls = 2;
  emuctl.has_wifi = 1;
  emuctl.wifi_status = "Connected to SSID Retro, Signal -55 dB, web UI: http://192.168.1.42/";
}

int backend_kind(void) {
  return BACKEND_DEMO;
}

const char *backend_name(void) {
  return "nothing: this is the demo with an emulated card";
}

int pgcard_plat_ctl(const pgctl_req *rq, const unsigned char *wr, unsigned char *rd, unsigned *got) {
  unsigned long k;
  unsigned i, n = 0;
  start();
  *got = 0;
  emuctl_select(rq->reg);
  for (i = 0; i < rq->nwrite; i++) {
    if ((rq->flags & PGCTL_WORD) && i == 0) emuctl_write_low(rq->reg, wr[i]);
    else emuctl_write_high(rq->reg, wr[i]);
  }
  for (k = 0; k < rq->skip; k++) {
    if (emuctl_read_high(rq->reg) == rq->stop && (rq->flags & PGCTL_STOP)) return PGCARD_OK;
  }
  for (i = 0; i < rq->nread; i++) {
    unsigned char b = ((rq->flags & PGCTL_WORD) && i == 0 && rq->skip == 0) ? emuctl_read_low(rq->reg)
                                                                              : emuctl_read_high(rq->reg);
    rd[n++] = b;
    if (b == rq->stop && (rq->flags & PGCTL_STOP)) break;
  }
  *got = n;
  return PGCARD_OK;
}

void pgcard_plat_sleep(unsigned ms) {
  Sleep(ms);
  emuctl_task();
}
