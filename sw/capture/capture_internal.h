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

// Shared between the source files of the song recorder (core 1 only)

#pragma once

#include "capture.h"
#include "ff.h"

// Time in ticks of 256 microseconds (wraps after 12 days)
typedef uint32_t cap_tick_t;

// seconds -> ticks: one second is 3906.25 ticks
#define CAP_TICKS(seconds) ((cap_tick_t)(seconds) * 15625u / 4u)
// Header and directory entry of a file being written are brought up to date
// this often, so that a song survives the PC being switched off
#define CAP_SYNC_TICKS CAP_TICKS(5)
// What is added after the last sound of a song, for its notes to ring out
#define CAP_TAIL_MS 1000u

// A file being written
typedef struct {
    FIL fil;
    char name[13];          // "" while no file is open
    bool dirty;             // written to since the last sync
    uint8_t frac;           // what cap_delta_ms() carries over (1/125 ms)
    cap_tick_t t_last;      // time of the last event written
    cap_tick_t t_start;     // time of the first note of the song
    cap_tick_t t_active;    // time of the last event that made or changed sound
    cap_tick_t t_sync;      // time of the last sync
    uint32_t ms;            // sum of the delays written (ms)
} cap_file_t;

extern capture_config_t cap_cfg;

// Create the next file of a stream. On failure the recorder is stopped
// (cap_fail()) unless there is no drive.
bool cap_create(cap_file_t *f, int stream, cap_tick_t now);
// Append. False, with the recorder stopped, if that failed.
bool cap_write(cap_file_t *f, const void *data, uint32_t len);
// Overwrite part of the file, then go back to its end
bool cap_patch(cap_file_t *f, uint32_t offset, const void *data, uint32_t len);
// Bring the file on the drive up to date
bool cap_sync(cap_file_t *f, cap_tick_t now);
// Close the file; delete it unless it is to be kept
void cap_close(cap_file_t *f, bool keep);
// Forget the file without touching the drive (the drive is gone), or just
// close it as it is (after an error)
void cap_drop(cap_file_t *f, bool drive_gone);
// Stop the recorder with a message. The streams are switched off by the next
// capture_task(), not from inside the call that failed.
void cap_fail(const char *why);
// Milliseconds since the last event written, without accumulating a rounding
// error. max_ticks > 0 limits the time that is counted.
uint32_t cap_delta_ms(cap_file_t *f, cap_tick_t t, cap_tick_t max_ticks);

// Each stream:
//   start   get ready to record. Returns the stream's ring, NULL without memory.
//   event   an event from the ring
//   poll    time passed: end songs, sync files
//   stop    end the song being recorded. cut: carry on with a new file for
//           what follows. abandon: the file can't be written any more.
//   file    the file being written
capture_ring_t *cap_opl_start(void);
void cap_opl_event(cap_tick_t t, uint32_t reg, uint32_t value);
void cap_opl_poll(cap_tick_t now);
void cap_opl_stop(bool cut, bool abandon, bool drive_gone);
const cap_file_t *cap_opl_file(uint8_t *state);

capture_ring_t *cap_midi_start(void);
void cap_midi_byte(cap_tick_t t, uint32_t byte);
void cap_midi_poll(cap_tick_t now);
void cap_midi_stop(bool cut, bool abandon, bool drive_gone);
const cap_file_t *cap_midi_file(uint8_t *state);
