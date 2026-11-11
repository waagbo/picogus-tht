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

// HTTP API of the PicoGUS web UI. Runs on core 1 from web_task().

#include <string.h>

#include "pico/stdlib.h"
#include "lwip/ip4_addr.h"

#include "../common/picogus.h"
#include "system/flash_settings.h"
#ifdef SOUND_MPU
#include "mpu401/export.h"
#endif
#ifdef CDROM
#include "cdrom/cdrom.h"
#include "cdrom/cdrom_image_manager.h"
extern cdrom_t cdrom;
#endif

#include "web.h"
#include "web_internal.h"
#include "web_assets.h"

extern Settings settings;
extern board_type_t BOARD_TYPE;

// ---- tiny JSON writer into web_scratch -------------------------------------

namespace {

struct Json {
    char *buf;
    uint32_t len;
    uint32_t cap;
    bool first;

    explicit Json(char *b, uint32_t c) : buf(b), len(0), cap(c), first(true) { buf[0] = 0; }

    void raw(const char *s) {
        while (*s && len < cap - 1) buf[len++] = *s++;
        buf[len] = 0;
    }
    void ch(char c) {
        if (len < cap - 1) buf[len++] = c;
        buf[len] = 0;
    }
    void uint(uint32_t v) {
        char num[11];
        char *p = num + sizeof(num) - 1;
        *p = 0;
        do { *--p = '0' + v % 10; v /= 10; } while (v);
        raw(p);
    }
    void sint(int32_t v) {
        if (v < 0) { ch('-'); uint((uint32_t)(-v)); } else { uint((uint32_t)v); }
    }
    void str(const char *s) {
        static const char hex[] = "0123456789abcdef";
        ch('"');
        for (; *s; ++s) {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\') { ch('\\'); ch((char)c); }
            else if (c < 0x20) { raw("\\u00"); ch(hex[c >> 4]); ch(hex[c & 15]); }
            else ch((char)c);
        }
        ch('"');
    }
    void key(const char *k) {
        if (!first) ch(',');
        first = false;
        str(k);
        ch(':');
    }
    void open(const char *k, char brace) {
        if (k) key(k); else if (!first) ch(',');
        ch(brace);
        first = true;
    }
    void close(char brace) { ch(brace); first = false; }
    void kv(const char *k, int32_t v) { key(k); sint(v); }
    void kvs(const char *k, const char *v) { key(k); str(v); }
    void item(const char *v) { if (!first) ch(','); first = false; str(v); }
    bool full() const { return len >= cap - 2; }
};

// Settings exposed in the web UI and the control register used to apply them
enum Kind : uint8_t { K_U8, K_BOOL, K_PORT, K_U16 };
struct SettingDef {
    const char *name;
    uint8_t reg;
    Kind kind;
    int32_t min, max;
};

const SettingDef setting_defs[] = {
    {"joy",        CMD_JOYEN,      K_BOOL, 0, 1},
    {"gusport",    CMD_GUSPORT,    K_PORT, 0, 0x3ff},
    {"gusbuf",     CMD_GUSBUF,     K_U8,   1, 256},
    {"gusdma",     CMD_GUSDMA,     K_U8,   0, 255},
    {"gus44k",     CMD_GUS44K,     K_BOOL, 0, 1},
    {"sbport",     CMD_SBPORT,     K_PORT, 0, 0x3ff},
    {"sbirq",      CMD_SBIRQ,      K_U8,   2, 15},
    {"sbdma",      CMD_SBDMA,      K_U8,   0, 3},
    {"sbtype",     CMD_SBTYPE,     K_U8,   1, 6},
    {"sbfixtc",    CMD_SBOPTS,     K_BOOL, 0, 1},
    {"sblockmixer", CMD_SBOPTS,    K_U8,   0, 2},
    {"oplport",    CMD_OPLPORT,    K_PORT, 0, 0x3ff},
    {"oplwait",    CMD_OPLWAIT,    K_BOOL, 0, 1},
    {"mpuport",    CMD_MPUPORT,    K_PORT, 0, 0x3ff},
    {"mpudelay",   CMD_MPUDELAY,   K_BOOL, 0, 1},
    {"mpufake",    CMD_MPUFAKE,    K_BOOL, 0, 1},
    {"mpuwait",    CMD_MPUWAIT,    K_BOOL, 0, 1},
    {"tandyport",  CMD_TANDYPORT,  K_PORT, 0, 0x3ff},
    {"cmsport",    CMD_CMSPORT,    K_PORT, 0, 0x3ff},
    {"mouseport",  CMD_MOUSEPORT,  K_PORT, 0, 0x3ff},
    {"mouseproto", CMD_MOUSEPROTO, K_U8,   0, 3},
    {"mouserate",  CMD_MOUSERATE,  K_U8,   20, 200},
    {"mousesen",   CMD_MOUSESEN,   K_U16,  0, 1024},
    {"cdport",     CMD_CDPORT,     K_PORT, 0, 0x3ff},
    {"cdauto",     CMD_CDAUTOADV,  K_BOOL, 0, 1},
    {"mainvol",    CMD_MAINVOL,    K_U8,   0, 100},
    {"oplvol",     CMD_OPLVOL,     K_U8,   0, 100},
    {"sbvol",      CMD_SBVOL,      K_U8,   0, 100},
    {"cdvol",      CMD_CDVOL,      K_U8,   0, 100},
    {"gusvol",     CMD_GUSVOL,     K_U8,   0, 100},
    {"psgvol",     CMD_PSGVOL,     K_U8,   0, 100},
};

uint16_t port_value(uint16_t port) {
    return port == 0xffff ? 0 : port;
}

// Current value of a setting, in the units used by the web UI
int32_t setting_value(const SettingDef &d) {
    switch (d.reg) {
    case CMD_JOYEN:      return settings.Joy.basePort == 0x201;
    case CMD_GUSPORT:    return port_value(settings.GUS.basePort);
    case CMD_GUSBUF:     return settings.GUS.audioBuffer;
    case CMD_GUSDMA:     return settings.GUS.dmaInterval;
    case CMD_GUS44K:     return settings.GUS.force44k;
    case CMD_SBPORT:     return port_value(settings.SB.basePort);
    case CMD_SBIRQ:      return settings.SB16.irq;
    case CMD_SBDMA:      return settings.SB16.dma;
    case CMD_SBTYPE:     return settings.SB16.sbType;
    case CMD_SBOPTS:     return d.name[2] == 'f' ? settings.SB16.fixTC : settings.SB16.lockMixer;
    case CMD_OPLPORT:    return port_value(settings.SB.oplBasePort);
    case CMD_OPLWAIT:    return settings.SB.oplSpeedSensitive;
    case CMD_MPUPORT:    return port_value(settings.MPU.basePort);
    case CMD_MPUDELAY:   return settings.MPU.delaySysex;
    case CMD_MPUFAKE:    return settings.MPU.fakeAllNotesOff;
    case CMD_MPUWAIT:    return settings.MPU.waitBusyStatus;
    case CMD_TANDYPORT:  return port_value(settings.Tandy.basePort);
    case CMD_CMSPORT:    return port_value(settings.CMS.basePort);
    case CMD_MOUSEPORT:  return port_value(settings.Mouse.basePort);
    case CMD_MOUSEPROTO: return settings.Mouse.protocol;
    case CMD_MOUSERATE:  return settings.Mouse.reportRate;
    case CMD_MOUSESEN:   return settings.Mouse.sensitivity;
    case CMD_CDPORT:     return port_value(settings.CD.basePort);
    case CMD_CDAUTOADV:  return settings.CD.autoAdvance;
    case CMD_MAINVOL:    return settings.Volume.mainVol;
    case CMD_OPLVOL:     return settings.Volume.oplVol;
    case CMD_SBVOL:      return settings.Volume.sbVol;
    case CMD_CDVOL:      return settings.Volume.cdVol;
    case CMD_GUSVOL:     return settings.Volume.gusVol;
    case CMD_PSGVOL:     return settings.Volume.psgVol;
    }
    return 0;
}

// Apply a setting through the same code path pgusinit uses
bool apply_setting(const SettingDef &d, int32_t v) {
    if (v < d.min || v > d.max) {
        return false;
    }
    uint16_t reg_value = (uint16_t)v;
    switch (d.reg) {
    case CMD_GUSBUF:
        reg_value = (uint16_t)(v - 1);  // register holds size - 1
        break;
    case CMD_SBOPTS: {
        uint8_t fixtc = settings.SB16.fixTC;
        uint8_t lock = settings.SB16.lockMixer;
        if (d.name[2] == 'f') fixtc = (uint8_t)v; else lock = (uint8_t)v;
        reg_value = (uint16_t)((fixtc & 1) | ((lock & 3) << 1));
        break;
    }
    case CMD_SBTYPE:
        if (v == 5) return false;       // no such SB type
        break;
    }
    return picogus_web_write(d.reg, reg_value);
}

void json_features(Json &j) {
    j.open("features", '[');
#ifdef SOUND_GUS
    j.item("gus");
#endif
#ifdef SOUND_SB
    j.item("sb");
#endif
#ifdef SOUND_OPL
    j.item("opl");
#endif
#ifdef SOUND_MPU
    j.item("mpu");
#endif
#ifdef SOUND_TANDY
    j.item("tandy");
#endif
#ifdef SOUND_CMS
    j.item("cms");
#endif
#ifdef USB_JOYSTICK
    j.item("joy");
#endif
#ifdef USB_MOUSE
    j.item("mouse");
#endif
#ifdef CDROM
    j.item("cd");
#endif
#ifdef PICOGUS_MULTIFW
    j.item("multifw");
#endif
#if PICOGUS_WEB_MDNS
    j.item("mdns");
#endif
    j.close(']');
}

void json_status(Json &j) {
    web_net_info_t net;
    web_get_net_info(&net);
    web_upload_info_t up;
    web_get_upload_info(&up);

    j.open(nullptr, '{');
    j.kvs("fw", firmware_string);
    j.kv("mode", settings.startupMode);
    j.kv("board", BOARD_TYPE);
    // the chip this firmware is for, as firmware files are per chip
#if PICO_RP2350
    j.kvs("chip", "RP2350");
#else
    j.kvs("chip", "RP2040");
#endif
    j.kv("uptime", (int32_t)(to_ms_since_boot(get_absolute_time()) / 1000));
    json_features(j);

    j.open("net", '{');
    j.kvs("ssid", net.ssid);
    j.kv("link", net.link);
    j.kv("rssi", net.rssi);
    ip4_addr_t ip;
    ip4_addr_set_u32(&ip, net.ip);
    j.kvs("ip", ip4addr_ntoa(&ip));
    char mac[18];
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        mac[i * 3] = hex[net.mac[i] >> 4];
        mac[i * 3 + 1] = hex[net.mac[i] & 15];
        mac[i * 3 + 2] = i < 5 ? ':' : 0;
    }
    j.kvs("mac", mac);
    j.kv("pollMaxUs", (int32_t)net.poll_max_us);
    j.kv("pollRecentUs", (int32_t)net.poll_max_recent_us);
    j.kv("loadPermille", net.load_permille);
    j.close('}');

    j.open("s", '{');
    for (const SettingDef &d : setting_defs) {
        j.kv(d.name, setting_value(d));
    }
    j.close('}');

    j.kv("savePending", picogus_save_pending());
#ifdef SOUND_MPU
    j.kv("midiFree", (int32_t)MIDI_InjectFree());
    j.kv("midiHeld", MIDI_InjectHeld());
#endif

    j.open("upload", '{');
    j.kv("kind", up.kind);
    j.kv("state", up.state);
    j.kv("done", (int32_t)up.done);
    j.kv("total", (int32_t)up.total);
    j.kvs("error", up.error ? up.error : "");
    j.close('}');

#ifdef CDROM
    j.open("cd", '{');
    j.kv("index", cdman_current_image_index());
    j.kvs("image", cdrom.disk_loaded ? cdrom.image_path : "");
    j.kv("status", cdrom.image_status);
    j.kvs("error", cdrom.error_str ? cdrom.error_str : "");
    j.close('}');
#endif
    j.close('}');
}

void send_status(http_conn_t *c) {
    Json j(web_scratch, WEB_SCRATCH_SIZE);
    json_status(j);
    http_send_json(c, j.buf, j.len);
}

void send_ok(http_conn_t *c) {
    static const char ok[] = "{\"ok\":true}";
    http_send_json(c, ok, sizeof(ok) - 1);
}

// ---- handlers ----------------------------------------------------------------

void h_index(http_conn_t *c) {
    http_send_static(c, "text/html; charset=utf-8",
                     (const uint8_t *)web_flash_uncached(web_index_html_gz), web_index_html_gz_len, true);
}

void h_status(http_conn_t *c) {
    send_status(c);
}

void h_set(http_conn_t *c) {
    const char *form = http_form_body(c);
    // validate everything first so a request is applied all or nothing
    for (const SettingDef &d : setting_defs) {
        int32_t v;
        char tmp[16];
        if (http_form_get(form, d.name, tmp, sizeof(tmp))) {
            if (!http_form_get_int(form, d.name, &v) || v < d.min || v > d.max || (d.reg == CMD_SBTYPE && v == 5)) {
                http_send_error(c, 400, "invalid value");
                return;
            }
        }
    }
    for (const SettingDef &d : setting_defs) {
        int32_t v;
        if (http_form_get_int(form, d.name, &v)) {
            apply_setting(d, v);
        }
    }
    send_status(c);
}

void h_save(http_conn_t *c) {
    picogus_request_save();
    send_ok(c);
}

void h_defaults(http_conn_t *c) {
    picogus_load_defaults();
    send_status(c);
}

void h_reboot(http_conn_t *c) {
    web_schedule_reboot(0, 500);
    send_ok(c);
}

void h_mode(http_conn_t *c) {
#ifdef PICOGUS_MULTIFW
    int32_t mode, save = 0;
    const char *form = http_form_body(c);
    if (!http_form_get_int(form, "mode", &mode) || mode < GUS_MODE || mode > USB_MODE) {
        http_send_error(c, 400, "invalid mode");
        return;
    }
    http_form_get_int(form, "save", &save);
    // same sequence as pgusinit /mode: set boot mode, optionally save, reboot
    picogus_web_write(CMD_BOOTMODE, (uint16_t)mode);
    if (save) {
        picogus_request_save();
    }
    web_schedule_reboot((uint8_t)mode, 500);
    send_ok(c);
#else
    http_send_error(c, 400, "this firmware has a single mode");
#endif
}

void h_wifi(http_conn_t *c) {
    const char *form = http_form_body(c);
    char ssid[33], pass[64];
    if (!http_form_get(form, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        http_send_error(c, 400, "missing ssid");
        return;
    }
    if (!http_form_get(form, "pass", pass, sizeof(pass))) {
        pass[0] = 0;
    }
    memset(settings.WiFi.ssid, 0, sizeof(settings.WiFi.ssid));
    memset(settings.WiFi.password, 0, sizeof(settings.WiFi.password));
    strncpy(settings.WiFi.ssid, ssid, sizeof(settings.WiFi.ssid) - 1);
    strncpy(settings.WiFi.password, pass, sizeof(settings.WiFi.password) - 1);
    // the response goes out before the old connection is dropped
    send_ok(c);
    web_wifi_request_connect();
}

#ifdef SOUND_MPU
int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

void h_midi(http_conn_t *c) {
    // hex=<MIDI bytes as hex digits, spaces allowed>
    char *hex = web_scratch;
    if (!http_form_get(http_form_body(c), "hex", hex, 1024)) {
        http_send_error(c, 400, "missing hex");
        return;
    }
    uint8_t *msg = (uint8_t *)(web_scratch + 1024);
    uint32_t len = 0;
    int hi = -1;
    for (const char *p = hex; *p; ++p) {
        if (*p == ' ' || *p == ',' || *p == ':') continue;
        int v = hexval(*p);
        if (v < 0) {
            http_send_error(c, 400, "invalid hex");
            return;
        }
        if (hi < 0) {
            hi = v;
        } else {
            msg[len++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    if (hi >= 0 || !len) {
        http_send_error(c, 400, "invalid hex");
        return;
    }
    const char *err = web_midi_send(msg, len);
    if (err) {
        http_send_error(c, 400, err);
        return;
    }
    send_ok(c);
}
#endif

#ifdef CDROM
bool cd_listing;

void h_cd_list(http_conn_t *c) {
    // Listing reads the USB drive, which happens in cdrom_tasks() in the
    // core 1 loop (as for pgusinit /cdlist). Start it and let the UI poll.
    if (!cd_listing) {
        // not while pgusinit /cdlist or a load is using the list
        if (cdrom.image_status != CD_STATUS_IDLE && cdrom.image_status != CD_STATUS_ERROR) {
            http_send_error(c, 409, "busy");
            return;
        }
        cd_listing = true;
        cdrom.image_status = CD_STATUS_BUSY;
        cdrom.image_command = CD_COMMAND_IMAGE_LIST;
    }
    if (cdrom.image_status == CD_STATUS_BUSY) {
        static const char busy[] = "{\"busy\":true}";
        http_send_json(c, busy, sizeof(busy) - 1);
        return;
    }
    cd_listing = false;
    Json j(web_scratch, WEB_SCRATCH_SIZE - 64);
    j.open(nullptr, '{');
    j.kv("current", cdman_current_image_index());
    j.open("images", '[');
    bool more = false;
    if (cdrom.image_status == CD_STATUS_READY && cdrom.image_list) {
        for (int i = 0; i < cdrom.image_count; ++i) {
            if (j.len + strlen(cdrom.image_list[i]) + 16 > j.cap) {
                more = true;
                break;
            }
            j.item(cdrom.image_list[i]);
        }
        cdman_list_images_free(cdrom.image_list, cdrom.image_count);
        cdrom.image_list = nullptr;
        cdrom.image_count = 0;
    }
    j.close(']');
    j.kv("truncated", more);
    j.kvs("error", cdrom.error_str ? cdrom.error_str : "");
    j.close('}');
    cdrom.image_status = CD_STATUS_IDLE;
    http_send_json(c, j.buf, j.len);
}

void h_cd_load(http_conn_t *c) {
    int32_t idx;
    if (!http_form_get_int(http_form_body(c), "index", &idx) || idx < 0 || idx > 255) {
        http_send_error(c, 400, "invalid index");
        return;
    }
    if (cdrom.image_status == CD_STATUS_BUSY) {
        http_send_error(c, 409, "busy");
        return;
    }
    // same as pgusinit /cdload n (0 unloads)
    cdrom.image_data = (uint8_t)idx;
    cdrom.image_status = CD_STATUS_BUSY;
    cdrom.image_command = CD_COMMAND_IMAGE_LOAD_INDEX;
    send_ok(c);
}
#endif

} // namespace

extern "C" {

const web_route_t web_routes[] = {
    {"/",               HTTP_GET,  nullptr, nullptr, h_index, nullptr},
    {"/index.html",     HTTP_GET,  nullptr, nullptr, h_index, nullptr},
    {"/api/status",     HTTP_GET,  nullptr, nullptr, h_status, nullptr},
    {"/api/set",        HTTP_POST, nullptr, nullptr, h_set, nullptr},
    {"/api/save",       HTTP_POST, nullptr, nullptr, h_save, nullptr},
    {"/api/defaults",   HTTP_POST, nullptr, nullptr, h_defaults, nullptr},
    {"/api/reboot",     HTTP_POST, nullptr, nullptr, h_reboot, nullptr},
    {"/api/mode",       HTTP_POST, nullptr, nullptr, h_mode, nullptr},
    {"/api/wifi",       HTTP_POST, nullptr, nullptr, h_wifi, nullptr},
#ifdef SOUND_MPU
    {"/api/midi",       HTTP_POST, nullptr, nullptr, h_midi, nullptr},
    {"/api/upload/syx", HTTP_POST, web_syx_begin, web_syx_body, web_syx_finish, web_syx_abort},
#endif
#ifdef CDROM
    {"/api/cd",         HTTP_GET,  nullptr, nullptr, h_cd_list, nullptr},
    {"/api/cd/load",    HTTP_POST, nullptr, nullptr, h_cd_load, nullptr},
#endif
    {"/api/upload/firmware", HTTP_POST, web_fw_begin, web_fw_body, web_fw_finish, web_fw_abort},
};
const uint32_t web_routes_count = sizeof(web_routes) / sizeof(web_routes[0]);

}
