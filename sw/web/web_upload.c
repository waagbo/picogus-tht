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

// File uploads from the web UI:
//  * firmware (.uf2), written to flash and then rebooted into
//  * MIDI system exclusive files (.syx), streamed to MIDI out at wire speed

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"

#include "web.h"
#include "web_internal.h"
#include "web_xip_sram.h"
#ifdef SOUND_MPU
#include "mpu401/export.h"
#endif

enum { UP_NONE = 0, UP_FIRMWARE = 1, UP_SYSEX = 2 };
enum { UP_IDLE = 0, UP_RUNNING = 1, UP_DONE = 2, UP_ERROR = 3 };

static web_upload_info_t info;
// The connection that owns the upload state, from begin() until finish()/abort()
static http_conn_t *owner;

void web_get_upload_info(web_upload_info_t *out) {
    *out = info;
}

static bool upload_busy(void) {
    return owner != NULL;
}

static void upload_start(http_conn_t *c, uint8_t kind, uint32_t total) {
    owner = c;
    info.kind = kind;
    info.state = UP_RUNNING;
    info.done = 0;
    info.total = total;
    info.error = NULL;
}

static void upload_fail(const char *error) {
    if (info.state == UP_RUNNING) {
        info.state = UP_ERROR;
        info.error = error;
    }
}

// ---- firmware (.uf2) ---------------------------------------------------------

#define UF2_MAGIC_START0            0x0A324655u
#define UF2_MAGIC_START1            0x9E5D5157u
#define UF2_MAGIC_END               0x0AB16F30u
#define UF2_FLAG_NOT_MAIN_FLASH     0x00000001u
#define UF2_FLAG_FAMILY_ID_PRESENT  0x00002000u
// Firmware for the chip this runs on (as PLATFORM_UF2_FAMILY_ID in
// system/platform.h, which the host tests can't include)
#if PICO_RP2350
#define FW_FAMILY_ID                0xe48bff59u     // RP2350, Arm Secure
#define FW_CHIP_NAME                "RP2350"
#else
#define FW_FAMILY_ID                0xe48bff56u     // RP2040
#define FW_CHIP_NAME                "RP2040"
#endif

// The last flash sector holds the settings: never overwritten by an upload
#define FW_SETTINGS_SECTOR  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FW_NUM_SECTORS      (PICO_FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t family_id;
    uint8_t data[476];
    uint32_t magic_end;
} uf2_block_t;

typedef struct {
    union {
        uint8_t raw[512];
        uf2_block_t uf2;
    } block;
    uint32_t fill;
    uint32_t written;           // pages written to flash
    bool flash_touched;         // flash was modified: the old firmware is gone
    uint8_t erased[FW_NUM_SECTORS / 8];
} fw_state_t;

// longest sysex message accepted: keeps the time other MIDI data waits short
#define SYX_MAX 512
typedef struct {
    uint8_t msg[SYX_MAX];
    uint32_t len;
    bool in_sysex;
    uint32_t messages;
} syx_state_t;

// Only one upload runs at a time: they share memory, in XIP cache SRAM
typedef union {
    fw_state_t fw;
    syx_state_t syx;
} upload_state_t;
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(upload_state_t) <= WEB_XIP_WEB_SIZE - 4096, "upload buffers too large");
#endif
static upload_state_t *up;
#define fw (up->fw)
#define syx (up->syx)

bool web_upload_init(void) {
    up = web_xip_alloc(sizeof(upload_state_t));
    return up != NULL;
}
static bool fw_locked;      // core 1 owns the flash for the firmware upload

static void fw_release(void) {
    if (fw_locked) {
        fw_locked = false;
        web_flash_release();
    }
}

// Once flash was modified the old firmware is gone: make that loud and clear
static void fw_fail(const char *error) {
    upload_fail(fw.flash_touched ? "firmware is incomplete - upload a valid file again before rebooting!" : error);
    fw_release();
}

int web_fw_begin(http_conn_t *c) {
    uint32_t len = http_content_length(c);
    if (upload_busy()) {
        return 409;
    }
    if (len == 0 || (len % 512) != 0 || len > 2 * PICO_FLASH_SIZE_BYTES) {
        return 400;
    }
    memset(&fw, 0, sizeof(fw));
    upload_start(c, UP_FIRMWARE, len / 512);
    // Core 0 must not save settings while flash is being rewritten
    web_flash_acquire_blocking();
    fw_locked = true;
    return 0;
}

// Validate and write one 512 byte UF2 block. Writing is done here, one block
// per call, so a single poll of the web server takes at most one sector erase
// (~50ms) - sound may stutter during a firmware update, which ends with a
// reboot anyway.
static void fw_process_block(void) {
    const uf2_block_t *b = &fw.block.uf2;
    if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 || b->magic_end != UF2_MAGIC_END) {
        fw_fail("not a UF2 file");
        return;
    }
    if ((b->flags & UF2_FLAG_NOT_MAIN_FLASH) ||
        !(b->flags & UF2_FLAG_FAMILY_ID_PRESENT) || b->family_id != FW_FAMILY_ID) {
        // Not for us (e.g. a block for another chip family in a combined UF2)
        return;
    }
    if (b->payload_size != FLASH_PAGE_SIZE || (b->target_addr & (FLASH_PAGE_SIZE - 1))
        || b->target_addr < XIP_BASE || b->target_addr >= XIP_BASE + PICO_FLASH_SIZE_BYTES) {
        fw_fail("UF2 block outside of flash");
        return;
    }
    uint32_t offset = b->target_addr - XIP_BASE;
    if (offset >= FW_SETTINGS_SECTOR) {
        return;     // keep the saved settings
    }
    uint32_t sector = offset / FLASH_SECTOR_SIZE;
    if (!(fw.erased[sector / 8] & (1u << (sector % 8)))) {
        // Point of no return once the first sector is erased. All firmware
        // runs from RAM, so the running sound card keeps working.
        fw.flash_touched = true;
        flash_range_erase(sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
        web_xip_cache_off();
        fw.erased[sector / 8] |= (uint8_t)(1u << (sector % 8));
    }
    // The flash routines must not read the data from the XIP block (where the
    // block buffer lives), so bounce it through the stack
    uint8_t page[FLASH_PAGE_SIZE];
    memcpy(page, b->data, FLASH_PAGE_SIZE);
    flash_range_program(offset, page, FLASH_PAGE_SIZE);
    web_xip_cache_off();
    ++fw.written;
}

int32_t web_fw_body(http_conn_t *c, const uint8_t *data, uint32_t len) {
    (void)c;
    if (info.state != UP_RUNNING) {
        return (int32_t)len;  // failed: discard the rest so the client gets our answer
    }
    uint32_t used = 0;
    while (used < len) {
        uint32_t n = 512 - fw.fill;
        if (n > len - used) {
            n = len - used;
        }
        memcpy(&fw.block.raw[fw.fill], data + used, n);
        fw.fill += n;
        used += n;
        if (fw.fill == 512) {
            fw.fill = 0;
            fw_process_block();
            ++info.done;
            // one block per call, the rest is offered again on the next poll
            break;
        }
    }
    return (int32_t)used;
}

void web_fw_finish(http_conn_t *c) {
    fw_release();
    owner = NULL;
    if (info.state == UP_RUNNING && fw.written == 0) {
        upload_fail("no " FW_CHIP_NAME " firmware found in file");
    }
    if (info.state != UP_RUNNING) {
        http_send_error(c, 400, info.error ? info.error : "upload failed");
        return;
    }
    info.state = UP_DONE;
    static const char ok[] = "{\"ok\":true,\"reboot\":true}";
    http_send_json(c, ok, sizeof(ok) - 1);
    // restart the mode that was running, now with the new firmware
    web_schedule_reboot(0, 1000);
}

void web_fw_abort(http_conn_t *c) {
    if (owner != c) {
        return;     // another request, refused while this upload ran
    }
    fw_fail("upload interrupted");
    owner = NULL;
}

// ---- MIDI --------------------------------------------------------------------

#ifdef SOUND_MPU
static uint32_t midi_msg_len(uint8_t status) {
    if (status < 0x80) return 0;
    if (status < 0xc0) return 3;
    if (status < 0xe0) return 2;
    if (status < 0xf0) return 3;
    switch (status) {
    case 0xf1: case 0xf3: return 2;
    case 0xf2: return 3;
    case 0xf4: case 0xf5: case 0xf7: case 0xf9: case 0xfd: return 0;  // undefined / stray EOX
    default: return 1;
    }
}

const char *web_midi_send(const uint8_t *msg, uint32_t len) {
    // Only complete, well formed messages may be queued: the MIDI out code
    // relies on that to keep injected and DOS program data from mixing
    uint32_t i = 0;
    while (i < len) {
        uint8_t status = msg[i];
        if (status == 0xf0) {
            uint32_t j = i + 1;
            while (j < len && msg[j] < 0x80) ++j;
            if (j >= len || msg[j] != 0xf7) return "unterminated sysex";
            i = j + 1;
            continue;
        }
        uint32_t n = midi_msg_len(status);
        if (!n) return "invalid status byte";
        if (i + n > len) return "truncated message";
        for (uint32_t k = 1; k < n; ++k) {
            if (msg[i + k] >= 0x80) return "invalid data byte";
        }
        i += n;
    }
    if (!MIDI_Inject(msg, len)) {
        return "MIDI queue full, try again";
    }
    return NULL;
}

// ---- sysex file (.syx) --------------------------------------------------------

int web_syx_begin(http_conn_t *c) {
    uint32_t len = http_content_length(c);
    if (upload_busy()) {
        return 409;
    }
    if (len == 0) {
        return 400;
    }
    memset(&syx, 0, sizeof(syx));
    upload_start(c, UP_SYSEX, len);
    return 0;
}

int32_t web_syx_body(http_conn_t *c, const uint8_t *data, uint32_t len) {
    (void)c;
    if (info.state != UP_RUNNING) {
        return (int32_t)len;
    }
    uint32_t i;
    for (i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (b == 0xf0) {
            syx.in_sysex = true;
            syx.len = 0;
        } else if (!syx.in_sysex) {
            continue;   // anything between messages is ignored
        } else if (b >= 0x80 && b != 0xf7) {
            syx.in_sysex = false;   // broken message, drop it
            continue;
        }
        if (syx.len >= SYX_MAX) {
            upload_fail("sysex message too long");
            return (int32_t)len;
        }
        syx.msg[syx.len++] = b;
        if (b == 0xf7) {
            if (!MIDI_Inject(syx.msg, syx.len)) {
                // queue full: take the 0xf7 again on the next poll
                --syx.len;
                break;
            }
            syx.in_sysex = false;
            ++syx.messages;
        }
    }
    info.done += i;
    return (int32_t)i;
}

void web_syx_finish(http_conn_t *c) {
    owner = NULL;
    if (info.state == UP_RUNNING && syx.messages == 0) {
        upload_fail("no sysex messages found in file");
    }
    if (info.state != UP_RUNNING) {
        http_send_error(c, 400, info.error ? info.error : "upload failed");
        return;
    }
    info.state = UP_DONE;
    char *p = web_scratch;
    const char *pre = "{\"ok\":true,\"messages\":";
    while (*pre) *p++ = *pre++;
    char num[11];
    char *n = num + sizeof(num) - 1;
    uint32_t v = syx.messages;
    *n = 0;
    do { *--n = '0' + v % 10; v /= 10; } while (v);
    while (*n) *p++ = *n++;
    *p++ = '}';
    http_send_json(c, web_scratch, (uint32_t)(p - web_scratch));
}

void web_syx_abort(http_conn_t *c) {
    if (owner != c) {
        return;
    }
    upload_fail("upload interrupted");
    owner = NULL;
}
#endif
