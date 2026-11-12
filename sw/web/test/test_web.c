// Host tests for the PicoGUS web server.
//
// Runs the real lwIP TCP/IP stack with a loopback interface, and the real
// web_http.c / web_api.cpp / web_upload.c on top of it. A raw-API TCP client
// in the same stack sends requests, in deliberately awkward pieces, and
// checks the answers. Hardware (flash, MIDI out, the rest of the firmware) is
// replaced by the stubs below.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#include "web.h"
#include "web_internal.h"
#include "web_xip_sram.h"
#include "hardware/flash.h"
#include "system/flash_settings.h"
#include "../../common/picogus.h"
#include "mpu401/export.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- simulated platform ------------------------------------------------------

uint64_t test_now_us = 1000000;
unsigned char test_lwip_heap[MEM_SIZE + 64] __attribute__((aligned(8)));
u32_t sys_now(void) { return (u32_t)(test_now_us / 1000); }
u32_t lwip_port_rand(void) { return (u32_t)rand(); }
unsigned int web_rand(void) { return (unsigned int)rand(); }

// larger than on the RP2040: pointers take 8 bytes here
static uint8_t xip_web[2 * WEB_XIP_WEB_SIZE] __attribute__((aligned(8)));
static uint32_t xip_used;
void *web_xip_alloc(size_t size) {
    size = (size + 3) & ~3u;
    if (xip_used + size > sizeof(xip_web)) return NULL;
    void *p = xip_web + xip_used;
    xip_used += size;
    memset(p, 0, size);
    return p;
}

void web_xip_cache_off(void) {}
const void *web_flash_uncached(const void *p) { return p; }

static int flash_lock_depth;
void web_flash_acquire_blocking(void) { ++flash_lock_depth; }
bool web_flash_try_acquire(void) { ++flash_lock_depth; return true; }
void web_flash_release(void) { --flash_lock_depth; }

// ---- simulated firmware --------------------------------------------------------

Settings settings;
board_type_t BOARD_TYPE = PICO_BASED;
const char *firmware_string = "picogus-test v0.0.0";

static int writes;
static uint8_t last_reg;
static uint16_t last_value;
bool picogus_web_write(uint8_t reg, uint16_t value) {
    ++writes;
    last_reg = reg;
    last_value = value;
    switch (reg) {
    case CMD_SBVOL: settings.Volume.sbVol = (uint8_t)value; break;
    case CMD_SBPORT: settings.SB.basePort = value ? value : 0xffff; break;
    case CMD_GUSBUF: settings.GUS.audioBuffer = (uint8_t)(value + 1); break;
    case CMD_SBOPTS: settings.SB16.options = (uint8_t)value; break;
    case CMD_BOOTMODE: settings.startupMode = (uint8_t)value; break;
    }
    return true;
}
static bool save_requested;
void picogus_request_save(void) { save_requested = true; }
bool picogus_save_pending(void) { return false; }
static bool defaults_loaded;
void picogus_load_defaults(void) { defaults_loaded = true; getDefaultSettings(&settings); }
static int reboots;
static uint8_t reboot_mode;
void web_schedule_reboot(uint8_t mode, uint32_t delay_ms) { (void)delay_ms; ++reboots; reboot_mode = mode; }
static int wifi_connects;
void web_wifi_request_connect(void) { ++wifi_connects; }
void web_get_net_info(web_net_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->ssid = "TestNet";
    info->link = 3;
    info->rssi = -50;
}

// flash: 2MB NOR flash
static uint8_t flash_mem[PICO_FLASH_SIZE_BYTES];
static int erases, programs;
void flash_range_erase(uint32_t offs, size_t count) {
    assert(offs % FLASH_SECTOR_SIZE == 0 && count % FLASH_SECTOR_SIZE == 0);
    assert(flash_lock_depth > 0);
    memset(flash_mem + offs, 0xff, count);
    ++erases;
}
void flash_range_program(uint32_t offs, const uint8_t *data, size_t count) {
    assert(offs % FLASH_PAGE_SIZE == 0 && count == FLASH_PAGE_SIZE);
    assert(flash_lock_depth > 0);
    // data must not come from the XIP SRAM area (see fw_process_block)
    assert(!(data >= xip_web && data < xip_web + sizeof(xip_web)));
    for (size_t i = 0; i < count; ++i) {
        assert(flash_mem[offs + i] == 0xff);  // programming needs an erase first
        flash_mem[offs + i] &= data[i];
    }
    ++programs;
}

// MIDI out: a small queue drained at "wire speed" by the main loop
static uint8_t midi_queue[1024];
static uint32_t midi_queued;
static uint8_t midi_out[65536];
static uint32_t midi_out_len;
bool MIDI_Inject(const uint8_t *msg, uint32_t len) {
    if (len > sizeof(midi_queue) - midi_queued) return false;
    memcpy(midi_queue + midi_queued, msg, len);
    midi_queued += len;
    return true;
}
uint32_t MIDI_InjectFree(void) { return (uint32_t)(sizeof(midi_queue) - midi_queued); }
bool MIDI_InjectHeld(void) { return false; }
static void midi_drain(uint32_t n) {
    if (n > midi_queued) n = midi_queued;
    memcpy(midi_out + midi_out_len, midi_queue, n);
    midi_out_len += n;
    memmove(midi_queue, midi_queue + n, midi_queued - n);
    midi_queued -= n;
}

// ---- test client -----------------------------------------------------------------

typedef struct {
    struct tcp_pcb *pcb;
    const uint8_t *req;
    size_t req_len, sent;
    size_t chunk;           // max bytes per tcp_write, to split the request up
    char resp[32768];
    size_t resp_len;
    bool done;
} client_t;

static void client_push(client_t *c) {
    if (!c->pcb) return;
    while (c->sent < c->req_len) {
        size_t n = c->req_len - c->sent;
        if (n > c->chunk) n = c->chunk;
        if (n > tcp_sndbuf(c->pcb)) n = tcp_sndbuf(c->pcb);
        if (n == 0 || tcp_write(c->pcb, c->req + c->sent, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        c->sent += n;
        tcp_output(c->pcb);  // one segment per piece
    }
}

static err_t client_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    client_t *c = arg;
    (void)err;
    if (!p) {
        c->done = true;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_close(pcb);
        c->pcb = NULL;
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next) {
        size_t n = q->len;
        if (c->resp_len + n >= sizeof(c->resp)) n = sizeof(c->resp) - 1 - c->resp_len;
        memcpy(c->resp + c->resp_len, q->payload, n);
        c->resp_len += n;
    }
    c->resp[c->resp_len] = 0;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t client_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb; (void)len;
    client_push(arg);
    return ERR_OK;
}

static void client_err(void *arg, err_t err) {
    client_t *c = arg;
    (void)err;
    c->pcb = NULL;
    c->done = true;
}

static err_t client_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb; (void)err;
    client_push(arg);
    return ERR_OK;
}

static uint32_t midi_rate = 4;  // MIDI bytes drained per simulated millisecond

static void run_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
        test_now_us += 1000;
        netif_poll_all();
        sys_check_timeouts();
        web_http_poll();
        midi_drain(midi_rate);
    }
}

static client_t *client_start(const char *head, const uint8_t *body, size_t body_len, size_t chunk) {
    client_t *c = calloc(1, sizeof(client_t));
    size_t hl = strlen(head);
    uint8_t *req = malloc(hl + body_len);
    memcpy(req, head, hl);
    if (body_len) memcpy(req + hl, body, body_len);
    c->req = req;
    c->req_len = hl + body_len;
    c->chunk = chunk;
    c->pcb = tcp_new();
    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, client_recv);
    tcp_sent(c->pcb, client_sent);
    tcp_err(c->pcb, client_err);
    ip_addr_t lo;
    IP4_ADDR(ip_2_ip4(&lo), 127, 0, 0, 1);
    tcp_connect(c->pcb, &lo, 80, client_connected);
    return c;
}

static int client_wait(client_t *c, uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms && !c->done; ++t) {
        run_ms(1);
        client_push(c);
    }
    int status = 0;
    sscanf(c->resp, "HTTP/1.0 %d", &status);
    return status;
}

static const char *client_body(client_t *c) {
    char *b = strstr(c->resp, "\r\n\r\n");
    return b ? b + 4 : "";
}

static void client_free(client_t *c) {
    free((void *)c->req);
    free(c);
}

// One request; returns the HTTP status, body copied to out
static int request(const char *method, const char *path, const char *ctype, const void *body, size_t len,
                   size_t chunk, char *out, size_t outlen) {
    char head[512];
    snprintf(head, sizeof(head),
             "%s %s HTTP/1.1\r\nHost: picogus\r\nUser-Agent: test\r\n%s%s%sContent-Length: %zu\r\n\r\n",
             method, path, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "", len);
    client_t *c = client_start(head, body, len, chunk);
    int status = client_wait(c, 120000);
    if (out) {
        snprintf(out, outlen, "%s", client_body(c));
    }
    client_free(c);
    return status;
}

static int form(const char *path, const char *body, char *out, size_t outlen) {
    return request("POST", path, "application/x-www-form-urlencoded", body, strlen(body), 1460, out, outlen);
}

// ---- UF2 helpers ---------------------------------------------------------------

typedef struct {
    uint32_t magic_start0, magic_start1, flags, target_addr, payload_size, block_no, num_blocks, family_id;
    uint8_t data[476];
    uint32_t magic_end;
} uf2_t;

static void uf2_block(uf2_t *b, uint32_t addr, uint32_t no, uint32_t num, uint32_t family, uint8_t fill) {
    memset(b, 0, sizeof(*b));
    b->magic_start0 = 0x0A324655;
    b->magic_start1 = 0x9E5D5157;
    b->magic_end = 0x0AB16F30;
    b->flags = 0x2000;
    b->family_id = family;
    b->target_addr = addr;
    b->payload_size = 256;
    b->block_no = no;
    b->num_blocks = num;
    for (int i = 0; i < 256; ++i) b->data[i] = (uint8_t)(fill + i);
}

// ---- tests -----------------------------------------------------------------------

static char out[16384];

static void test_static_and_errors(void) {
    client_t *c = client_start("GET / HTTP/1.1\r\nHost: x\r\n\r\n", NULL, 0, 1460);
    int st = client_wait(c, 10000);
    CHECK(st == 200, "GET / -> %d", st);
    CHECK(strstr(c->resp, "Content-Encoding: gzip") != NULL, "page is gzip encoded");
    CHECK(strstr(c->resp, "Content-Type: text/html; charset=utf-8\r\n") != NULL, "page content type");
    const char *cl = strstr(c->resp, "Content-Length: ");
    size_t body_len = c->resp_len - (size_t)(client_body(c) - c->resp);
    CHECK(cl && (size_t)atoi(cl + 16) == body_len, "content length %d vs body %zu", cl ? atoi(cl + 16) : -1, body_len);
    CHECK(body_len > 20000 && (unsigned char)client_body(c)[0] == 0x1f, "the whole page, gzip magic (%zu bytes)", body_len);
    // the browser may keep the page, but has to ask whether it changed
    char etag[16] = "";
    const char *et = strstr(c->resp, "ETag: \"");
    if (et) memcpy(etag, et + 7, 8);
    CHECK(strlen(etag) == 8 && strstr(c->resp, "Cache-Control: no-cache"), "page has an ETag: %s", etag);
    client_free(c);

    // asked again with that ETag: 304 and no body
    char head[256];
    snprintf(head, sizeof(head), "GET /index.html HTTP/1.1\r\nHost: x\r\nIf-None-Match: \"%s\"\r\n\r\n", etag);
    c = client_start(head, NULL, 0, 1460);
    st = client_wait(c, 10000);
    CHECK(st == 304 && !*client_body(c) && strstr(c->resp, etag), "unchanged page -> %d, body %zu bytes", st, strlen(client_body(c)));
    client_free(c);
    c = client_start("GET / HTTP/1.1\r\nIf-None-Match: W/\"00000000\"\r\n\r\n", NULL, 0, 1460);
    st = client_wait(c, 10000);
    CHECK(st == 200, "another ETag gets the page: %d", st);
    client_free(c);

    // other files of the pack: text is compressed, pictures are not
    c = client_start("GET /static/hello.txt HTTP/1.1\r\n\r\n", NULL, 0, 1460);
    st = client_wait(c, 10000);
    CHECK(st == 200 && strstr(c->resp, "Content-Type: text/plain") && strstr(c->resp, "Content-Encoding: gzip"), "text file -> %d", st);
    client_free(c);
    c = client_start("GET /static/dot.png HTTP/1.1\r\n\r\n", NULL, 0, 1460);
    st = client_wait(c, 10000);
    CHECK(st == 200 && strstr(c->resp, "Content-Type: image/png") && !strstr(c->resp, "Content-Encoding")
          && !memcmp(client_body(c), "\x89PNG\r\n\x1a\n not really a picture", 29), "picture -> %d", st);
    client_free(c);
    CHECK(request("GET", "/static/nope.png", NULL, NULL, 0, 1460, out, sizeof(out)) == 404, "no such file");
    CHECK(request("POST", "/static/hello.txt", NULL, NULL, 0, 1460, out, sizeof(out)) == 404, "files can only be read");

    CHECK(request("GET", "/nope", NULL, NULL, 0, 1460, out, sizeof(out)) == 404, "404");
    CHECK(request("GET", "/api/set", NULL, NULL, 0, 1460, out, sizeof(out)) == 405, "405 on wrong method");
    CHECK(request("PUT", "/", NULL, NULL, 0, 1460, out, sizeof(out)) == 405, "405 on unknown method");
}

static void test_status(void) {
    // query strings are ignored, the request is sent one byte per segment
    client_t *c = client_start("GET /api/status?x=1 HTTP/1.1\r\nHost: x\r\nAccept: */*\r\n\r\n", NULL, 0, 1);
    int st = client_wait(c, 10000);
    CHECK(st == 200, "status -> %d", st);
    const char *b = client_body(c);
    CHECK(strstr(b, "\"fw\":\"picogus-test v0.0.0\"") != NULL, "fw in status: %s", b);
    CHECK(strstr(b, "\"sbvol\":") != NULL, "settings in status");
    CHECK(strstr(b, "\"ssid\":\"TestNet\"") != NULL, "net info in status");
    FILE *f = fopen("status.json", "w");
    if (f) { fputs(b, f); fclose(f); }
    client_free(c);
}

static void test_big_headers(void) {
    // A cookie header far longer than the server's buffer is skipped
    static char head[8192];
    char cookie[5000];
    memset(cookie, 'a', sizeof(cookie) - 1);
    cookie[sizeof(cookie) - 1] = 0;
    snprintf(head, sizeof(head), "POST /api/set HTTP/1.1\r\nHost: x\r\nCookie: c=%s\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 8\r\n\r\nsbvol=42", cookie);
    client_t *c = client_start(head, NULL, 0, 700);
    int st = client_wait(c, 10000);
    CHECK(st == 200, "big cookie -> %d", st);
    CHECK(settings.Volume.sbVol == 42, "sbvol applied: %d", settings.Volume.sbVol);
    client_free(c);

    // An over-long request line is refused
    memset(head, 0, sizeof(head));
    strcpy(head, "GET /");
    memset(head + 5, 'b', 2000);
    strcat(head, " HTTP/1.1\r\n\r\n");
    c = client_start(head, NULL, 0, 1460);
    st = client_wait(c, 10000);
    CHECK(st == 431, "long request line -> %d", st);
    client_free(c);
}

static void test_settings(void) {
    writes = 0;
    int st = form("/api/set", "sbvol=55&sbport=0x240&gusbuf=8", out, sizeof(out));
    CHECK(st == 200, "set -> %d %s", st, out);
    CHECK(settings.Volume.sbVol == 55 && settings.SB.basePort == 0x240, "applied: vol %d port %x",
          settings.Volume.sbVol, settings.SB.basePort);
    CHECK(settings.GUS.audioBuffer == 8, "gusbuf register holds size-1: %d", settings.GUS.audioBuffer);
    CHECK(writes == 3, "3 writes, got %d", writes);

    // all or nothing: one bad value rejects the whole request
    writes = 0;
    st = form("/api/set", "sbvol=10&sbtype=5", out, sizeof(out));
    CHECK(st == 400 && writes == 0, "invalid sb type -> %d, %d writes", st, writes);
    st = form("/api/set", "mainvol=101", out, sizeof(out));
    CHECK(st == 400, "volume range -> %d", st);
    st = form("/api/set", "sbvol=abc", out, sizeof(out));
    CHECK(st == 400, "not a number -> %d", st);

    // SB options are combined into one register
    settings.SB16.options = 0;
    st = form("/api/set", "sbfixtc=1&sblockmixer=2", out, sizeof(out));
    CHECK(st == 200 && settings.SB16.options == (1 | (2 << 1)), "sb options %x", settings.SB16.options);

    // a form body too large for the buffer
    static char big[2000];
    memset(big, 'x', sizeof(big) - 1);
    st = form("/api/set", big, out, sizeof(out));
    CHECK(st == 413, "huge form -> %d", st);

    save_requested = false;
    CHECK(form("/api/save", "", out, sizeof(out)) == 200 && save_requested, "save");
    CHECK(form("/api/defaults", "", out, sizeof(out)) == 200 && defaults_loaded, "defaults");
    reboots = 0;
    CHECK(form("/api/reboot", "", out, sizeof(out)) == 200 && reboots == 1, "reboot");
    CHECK(form("/api/wifi", "ssid=My+Net&pass=p%26ss", out, sizeof(out)) == 200, "wifi");
    CHECK(!strcmp(settings.WiFi.ssid, "My Net") && !strcmp(settings.WiFi.password, "p&ss") && wifi_connects == 1,
          "wifi credentials decoded: '%s' '%s'", settings.WiFi.ssid, settings.WiFi.password);
    CHECK(form("/api/wifi", "pass=x", out, sizeof(out)) == 400, "wifi without ssid");
}

static void test_mode(void) {
    reboots = 0;
    save_requested = false;
    int st = form("/api/mode", "mode=5&save=1", out, sizeof(out));
#ifdef PICOGUS_MULTIFW
    CHECK(st == 200 && reboots == 1 && reboot_mode == 5 && save_requested && settings.startupMode == 5,
          "mode switch -> %d", st);
    CHECK(form("/api/mode", "mode=7", out, sizeof(out)) == 400, "NE2000 is not in the multifw image");
#else
    CHECK(st == 400 && reboots == 0, "single mode firmware -> %d", st);
#endif
}

static void test_midi(void) {
    midi_out_len = 0;
    int st = form("/api/midi", "hex=F07D0301F7", out, sizeof(out));
    CHECK(st == 200, "midi -> %d %s", st, out);
    st = form("/api/midi", "hex=90+3C+7F+80%203C%2000", out, sizeof(out));
    CHECK(st == 200, "note on/off with spaces -> %d %s", st, out);
    run_ms(100);
    static const uint8_t want[] = {0xF0, 0x7D, 0x03, 0x01, 0xF7, 0x90, 0x3C, 0x7F, 0x80, 0x3C, 0x00};
    CHECK(midi_out_len == sizeof(want) && !memcmp(midi_out, want, sizeof(want)), "midi bytes out (%u)", midi_out_len);

    CHECK(form("/api/midi", "hex=F07D03", out, sizeof(out)) == 400, "unterminated sysex");
    CHECK(form("/api/midi", "hex=903C", out, sizeof(out)) == 400, "truncated note on");
    CHECK(form("/api/midi", "hex=3C7F", out, sizeof(out)) == 400, "data without status");
    CHECK(form("/api/midi", "hex=90FF00", out, sizeof(out)) == 400, "status byte as data");
    CHECK(form("/api/midi", "hex=F0Z", out, sizeof(out)) == 400, "bad hex");
    CHECK(form("/api/midi", "hex=F07", out, sizeof(out)) == 400, "odd digits");
}

static void test_syx_upload(void) {
    // 40 sysex messages of 300 bytes with junk in between; MIDI drains slowly so
    // the upload has to be throttled through TCP backpressure
    static uint8_t file[40 * 310];
    size_t n = 0;
    for (int m = 0; m < 40; ++m) {
        file[n++] = 0xF0;
        for (int i = 0; i < 298; ++i) file[n++] = (uint8_t)((m + i) & 0x7f);
        file[n++] = 0xF7;
        file[n++] = 0x0a;   // junk between messages is ignored
    }
    midi_out_len = 0;
    midi_rate = 3;          // ~3 bytes per ms = MIDI wire speed
    int st = request("POST", "/api/upload/syx", "application/octet-stream", file, n, 1460, out, sizeof(out));
    CHECK(st == 200 && strstr(out, "\"messages\":40"), "syx upload -> %d %s", st, out);
    run_ms(2000);
    CHECK(midi_out_len == 40 * 300, "all sysex bytes out: %u", midi_out_len);
    bool ok = true;
    for (int m = 0, k = 0; m < 40 && ok; ++m) {
        ok = midi_out[k++] == 0xF0;
        for (int i = 0; i < 298 && ok; ++i) ok = midi_out[k++] == ((m + i) & 0x7f);
        ok = ok && midi_out[k++] == 0xF7;
    }
    CHECK(ok, "sysex stream intact and in order");

    static const uint8_t junk[] = "hello world";
    st = request("POST", "/api/upload/syx", "application/octet-stream", junk, sizeof(junk), 1460, out, sizeof(out));
    CHECK(st == 400, "no sysex in file -> %d %s", st, out);
    midi_rate = 4;
}

static void test_fw_upload(void) {
    // Image: 3 sectors at the start of flash, one block for the settings
    // sector (must be skipped) and one block for another chip family
    const uint32_t RP2040 = 0xe48bff56;
    enum { N = 3 * 16 + 2 };
    static uf2_t blocks[N];
    int k = 0;
    for (int i = 0; i < 48; ++i) {
        uf2_block(&blocks[k], 0x10000000 + i * 256, k, N, RP2040, (uint8_t)i);
        ++k;
    }
    uf2_block(&blocks[k], 0x10000000 + PICO_FLASH_SIZE_BYTES - 4096, k, N, RP2040, 0x55); ++k;
    uf2_block(&blocks[k], 0x10003000, k, N, 0xe48bff59 /* RP2350 */, 0x66); ++k;

    memset(flash_mem, 0xa5, sizeof(flash_mem));
    erases = programs = 0;
    reboots = 0;
    int st = request("POST", "/api/upload/firmware", "application/octet-stream", blocks, sizeof(blocks), 1000, out, sizeof(out));
    CHECK(st == 200 && reboots == 1 && reboot_mode == 0, "fw upload -> %d %s", st, out);
    CHECK(erases == 3 && programs == 48, "erases %d programs %d", erases, programs);
    bool ok = true;
    for (int i = 0; i < 48 && ok; ++i)
        for (int j = 0; j < 256 && ok; ++j) ok = flash_mem[i * 256 + j] == (uint8_t)(i + j);
    CHECK(ok, "flash content");
    CHECK(flash_mem[0x3000] == 0xa5 && flash_mem[PICO_FLASH_SIZE_BYTES - 4096] == 0xa5,
          "other family / settings sector untouched");
    CHECK(flash_lock_depth == 0, "flash lock released (%d)", flash_lock_depth);

    // not a UF2: rejected before touching flash
    static uint8_t junk[1024];
    memset(junk, 0x11, sizeof(junk));
    erases = programs = 0;
    st = request("POST", "/api/upload/firmware", "application/octet-stream", junk, sizeof(junk), 1460, out, sizeof(out));
    CHECK(st == 400 && strstr(out, "not a UF2") && erases == 0, "junk upload -> %d %s", st, out);
    CHECK(request("POST", "/api/upload/firmware", "application/octet-stream", junk, 1000, 1460, out, sizeof(out)) == 400,
          "size not a multiple of 512");

    // broken in the middle: the error says the firmware is incomplete
    memcpy(&blocks[20], junk, 512);
    st = request("POST", "/api/upload/firmware", "application/octet-stream", blocks, sizeof(blocks), 1460, out, sizeof(out));
    CHECK(st == 400 && strstr(out, "incomplete"), "broken upload -> %d %s", st, out);
    CHECK(flash_lock_depth == 0, "flash lock released after failure (%d)", flash_lock_depth);

    // connection lost mid-upload, then a new upload must be possible
    client_t *c = client_start("POST /api/upload/firmware HTTP/1.1\r\nContent-Length: 25600\r\n\r\n",
                               (const uint8_t *)blocks, 5120, 1460);
    run_ms(200);
    tcp_abort(c->pcb);
    c->pcb = NULL;
    run_ms(200);
    CHECK(flash_lock_depth == 0, "flash lock released after abort (%d)", flash_lock_depth);
    web_upload_info_t info;
    web_get_upload_info(&info);
    CHECK(info.state == 3 && info.error && strstr(info.error, "incomplete"), "abort is reported");
    client_free(c);
    uf2_block(&blocks[20], 0x10000000 + 20 * 256, 20, N, RP2040, 20);
    st = request("POST", "/api/upload/firmware", "application/octet-stream", blocks, sizeof(blocks), 1460, out, sizeof(out));
    CHECK(st == 200, "upload after abort -> %d %s", st, out);
}

static void test_connection_limit(void) {
    // Three slow clients occupy all connections; a fourth is refused, then all
    // clean up and the server is usable again
    client_t *c[4];
    for (int i = 0; i < 4; ++i) {
        c[i] = client_start("GET /api/status HTTP/1.1\r\n", NULL, 0, 1460);
        run_ms(5);
    }
    run_ms(50);
    CHECK(c[3]->done && c[3]->resp_len == 0, "fourth connection refused");
    run_ms(20000);  // idle timeout
    for (int i = 0; i < 3; ++i) {
        CHECK(c[i]->done, "idle connection %d closed", i);
    }
    for (int i = 0; i < 4; ++i) {
        if (c[i]->pcb) tcp_abort(c[i]->pcb);
        client_free(c[i]);
    }
    CHECK(request("GET", "/api/status", NULL, NULL, 0, 1460, out, sizeof(out)) == 200, "server usable again");
}

static void test_long_run(void) {
    // 10 minutes of the UI polling every 3s with a page reload every 30s:
    // nothing may leak (TIME_WAIT connections come out of lwIP's small heap)
    int bad = 0;
    for (int i = 0; i < 200; ++i) {
        if (request("GET", "/api/status", NULL, NULL, 0, 1460, out, sizeof(out)) != 200) ++bad;
        if (i % 10 == 0 && request("GET", "/", NULL, NULL, 0, 1460, NULL, 0) != 200) ++bad;
        run_ms(3000);
    }
    CHECK(bad == 0, "%d failed requests during the long run", bad);
}

int main(void) {
    lwip_init();
    getDefaultSettings(&settings);
    settings.startupMode = SB_MODE;
    CHECK(web_http_init() && web_upload_init(), "buffers fit in the XIP SRAM share (%u bytes)", xip_used);
    CHECK(web_http_start(), "server starts");

    test_static_and_errors();
    test_status();
    test_big_headers();
    test_settings();
    test_mode();
    test_midi();
    test_syx_upload();
    test_fw_upload();
    test_connection_limit();
    test_long_run();

    printf("XIP SRAM used by web buffers: %u of %u bytes\n", xip_used, (unsigned)WEB_XIP_WEB_SIZE);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
