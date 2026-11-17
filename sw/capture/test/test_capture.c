// Host tests of the song recorder: the taps are fed what a program would write,
// the test sets the time, and the files that appear on a RAM disk are read
// back and checked as a player would read them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "ff.h"
#include "msc_app.h"
#include "ramdisk_diskio.h"

uint64_t test_clock_us;
void (*msc_app_io_wait_hook)(void);

DWORD get_fattime(void) {
    return ((DWORD)(2026 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

static int checks, failures;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++checks; long long a_ = (long long)(a), b_ = (long long)(b); \
    if (a_ != b_) { ++failures; printf("  FAILED %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

// ---- the card ---------------------------------------------------------------

static FATFS fs;
static BYTE mkfs_work[4096];

static void mount_new_drive(void) {
    CHECK(ramdisk_init(16384));      // 8 MB
    MKFS_PARM parm = { FM_FAT | FM_SFD, 0, 0, 0, 0 };
    CHECK_EQ(f_mkfs("", &parm, mkfs_work, sizeof(mkfs_work)), FR_OK);
    CHECK_EQ(f_mount(&fs, "", 1), FR_OK);
    capture_on_drive_mounted();
}

static void unmount_drive(void) {
    capture_on_drive_unmounted();
    f_unmount("");
}

// Let time pass, with the core 1 loop running
static void run_ms(uint32_t ms) {
    while (ms--) {
        test_clock_us += 1000;
        capture_task();
    }
}

static void opl(uint32_t reg, uint32_t value) {
    capture_opl_write(reg, value);
}

static void midi_bytes(const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        capture_midi_byte(bytes[i]);
    }
}
#define MIDI(...) do { const uint8_t b_[] = { __VA_ARGS__ }; midi_bytes(b_, sizeof(b_)); } while (0)

static void configure(int opl_on, int midi_on, int split, int gap, int minlen) {
    capture_config_t c = { { (uint8_t)opl_on, (uint8_t)midi_on }, (uint8_t)split, (uint8_t)gap, (uint8_t)minlen };
    const char *err = capture_set_config(&c);
    if (err) {
        printf("  capture_set_config: %s\n", err);
    }
    CHECK(err == NULL);
}

// ---- files --------------------------------------------------------------------

static uint8_t file_data[1 << 20];
static uint32_t file_len;

static bool read_file(const char *name) {
    char path[32];
    FIL fp;
    UINT got = 0;
    snprintf(path, sizeof(path), CAPTURE_DIR "/%s", name);
    file_len = 0;
    if (f_open(&fp, path, FA_READ) != FR_OK) {
        return false;
    }
    f_read(&fp, file_data, sizeof(file_data), &got);
    f_close(&fp);
    file_len = got;
    return true;
}

static int count_files(void) {
    capture_file_t f;
    int n = 0;
    if (!capture_list_open()) {
        return 0;
    }
    while (capture_list_next(&f)) {
        ++n;
    }
    capture_list_close();
    return n;
}

// ---- a DRO file as a player reads it --------------------------------------------

typedef struct { uint32_t ms, reg, value; } opl_write_t;
static opl_write_t dro[65536];
static uint32_t dro_count, dro_ms, dro_hardware;

static bool parse_dro(void) {
    dro_count = 0;
    if (file_len < 26 || memcmp(file_data, "DBRAWOPL", 8) || file_data[8] != 2 || file_data[10] != 0) {
        return false;
    }
    uint32_t commands = file_data[12] | file_data[13] << 8 | file_data[14] << 16 | (uint32_t)file_data[15] << 24;
    uint32_t ms = file_data[16] | file_data[17] << 8 | file_data[18] << 16 | (uint32_t)file_data[19] << 24;
    dro_hardware = file_data[20];
    uint32_t delay256 = file_data[23], delay_shift8 = file_data[24], map_len = file_data[25];
    if (file_data[21] || file_data[22] || 26 + map_len + commands * 2 != file_len) {
        printf("  DRO: %u commands in the header, file of %u bytes\n", commands, file_len);
        return false;
    }
    const uint8_t *map = file_data + 26, *p = map + map_len;
    uint32_t now = 0;
    for (uint32_t i = 0; i < commands; ++i, p += 2) {
        if (p[0] == delay256) {
            now += p[1] + 1u;
        } else if (p[0] == delay_shift8) {
            now += (p[1] + 1u) << 8;
        } else {
            if ((p[0] & 0x7f) >= map_len) {
                return false;
            }
            dro[dro_count++] = (opl_write_t){ now, map[p[0] & 0x7f] | (p[0] & 0x80) << 1, p[1] };
        }
    }
    dro_ms = now;
    if (now != ms) {
        printf("  DRO: %u ms in the header, %u ms of delays\n", ms, now);
        return false;
    }
    return true;
}

// first write to a register at or after index from; -1 if none
static int dro_find(uint32_t reg, uint32_t value, int from) {
    for (uint32_t i = (uint32_t)from; i < dro_count; ++i) {
        if (dro[i].reg == reg && dro[i].value == value) {
            return (int)i;
        }
    }
    return -1;
}

// ---- a MIDI file as a player reads it ---------------------------------------------

typedef struct { uint32_t ms; uint8_t status; uint8_t d1, d2; uint32_t len; const uint8_t *data; } midi_event_t;
static midi_event_t smf[65536];
static uint32_t smf_count, smf_ms;

// complete: the song has ended, so the track has its end
static bool parse_smf_as(bool complete) {
    static const uint8_t head[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xf4, 'M', 'T', 'r', 'k' };
    smf_count = 0;
    if (file_len < 22 || memcmp(file_data, head, sizeof(head))) {
        return false;
    }
    uint32_t len = (uint32_t)file_data[18] << 24 | file_data[19] << 16 | file_data[20] << 8 | file_data[21];
    if (22 + len != file_len) {
        printf("  SMF: track of %u bytes in a file of %u\n", len, file_len);
        return false;
    }
    const uint8_t *p = file_data + 22, *end = file_data + file_len;
    uint32_t now = 0;
    bool ended = false;
    while (p < end) {
        uint32_t delta = 0, n = 0;
        do {
            if (p >= end) return false;
            delta = delta << 7 | (*p & 0x7f);
        } while (*p++ & 0x80);
        now += delta;
        if (p >= end || ended) return false;
        uint8_t status = *p++;
        midi_event_t *e = &smf[smf_count++];
        *e = (midi_event_t){ now, status, 0, 0, 0, NULL };
        if (status == 0xff) {
            if (p + 2 > end) return false;
            e->d1 = *p++;
            n = *p++;
            e->data = p;
            e->len = n;
            p += n;
            ended = e->d1 == 0x2f;
        } else if (status == 0xf0 || status == 0xf7) {
            do {
                if (p >= end) return false;
                n = n << 7 | (*p & 0x7f);
            } while (*p++ & 0x80);
            e->data = p;
            e->len = n;
            p += n;
        } else if (status >= 0x80 && status < 0xf0) {
            // no running status in what we write
            e->d1 = *p++;
            if ((status & 0xe0) != 0xc0) {
                e->d2 = *p++;
            }
            if ((e->d1 | e->d2) & 0x80) return false;
        } else {
            return false;
        }
        if (p > end) return false;
    }
    smf_ms = now;
    // tempo first, end of track last
    return ended == complete && smf_count >= 2 && smf[0].status == 0xff && smf[0].d1 == 0x51 && smf[0].ms == 0;
}

static bool parse_smf(void) {
    return parse_smf_as(true);
}

static int smf_find(uint8_t status, uint8_t d1, uint8_t d2, int from) {
    for (uint32_t i = (uint32_t)from; i < smf_count; ++i) {
        if (smf[i].status == status && smf[i].d1 == d1 && smf[i].d2 == d2) {
            return (int)i;
        }
    }
    return -1;
}

static int smf_find_sysex(uint8_t status, int from) {
    for (uint32_t i = (uint32_t)from; i < smf_count; ++i) {
        if (smf[i].status == status) {
            return (int)i;
        }
    }
    return -1;
}

// ---- tests ----------------------------------------------------------------------

// A two operator instrument on channel 0, as a player sets one up
static void opl_instrument(uint32_t level) {
    opl(0x20, 0x01); opl(0x23, 0x01);
    opl(0x40, level); opl(0x43, 0x00);
    opl(0x60, 0xf0); opl(0x63, 0xf0);
    opl(0x80, 0x77); opl(0x83, 0x77);
    opl(0xc0, 0x06);
}

// notes of 200 ms every 250 ms on channel 0
static void opl_play(int notes) {
    for (int i = 0; i < notes; ++i) {
        opl(0xa0, 0x98 + (uint32_t)(i & 7));
        opl(0xb0, 0x31);
        run_ms(200);
        opl(0xb0, 0x11);
        run_ms(50);
    }
}

static void test_opl_songs(void) {
    printf("FM: songs are found, begin with the registers and end at the last sound\n");
    configure(1, 0, 1, 3, 5);
    // the AdLib detection and the set-up, long before the music
    opl(0x04, 0x60); opl(0x04, 0x80); opl(0x02, 0xff); opl(0x04, 0x21);
    opl(0x01, 0x20);
    opl_instrument(0x10);
    opl(0x3f, 0x55);                    // no such register
    run_ms(2000);
    CHECK_EQ(count_files(), 0);

    opl_play(40);                       // 10 s
    capture_status_t st;
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_RECORDING);
    CHECK(!strcmp(st.file[CAPTURE_OPL], "OPL00001.DRO"));
    CHECK_EQ(st.seconds[CAPTURE_OPL], 10);
    // the next song's instrument, written while the chip is silent
    run_ms(1000);
    opl_instrument(0x3f);
    run_ms(1900);
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_RECORDING);     // 2.95 s of silence: not yet
    run_ms(200);
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_WAITING);
    CHECK_EQ(st.songs, 1);
    CHECK(st.file[CAPTURE_OPL][0] == 0);

    CHECK(read_file("OPL00001.DRO"));
    CHECK(parse_dro());
    CHECK_EQ(dro_hardware, 0);
    // the registers first, at time 0, with the note still off
    int first_on = dro_find(0xb0, 0x31, 0);
    CHECK(first_on > 0);
    CHECK_EQ(dro[first_on].ms, 0);
    CHECK(dro_find(0x01, 0x20, 0) >= 0 && dro_find(0x01, 0x20, 0) < first_on);
    CHECK(dro_find(0x40, 0x10, 0) >= 0 && dro_find(0x40, 0x10, 0) < first_on);
    CHECK(dro_find(0xc0, 0x06, 0) >= 0 && dro_find(0xc0, 0x06, 0) < first_on);
    CHECK(dro_find(0xa0, 0x98, 0) >= 0 && dro_find(0xa0, 0x98, 0) < first_on);
    for (int i = 0; i < first_on; ++i) {
        CHECK(!(dro[i].reg == 0xb0 && (dro[i].value & 0x20)));
        CHECK(dro[i].reg != 0x04 && dro[i].reg != 0x02 && dro[i].reg != 0x3f);
    }
    // 40 notes, on time
    int n = 0, at = 0;
    while ((at = dro_find(0xb0, 0x31, at)) >= 0) {
        CHECK_EQ(dro[at].ms, (uint32_t)n * 250);
        ++n;
        ++at;
    }
    CHECK_EQ(n, 40);
    // ends with the last key off and a second to ring out; the instrument
    // that was written into the silence is not part of it
    CHECK(dro[dro_count - 1].reg == 0xb0 && dro[dro_count - 1].value == 0x11);
    CHECK_EQ(dro[dro_count - 1].ms, 39 * 250 + 200);
    CHECK_EQ(dro_ms, 39 * 250 + 200 + 1000);
    CHECK(dro_find(0x40, 0x3f, 0) < 0);

    printf("FM: the next song has the instrument that was set up in between\n");
    opl_play(24);
    run_ms(3100);
    CHECK(read_file("OPL00002.DRO"));
    CHECK(parse_dro());
    first_on = dro_find(0xb0, 0x31, 0);
    CHECK(first_on > 0 && dro[first_on].ms == 0);
    CHECK(dro_find(0x40, 0x3f, 0) >= 0 && dro_find(0x40, 0x3f, 0) < first_on);
    CHECK(dro_find(0x40, 0x10, 0) < 0);
    CHECK_EQ(count_files(), 2);

    printf("FM: a sound shorter than the shortest song is not kept\n");
    opl_play(8);                        // 2 s
    run_ms(3100);
    CHECK_EQ(count_files(), 2);
    capture_get_status(&st);
    CHECK_EQ(st.songs, 2);
    // and its number is used again
    opl_play(24);
    run_ms(3100);
    CHECK_EQ(count_files(), 3);
    CHECK(read_file("OPL00004.DRO") || read_file("OPL00003.DRO"));
    CHECK(parse_dro());

    printf("FM: a pause shorter than the gap does not end a song\n");
    opl_play(12);
    run_ms(2500);
    opl(0x40, 0x08);                    // while silent
    opl_play(12);
    run_ms(3100);
    CHECK_EQ(count_files(), 4);
    capture_get_status(&st);
    CHECK_EQ(st.songs, 4);
    configure(0, 0, 1, 3, 5);
}

static void test_opl3(void) {
    printf("FM: OPL3 writes, and a file per session with splitting off\n");
    configure(1, 0, 0, 3, 5);
    opl(0x105, 0x01);
    opl(0x104, 0x01);
    opl(0x120, 0x21);
    opl(0x1c0, 0x30);
    run_ms(500);
    opl(0x1a0, 0x40);
    opl(0x1b0, 0x2a);
    run_ms(1000);
    opl(0x1b0, 0x0a);
    run_ms(10000);                      // far longer than the gap
    opl(0xbd, 0x30);                    // rhythm: bass drum
    run_ms(100);
    opl(0xbd, 0x20);
    run_ms(8000);
    capture_status_t st;
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_RECORDING);
    // the file on the drive is complete as it is, every few seconds
    char name[13];
    strcpy(name, st.file[CAPTURE_OPL]);
    CHECK(read_file(name));
    CHECK(parse_dro());
    CHECK_EQ(dro_hardware, 2);
    configure(0, 0, 1, 3, 5);           // off: the file is finished, short as the music was
    CHECK(read_file(name));
    CHECK(parse_dro());
    CHECK_EQ(dro_hardware, 2);
    CHECK(dro[0].reg == 0x105 && dro[0].value == 1);
    int on = dro_find(0x1b0, 0x2a, 0);
    CHECK(on > 0 && dro[on].ms == 0);
    CHECK(dro_find(0x120, 0x21, 0) >= 0 && dro_find(0x120, 0x21, 0) < on);
    int drum = dro_find(0xbd, 0x30, 0);
    CHECK(drum > on);
    CHECK_EQ(dro[drum].ms, 11000);
    CHECK_EQ(dro_ms, 11100 + 1000);
}

static void test_opl_time(void) {
    printf("FM: times stay right across the wrap of the time stamps and of the timer\n");
    configure(1, 0, 0, 3, 0);
    opl(0xa1, 0x10);
    opl(0xb1, 0x30);
    uint32_t expect = 0;
    // pauses up to 8 s (the time in an event wraps after 8.4 s), for 3 hours
    // (the 32 bit microsecond timer wraps after 71 minutes)
    for (int i = 0; i < 2400; ++i) {
        uint32_t pause = 1 + (uint32_t)(i * 7919) % 8000;
        run_ms(pause);
        expect += pause;
        opl(0xa1, (uint32_t)i & 0xff);
    }
    capture_status_t st;
    capture_get_status(&st);
    char name[13];
    strcpy(name, st.file[CAPTURE_OPL]);
    CHECK_EQ(st.lost, 0);
    configure(0, 0, 1, 3, 5);
    CHECK(read_file(name));
    CHECK(parse_dro());
    // each event is stamped to 0.256 ms and written to the millisecond,
    // without the error adding up
    CHECK(dro_count > 2400);
    long long diff = (long long)dro[dro_count - 1].ms - (long long)expect;
    if (diff < -1 || diff > 1) printf("  last write at %u ms, expected %u; %u writes\n", dro[dro_count - 1].ms, expect, dro_count);
    CHECK(diff >= -1 && diff <= 1);
    uint32_t at = 0;
    for (int i = 0, k = dro_find(0xb1, 0x30, 0) + 1; i < 2400 && k < (int)dro_count; ++i, ++k) {
        at += 1 + (uint32_t)(i * 7919) % 8000;
        diff = (long long)dro[k].ms - (long long)at;
        if (diff < -1 || diff > 1) {
            printf("  write %d at %u ms, expected %u\n", i, dro[k].ms, at);
            CHECK(diff >= -1 && diff <= 1);
            break;
        }
    }

    printf("FM: a burst larger than the ring is counted as lost\n");
    configure(1, 0, 1, 3, 5);
    for (uint32_t i = 0; i < CAPTURE_RING_SIZE + 10; ++i) {
        opl(0xa2, i & 0xff);
    }
    run_ms(10);
    capture_get_status(&st);
    CHECK_EQ(st.lost, 10);
    configure(0, 0, 1, 3, 5);
}

static void test_midi_songs(void) {
    printf("MIDI: set-up, song, and what follows a song\n");
    capture_status_t st;
    capture_get_status(&st);
    uint32_t songs0 = st.songs;
    int files0 = count_files();
    configure(0, 1, 1, 3, 5);
    // a game starts: SysEx, programs and controllers, then nothing for a while
    MIDI(0xf0, 0x41, 0x10, 0x16, 0x12, 0x20, 0x00, 0x00, 'H', 'i', 0x0a, 0xf7);
    run_ms(300);
    MIDI(0xc0, 0x05, 0xb0, 0x07, 100, 0xb0, 0x0a, 20);
    MIDI(0xb1, 0x65, 0x00, 0xb1, 0x64, 0x00, 0xb1, 0x06, 12);    // pitch bend range of channel 2
    MIDI(0xf8, 0xfe);                                           // realtime: ignored
    run_ms(20000);
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_WAITING);
    // notes of 400 ms every 500 ms, with running status and a realtime byte inside
    for (int i = 0; i < 20; ++i) {
        MIDI(0x90, 60, 100);
        MIDI(64, 0xf8, 90);
        run_ms(400);
        MIDI(0x80, 60, 0, 64, 0);
        run_ms(100);
    }
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_RECORDING);
    CHECK(!strcmp(st.file[CAPTURE_MIDI], "MID00001.MID"));
    // after the song: the program of the next one, sent within the gap
    run_ms(1000);
    MIDI(0xc0, 0x30);
    MIDI(0xe0, 0x00, 0x50);
    run_ms(2500);
    capture_get_status(&st);
    CHECK_EQ(st.songs, songs0 + 1);

    CHECK(read_file("MID00001.MID"));
    CHECK(parse_smf());
    int sx = smf_find_sysex(0xf0, 0);
    CHECK(sx == 1);
    CHECK(smf[sx].len == 11 && smf[sx].data[0] == 0x41 && smf[sx].data[10] == 0xf7);
    int prog = smf_find(0xc0, 5, 0, 0), vol = smf_find(0xb0, 7, 100, 0), range = smf_find(0xb1, 6, 12, 0);
    int first = smf_find(0x90, 60, 100, 0);
    CHECK(prog > sx && vol > prog && range > vol && first > range);
    // the 20 seconds before the first note are not in the file
    CHECK(smf[first].ms <= 200);
    uint32_t t0 = smf[first].ms;
    CHECK_EQ(smf[first + 1].status, 0x90);
    CHECK_EQ(smf[first + 1].d1, 64);
    int n = 0, at = 0;
    while ((at = smf_find(0x90, 60, 100, at)) >= 0) {
        CHECK_EQ(smf[at].ms, t0 + (uint32_t)n * 500);
        ++n;
        ++at;
    }
    CHECK_EQ(n, 20);
    // ends with the last note off, a second later the end of the track
    CHECK(smf[smf_count - 2].status == 0x80 && smf[smf_count - 2].d1 == 64);
    CHECK_EQ(smf[smf_count - 2].ms, t0 + 19 * 500 + 400);
    CHECK_EQ(smf_ms, t0 + 19 * 500 + 400 + 1000);
    CHECK(smf_find(0xc0, 0x30, 0, 0) < 0);

    printf("MIDI: the next file starts with the channels as they are\n");
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_WAITING);
    run_ms(5000);
    for (int i = 0; i < 12; ++i) {
        MIDI(0x91, 50, 100);
        run_ms(450);
        MIDI(0x91, 50, 0);              // note on with velocity 0: off
        run_ms(50);
    }
    run_ms(3100);
    CHECK(read_file("MID00002.MID"));
    CHECK(parse_smf());
    first = smf_find(0x91, 50, 100, 0);
    CHECK(first > 0);
    // from the state: volume, pan, the old program replaced by the new one,
    // the pitch bend range through its RPN, the pitch bend
    CHECK(smf_find(0xb0, 7, 100, 0) > 0 && smf_find(0xb0, 7, 100, 0) < first);
    CHECK(smf_find(0xb0, 10, 20, 0) > 0);
    CHECK(smf_find(0xc0, 0x30, 0, 0) > 0 && smf_find(0xc0, 0x30, 0, 0) < first);
    CHECK(smf_find(0xc0, 5, 0, 0) < 0);
    int r101 = smf_find(0xb1, 101, 0, 0), r100 = smf_find(0xb1, 100, 0, 0), r6 = smf_find(0xb1, 6, 12, 0);
    CHECK(r101 > 0 && r100 > r101 && r6 > r100 && r6 < first);
    CHECK(smf_find(0xe0, 0x00, 0x50, 0) > 0);
    CHECK(smf[first].ms <= 200);
    CHECK_EQ(count_files(), files0 + 2);

    printf("MIDI: a short sound is dropped, unless its file has SysEx\n");
    MIDI(0x92, 40, 100);
    run_ms(500);
    MIDI(0x82, 40, 0);
    run_ms(3100);
    CHECK_EQ(count_files(), files0 + 2);
    MIDI(0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7);
    run_ms(100);
    MIDI(0x92, 40, 100);
    run_ms(500);
    MIDI(0x82, 40, 0);
    run_ms(3100);
    CHECK_EQ(count_files(), files0 + 3);
    configure(0, 0, 1, 3, 5);
}

static void test_midi_hold(void) {
    printf("MIDI: a set-up burst after a song starts the next file at once\n");
    configure(0, 1, 1, 10, 0);
    for (int i = 0; i < 4; ++i) {
        MIDI(0x90, 60, 100);
        run_ms(400);
        MIDI(0x80, 60, 0);
        run_ms(100);
    }
    capture_status_t st;
    capture_get_status(&st);
    char first_name[13];
    strcpy(first_name, st.file[CAPTURE_MIDI]);
    // a second later, well within the gap: 3 SysEx messages of 150 bytes
    run_ms(1000);
    for (int k = 0; k < 3; ++k) {
        uint8_t sx[150];
        memset(sx, k + 1, sizeof(sx));
        sx[0] = 0xf0;
        sx[149] = 0xf7;
        midi_bytes(sx, sizeof(sx));
        run_ms(60);
    }
    MIDI(0xc3, 9);
    run_ms(500);
    for (int i = 0; i < 4; ++i) {
        MIDI(0x93, 62, 100);
        run_ms(400);
        MIDI(0x83, 62, 0);
        run_ms(100);
    }
    capture_get_status(&st);
    char second_name[13];
    strcpy(second_name, st.file[CAPTURE_MIDI]);
    CHECK(strcmp(first_name, second_name) != 0);
    configure(0, 0, 1, 3, 5);

    CHECK(read_file(first_name));
    CHECK(parse_smf());
    CHECK(smf_find_sysex(0xf0, 0) < 0);
    CHECK(smf_find(0x90, 60, 100, 0) > 0);
    CHECK(smf_find(0x93, 62, 100, 0) < 0);
    CHECK_EQ(smf[smf_count - 2].status, 0x80);

    CHECK(read_file(second_name));
    CHECK(parse_smf());
    int a = smf_find_sysex(0xf0, 0);
    CHECK(a > 0 && smf[a].len == 149 && smf[a].data[0] == 1);
    int b = smf_find_sysex(0xf0, a + 1);
    CHECK(b > a && smf[b].data[0] == 2);
    int c = smf_find_sysex(0xf0, b + 1);
    CHECK(c > b && smf[c].data[0] == 3 && smf[c].data[148] == 0xf7);
    int prog = smf_find(0xc3, 9, 0, c);
    CHECK(prog > c);
    int note = smf_find(0x93, 62, 100, 0);
    CHECK(note > prog);
    // the set-up is played without the waits it was sent with
    CHECK(smf[note].ms <= 400);

    printf("MIDI: a SysEx message longer than the buffer goes in parts\n");
    configure(0, 1, 0, 3, 0);
    static uint8_t big[1000];
    for (int i = 0; i < 1000; ++i) {
        big[i] = (uint8_t)(i % 100);
    }
    big[0] = 0xf0;
    for (int i = 0; i < 1000; i += 50) {
        midi_bytes(big + i, 50);    // at the speed of the MIDI output
        run_ms(16);
    }
    MIDI(0x90, 60, 100);                // ends the SysEx message without an F7
    run_ms(100);
    MIDI(0x80, 60, 0);
    capture_get_status(&st);
    strcpy(first_name, st.file[CAPTURE_MIDI]);
    configure(0, 0, 1, 3, 5);
    CHECK(read_file(first_name));
    CHECK(parse_smf());
    // put the parts together again
    static uint8_t whole[1200];
    uint32_t n = 0;
    a = smf_find_sysex(0xf0, 0);
    CHECK(a > 0);
    whole[n++] = 0xf0;
    memcpy(whole + n, smf[a].data, smf[a].len);
    n += smf[a].len;
    for (uint32_t i = (uint32_t)a + 1; i < smf_count && smf[i].status == 0xf7; ++i) {
        memcpy(whole + n, smf[i].data, smf[i].len);
        n += smf[i].len;
    }
    CHECK_EQ(n, 1001);
    CHECK(!memcmp(whole, big, 1000));
    CHECK_EQ(whole[1000], 0xf7);
    CHECK(smf_find(0x90, 60, 100, 0) > a);
}

static void test_midi_sync(void) {
    printf("MIDI: the file on the drive is complete while it is being written\n");
    configure(0, 1, 1, 3, 0);
    for (int i = 0; i < 30; ++i) {
        MIDI(0x90, 60, 100);
        run_ms(400);
        MIDI(0xc0, (uint8_t)i);         // events of 3 bytes: less than the end of track they overwrite
        MIDI(0x80, 60, 0);
        run_ms(100);
        if (i % 7 == 6 && i > 12) {      // the first sync is 5 s into the file
            capture_status_t st;
            capture_get_status(&st);
            CHECK(read_file(st.file[CAPTURE_MIDI]));
            CHECK(parse_smf_as(false));
            CHECK(smf_ms + 6000 >= (uint32_t)(i + 1) * 500);     // at most a sync interval behind
        }
    }
    capture_status_t st;
    capture_get_status(&st);
    char name[13];
    strcpy(name, st.file[CAPTURE_MIDI]);
    capture_cut();                      // new song now
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_WAITING);
    CHECK(read_file(name));
    CHECK(parse_smf());
    int n = 0, at = 0;
    while ((at = smf_find(0x90, 60, 100, at)) >= 0) {
        ++n;
        ++at;
    }
    CHECK_EQ(n, 30);
    configure(0, 0, 1, 3, 5);
}

static void test_store(void) {
    printf("Files: listing, reading, deleting, and names that are not ours\n");
    int before = count_files();
    CHECK(before > 0);
    // other files in the folder are not listed
    FIL fp;
    UINT bw;
    CHECK_EQ(f_open(&fp, CAPTURE_DIR "/NOTES.TXT", FA_CREATE_ALWAYS | FA_WRITE), FR_OK);
    f_write(&fp, "hello", 5, &bw);
    f_close(&fp);
    CHECK_EQ(f_mkdir(CAPTURE_DIR "/OPL99999.DRO"), FR_OK);  // a folder with a name of ours
    CHECK_EQ(count_files(), before);

    capture_file_t f;
    CHECK(capture_list_open());
    CHECK(capture_list_next(&f));
    capture_list_close();
    CHECK(f.size > 0 && !f.busy);
    uint32_t size = 0;
    CHECK(capture_read_open(f.name, &size) == NULL);
    CHECK_EQ(size, f.size);
    CHECK(capture_read_open(f.name, &size) != NULL);        // one at a time
    CHECK(capture_delete(f.name) != NULL);                  // not while it is read
    static uint8_t buf[4096];
    CHECK(read_file(f.name));
    uint32_t off = 0;
    while (off < size) {
        uint32_t n = size - off < 1000 ? size - off : 1000;
        CHECK_EQ(capture_read(off, buf, n), n);
        CHECK(!memcmp(buf, file_data + off, n));
        off += n;
    }
    CHECK_EQ(capture_read(0, buf, 16), 16);                 // and again from the start
    CHECK(!memcmp(buf, file_data, 16));
    capture_read_close();
    CHECK_EQ(capture_read(0, buf, 16), 0);

    CHECK(capture_read_open("NOTES.TXT", &size) != NULL);
    CHECK(capture_read_open("../CAPTURE/" "OPL00001.DRO", &size) != NULL);
    CHECK(capture_read_open("OPL0001.DRO", &size) != NULL);
    CHECK(capture_read_open("OPL00001.MID", &size) != NULL);
    CHECK(capture_read_open("OPL0000A.DRO", &size) != NULL);
    CHECK(capture_delete("CAPTURE.CFG") != NULL);
    CHECK(capture_delete("opl00001.dro") == NULL || capture_delete(f.name) == NULL);
    CHECK_EQ(count_files(), before - 1);
    CHECK(capture_delete("OPL77777.DRO") != NULL);

    printf("Files: a song being recorded can't be read or deleted\n");
    configure(1, 0, 1, 3, 0);
    opl(0xa0, 0x10);
    opl(0xb0, 0x30);
    run_ms(100);
    capture_status_t st;
    capture_get_status(&st);
    CHECK(st.file[CAPTURE_OPL][0]);
    CHECK(capture_read_open(st.file[CAPTURE_OPL], &size) != NULL);
    CHECK(capture_delete(st.file[CAPTURE_OPL]) != NULL);
    bool seen = false;
    CHECK(capture_list_open());
    while (capture_list_next(&f)) {
        if (!strcmp(f.name, st.file[CAPTURE_OPL])) {
            seen = true;
            CHECK(f.busy);
        } else {
            CHECK(!f.busy);
        }
    }
    capture_list_close();
    CHECK(seen);
    opl(0xb0, 0x10);
    configure(0, 0, 1, 3, 0);
    CHECK(capture_delete(st.file[CAPTURE_OPL]) == NULL);
}

static void test_config_and_drive(void) {
    printf("Drive: the settings are kept on the drive\n");
    configure(1, 1, 0, 7, 2);
    CHECK(read_file("CAPTURE.CFG"));
    file_data[file_len] = 0;
    CHECK(!strcmp((char *)file_data, "opl=1 midi=1 split=0 gap=7 min=2\r\n"));
    capture_config_t c;
    // values that make no sense are refused, and nothing changes
    capture_config_t bad = { {1, 1}, 1, 0, 5 };
    CHECK(capture_set_config(&bad) != NULL);
    bad.gap = 61;
    CHECK(capture_set_config(&bad) != NULL);
    capture_get_config(&c);
    CHECK(c.on[0] == 1 && c.on[1] == 1 && c.split == 0 && c.gap == 7 && c.minlen == 2);

    printf("Drive: pulled in the middle of a song, and back\n");
    opl(0xa0, 0x10);
    opl(0xb0, 0x30);
    MIDI(0x90, 60, 100);
    run_ms(500);
    capture_status_t st;
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_RECORDING);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_RECORDING);
    unmount_drive();
    capture_get_status(&st);
    CHECK_EQ(st.drive, 0);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_WAITING);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_WAITING);
    // music goes on without a drive: nothing is written, nothing breaks
    opl(0xb0, 0x10);
    run_ms(100);
    opl(0xb0, 0x30);
    MIDI(0x80, 60, 0, 0x90, 62, 100);
    run_ms(1000);
    CHECK(!capture_list_open());
    uint32_t size;
    CHECK(capture_read_open("OPL00001.DRO", &size) != NULL);
    capture_get_status(&st);
    CHECK(st.error[0] == 0);
    CHECK_EQ(f_mount(&fs, "", 1), FR_OK);
    capture_on_drive_mounted();
    capture_get_status(&st);
    CHECK_EQ(st.drive, 1);
    int before = count_files();
    opl(0xb0, 0x10);
    run_ms(100);
    opl(0xb0, 0x30);
    MIDI(0x80, 62, 0, 0x90, 64, 100);
    run_ms(500);
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_RECORDING);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_RECORDING);
    CHECK_EQ(count_files(), before + 2);

    printf("Drive: a write error stops the recorder and says so\n");
    ramdisk_set_write_fault(true);
    for (int i = 0; i < 400; ++i) {
        opl(0xa0, (uint32_t)i & 0xff);
        MIDI(0xb0, 1, (uint8_t)(i & 0x7f));
        run_ms(20);
    }
    ramdisk_set_write_fault(false);
    capture_get_status(&st);
    CHECK(st.error[0] != 0);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_OFF);
    CHECK_EQ(st.state[CAPTURE_MIDI], CAPTURE_OFF);
    capture_get_config(&c);
    CHECK(c.on[0] == 0 && c.on[1] == 0);
    CHECK(capture_rings[CAPTURE_OPL] == NULL && capture_rings[CAPTURE_MIDI] == NULL);
    // switching it on again clears the message
    configure(1, 0, 1, 3, 5);
    capture_get_status(&st);
    CHECK(st.error[0] == 0);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_WAITING);

    printf("Drive: an error while a song is ended by switching off\n");
    opl_play(24);
    capture_get_status(&st);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_RECORDING);
    ramdisk_set_write_fault(true);
    c = (capture_config_t){ {0, 0}, 1, 3, 0 };
    CHECK(capture_set_config(&c) != NULL);          // the song could not be finished
    ramdisk_set_write_fault(false);
    capture_get_status(&st);
    CHECK(st.error[0] != 0);
    CHECK_EQ(st.state[CAPTURE_OPL], CAPTURE_OFF);
    // and the recorder works again afterwards
    configure(1, 0, 1, 3, 0);
    int before2 = count_files();
    opl_play(4);
    run_ms(3100);
    capture_get_status(&st);
    CHECK(st.error[0] == 0);
    CHECK_EQ(count_files(), before2 + 1);
    configure(0, 0, 1, 3, 5);
}

// Copy the recorded files to a folder of the PC, for other programs to check
static void export_files(const char *dir) {
    capture_file_t f;
    char names[512][13];
    int n = 0;
    if (!capture_list_open()) {
        return;
    }
    while (n < 512 && capture_list_next(&f)) {
        strcpy(names[n++], f.name);
    }
    capture_list_close();
    for (int i = 0; i < n; ++i) {
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        FILE *out = fopen(path, "wb");
        if (out && read_file(names[i])) {
            fwrite(file_data, 1, file_len, out);
        }
        if (out) {
            fclose(out);
        }
    }
    printf("%d files copied to %s\n", n, dir);
}

// test_capture [folder]: with a folder, the files recorded by the tests are
// copied there at the end
int main(int argc, char **argv) {
    // close to where the 32 bit microsecond timer wraps
    test_clock_us = 0xffffffffull - 3000000;

    printf("Without a drive: nothing is recorded and nothing fails\n");
    opl(0xb0, 0x30);                    // not switched on: the tap does nothing
    CHECK(capture_rings[CAPTURE_OPL] == NULL);
    capture_task();
    CHECK(!capture_list_open());
    capture_status_t st;
    capture_get_status(&st);
    CHECK(st.drive == 0 && st.state[0] == CAPTURE_OFF && st.state[1] == CAPTURE_OFF);

    mount_new_drive();
    capture_config_t c;
    capture_get_config(&c);
    CHECK(c.on[0] == 0 && c.on[1] == 0 && c.split == 1 && c.gap == 3 && c.minlen == 5);
    CHECK_EQ(count_files(), 0);

    test_opl_songs();
    test_opl3();
    test_opl_time();
    test_midi_songs();
    test_midi_hold();
    test_midi_sync();
    test_store();
    test_config_and_drive();

    if (argc > 1) {
        export_files(argv[1]);
    }
    unmount_drive();
    ramdisk_free();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
