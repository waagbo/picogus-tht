/*
 *  PGBOOT start-up tunes: melodies in RTTTL text, compiled on the card to
 *  the note stream the boot ROM plays on the PC speaker (PROTOCOL.md,
 *  CMD_BDTUNEDATA).
 *
 *  Portable C, no Pico SDK dependencies (host tests: test/test_bootdisk.c).
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>

/* The stream: n, n name characters, then 4 bytes per entry (timer divisor
 * u16, 0 = silence; length u16 in BD_TUNE_UNITs), then FFh FFh. */
#define BD_TUNE_NAME_MAX    24
#define BD_TUNE_ENTRIES     160     /* notes, the gaps after them, rests */
#define BD_TUNE_STREAM_MAX  (1 + BD_TUNE_NAME_MAX + 4 * BD_TUNE_ENTRIES + 2)

/* One length unit is 1/32 of a BIOS timer tick: 65536 / 1193182 Hz / 32 =
 * 1716.42 us. The ROM measures it with timer channel 0. */
#define BD_TUNE_UNIT_NS     1716419u
#define BD_TUNE_ENTRY_UNITS 4000    /* longest entry: the ROM shifts it left by up to 4 in 16 bits */
#define BD_TUNE_TOTAL_UNITS 11650   /* a melody is cut after about 20 s */
#define BD_TUNE_PIT_HZ      1193182u

/* The melodies that come with the firmware, as RTTTL: tune 1 is index 0 */
#define BD_TUNE_BUILTIN     8
extern const char *const bd_tune_builtin[BD_TUNE_BUILTIN];

/* Compiles RTTTL text ("name:d=4,o=5,b=120:c,e,g", the name and the
 * defaults may be left out) into out[BD_TUNE_STREAM_MAX]. name: used when
 * the text has none (may be NULL). Returns the stream length. Text that
 * holds no note gives a stream with a name and no entries: silence. */
size_t bd_tune_compile(const char *rtttl, const char *name, uint8_t *out);

#ifdef __cplusplus
}
#endif
