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

// Song recorder, MIDI: the bytes sent to the MIDI output to standard MIDI
// files (format 0, one tick per millisecond, as DOSBox's capture writes them).
//
// A file begins with the state of the channels as far as it is known
// (programs, the usual controllers, pitch bend), then holds what the program
// sends before its first note (set-up: SysEx, programs, controllers) without
// the waiting in between, and from the first note on everything with its
// time. A song ends when, for the configured time, no note was on and none
// started. What is sent while no note sounds is held back: it belongs to the
// same song if a note follows in time, and starts the next file otherwise, or
// when it is more than a pause in a song would hold. Realtime and system
// common messages are not recorded.

#include <stdlib.h>
#include <string.h>

#include "capture_internal.h"

#ifdef SOUND_MPU

#define SYSEX_MAX       320     // longer SysEx messages are written in parts
#define HOLD_MAX        256     // bytes held back while no note sounds
#define MTRK_LEN_OFFSET 18
#define MTRK_DATA       22      // where the track's events start
// Before the first note, waits are cut down to this
#define SETUP_MAX_TICKS (CAP_TICKS(1) / 10)

// CAPTURE_WAITING: no file. Then:
#define MIDI_SETUP      (CAPTURE_RECORDING + 1)     // a file with set-up, no note yet

// The controllers a file starts with, in this order; then the pitch bend range
static const uint8_t cc_list[] = { 0, 32, 7, 10, 11, 1, 64, 91, 93, 101, 100 };
#define NCC         ((int)sizeof(cc_list))
#define CC_RPN_MSB  9
#define CC_RPN_LSB  10
#define CC_RANGE    NCC         // not a controller: data entry for RPN 0
#define UNSEEN      0xff

typedef struct {
    capture_ring_t ring;
    cap_file_t f;
    uint8_t state;
    // the message being received
    uint8_t status, need, have, data[2];
    bool in_sysex, sysex_cont;
    // a file with SysEx is kept however short it is: later songs may need it
    bool keep, hold_keep;
    uint16_t sysex_len;
    uint16_t hold_len;
    uint16_t sounding;              // notes that are on
    uint32_t notes[16][4];
    // state of the channels, UNSEEN where nothing was sent
    uint8_t prog[16];
    uint8_t cc[16][NCC + 1];
    uint16_t bend[16];
    uint8_t sysex[SYSEX_MAX + 1];
    uint8_t hold[HOLD_MAX];
} cap_midi_t;

static cap_midi_t *midi;

static uint32_t varlen(uint8_t *out, uint32_t v) {
    uint8_t tmp[5];
    uint32_t n = 0;
    do {
        tmp[n++] = v & 0x7f;
        v >>= 7;
    } while (v);
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = (uint8_t)(tmp[n - 1 - i] | (i + 1 < n ? 0x80 : 0));
    }
    return n;
}

static bool midi_channel_event(cap_midi_t *m, uint32_t status, uint32_t d1, uint32_t d2) {
    uint8_t ev[4] = { 0, (uint8_t)status, (uint8_t)d1, (uint8_t)d2 };
    return cap_write(&m->f, ev, (status & 0xe0) == 0xc0 ? 3 : 4);
}

// The state of the channels, at the start of a file
static bool midi_snapshot(cap_midi_t *m) {
    for (uint32_t ch = 0; ch < 16; ++ch) {
        const uint8_t *cc = m->cc[ch];
        for (int i = 0; i < NCC; ++i) {
            if (i == 2 && m->prog[ch] != UNSEEN && !midi_channel_event(m, 0xc0 | ch, m->prog[ch], 0)) {
                return false;       // the program, after its bank
            }
            if (i == CC_RPN_MSB && cc[CC_RANGE] != UNSEEN
                && !(midi_channel_event(m, 0xb0 | ch, 101, 0) && midi_channel_event(m, 0xb0 | ch, 100, 0)
                     && midi_channel_event(m, 0xb0 | ch, 6, cc[CC_RANGE]))) {
                return false;       // the pitch bend range, before the parameter that is selected now
            }
            if (cc[i] != UNSEEN && !midi_channel_event(m, 0xb0 | ch, cc_list[i], cc[i])) {
                return false;
            }
        }
        if (m->bend[ch] != 0xffff && !midi_channel_event(m, 0xe0 | ch, m->bend[ch] & 0x7f, m->bend[ch] >> 7)) {
            return false;
        }
    }
    return true;
}

// Start a file: header, the channels as they are, and what was held back
static bool midi_begin(cap_midi_t *m, cap_tick_t t) {
    static const uint8_t header[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xf4,     // format 0, 500 ticks per quarter note
        'M', 'T', 'r', 'k', 0, 0, 0, 0,
        0, 0xff, 0x51, 3, 0x07, 0xa1, 0x20,                         // 500000 us per quarter note
    };
    if (!cap_create(&m->f, CAPTURE_MIDI, t)) {
        return false;
    }
    if (!cap_write(&m->f, header, sizeof(header)) || !midi_snapshot(m)) {
        return false;
    }
    m->keep = false;
    if (m->hold_len) {
        // its first event was timed from the end of the song before
        uint32_t skip = 0;
        while (m->hold[skip] & 0x80) {
            ++skip;
        }
        m->hold[skip] = 0;
        bool ok = cap_write(&m->f, m->hold + skip, m->hold_len - skip);
        m->hold_len = 0;
        m->keep = m->hold_keep;
        m->hold_keep = false;
        if (!ok) {
            return false;
        }
    }
    m->state = MIDI_SETUP;
    return true;
}

static bool midi_patch_length(cap_midi_t *m) {
    uint32_t len = (uint32_t)f_tell(&m->f.fil) - MTRK_DATA;
    uint8_t be[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    return cap_patch(&m->f, MTRK_LEN_OFFSET, be, 4);
}

static void midi_end(cap_midi_t *m, bool abandon, bool drive_gone) {
    if (m->state == CAPTURE_WAITING) {
        return;
    }
    bool song = m->state == CAPTURE_RECORDING;
    m->state = CAPTURE_WAITING;
    m->sounding = 0;
    memset(m->notes, 0, sizeof(m->notes));
    if (abandon) {
        cap_drop(&m->f, drive_gone);
        m->hold_len = 0;
        m->hold_keep = false;
        return;
    }
    // set-up that no note followed is not a song
    bool keep = song && (m->keep || !cap_cfg.split
                         || (cap_tick_t)(m->f.t_active - m->f.t_start) >= CAP_TICKS(cap_cfg.minlen));
    if (keep) {
        // end of track, after time for the last notes to ring out
        uint8_t eot[5];
        uint32_t n = varlen(eot, CAP_TAIL_MS);
        eot[n++] = 0xff;
        eot[n++] = 0x2f;
        eot[n++] = 0;
        if (!cap_write(&m->f, eot, n) || !midi_patch_length(m)) {
            cap_drop(&m->f, false);     // as far as it got
            return;
        }
    }
    cap_close(&m->f, keep);
}

// The end of the song: what was held back since starts the next file
static void midi_next_song(cap_midi_t *m, cap_tick_t t) {
    midi_end(m, false, false);
    if (m->hold_len) {
        midi_begin(m, t);
    }
}

// One event of the track: ev (status and, for SysEx, its length) and body.
// sounding: a note was on before this event.
static void midi_out(cap_midi_t *m, cap_tick_t t, const uint8_t *ev, uint32_t evlen,
                     const uint8_t *body, uint32_t bodylen, bool note_on, bool sounding, bool sysex) {
    if (m->state == CAPTURE_WAITING && !midi_begin(m, t)) {
        return;
    }
    // While no note sounds, what is sent may belong to the next song
    bool hold = m->state == CAPTURE_RECORDING && cap_cfg.split && !note_on && !sounding;
    if (hold && m->hold_len + 5 + evlen + bodylen > HOLD_MAX) {
        // more than a pause in a song would hold: this is the next song
        midi_next_song(m, t);
        if (m->state == CAPTURE_WAITING && !midi_begin(m, t)) {
            return;
        }
        hold = false;
    }
    if (!hold && m->hold_len) {
        // the song goes on: what was held back is part of it
        bool ok = cap_write(&m->f, m->hold, m->hold_len);
        m->hold_len = 0;
        m->keep |= m->hold_keep;
        m->hold_keep = false;
        if (!ok) {
            return;
        }
    }
    uint8_t delta[5];
    uint32_t n = varlen(delta, cap_delta_ms(&m->f, t, m->state == MIDI_SETUP ? SETUP_MAX_TICKS : 0));
    if (hold) {
        memcpy(m->hold + m->hold_len, delta, n);
        memcpy(m->hold + m->hold_len + n, ev, evlen);
        if (bodylen) {
            memcpy(m->hold + m->hold_len + n + evlen, body, bodylen);
        }
        m->hold_len += (uint16_t)(n + evlen + bodylen);
        m->hold_keep |= sysex;
        return;
    }
    if (!cap_write(&m->f, delta, n) || !cap_write(&m->f, ev, evlen) || (bodylen && !cap_write(&m->f, body, bodylen))) {
        return;
    }
    m->keep |= sysex;
    if (note_on && m->state == MIDI_SETUP) {
        m->state = CAPTURE_RECORDING;
        m->f.t_start = t;
    }
    if (note_on || sounding) {
        m->f.t_active = t;
    }
}

// A SysEx message, or as much of it as there is. A message that is longer
// than the buffer goes into the file in parts, as the file format has it:
// F0 <length> <data>, then F7 <length> <data> for each further part.
static void midi_sysex(cap_midi_t *m, cap_tick_t t, bool complete) {
    uint8_t ev[6];
    if (complete) {
        m->sysex[m->sysex_len++] = 0xf7;
    }
    ev[0] = m->sysex_cont ? 0xf7 : 0xf0;
    uint32_t n = 1 + varlen(ev + 1, m->sysex_len);
    midi_out(m, t, ev, n, m->sysex, m->sysex_len, false, m->sounding != 0, true);
    m->sysex_cont = !complete;
    m->sysex_len = 0;
}

static void midi_message(cap_midi_t *m, cap_tick_t t, uint32_t status, uint32_t d1, uint32_t d2) {
    uint32_t ch = status & 15, type = status & 0xf0;
    bool note_on = false, sounding = m->sounding != 0;
    if (type == 0x90 || type == 0x80) {
        uint32_t *word = &m->notes[ch][d1 >> 5], bit = 1u << (d1 & 31);
        if (type == 0x90 && d2) {
            note_on = true;
            if (!(*word & bit)) {
                *word |= bit;
                ++m->sounding;
            }
        } else if (*word & bit) {
            *word &= ~bit;
            --m->sounding;
        }
    } else if (type == 0xc0) {
        m->prog[ch] = (uint8_t)d1;
    } else if (type == 0xe0) {
        m->bend[ch] = (uint16_t)(d1 | d2 << 7);
    } else if (type == 0xb0) {
        uint8_t *cc = m->cc[ch];
        if (d1 == 6 && cc[CC_RPN_MSB] == 0 && cc[CC_RPN_LSB] == 0) {
            cc[CC_RANGE] = (uint8_t)d2;
        } else if (d1 == 121) {
            // reset all controllers: modulation, expression, sustain, RPN, pitch bend
            cc[4] = cc[5] = cc[6] = cc[CC_RPN_MSB] = cc[CC_RPN_LSB] = UNSEEN;
            m->bend[ch] = 0xffff;
        } else {
            for (int i = 0; i < NCC; ++i) {
                if (cc_list[i] == d1) {
                    cc[i] = (uint8_t)d2;
                }
            }
        }
    }
    uint8_t ev[3] = { (uint8_t)status, (uint8_t)d1, (uint8_t)d2 };
    midi_out(m, t, ev, type == 0xc0 || type == 0xd0 ? 2 : 3, NULL, 0, note_on, sounding, false);
    // all sound off, all notes off (and the mode messages, which imply it):
    // after the message, so that it counts as part of the song
    if (type == 0xb0 && (d1 == 120 || d1 >= 123)) {
        for (int i = 0; i < 4; ++i) {
            m->sounding -= (uint16_t)__builtin_popcount(m->notes[ch][i]);
            m->notes[ch][i] = 0;
        }
    }
}

void cap_midi_byte(cap_tick_t t, uint32_t b) {
    cap_midi_t *m = midi;
    if (b >= 0xf8) {
        return;                     // realtime: may come in the middle of a message
    }
    if (b & 0x80) {
        if (m->in_sysex) {
            // F7 ends a SysEx message, and so does any other status byte
            m->in_sysex = false;
            midi_sysex(m, t, true);
        }
        m->status = 0;
        if (b == 0xf0) {
            m->in_sysex = true;
            m->sysex_cont = false;
            m->sysex_len = 0;
        } else if (b < 0xf0) {
            m->status = (uint8_t)b;
            m->need = (b & 0xe0) == 0xc0 ? 1 : 2;
            m->have = 0;
        }
        return;
    }
    if (m->in_sysex) {
        if (m->sysex_len == SYSEX_MAX) {
            midi_sysex(m, t, false);
        }
        m->sysex[m->sysex_len++] = (uint8_t)b;
    } else if (m->status) {
        m->data[m->have++] = (uint8_t)b;
        if (m->have == m->need) {
            m->have = 0;            // running status
            midi_message(m, t, m->status, m->data[0], m->need == 2 ? m->data[1] : 0);
        }
    }
}

void cap_midi_poll(cap_tick_t now) {
    cap_midi_t *m = midi;
    if (m->state == CAPTURE_WAITING) {
        return;
    }
    if (m->state == CAPTURE_RECORDING && cap_cfg.split && (cap_tick_t)(now - m->f.t_active) >= CAP_TICKS(cap_cfg.gap)) {
        midi_next_song(m, now);
    } else if (m->f.dirty && (cap_tick_t)(now - m->f.t_sync) >= CAP_SYNC_TICKS) {
        // The file on the drive then holds every event so far. Its end of
        // track is only written when the song ends: what is on the drive
        // must not change again, or a file left behind by a power cut could
        // end in the middle of an event.
        if (midi_patch_length(m)) {
            cap_sync(&m->f, now);
        }
    }
}

void cap_midi_stop(bool cut, bool abandon, bool drive_gone) {
    cap_midi_t *m = midi;
    if (!m) {
        return;
    }
    if (cut) {
        // a new file for what follows; set-up without a note yet stays as it is
        if (m->state == CAPTURE_RECORDING) {
            midi_next_song(m, m->f.t_last);
        }
        return;
    }
    midi_end(m, abandon, drive_gone);
    m->hold_len = 0;
    m->hold_keep = false;
}

capture_ring_t *cap_midi_start(void) {
    cap_midi_t *m = midi;
    if (!m) {
        m = midi = calloc(1, sizeof(*midi));
        if (!m) {
            return NULL;
        }
    }
    // The channels are only known from what is seen while recording
    m->state = CAPTURE_WAITING;
    m->status = 0;
    m->in_sysex = false;
    m->hold_len = 0;
    m->hold_keep = false;
    m->sounding = 0;
    memset(m->notes, 0, sizeof(m->notes));
    memset(m->prog, UNSEEN, sizeof(m->prog));
    memset(m->cc, UNSEEN, sizeof(m->cc));
    memset(m->bend, 0xff, sizeof(m->bend));
    return &m->ring;
}

const cap_file_t *cap_midi_file(uint8_t *state) {
    if (!midi) {
        *state = CAPTURE_OFF;
        return NULL;
    }
    *state = midi->state == CAPTURE_RECORDING ? CAPTURE_RECORDING : CAPTURE_WAITING;
    return &midi->f;
}

#endif // SOUND_MPU
