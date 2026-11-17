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

// Song recorder, FM: OPL2/OPL3 register writes to DOSBox raw OPL files
// (.DRO, version 2.0), as DOSBox's own capture writes them.
//
// A song starts with the first note that is keyed on. Like DOSBox, the file
// then begins with the registers as they are at that moment (the instruments
// were usually set up before), followed by every write with its time. A song
// ends when, for the configured time, nothing was written while a note was
// on: what a program writes while the chip is silent (the instruments of the
// next song, say) does not keep a song going. The file is then cut back to
// the last write that counted.

#include <stdlib.h>
#include <string.h>

#include "capture_internal.h"

#ifdef SOUND_OPL

// The registers a DRO file can hold have codes 0..121 (see opl_code()); the
// next two codes are the delays
#define OPL_CODES           122
#define DRO_DELAY256        OPL_CODES           // value + 1 ms
#define DRO_DELAY_SHIFT8    (OPL_CODES + 1)     // (value + 1) * 256 ms
#define DRO_HEADER_SIZE     26
#define DRO_COUNTS_OFFSET   12      // commands, milliseconds, hardware

typedef struct {
    capture_ring_t ring;
    cap_file_t f;
    uint8_t state;                  // CAPTURE_WAITING or CAPTURE_RECORDING
    bool opl3;                      // the file has writes to the second register set
    uint32_t kon;                   // notes that are on: bits 0-17 channels, 18-22 rhythm
    uint32_t count;                 // command pairs written
    // where the file ends if nothing more happens
    uint32_t end_pos, end_count, end_ms;
    uint8_t shadow[2 * OPL_CODES];  // last value written to each register
} cap_opl_t;

static cap_opl_t *opl;

// Code of a register in the file: the table of DOSBox's capture. -1 for
// registers that don't exist.
static int opl_code(uint32_t r) {
    switch (r) {
    case 0x01: return 0;    // waveform select enable
    case 0x04: return 1;    // 104h: four operator enable
    case 0x05: return 2;    // 105h: OPL3 enable
    case 0x08: return 3;    // CSW, note select
    case 0xbd: return 4;    // tremolo/vibrato depth, rhythm
    }
    // the operators: 20h, 40h, 60h, 80h and E0h, +0..5, +8..13, +16..21
    uint32_t hi = r & 0xe0, i = r & 0x1f;
    if (i < 24 && (i & 7) < 6) {
        int group = hi == 0x20 ? 0 : hi == 0x40 ? 1 : hi == 0x60 ? 2 : hi == 0x80 ? 3 : hi == 0xe0 ? 4 : -1;
        if (group >= 0) {
            return 5 + (int)((i >> 3) * 6 + (i & 7)) * 5 + group;
        }
    }
    // the channels: A0h, B0h and C0h, +0..8
    hi = r & 0xf0;
    i = r & 0x0f;
    if (i < 9 && hi >= 0xa0 && hi <= 0xc0) {
        return 95 + (int)i * 3 + (int)((hi - 0xa0) >> 4);
    }
    return -1;
}

static bool opl_pair(cap_opl_t *o, uint32_t code, uint32_t value) {
    uint8_t pair[2] = { (uint8_t)code, (uint8_t)value };
    if (!cap_write(&o->f, pair, 2)) {
        return false;
    }
    ++o->count;
    return true;
}

static bool opl_delay(cap_opl_t *o, uint32_t ms) {
    while (ms) {
        uint32_t n;
        if (ms >= 256) {
            n = ms >> 8;
            if (n > 256) {
                n = 256;
            }
            ms -= n << 8;
            if (!opl_pair(o, DRO_DELAY_SHIFT8, n - 1)) {
                return false;
            }
        } else {
            n = ms;
            ms = 0;
            if (!opl_pair(o, DRO_DELAY256, n - 1)) {
                return false;
            }
        }
    }
    return true;
}

static bool opl_patch_counts(cap_opl_t *o) {
    uint8_t h[9];
    for (int i = 0; i < 4; ++i) {
        h[i] = (uint8_t)(o->count >> (8 * i));
        h[4 + i] = (uint8_t)(o->f.ms >> (8 * i));
    }
    h[8] = o->opl3 ? 2 : 0;     // hardware: OPL2 or OPL3
    return cap_patch(&o->f, DRO_COUNTS_OFFSET, h, sizeof(h));
}

// Start a file: header, then the registers as they are now, silent
static bool opl_begin(cap_opl_t *o, cap_tick_t t) {
    uint8_t h[DRO_HEADER_SIZE + OPL_CODES];
    if (!cap_create(&o->f, CAPTURE_OPL, t)) {
        return false;
    }
    memset(h, 0, sizeof(h));
    memcpy(h, "DBRAWOPL", 8);
    h[8] = 2;                       // version 2.0
    h[23] = DRO_DELAY256;
    h[24] = DRO_DELAY_SHIFT8;
    h[25] = OPL_CODES;
    for (uint32_t r = 0; r < 256; ++r) {
        int code = opl_code(r);
        if (code >= 0) {
            h[DRO_HEADER_SIZE + code] = (uint8_t)r;
        }
    }
    if (!cap_write(&o->f, h, sizeof(h))) {
        return false;
    }
    o->count = 0;
    o->opl3 = false;
    // OPL3 enable first: without it the second register set can't be written
    if (o->shadow[OPL_CODES + 2]) {
        o->opl3 = true;
        if (!opl_pair(o, 0x80 | 2, o->shadow[OPL_CODES + 2])) {
            return false;
        }
    }
    for (uint32_t i = 0; i < 2 * OPL_CODES; ++i) {
        uint32_t code = i % OPL_CODES, bank = i / OPL_CODES, v = o->shadow[i];
        if (i == OPL_CODES + 2) {
            continue;
        }
        if (code >= 95 && (code - 95) % 3 == 1) {
            v &= ~0x20u;            // B0h-B8h: key off
        } else if (code == 4 && !bank) {
            v &= ~0x1fu;            // BDh: rhythm instruments off
        }
        if (v) {
            o->opl3 |= bank != 0;
            if (!opl_pair(o, code | bank << 7, v)) {
                return false;
            }
        }
    }
    o->state = CAPTURE_RECORDING;
    return true;
}

void cap_opl_event(cap_tick_t t, uint32_t reg, uint32_t value) {
    cap_opl_t *o = opl;
    uint32_t r = reg & 0xff, bank = reg >> 8;
    if (!bank && r >= 2 && r <= 4) {
        return;                     // the timers make no sound
    }
    int code = opl_code(r);
    if (code < 0) {
        return;
    }
    uint32_t was_on = o->kon;
    if (r >= 0xb0 && r <= 0xb8) {
        uint32_t bit = 1u << (r - 0xb0 + bank * 9);
        o->kon = (value & 0x20) ? o->kon | bit : o->kon & ~bit;
    } else if (r == 0xbd && !bank) {
        o->kon = (o->kon & 0x3ffffu) | ((value & 0x20) ? (value & 0x1f) << 18 : 0);
    }
    bool key_on = (o->kon & ~was_on) != 0;
    o->shadow[(uint32_t)code + bank * OPL_CODES] = (uint8_t)value;

    if (o->state != CAPTURE_RECORDING) {
        if (!key_on || !opl_begin(o, t)) {
            return;
        }
    }
    o->opl3 |= bank != 0;
    if (!opl_delay(o, cap_delta_ms(&o->f, t, 0)) || !opl_pair(o, (uint32_t)code | bank << 7, value)) {
        return;
    }
    if (key_on || was_on) {
        // this made or changed sound: the song goes on, at least up to here
        o->f.t_active = t;
        o->end_pos = (uint32_t)f_tell(&o->f.fil);
        o->end_count = o->count;
        o->end_ms = o->f.ms;
    }
}

static void opl_end(cap_opl_t *o, bool abandon, bool drive_gone) {
    if (o->state != CAPTURE_RECORDING) {
        return;
    }
    o->state = CAPTURE_WAITING;
    if (abandon) {
        cap_drop(&o->f, drive_gone);
        return;
    }
    bool keep = !cap_cfg.split || (cap_tick_t)(o->f.t_active - o->f.t_start) >= CAP_TICKS(cap_cfg.minlen);
    if (keep) {
        // Cut back to the last sound, and give it time to ring out
        if (f_lseek(&o->f.fil, o->end_pos) != FR_OK || f_truncate(&o->f.fil) != FR_OK) {
            cap_fail("Can't write to the USB drive.");
        }
        o->count = o->end_count;
        o->f.ms = o->end_ms + CAP_TAIL_MS;
        keep = opl_delay(o, CAP_TAIL_MS) && opl_patch_counts(o);
        if (!keep) {
            cap_drop(&o->f, false);     // as far as it got
            return;
        }
    }
    cap_close(&o->f, keep);
}

void cap_opl_poll(cap_tick_t now) {
    cap_opl_t *o = opl;
    if (o->state != CAPTURE_RECORDING) {
        return;
    }
    if (cap_cfg.split && (cap_tick_t)(now - o->f.t_active) >= CAP_TICKS(cap_cfg.gap)) {
        opl_end(o, false, false);
    } else if (o->f.dirty && (cap_tick_t)(now - o->f.t_sync) >= CAP_SYNC_TICKS) {
        if (opl_patch_counts(o)) {
            cap_sync(&o->f, now);
        }
    }
}

void cap_opl_stop(bool cut, bool abandon, bool drive_gone) {
    (void)cut;      // what follows starts a new file at its first note anyway
    if (opl) {
        opl_end(opl, abandon, drive_gone);
    }
}

capture_ring_t *cap_opl_start(void) {
    if (!opl) {
        opl = calloc(1, sizeof(*opl));
        if (!opl) {
            return NULL;
        }
    }
    // The registers are only known from what is seen while recording: start
    // from a silent chip
    opl->state = CAPTURE_WAITING;
    opl->kon = 0;
    memset(opl->shadow, 0, sizeof(opl->shadow));
    return &opl->ring;
}

const cap_file_t *cap_opl_file(uint8_t *state) {
    if (!opl) {
        *state = CAPTURE_OFF;
        return NULL;
    }
    *state = opl->state;
    return &opl->f;
}

#endif // SOUND_OPL
