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

// Internal interfaces shared between the web server source files (core 1 only)

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- web_main.c ------------------------------------------------------------

typedef struct {
    int8_t link;            // CYW43_LINK_* of the station interface
    int32_t rssi;           // dBm, 0 if unknown
    uint32_t ip;            // IPv4 address in network byte order, 0 if none
    uint8_t mac[6];
    const char *ssid;
    uint32_t poll_max_us;   // longest single web_task() call since boot
    uint32_t poll_max_recent_us; // longest web_task() call in the last ~10s
    uint16_t load_permille; // share of wall clock time spent in web_task()
} web_net_info_t;

void web_get_net_info(web_net_info_t *info);
// Reboot after the current HTTP responses had a chance to go out.
// mode: card_mode_t to boot into (multifw), or 0 for the mode that is running
void web_schedule_reboot(uint8_t mode, uint32_t delay_ms);
// Core 1 ownership of the flash (nests). While core 1 owns it, core 0 does not
// erase or write flash, so core 1 may read flash (web page, WiFi firmware) or
// write it (firmware upload).
void web_flash_acquire_blocking(void);
bool web_flash_try_acquire(void);
void web_flash_release(void);
// Zeroed memory from the web server's part of the XIP cache SRAM (see
// web_xip_sram.h). Only during startup; NULL when out of space.
void *web_xip_alloc(size_t size);
// The boot ROM's flash routines turn the XIP cache on: call after each one
void web_xip_cache_off(void);
// Flash address -> its alias that bypasses the XIP cache (other pointers unchanged)
const void *web_flash_uncached(const void *p);

// ---- web_http.c ------------------------------------------------------------

typedef struct http_conn http_conn_t;

enum { HTTP_GET = 1, HTTP_POST = 2 };

typedef struct {
    const char *path;
    uint8_t method;
    // Called once the request headers are parsed. Return 0 to accept the body
    // or an HTTP status code to reject the request.
    int (*begin)(http_conn_t *c);
    // Streaming body consumer (uploads). Returns the number of bytes consumed;
    // fewer than len applies TCP backpressure and the rest is offered again on
    // a later poll. NULL = small form body, collected for finish().
    int32_t (*body)(http_conn_t *c, const uint8_t *data, uint32_t len);
    // Called once the whole body was received. Must send a response.
    void (*finish)(http_conn_t *c);
    // Called if the connection dies before finish() (uploads)
    void (*abort)(http_conn_t *c);
} web_route_t;

bool web_http_init(void);   // allocate buffers
bool web_http_start(void);
void web_http_poll(void);

// request accessors
const char *http_form_body(http_conn_t *c);   // urlencoded body (NUL terminated)
uint32_t http_content_length(http_conn_t *c);
// find "key" in an urlencoded string, decode its value into buf. false if missing.
bool http_form_get(const char *form, const char *key, char *buf, size_t buflen);
bool http_form_get_int(const char *form, const char *key, int32_t *out);

// responses (exactly one per request)
void http_send_json(http_conn_t *c, const char *json, uint32_t len);
void http_send_error(http_conn_t *c, int status, const char *message);

// Scratch buffer for building responses (core 1 only, not reentrant)
#define WEB_SCRATCH_SIZE 2048
extern char *web_scratch;

// ---- web_assets.c ----------------------------------------------------------

// A file of the web page, in flash
typedef struct {
    const char *type;       // content type
    const uint8_t *data;
    uint32_t length;
    uint32_t etag;          // changes when the file does
    bool gzip;              // data is gzip compressed
} web_asset_t;

// Index of the file served at a path, -1 if there is none
int web_asset_find(const char *path);
bool web_asset_get(int index, web_asset_t *asset);

// ---- web_api.cpp -----------------------------------------------------------

extern const web_route_t web_routes[];
extern const uint32_t web_routes_count;

// ---- web_upload.c ----------------------------------------------------------

bool web_upload_init(void);  // allocate buffers
int web_fw_begin(http_conn_t *c);
int32_t web_fw_body(http_conn_t *c, const uint8_t *data, uint32_t len);
void web_fw_finish(http_conn_t *c);
void web_fw_abort(http_conn_t *c);
int web_syx_begin(http_conn_t *c);
int32_t web_syx_body(http_conn_t *c, const uint8_t *data, uint32_t len);
void web_syx_finish(http_conn_t *c);
void web_syx_abort(http_conn_t *c);
// Progress of the current/last upload for the status page
typedef struct {
    uint8_t kind;           // 0 none, 1 firmware, 2 sysex
    uint8_t state;          // 0 idle, 1 running, 2 done, 3 error
    uint32_t done;          // bytes or blocks processed
    uint32_t total;
    const char *error;
} web_upload_info_t;
void web_get_upload_info(web_upload_info_t *info);

// Validate a buffer of complete MIDI messages and queue it for MIDI out.
// Returns NULL on success, or an error message.
const char *web_midi_send(const uint8_t *msg, uint32_t len);

#ifdef __cplusplus
}
#endif
