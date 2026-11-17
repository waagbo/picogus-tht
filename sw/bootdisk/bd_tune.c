/*
 *  PGBOOT start-up tunes: RTTTL text -> note stream for the boot ROM.
 *  See bd_tune.h and PROTOCOL.md (CMD_BDTUNE, CMD_BDMELODY, CMD_BDTUNEDATA).
 *
 *  RTTTL as phones and many tools write it:
 *      name:d=4,o=5,b=120:8c,e,g#.,2p,16a6
 *  d = default note length (1, 2, 4, 8, 16, 32), o = default octave, b =
 *  quarter notes per minute; a note is [length]letter[#][.][octave][.], p is
 *  a pause. Octave 4 holds the A of 440 Hz. Spaces and letter case do not
 *  matter; what cannot be read is skipped.
 */
#include <string.h>
#include "bd_tune.h"

/* The melodies that come with the firmware. 1-7 were made for PGBOOT: 5-7
 * in the manner of the soundtracks that made the Gravis UltraSound worth
 * having (a metal riff, a bouncy platform tune, an easy swing), not taken
 * from them. 8 is the opening of a game's theme, as the maintainer supplied
 * it. Add one by adding a line at the end and raising BD_TUNE_BUILTIN; the
 * numbers of the others then stay what users have saved. */
const char *const bd_tune_builtin[BD_TUNE_BUILTIN] = {
    "Rising:d=16,o=5,b=136:c,e,g,8c6",
    "Fanfare:d=16,o=5,b=136:c,g4,c,e,g.,32p,32g,8c6.",
    "Sweep:d=32,o=6,b=136:e,c,g5,e5,g5,c,e,8g",
    "Power Up:d=32,o=5,b=150:c,e,g,c6,e,g,c6,e6,g,c6,e6,8g6",
    "Demon Riff:d=16,o=4,b=160:8a,a,a,8c5,8a,a,a,8d#5,8a,a,a,8d5,8c5,8a#,4a",
    "Rabbit Run:d=16,o=5,b=170:c,p,e,g,8a,g,e,8c6,a,g,8e,8g,4c6",
    "Tycoon Swing:d=8,o=5,b=132:f.,16a,c6.,16d6,c6.,16a,g.,16a#,d6.,16c6,4a.,f",
    "Doom:d=4,o=5,b=200:8e4,8e4,8e5,8e4,8e4,8d5,8e4,8e4,8c5,8e4,8e4,8a#4,8e4,8e4,8b4,8c5,"
        "8e4,8e4,8e5,8e4,8e4,8d5,8e4,8e4,8c5,8e4,8e4,2a#4",
};

/* Timer divisors of C1..B1 (1193182 Hz / frequency, equal temperament,
 * A4 = 440 Hz); each octave up halves them */
static const uint16_t div_octave1[12] = {
    36485, 34437, 32505, 30680, 28958, 27333, 25799, 24351, 22984, 21694, 20477, 19327
};

/* semitone above C for the letters a..h (h is the German b), -1: no note */
static const int8_t semitone[8] = { 9, 11, 0, 2, 4, 5, 7, 11 };

typedef struct {
    uint8_t *out;
    size_t   len;
    unsigned entries;
    uint32_t units;         /* length of the stream so far */
} tune_out_t;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static unsigned number(const char **p) {
    unsigned v = 0;
    while (**p >= '0' && **p <= '9') {
        if (v < 10000) v = v * 10 + (unsigned)(**p - '0');
        (*p)++;
    }
    return v;
}

/* One stream entry, in pieces of at most BD_TUNE_ENTRY_UNITS. False: full. */
static int emit(tune_out_t *o, uint16_t div, uint32_t units) {
    while (units) {
        uint32_t n = units > BD_TUNE_ENTRY_UNITS ? BD_TUNE_ENTRY_UNITS : units;
        if (o->entries >= BD_TUNE_ENTRIES || o->units + n > BD_TUNE_TOTAL_UNITS) return 0;
        o->out[o->len++] = (uint8_t)div;
        o->out[o->len++] = (uint8_t)(div >> 8);
        o->out[o->len++] = (uint8_t)n;
        o->out[o->len++] = (uint8_t)(n >> 8);
        o->entries++;
        o->units += n;
        units -= n;
    }
    return 1;
}

size_t bd_tune_compile(const char *s, const char *name, uint8_t *out) {
    tune_out_t o = { out, 0, 0, 0 };
    unsigned def_len = 4, def_oct = 6, bpm = 63;
    const char *p, *colon, *q;
    uint64_t ns = 0;            /* where the melody is, exactly */
    size_t n = 0;

    if (s == NULL) s = "";
    /* name: up to the first colon, when a second one follows or the part
     * holds no '=' (then it is not the defaults) */
    p = s;
    colon = strchr(s, ':');
    if (colon && (strchr(colon + 1, ':') || !memchr(s, '=', (size_t)(colon - s)))) {
        while (is_space(*p)) p++;
        for (q = p; q < colon && n < BD_TUNE_NAME_MAX; q++) out[1 + n++] = (uint8_t)*q;
        while (n && is_space((char)out[n])) n--;
        p = colon + 1;
    }
    if (n == 0 && name) {
        n = strlen(name);
        if (n > BD_TUNE_NAME_MAX) n = BD_TUNE_NAME_MAX;
        memcpy(out + 1, name, n);
    }
    out[0] = (uint8_t)n;
    o.len = 1 + n;

    /* defaults: up to the next colon, if there is one */
    colon = strchr(p, ':');
    if (colon) {
        while (p < colon) {
            char key = lower(*p++);
            if ((key == 'd' || key == 'o' || key == 'b') && *p == '=') {
                unsigned v;
                p++;
                v = number(&p);
                if (key == 'd' && v >= 1 && v <= 64) def_len = v;
                if (key == 'o' && v >= 1 && v <= 8) def_oct = v;
                if (key == 'b' && v >= 20 && v <= 900) bpm = v;
            }
        }
        p = colon + 1;
    }

    /* notes */
    while (*p) {
        unsigned len, oct = def_oct, dotted = 0;
        uint32_t units, gap = 0;
        uint16_t div = 0;
        int semi;
        char c;

        while (is_space(*p) || *p == ',') p++;
        if (*p == 0) break;
        len = number(&p);
        if (len == 0 || len > 64) len = def_len;
        c = lower(*p);
        if (c == 'p') {
            semi = -1;
        } else if (c >= 'a' && c <= 'h') {
            semi = semitone[c - 'a'];
        } else {
            if (*p) p++;        /* not a note: on to the next thing */
            continue;
        }
        p++;
        if (*p == '#') { semi++; p++; }
        if (*p == '.') { dotted = 1; p++; }
        if (*p >= '1' && *p <= '8') oct = (unsigned)(*p++ - '0');
        if (*p == '.') { dotted = 1; p++; }

        /* a whole note is four quarters: 240 s / bpm */
        {
            uint64_t note_ns = 240000000000ull / bpm / len;
            if (dotted) note_ns += note_ns / 2;
            ns += note_ns;
        }
        units = (uint32_t)(ns / BD_TUNE_UNIT_NS) - o.units;     /* no rounding error adds up */
        if (units == 0) continue;
        if (semi >= 0) {
            if (semi > 11) { semi -= 12; if (oct < 8) oct++; }  /* b# */
            div = (uint16_t)((div_octave1[semi] + (1u << (oct - 1)) / 2) >> (oct - 1));
            /* a short silence ends every note, so that two of the same
             * pitch are two notes: an eighth of it, 20 ms at most */
            gap = units / 8;
            if (gap > 12) gap = 12;
            if (gap == 0 && units >= 4) gap = 1;
        }
        if (!emit(&o, div, units - gap)) break;
        if (gap && !emit(&o, 0, gap)) break;
    }
    out[o.len++] = 0xFF;
    out[o.len++] = 0xFF;
    return o.len;
}
