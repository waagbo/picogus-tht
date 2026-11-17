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

// Song recorder: control, the files on the USB drive, and the loop that feeds
// the events of the taps to the two streams (capture_opl.c, capture_midi.c).
// All of it runs on core 1 in thread mode. printf is not available in release
// builds, hence the hand-made formatting.

#include <stdlib.h>
#include <string.h>

#include "capture_internal.h"
#include "msc_app.h"

// Which streams this firmware has
#ifdef SOUND_OPL
#define HAS_OPL 1
#else
#define HAS_OPL 0
#endif
#ifdef SOUND_MPU
#define HAS_MIDI 1
#else
#define HAS_MIDI 0
#endif

#define CFG_PATH CAPTURE_DIR "/CAPTURE.CFG"
// Events taken from a ring per capture_task(): bounds the time of one call
#define MAX_EVENTS_PER_TASK 128u

capture_ring_t *volatile capture_rings[CAPTURE_STREAMS];

// What the user asked for. A stream this firmware doesn't have stays off
// whatever this says: the setting is for all modes of the card.
capture_config_t cap_cfg = { {0, 0}, 1, 3, 5 };

static bool drive;
static bool cfg_from_user;      // set on the web page since the card started
static const char *error = "";
static bool failed;             // cap_fail() was called: stop at the next capture_task()
static uint32_t songs;
static uint32_t next_no[CAPTURE_STREAMS];   // number of the next file, 0 = not known yet
static void (*audio_pump)(void);

static FIL *rd_file;            // the file being read (downloaded), NULL if none
static char rd_name[13];
static DIR list_dir;
static bool listing;

static const char prefix[CAPTURE_STREAMS][4] = { "OPL", "MID" };
static const char suffix[CAPTURE_STREAMS][4] = { "DRO", "MID" };

static cap_tick_t cap_now(void) {
    return (cap_tick_t)(time_us_64() >> CAPTURE_TICK_SHIFT);
}

static bool has_stream(int s) {
    return s == CAPTURE_OPL ? HAS_OPL : HAS_MIDI;
}

// Keep the sound of the mode going while the USB drive is waited for, as long
// as the recorder has a reason to use the drive
static void update_pump(void) {
    msc_app_io_wait_hook = (capture_rings[CAPTURE_OPL] || capture_rings[CAPTURE_MIDI] || rd_file) ? audio_pump : NULL;
}

void capture_set_audio_pump(void (*pump)(void)) {
    audio_pump = pump;
    update_pump();
}

// ---- names ------------------------------------------------------------------

// "OPL00012.DRO" -> stream and number. Anything else is not one of our files.
static bool parse_name(const char *name, int *stream, uint32_t *number) {
    if (strlen(name) != 12 || name[8] != '.') {
        return false;
    }
    char up[13];
    for (int i = 0; i < 13; ++i) {
        up[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
    }
    for (int s = 0; s < CAPTURE_STREAMS; ++s) {
        if (!memcmp(up, prefix[s], 3) && !memcmp(up + 9, suffix[s], 3)) {
            uint32_t n = 0;
            for (int i = 3; i < 8; ++i) {
                if (up[i] < '0' || up[i] > '9') {
                    return false;
                }
                n = n * 10 + (uint32_t)(up[i] - '0');
            }
            *stream = s;
            *number = n;
            return true;
        }
    }
    return false;
}

static void make_name(char *name, int stream, uint32_t number) {
    memcpy(name, prefix[stream], 3);
    for (int i = 7; i >= 3; --i) {
        name[i] = (char)('0' + number % 10);
        number /= 10;
    }
    name[8] = '.';
    memcpy(name + 9, suffix[stream], 3);
    name[12] = 0;
}

// "CAPTURE/OPL00012.DRO"
static void make_path(char *path, const char *name) {
    memcpy(path, CAPTURE_DIR "/", sizeof(CAPTURE_DIR));
    strcpy(path + sizeof(CAPTURE_DIR), name);
}
#define PATH_LEN (sizeof(CAPTURE_DIR) + 13)

static bool is_recording(const char *name) {
    uint8_t state;
    const cap_file_t *f;
#if HAS_OPL
    f = cap_opl_file(&state);
    if (f && !strcmp(f->name, name)) {
        return true;
    }
#endif
#if HAS_MIDI
    f = cap_midi_file(&state);
    if (f && !strcmp(f->name, name)) {
        return true;
    }
#endif
    (void)state;
    (void)f;
    return false;
}

// ---- files being written ------------------------------------------------------

void cap_fail(const char *why) {
    error = why;
    failed = true;
}

// New files continue after the highest number on the drive
static void scan_numbers(void) {
    capture_file_t file;
    next_no[CAPTURE_OPL] = next_no[CAPTURE_MIDI] = 1;
    if (!capture_list_open()) {
        return;
    }
    while (capture_list_next(&file)) {
        int s;
        uint32_t n;
        if (parse_name(file.name, &s, &n) && n >= next_no[s]) {
            next_no[s] = n + 1;
        }
    }
    capture_list_close();
}

bool cap_create(cap_file_t *f, int stream, cap_tick_t now) {
    if (!drive) {
        return false;
    }
    if (!next_no[stream]) {
        scan_numbers();
    }
    FRESULT fr = FR_EXIST;
    for (int tries = 0; tries < 100; ++tries) {
        char path[PATH_LEN];
        if (next_no[stream] > 99999) {
            next_no[stream] = 1;
        }
        make_name(f->name, stream, next_no[stream]);
        make_path(path, f->name);
        fr = f_open(&f->fil, path, FA_CREATE_NEW | FA_WRITE);
        if (fr == FR_OK) {
            ++next_no[stream];
            f->dirty = true;
            f->frac = 62;       // half a millisecond: times are rounded, not cut
            f->ms = 0;
            f->t_last = f->t_start = f->t_active = f->t_sync = now;
            return true;
        }
        if (fr == FR_NO_PATH) {
            if (f_mkdir(CAPTURE_DIR) != FR_OK) {
                break;
            }
        } else if (fr == FR_EXIST) {
            ++next_no[stream];
        } else {
            break;
        }
    }
    f->name[0] = 0;
    cap_fail(fr == FR_EXIST ? "The " CAPTURE_DIR " folder is full." : "Can't create a file on the USB drive.");
    return false;
}

bool cap_write(cap_file_t *f, const void *data, uint32_t len) {
    UINT written = 0;
    if (!f->name[0] || failed) {
        return false;
    }
    FRESULT fr = f_write(&f->fil, data, len, &written);
    if (fr != FR_OK || written != len) {
        cap_fail(fr == FR_OK ? "The USB drive is full." : "Can't write to the USB drive.");
        return false;
    }
    f->dirty = true;
    return true;
}

bool cap_patch(cap_file_t *f, uint32_t offset, const void *data, uint32_t len) {
    if (!f->name[0] || failed) {
        return false;
    }
    if (f_lseek(&f->fil, offset) != FR_OK) {
        cap_fail("Can't write to the USB drive.");
        return false;
    }
    if (!cap_write(f, data, len)) {
        return false;
    }
    if (f_lseek(&f->fil, f_size(&f->fil)) != FR_OK) {
        cap_fail("Can't write to the USB drive.");
        return false;
    }
    return true;
}

bool cap_sync(cap_file_t *f, cap_tick_t now) {
    if (!f->name[0] || failed) {
        return false;
    }
    f->dirty = false;
    f->t_sync = now;
    if (f_sync(&f->fil) != FR_OK) {
        cap_fail("Can't write to the USB drive.");
        return false;
    }
    return true;
}

void cap_close(cap_file_t *f, bool keep) {
    if (!f->name[0]) {
        return;
    }
    FRESULT fr = f_close(&f->fil);
    if (!keep) {
        char path[PATH_LEN];
        make_path(path, f->name);
        f_unlink(path);
    } else if (fr != FR_OK) {
        cap_fail("Can't write to the USB drive.");
    } else {
        ++songs;
    }
    f->name[0] = 0;
}

void cap_drop(cap_file_t *f, bool drive_gone) {
    if (f->name[0] && !drive_gone) {
        f_close(&f->fil);
    }
    f->name[0] = 0;
}

uint32_t cap_delta_ms(cap_file_t *f, cap_tick_t t, cap_tick_t max_ticks) {
    uint32_t d = t - f->t_last;
    if ((int32_t)d < 0) {
        d = 0;
    }
    f->t_last = t;
    if (max_ticks && d > max_ticks) {
        d = max_ticks;
    }
    if (d > (1u << 26)) {
        d = 1u << 26;       // 4.7 hours; keeps the sum below in 32 bits
    }
    // a tick is 32/125 ms
    uint32_t acc = f->frac + d * 32u;
    f->frac = (uint8_t)(acc % 125u);
    acc /= 125u;
    f->ms += acc;
    return acc;
}

// ---- streams ------------------------------------------------------------------

static void drain(int s, capture_ring_t *q, uint32_t max, cap_tick_t *now);

static void stream_stop(int s, bool cut, bool abandon, bool drive_gone) {
#if HAS_OPL
    if (s == CAPTURE_OPL) {
        cap_opl_stop(cut, abandon, drive_gone);
    }
#endif
#if HAS_MIDI
    if (s == CAPTURE_MIDI) {
        cap_midi_stop(cut, abandon, drive_gone);
    }
#endif
    (void)s; (void)cut; (void)abandon; (void)drive_gone;
}

// Switch the recording of a stream on or off, as the configuration says
static const char *stream_apply(int s) {
    bool want = cap_cfg.on[s] && has_stream(s);
    capture_ring_t *q = capture_rings[s];
    if (want && !q) {
#if HAS_OPL
        if (s == CAPTURE_OPL) {
            q = cap_opl_start();
        }
#endif
#if HAS_MIDI
        if (s == CAPTURE_MIDI) {
            q = cap_midi_start();
        }
#endif
        if (!q) {
            cap_cfg.on[s] = 0;
            return "Not enough memory to record.";
        }
        // whatever a tap left behind when the stream was last switched off
        q->tail = q->head;
        capture_rings[s] = q;
    } else if (!want && q) {
        // The memory is kept: a tap may still be about to write to it
        cap_tick_t now;
        capture_rings[s] = NULL;
        drain(s, q, CAPTURE_RING_SIZE, &now);   // what was sent up to now is part of the song
        stream_stop(s, false, false, false);
    }
    return NULL;
}

// Feed the events of a ring to its stream; at most max of them
static void drain(int s, capture_ring_t *q, uint32_t max, cap_tick_t *now) {
    // Every event up to head was stamped before the time is read here
    uint32_t head = q->head;
    *now = cap_now();
    uint32_t tail = q->tail;
    uint32_t n = head - tail;
    if (n > max) {
        n = max;
    }
    for (; n && !failed; --n) {
        uint32_t ev = q->ev[tail & (CAPTURE_RING_SIZE - 1)];
#if HAS_OPL
        if (s == CAPTURE_OPL) {
            uint32_t mask = (1u << CAPTURE_OPL_TIME_BITS) - 1;
            cap_opl_event(*now - ((*now - (ev >> 17)) & mask), (ev >> 8) & 0x1ff, ev & 0xff);
        }
#endif
#if HAS_MIDI
        if (s == CAPTURE_MIDI) {
            cap_midi_byte(*now - ((*now - (ev >> 8)) & 0xffffffu), ev & 0xff);
        }
#endif
        q->tail = ++tail;
    }
}

// After anything that writes to the drive: if that failed (cap_fail()), the
// recorder is switched off and the files are left as they are
static void stop_if_failed(void) {
    if (!failed) {
        return;
    }
    failed = false;
    for (int s = 0; s < CAPTURE_STREAMS; ++s) {
        capture_rings[s] = NULL;
        cap_cfg.on[s] = 0;
        stream_stop(s, false, true, false);
    }
    update_pump();
}

void capture_task(void) {
    if (!capture_rings[CAPTURE_OPL] && !capture_rings[CAPTURE_MIDI]) {
        return;
    }
    cap_tick_t now = cap_now();
#if HAS_OPL
    if (capture_rings[CAPTURE_OPL]) {
        drain(CAPTURE_OPL, capture_rings[CAPTURE_OPL], MAX_EVENTS_PER_TASK, &now);
        if (!failed) {
            cap_opl_poll(now);
        }
    }
#endif
#if HAS_MIDI
    if (capture_rings[CAPTURE_MIDI]) {
        drain(CAPTURE_MIDI, capture_rings[CAPTURE_MIDI], MAX_EVENTS_PER_TASK, &now);
        if (!failed) {
            cap_midi_poll(now);
        }
    }
#endif
    stop_if_failed();
}

// ---- configuration ----------------------------------------------------------------

// The configuration is kept in a text file on the drive, so that it survives
// a restart and can be edited: "opl=1 midi=1 split=1 gap=3 min=5"

static bool cfg_valid(const capture_config_t *c) {
    return c->on[0] <= 1 && c->on[1] <= 1 && c->split <= 1
        && c->gap >= CAPTURE_GAP_MIN && c->gap <= CAPTURE_GAP_MAX && c->minlen <= CAPTURE_MINLEN_MAX;
}

static const char *const cfg_keys[] = { "opl", "midi", "split", "gap", "min" };

static uint8_t *cfg_field(capture_config_t *c, int i) {
    return i == 0 ? &c->on[CAPTURE_OPL] : i == 1 ? &c->on[CAPTURE_MIDI] : i == 2 ? &c->split : i == 3 ? &c->gap : &c->minlen;
}

static void cfg_load(void) {
    char text[96];
    UINT len = 0;
    FIL *fp = malloc(sizeof(FIL));
    if (!fp) {
        return;
    }
    if (f_open(fp, CFG_PATH, FA_READ) == FR_OK) {
        f_read(fp, text, sizeof(text) - 1, &len);
        f_close(fp);
    }
    free(fp);
    text[len] = 0;
    capture_config_t c = cap_cfg;
    bool any = false;
    for (char *p = text; *p;) {
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') {
            ++p;
        }
        char *key = p;
        while (*p && *p != '=' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') {
            ++p;
        }
        if (*p != '=') {
            continue;
        }
        size_t keylen = (size_t)(p - key);
        uint32_t v = 0;
        for (++p; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + (uint32_t)(*p - '0');
            if (v > 255) {
                v = 255;
            }
        }
        for (int i = 0; i < 5; ++i) {
            if (strlen(cfg_keys[i]) == keylen && !memcmp(cfg_keys[i], key, keylen)) {
                *cfg_field(&c, i) = (uint8_t)v;
                any = true;
            }
        }
    }
    if (any && cfg_valid(&c)) {
        cap_cfg = c;
    }
}

static void cfg_save(void) {
    char text[64], *p = text;
    for (int i = 0; i < 5; ++i) {
        for (const char *k = cfg_keys[i]; *k;) {
            *p++ = *k++;
        }
        *p++ = '=';
        uint32_t v = *cfg_field(&cap_cfg, i);
        if (v >= 10) {
            *p++ = (char)('0' + v / 10);
        }
        *p++ = (char)('0' + v % 10);
        *p++ = i < 4 ? ' ' : '\r';
    }
    *p++ = '\n';
    FIL *fp = malloc(sizeof(FIL));
    if (!fp) {
        return;
    }
    FRESULT fr = f_open(fp, CFG_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_NO_PATH && f_mkdir(CAPTURE_DIR) == FR_OK) {
        fr = f_open(fp, CFG_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    }
    if (fr == FR_OK) {
        UINT written;
        f_write(fp, text, (UINT)(p - text), &written);
        f_close(fp);
    }
    free(fp);
}

static const char *cfg_apply(void) {
    const char *err = NULL;
    for (int s = 0; s < CAPTURE_STREAMS; ++s) {
        const char *e = stream_apply(s);
        if (e) {
            err = e;
        }
    }
    update_pump();
    return err;
}

void capture_get_config(capture_config_t *cfg) {
    *cfg = cap_cfg;
}

const char *capture_set_config(const capture_config_t *cfg) {
    if (!cfg_valid(cfg)) {
        return "Invalid value.";
    }
    cap_cfg = *cfg;
    cfg_from_user = true;
    error = "";
    const char *err = cfg_apply();
    if (failed) {
        // ending a song did not work: the recorder is off, whatever was asked
        stop_if_failed();
        return error;
    }
    if (drive) {
        cfg_save();
    }
    return err;
}

void capture_cut(void) {
    capture_task();     // what was sent up to now is part of the songs
    for (int s = 0; s < CAPTURE_STREAMS; ++s) {
        if (capture_rings[s]) {
            stream_stop(s, true, false, false);
        }
    }
    stop_if_failed();
}

void capture_on_drive_mounted(void) {
    drive = true;
    next_no[CAPTURE_OPL] = next_no[CAPTURE_MIDI] = 0;
    error = "";
    if (cfg_from_user) {
        cfg_save();
    } else {
        cfg_load();
        cfg_apply();
    }
    stop_if_failed();
}

void capture_on_drive_unmounted(void) {
    drive = false;
    for (int s = 0; s < CAPTURE_STREAMS; ++s) {
        stream_stop(s, false, true, true);
    }
    // the volume is going away: nothing of it may be used any more
    free(rd_file);
    rd_file = NULL;
    listing = false;
    update_pump();
}

void capture_get_status(capture_status_t *st) {
    memset(st, 0, sizeof(*st));
    st->drive = drive;
    st->songs = songs;
    st->error = error;
    cap_tick_t now = cap_now();
    for (int s = 0; s < CAPTURE_STREAMS; ++s) {
        capture_ring_t *q = capture_rings[s];
        const cap_file_t *f = NULL;
        uint8_t state = CAPTURE_OFF;
#if HAS_OPL
        if (s == CAPTURE_OPL) {
            f = cap_opl_file(&state);
        }
#endif
#if HAS_MIDI
        if (s == CAPTURE_MIDI) {
            f = cap_midi_file(&state);
        }
#endif
        if (!q) {
            continue;
        }
        st->lost += q->lost;
        st->state[s] = state;
        if (f && state == CAPTURE_RECORDING) {
            strcpy(st->file[s], f->name);
            st->seconds[s] = (now - f->t_start) / CAP_TICKS(1);
        }
    }
}

// ---- the recorded files ---------------------------------------------------------

bool capture_list_open(void) {
    if (!drive || listing) {
        return false;
    }
    listing = f_opendir(&list_dir, CAPTURE_DIR) == FR_OK;
    return listing;
}

bool capture_list_next(capture_file_t *file) {
    FILINFO info;
    int s;
    uint32_t n;
    while (listing && f_readdir(&list_dir, &info) == FR_OK && info.fname[0]) {
        if (!(info.fattrib & AM_DIR) && parse_name(info.fname, &s, &n)) {
            make_name(file->name, s, n);
            file->size = (uint32_t)info.fsize;
            file->busy = is_recording(file->name);
            return true;
        }
    }
    return false;
}

void capture_list_close(void) {
    if (listing) {
        f_closedir(&list_dir);
        listing = false;
    }
}

// The file of a request: its name as we write it, checked
static const char *request_name(const char *name, char *clean) {
    int s;
    uint32_t n;
    if (!drive) {
        return "There is no USB drive.";
    }
    if (!parse_name(name, &s, &n)) {
        return "Not a recorded file.";
    }
    make_name(clean, s, n);
    if (is_recording(clean)) {
        return "This song is still being recorded.";
    }
    return NULL;
}

const char *capture_read_open(const char *name, uint32_t *size) {
    char clean[13], path[PATH_LEN];
    const char *err = request_name(name, clean);
    if (err) {
        return err;
    }
    if (rd_file) {
        return "Another file is being read.";
    }
    rd_file = malloc(sizeof(FIL));
    if (!rd_file) {
        return "Not enough memory.";
    }
    make_path(path, clean);
    if (f_open(rd_file, path, FA_READ) != FR_OK) {
        free(rd_file);
        rd_file = NULL;
        return "No such file.";
    }
    strcpy(rd_name, clean);
    *size = (uint32_t)f_size(rd_file);
    update_pump();
    return NULL;
}

uint32_t capture_read(uint32_t offset, uint8_t *buf, uint32_t len) {
    UINT got = 0;
    if (!rd_file || (f_tell(rd_file) != offset && f_lseek(rd_file, offset) != FR_OK)) {
        return 0;
    }
    if (f_read(rd_file, buf, len, &got) != FR_OK) {
        return 0;
    }
    return got;
}

void capture_read_close(void) {
    if (rd_file) {
        f_close(rd_file);
        free(rd_file);
        rd_file = NULL;
        update_pump();
    }
}

const char *capture_delete(const char *name) {
    char clean[13], path[PATH_LEN];
    const char *err = request_name(name, clean);
    if (err) {
        return err;
    }
    if (rd_file && !strcmp(rd_name, clean)) {
        return "This file is being read.";
    }
    make_path(path, clean);
    return f_unlink(path) == FR_OK ? NULL : "No such file.";
}
