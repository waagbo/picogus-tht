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

// Minimal HTTP/1.0 server on the lwIP raw TCP API.
//
// One request per connection ("Connection: close"). Headers are parsed line by
// line and dropped, only the request line and Content-Length are kept. Request
// bodies are either small urlencoded forms, collected for the route's finish()
// handler, or streamed to the route's body() callback. A body consumer may take
// less than offered: the rest is held back and TCP's receive window is not
// reopened until it has been consumed, which throttles the sender (e.g. to MIDI
// wire speed or flash write speed).
//
// All of this runs inside web_task() on core 1.

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "pico/time.h"

#include "web_internal.h"
#include "web_xip_sram.h"

#ifndef WEB_HTTP_PORT
#define WEB_HTTP_PORT 80
#endif
#define HTTP_MAX_CONN       3
#define HTTP_BUF            576    // one header line at a time, then the form body
#define HTTP_IDLE_TIMEOUT_MS 15000
#define HTTP_MAX_DISCARD    65536  // body bytes read and dropped to answer an error cleanly
#define HTTP_POLL_INTERVAL  2      // lwIP poll callback every 2 * 500ms

typedef enum {
    CONN_FREE = 0,
    CONN_HEADERS,
    CONN_BODY,
    CONN_SENDING,
} conn_state_t;

struct http_conn {
    struct tcp_pcb *pcb;
    conn_state_t state;
    const web_route_t *route;
    int16_t route_status;       // HTTP error for the request line, 0 if routed
    int8_t asset;               // file of the web page that was asked for, -1 if none
    bool has_etag;              // the browser has a copy: If-None-Match
    uint32_t etag;
    bool have_request_line;
    bool skip_line;             // dropping the rest of an over-long header line
    bool responded;
    bool began;                 // the route's begin() accepted the request
    bool discard;               // dropping the body, then answering with err_status
    int16_t err_status;
    const char *err_msg;
    uint32_t unacked;           // received bytes not yet passed to tcp_recved()
    uint32_t content_length;
    uint32_t body_received;
    uint32_t last_activity_ms;
    // received data that the body consumer has not taken yet
    struct pbuf *pending;
    uint16_t pending_offset;
    // response body still to be queued (copied into lwIP buffers in chunks)
    const uint8_t *tx_ptr;
    uint32_t tx_left;
    uint16_t buf_len;
    char buf[HTTP_BUF];
};

// in XIP cache SRAM, see web_http_init()
static http_conn_t *conns;
char *web_scratch;
// set when a callback had to tcp_abort() its pcb; lwIP then wants ERR_ABRT back
static bool pcb_aborted;

// keep within this module's share of the web server's XIP SRAM (on the RP2040)
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(HTTP_MAX_CONN * sizeof(http_conn_t) + WEB_SCRATCH_SIZE <= 4096, "HTTP buffers too large");
#endif

bool web_http_init(void) {
    conns = web_xip_alloc(HTTP_MAX_CONN * sizeof(http_conn_t));
    web_scratch = web_xip_alloc(WEB_SCRATCH_SIZE);
    return conns && web_scratch;
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// ---- request helpers -------------------------------------------------------

const char *http_form_body(http_conn_t *c) {
    return c->buf;
}

uint32_t http_content_length(http_conn_t *c) {
    return c->content_length;
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool http_form_get(const char *form, const char *key, char *buf, size_t buflen) {
    size_t keylen = strlen(key);
    const char *p = form;
    while (p && *p) {
        if (!strncmp(p, key, keylen) && p[keylen] == '=') {
            p += keylen + 1;
            size_t n = 0;
            while (*p && *p != '&') {
                char ch = *p++;
                if (ch == '+') {
                    ch = ' ';
                } else if (ch == '%' && hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
                    ch = (char)(hexval(p[0]) << 4 | hexval(p[1]));
                    p += 2;
                }
                if (n + 1 < buflen) {
                    buf[n++] = ch;
                }
            }
            if (buflen) {
                buf[n] = 0;
            }
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            ++p;
        }
    }
    return false;
}

bool http_form_get_int(const char *form, const char *key, int32_t *out) {
    char buf[16];
    if (!http_form_get(form, key, buf, sizeof(buf)) || !buf[0]) {
        return false;
    }
    char *end;
    long v = strtol(buf, &end, 0);
    if (*end) {
        return false;
    }
    *out = (int32_t)v;
    return true;
}

// ---- connection management -------------------------------------------------

static void conn_free(http_conn_t *c, bool abort_pcb) {
    if (c->state == CONN_FREE) {
        return;
    }
    if (c->route && c->route->abort && c->began && !c->responded) {
        c->route->abort(c);
    }
    if (c->pending) {
        pbuf_free(c->pending);
        c->pending = NULL;
    }
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_sent(c->pcb, NULL);
        tcp_poll(c->pcb, NULL, 0);
        tcp_err(c->pcb, NULL);
        // lwIP resets a connection that is closed with unacknowledged data
        if (c->unacked) {
            tcp_recved(c->pcb, (u16_t)c->unacked);
            c->unacked = 0;
        }
        if (abort_pcb || tcp_close(c->pcb) != ERR_OK) {
            tcp_abort(c->pcb);
            pcb_aborted = true;
        }
        c->pcb = NULL;
    }
    c->state = CONN_FREE;
}

// Queue as much of the response body as lwIP takes, close the connection once
// everything is queued (lwIP keeps sending after the close). Data is copied:
// it may live in flash, which core 0 could erase between two web_task() calls.
static void conn_push(http_conn_t *c) {
    while (c->tx_left) {
        uint32_t n = tcp_sndbuf(c->pcb);
        if (n > c->tx_left) n = c->tx_left;
        if (n > TCP_MSS) n = TCP_MSS;
        if (n == 0 || tcp_sndqueuelen(c->pcb) >= TCP_SND_QUEUELEN - 1
            || tcp_write(c->pcb, c->tx_ptr, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;  // continued from the sent or poll callback
        }
        c->tx_ptr += n;
        c->tx_left -= n;
    }
    tcp_output(c->pcb);
    if (!c->tx_left) {
        conn_free(c, false);
    }
}

static char *put_str(char *p, char *end, const char *s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *put_uint(char *p, char *end, uint32_t v) {
    char num[11];
    char *n = num + sizeof(num) - 1;
    *n = 0;
    do { *--n = (char)('0' + v % 10); v /= 10; } while (v);
    return put_str(p, end, n);
}

// printf is not available in release builds, hence the hand-rolled formatting
// etag: for a file of the page, which the browser may keep but must ask about
// each time (a firmware update changes it); 0 for answers that must not be kept
static bool send_head(http_conn_t *c, int status, const char *content_type, uint32_t len, bool gzip, uint32_t etag) {
    const char *reason = status == 200 ? "OK" : status == 304 ? "Not Modified"
                       : status == 400 ? "Bad Request" : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict"
                       : status == 413 ? "Payload Too Large" : status == 431 ? "Header Too Large"
                       : status == 503 ? "Service Unavailable" : "Error";
    char head[224];
    char *end = head + sizeof(head);
    char *p = put_str(head, end, "HTTP/1.0 ");
    p = put_uint(p, end, (uint32_t)status);
    p = put_str(p, end, " ");
    p = put_str(p, end, reason);
    if (status != 304) {
        p = put_str(p, end, "\r\nContent-Type: ");
        p = put_str(p, end, content_type);
        p = put_str(p, end, "\r\nContent-Length: ");
        p = put_uint(p, end, len);
        if (gzip) {
            p = put_str(p, end, "\r\nContent-Encoding: gzip");
        }
    }
    if (etag) {
        static const char hex[] = "0123456789abcdef";
        char tag[9];
        for (int i = 0; i < 8; ++i) {
            tag[i] = hex[(etag >> (28 - 4 * i)) & 15];
        }
        tag[8] = 0;
        p = put_str(p, end, "\r\nETag: \"");
        p = put_str(p, end, tag);
        p = put_str(p, end, "\"\r\nCache-Control: no-cache");
    } else {
        p = put_str(p, end, "\r\nCache-Control: no-store");
    }
    p = put_str(p, end, "\r\nConnection: close\r\n\r\n");
    return tcp_write(c->pcb, head, (u16_t)(p - head), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) == ERR_OK;
}

static void respond(http_conn_t *c, int status, const char *content_type, const void *body, uint32_t len,
                    bool gzip, uint32_t etag) {
    if (c->responded || !c->pcb) {
        return;
    }
    c->responded = true;
    c->state = CONN_SENDING;
    if (!send_head(c, status, content_type, len, gzip, etag)) {
        conn_free(c, true);     // out of lwIP memory: drop rather than send garbage
        return;
    }
    c->tx_ptr = (const uint8_t *)body;
    c->tx_left = len;
    conn_push(c);
}

// A file of the web page, or "you have it" if the browser's copy is current
static void send_asset(http_conn_t *c) {
    web_asset_t a;
    if (!web_asset_get(c->asset, &a)) {
        http_send_error(c, 503, "the web page is being replaced");
    } else if (c->has_etag && c->etag == a.etag) {
        respond(c, 304, "", NULL, 0, false, a.etag);
    } else {
        respond(c, 200, a.type, a.data, a.length, a.gzip, a.etag);
    }
}

void http_send_json(http_conn_t *c, const char *json, uint32_t len) {
    // The JSON is built in web_scratch, which the next request reuses: it must
    // all go into lwIP's buffers now. A fresh connection has room for 2920 bytes.
    if (len + 200 > tcp_sndbuf(c->pcb)) {
        http_send_error(c, 503, "response too large");
        return;
    }
    respond(c, 200, "application/json", json, len, false, 0);
    if (c->state == CONN_SENDING && c->tx_left) {
        conn_free(c, true);
    }
}

void http_send_error(http_conn_t *c, int status, const char *message) {
    // {"error":"..."}; messages are fixed strings without quotes
    static char body[96];
    char *end = body + sizeof(body) - 2;
    char *p = put_str(body, end, "{\"error\":\"");
    p = put_str(p, end, message);
    *p++ = '"';
    *p++ = '}';
    respond(c, status, "application/json", body, (uint32_t)(p - body), false, 0);
}

// ---- request parsing -------------------------------------------------------

static void parse_request_line(http_conn_t *c, char *line) {
    uint8_t method;
    char *path;
    if (!strncmp(line, "GET ", 4)) {
        method = HTTP_GET;
        path = line + 4;
    } else if (!strncmp(line, "POST ", 5)) {
        method = HTTP_POST;
        path = line + 5;
    } else {
        c->route_status = 405;
        return;
    }
    // path ends at the query string or the protocol version
    path[strcspn(path, " ?")] = 0;
    bool path_found = false;
    for (uint32_t i = 0; i < web_routes_count; ++i) {
        if (!strcmp(web_routes[i].path, path)) {
            path_found = true;
            if (web_routes[i].method == method) {
                c->route = &web_routes[i];
                return;
            }
        }
    }
    // not part of the API: a file of the web page?
    if (method == HTTP_GET && !path_found) {
        c->asset = (int8_t)web_asset_find(path);
        // without the page, say what to do about it
        c->route_status = c->asset >= 0 ? 0 : strcmp(path, "/") ? 404 : 503;
        return;
    }
    c->route_status = path_found ? 405 : 404;
}

static void parse_header_line(http_conn_t *c, char *line) {
    if (!strncasecmp(line, "Content-Length:", 15)) {
        c->content_length = (uint32_t)strtoul(line + 15, NULL, 10);
    } else if (!strncasecmp(line, "If-None-Match:", 14)) {
        // "1a2b3c4d", possibly marked weak: W/"1a2b3c4d"
        const char *q = strchr(line + 14, '"');
        char *end;
        if (q) {
            c->etag = (uint32_t)strtoul(q + 1, &end, 16);
            c->has_etag = end == q + 9 && *end == '"';
        }
    }
}

static void body_complete(http_conn_t *c) {
    if (!c->route->body) {
        c->buf[c->buf_len] = 0;
    }
    c->route->finish(c);
    if (!c->responded) {
        http_send_error(c, 500, "no response");
    }
}

// Answer with an error. If the client is still sending a request body, read it
// first: closing a connection while data is coming in makes lwIP reset it,
// and the client may never see the answer.
static void fail_request(http_conn_t *c, int status, const char *msg) {
    if (c->content_length && c->content_length <= HTTP_MAX_DISCARD && c->body_received < c->content_length) {
        c->discard = true;
        c->err_status = (int16_t)status;
        c->err_msg = msg;
        c->state = CONN_BODY;
    } else {
        http_send_error(c, status, msg);
    }
}

static void headers_complete(http_conn_t *c) {
    c->body_received = 0;
    if (!c->route && c->asset >= 0) {
        send_asset(c);
        return;
    }
    if (!c->route) {
        fail_request(c, c->route_status ? c->route_status : 400,
                     c->route_status == 404 ? "not found" : c->route_status == 405 ? "method not allowed"
                     : c->route_status == 503 ? "the web page is missing: install the complete firmware file again"
                     : "bad request");
        return;
    }
    if (!c->route->body && c->content_length >= HTTP_BUF) {
        fail_request(c, 413, "request too large");
        return;
    }
    if (c->route->begin) {
        int status = c->route->begin(c);
        if (status) {
            fail_request(c, status, status == 409 ? "busy" : "rejected");
            return;
        }
        c->began = true;
    }
    c->state = CONN_BODY;
    c->body_received = 0;
    c->buf_len = 0;
    c->buf[0] = 0;
    if (c->content_length == 0) {
        body_complete(c);
    }
}

// Feed received bytes to the state machine. Returns how many were consumed.
static uint32_t conn_feed(http_conn_t *c, const uint8_t *data, uint32_t len) {
    uint32_t used = 0;
    while (used < len && !c->responded) {
        if (c->state == CONN_HEADERS) {
            char ch = (char)data[used++];
            if (c->skip_line) {
                c->skip_line = ch != '\n';
                continue;
            }
            if (ch != '\n') {
                if (c->buf_len < HTTP_BUF - 1) {
                    c->buf[c->buf_len++] = ch;
                } else if (!c->have_request_line) {
                    http_send_error(c, 431, "request line too long");
                } else {
                    // a header we don't care about (e.g. huge cookies): skip it
                    c->skip_line = true;
                    c->buf_len = 0;
                }
                continue;
            }
            // end of line
            if (c->buf_len && c->buf[c->buf_len - 1] == '\r') {
                --c->buf_len;
            }
            c->buf[c->buf_len] = 0;
            if (!c->have_request_line) {
                c->have_request_line = true;
                parse_request_line(c, c->buf);
            } else if (c->buf_len == 0) {
                headers_complete(c);
            } else {
                parse_header_line(c, c->buf);
            }
            c->buf_len = 0;
        } else if (c->state == CONN_BODY) {
            uint32_t want = c->content_length - c->body_received;
            uint32_t n = len - used < want ? len - used : want;
            if (c->discard) {
                used += n;
                c->body_received += n;
                if (c->body_received >= c->content_length) {
                    http_send_error(c, c->err_status, c->err_msg);
                }
                continue;
            }
            if (c->route->body) {
                int32_t took = c->route->body(c, data + used, n);
                if (took < 0) {
                    http_send_error(c, 400, "upload rejected");
                    break;
                }
                used += (uint32_t)took;
                c->body_received += (uint32_t)took;
                if ((uint32_t)took < n) {
                    return used;    // consumer is full: backpressure
                }
            } else {
                memcpy(&c->buf[c->buf_len], data + used, n);
                c->buf_len += (uint16_t)n;
                used += n;
                c->body_received += n;
            }
            if (c->body_received >= c->content_length) {
                body_complete(c);
            }
        } else {
            break;  // anything after the request is ignored
        }
    }
    return c->responded ? len : used;
}

// Process held-back data. Returns once everything was consumed or the
// consumer is full.
static void conn_drain(http_conn_t *c) {
    while (c->pending && c->state != CONN_FREE) {
        struct pbuf *p = c->pending;
        uint32_t avail = p->len - c->pending_offset;
        uint32_t took = conn_feed(c, (const uint8_t *)p->payload + c->pending_offset, avail);
        if (c->state == CONN_FREE || !c->pcb) {
            return;     // connection closed while handling the request
        }
        c->pending_offset += (uint16_t)took;
        if (c->pending_offset < p->len) {
            return;     // consumer is full
        }
        // segment fully consumed: unlink it (the rest of the chain keeps the
        // chain's reference) and reopen the receive window by its size
        uint16_t seglen = p->len;
        c->pending = p->next;
        p->next = NULL;
        p->tot_len = p->len;
        pbuf_free(p);
        c->pending_offset = 0;
        tcp_recved(c->pcb, seglen);
        c->unacked -= seglen;
    }
}

// ---- lwIP callbacks --------------------------------------------------------

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_conn_t *c = (http_conn_t *)arg;
    pcb_aborted = false;
    if (!c) {
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p || err != ERR_OK) {
        if (p) {
            pbuf_free(p);
        }
        // the client closed its side: an incomplete request is dropped,
        // a response being sent still goes out
        if (c->state != CONN_SENDING) {
            conn_free(c, false);
        }
        return pcb_aborted ? ERR_ABRT : ERR_OK;
    }
    c->last_activity_ms = now_ms();
    if (c->state == CONN_SENDING) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }
    c->unacked += p->tot_len;
    if (c->pending) {
        pbuf_cat(c->pending, p);
    } else {
        c->pending = p;
        c->pending_offset = 0;
    }
    conn_drain(c);
    return pcb_aborted ? ERR_ABRT : ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb;
    (void)len;
    http_conn_t *c = (http_conn_t *)arg;
    pcb_aborted = false;
    if (c) {
        c->last_activity_ms = now_ms();
        if (c->state == CONN_SENDING) {
            conn_push(c);
        }
    }
    return pcb_aborted ? ERR_ABRT : ERR_OK;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    (void)pcb;
    http_conn_t *c = (http_conn_t *)arg;
    pcb_aborted = false;
    if (!c) {
        return ERR_OK;
    }
    if (now_ms() - c->last_activity_ms > HTTP_IDLE_TIMEOUT_MS) {
        conn_free(c, true);
    } else if (c->state == CONN_SENDING) {
        conn_push(c);   // retry after lwIP ran out of buffers
    }
    return pcb_aborted ? ERR_ABRT : ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)err;
    http_conn_t *c = (http_conn_t *)arg;
    if (c) {
        c->pcb = NULL;  // lwIP already freed it
        conn_free(c, false);
    }
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    http_conn_t *c = NULL;
    for (int i = 0; i < HTTP_MAX_CONN; ++i) {
        if (conns[i].state == CONN_FREE) {
            c = &conns[i];
            break;
        }
    }
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    memset(c, 0, offsetof(http_conn_t, buf));
    c->asset = -1;
    c->pcb = pcb;
    c->state = CONN_HEADERS;
    c->last_activity_ms = now_ms();
    tcp_setprio(pcb, TCP_PRIO_MIN);
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_poll(pcb, on_poll, HTTP_POLL_INTERVAL);
    tcp_err(pcb, on_err);
    return ERR_OK;
}

bool web_http_start(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, WEB_HTTP_PORT) != ERR_OK) {
        tcp_abort(pcb);
        return false;
    }
    struct tcp_pcb *listen = tcp_listen_with_backlog(pcb, HTTP_MAX_CONN);
    if (!listen) {
        tcp_abort(pcb);
        return false;
    }
    tcp_accept(listen, on_accept);
    return true;
}

// Called from web_task() after the network stack was polled: offer held-back
// body data again to consumers that were full (e.g. the MIDI queue).
void web_http_poll(void) {
    for (int i = 0; i < HTTP_MAX_CONN; ++i) {
        http_conn_t *c = &conns[i];
        if (c->state == CONN_BODY && c->pending) {
            c->last_activity_ms = now_ms();
            conn_drain(c);
        }
    }
}
