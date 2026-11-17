/*
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
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

// Web server for PicoGUS on Pico W (CYW43) boards.
//
// Design rules (the sound card always wins):
//  * Everything network related runs on core 1, in thread mode, from web_task().
//    Audio, PIC timers and USB are interrupt driven or run in the same loop, so
//    they preempt or share time with the web server, never the other way round.
//  * Core 0 (the ISA bus loop) only ever touches a handful of flags through the
//    non-blocking functions marked "core 0" below.
//  * The CYW43 driver runs in lwIP "poll" mode: no background interrupts besides
//    the tiny host-wake GPIO handler, which runs at the lowest NVIC priority.
//  * web_task() is rate limited and each call does a bounded amount of work.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- core 0 (ISA loop) hooks: all non-blocking ----------------------------

// Call once from main() on core 0 before core 1 is launched.
void web_init_core0(void);
// Call from main() on core 0 once all PIO/DMA resources have been claimed.
// WiFi is not brought up before this, so the CYW43 driver can never grab a
// state machine that the ISA bus code expects to be free.
void web_core0_ready(void);
// pgusinit /wifissid + /wifipass were sent: reconnect with the new credentials
void web_wifi_request_connect(void);
// pgusinit /wifistatus: latch the current status string for reading
void web_wifi_request_status(void);
// pgusinit /wifistatus: next character of the status string (0 terminates)
char web_wifi_read_status_char(void);

// Flash access arbitration between core 0 (settings save from pgusinit) and
// core 1 (reading the CYW43 firmware blob, firmware upload from the web).
// Returns false if core 1 is currently using the flash; retry later.
bool web_flash_try_lock(void);
void web_flash_unlock(void);

// ---- core 1 --------------------------------------------------------------

// Call from the core 1 main loop of every mode. Cheap when there is no work.
void web_task(void);

// ---- provided by picogus.cpp, used by the web API (core 1) -----------------

// Apply one PicoGUS control register value, exactly like pgusinit would.
// Ports and the mouse sensitivity take their full 16 bit value. Returns false
// if the register may not be written from the web UI.
bool picogus_web_write(uint8_t reg, uint16_t value);
// Ask core 0 to save settings to flash (done in the same place pgusinit's
// /save is handled, so both paths behave identically).
void picogus_request_save(void);
bool picogus_save_pending(void);
// Reset settings to defaults (same as pgusinit /defaults)
void picogus_load_defaults(void);
extern const char *firmware_string;

#ifdef __cplusplus
}
#endif
