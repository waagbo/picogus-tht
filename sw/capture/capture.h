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

// Song recorder: writes what a program sends to the FM chip (OPL2/OPL3
// register writes) and to the MIDI output to files on the USB drive, one file
// per song: DOSBox raw OPL (.DRO) and standard MIDI files (.MID).
//
//  * The taps (capture_opl_write(), capture_midi_byte()) only put a time
//    stamped event into a ring buffer. They cost a few instructions, do
//    nothing while their stream is not being recorded, and may be called from
//    core 0.
//  * Everything else runs on core 1 in thread mode, from capture_task(): the
//    events are read from the rings, split into songs and written with FatFs.
//  * Memory for a stream is allocated the first time it is switched on.
//
// See sw/experimental/capture.md.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// The two streams
#define CAPTURE_OPL     0
#define CAPTURE_MIDI    1
#define CAPTURE_STREAMS 2

// ---- taps -------------------------------------------------------------------

// A ring holds what is written while capture_task() waits for the USB drive:
// 2048 events (8KB) are 0.4 s of the busiest FM music, or 0.6 s of MIDI
#define CAPTURE_RING_SIZE 2048u     // events; a power of two

typedef struct {
    volatile uint32_t head;         // written by the tap only
    volatile uint32_t tail;         // written by capture_task() only
    volatile uint32_t lost;         // events dropped because the ring was full
    uint32_t ev[CAPTURE_RING_SIZE];
} capture_ring_t;

// The ring of each stream, NULL while it is not being recorded
extern capture_ring_t *volatile capture_rings[CAPTURE_STREAMS];

// Time stamps count 256 microseconds. An event carries the low bits of its
// time; capture_task() knows the rest, as it reads the rings far more often
// than those bits wrap around.
#define CAPTURE_TICK_SHIFT    8
#define CAPTURE_OPL_TIME_BITS 15    // 8.4 s; the other 17 bits: register, value

static inline void capture_ring_put(capture_ring_t *q, uint32_t ev) {
    uint32_t head = q->head;
    if (head - q->tail >= CAPTURE_RING_SIZE) {
        q->lost = q->lost + 1;
        return;
    }
    q->ev[head & (CAPTURE_RING_SIZE - 1)] = ev;
    q->head = head + 1;
}

// A write to an FM register (bit 8 of reg: the OPL3's second register set).
// Called by the one core that serves the ISA bus.
static inline void capture_opl_write(uint32_t reg, uint32_t value) {
    capture_ring_t *q = capture_rings[CAPTURE_OPL];
    if (q) {
        capture_ring_put(q, (time_us_32() >> CAPTURE_TICK_SHIFT) << 17 | (reg & 0x1ff) << 8 | (value & 0xff));
    }
}

// A byte on its way to the MIDI output. The callers are serialized by the
// MIDI output's critical section (mpu401/midi.c), which covers both cores.
static inline void capture_midi_byte(uint32_t byte) {
    capture_ring_t *q = capture_rings[CAPTURE_MIDI];
    if (q) {
        capture_ring_put(q, (time_us_32() >> CAPTURE_TICK_SHIFT) << 8 | (byte & 0xff));
    }
}

// ---- core 1 -----------------------------------------------------------------

// Call from the core 1 main loop of every mode. Cheap while nothing is recorded.
void capture_task(void);
// The USB drive was mounted / is about to be unmounted (msc_app_task(), outside
// of any FatFs call)
void capture_on_drive_mounted(void);
void capture_on_drive_unmounted(void);
// Modes whose sound is made in the core 1 main loop (FM) give a function that
// keeps it going. It is called while a transfer to the USB drive is waited
// for, as long as something is recorded or downloaded.
void capture_set_audio_pump(void (*pump)(void));

typedef struct {
    uint8_t on[CAPTURE_STREAMS];    // record this stream
    uint8_t split;                  // one file per song (else one file until switched off)
    uint8_t gap;                    // seconds without sound that end a song
    uint8_t minlen;                 // songs shorter than this many seconds are not kept
} capture_config_t;

#define CAPTURE_GAP_MIN     1
#define CAPTURE_GAP_MAX     60
#define CAPTURE_MINLEN_MAX  60

enum {
    CAPTURE_OFF = 0,        // not recorded
    CAPTURE_WAITING,        // waiting for a song to start
    CAPTURE_RECORDING,      // a song is being written
};

typedef struct {
    uint8_t drive;                      // a USB drive is mounted
    uint8_t state[CAPTURE_STREAMS];     // CAPTURE_OFF, ...
    char file[CAPTURE_STREAMS][13];     // the file being written, "" if none
    uint32_t seconds[CAPTURE_STREAMS];  // length of the song so far
    uint32_t songs;                     // songs kept since the card started
    uint32_t lost;                      // events lost since the card started
    const char *error;                  // what stopped the recorder, "" if nothing
} capture_status_t;

void capture_get_config(capture_config_t *cfg);
// Returns NULL, or why the configuration was not taken
const char *capture_set_config(const capture_config_t *cfg);
// End the songs that are being recorded now: what follows goes to new files
void capture_cut(void);
void capture_get_status(capture_status_t *st);

// ---- the recorded files (core 1) ----------------------------------------------
//
// Names are OPLnnnnn.DRO and MIDnnnnn.MID in the CAPTURE folder of the drive.
// Only such names are accepted, so a name can't lead anywhere else.

#define CAPTURE_DIR "CAPTURE"

typedef struct {
    char name[13];
    uint32_t size;
    uint8_t busy;           // being recorded: can't be read or deleted yet
} capture_file_t;

// Directory listing, in the order of the directory. Only one at a time.
bool capture_list_open(void);
bool capture_list_next(capture_file_t *file);   // false: no more files
void capture_list_close(void);

// Reading a file. Only one at a time. The functions that can fail return NULL
// or what went wrong.
const char *capture_read_open(const char *name, uint32_t *size);
// Returns the number of bytes read, less than len only on an error
uint32_t capture_read(uint32_t offset, uint8_t *buf, uint32_t len);
void capture_read_close(void);

const char *capture_delete(const char *name);

#ifdef __cplusplus
}
#endif
