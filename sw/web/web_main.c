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

// Web server main loop, WiFi management and core 0 <-> core 1 glue.

#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#if PICOGUS_WEB_MDNS
#include "lwip/apps/mdns.h"
#endif

#include "../include/pg_debug.h"
#include "../system/flash_settings.h"
#include "../../common/picogus.h"

#include "web.h"
#include "web_internal.h"
#include "web_xip_sram.h"
#include "../system/platform.h"
#include "hardware/structs/rosc.h"

extern Settings settings;

// How often web_task() does any work at all. The CYW43 chip buffers incoming
// packets, so polling every couple of milliseconds is plenty for a web UI.
#define WEB_POLL_INTERVAL_US    2000
// Wait this long after core 0 is ready before bringing up WiFi, so the mode's
// own startup (USB enumeration, DSP init...) is finished first.
#define WEB_START_DELAY_US      (500 * 1000)
#define WEB_LINK_CHECK_US       (1000 * 1000)
// Joining a network blocks for several milliseconds: retry rarely, backing off
// from 20s to ~5 minutes while the network is unreachable
#define WEB_RECONNECT_US        (20 * 1000 * 1000)
#define WEB_RECONNECT_MAX_SHIFT 4
#define WEB_RSSI_REFRESH_US     (5 * 1000 * 1000)
#define WEB_STATS_WINDOW_US     (10 * 1000 * 1000)

typedef enum {
    WEB_WAIT_CORE0 = 0,
    WEB_WAIT_START,
    WEB_IDLE,       // no WiFi network configured: the WiFi chip stays off
    WEB_RUNNING,
    WEB_FAILED,
} web_state_t;

static web_state_t state = WEB_WAIT_CORE0;
static uint32_t next_run_us;
static uint32_t start_at_us;

// core 0 -> core 1 requests
static volatile bool core0_ready;
static uint8_t running_mode;
static volatile bool connect_requested;
static volatile bool status_requested;

// Flash arbitration between the cores
static spin_lock_t *flash_lock;

// WiFi state (core 1)
static int last_link = CYW43_LINK_DOWN;
static uint32_t last_connect_us;
static uint8_t reconnect_shift;
static uint32_t last_link_check_us;
static int32_t rssi;
static uint32_t last_rssi_us;
static char ssid[33];
static char password[64];

// Reboot request (core 1)
static bool reboot_pending;
static uint8_t reboot_mode;
static uint32_t reboot_at_us;

// Timing statistics
static uint32_t poll_max_us;
static uint32_t poll_max_window_us;
static uint32_t poll_max_prev_window_us;
static uint32_t window_start_us;
static uint32_t window_busy_us;
static uint16_t load_permille;

// ---- pgusinit /wifistatus support -----------------------------------------
// Core 1 builds the status text into the unpublished buffer and then flips the
// index; core 0 latches the published buffer when pgusinit starts reading, so
// it never waits for core 1 (pgusinit only polls a limited number of times).
#define STATUS_LEN 112
static char status_buf[2][STATUS_LEN] = { "Starting WiFi...", "Starting WiFi..." };
static volatile uint8_t status_pub;
static const char *status_read_ptr;
static uint8_t status_read_idx;

void web_wifi_request_status(void) {
    status_read_ptr = status_buf[status_pub];
    status_read_idx = 0;
    status_requested = true;
}

char web_wifi_read_status_char(void) {
    if (!status_read_ptr) {
        return 0;
    }
    char c = status_read_ptr[status_read_idx];
    if (c == 0 || status_read_idx >= STATUS_LEN - 1) {
        status_read_idx = 0;
        return 0;
    }
    ++status_read_idx;
    return c;
}

void web_wifi_request_connect(void) {
    connect_requested = true;
}

// ---- random numbers for lwIP ---------------------------------------------------

unsigned int web_rand(void) {
    // xorshift32, stirred with the ring oscillator's random bit and the time
    static uint32_t state = 0x9e3779b9u;
    uint32_t noise = time_us_32();
    for (int i = 0; i < 8; ++i) {
        noise = (noise << 1) | (rosc_hw->randombit & 1);
    }
    state ^= noise;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// ---- XIP cache SRAM ----------------------------------------------------------

void web_xip_cache_off(void) {
    platform_disable_xip_cache();
}

const void *web_flash_uncached(const void *p) {
    uintptr_t a = (uintptr_t)p;
    if (a >= XIP_BASE && a < XIP_BASE + PICO_FLASH_SIZE_BYTES) {
        a = a - XIP_BASE + XIP_NOCACHE_NOALLOC_BASE;
    }
    return (const void *)a;
}

static uint32_t xip_used;
#ifndef WEB_XIP_SRAM_BASE
// RP2350: ordinary memory instead of the RP2040's XIP cache SRAM (web_xip_sram.h)
static uint32_t web_buffers[WEB_XIP_WEB_SIZE / 4];
#define WEB_XIP_SRAM_BASE ((uintptr_t)web_buffers)
#endif

void *web_xip_alloc(size_t size) {
    size = (size + 3) & ~3u;
    if (xip_used + size > WEB_XIP_WEB_SIZE) {
        return NULL;
    }
    void *p = (void *)(WEB_XIP_SRAM_BASE + xip_used);
    xip_used += size;
    memset(p, 0, size);
    return p;
}

// ---- core 0 hooks ----------------------------------------------------------

void web_init_core0(void) {
    flash_lock = spin_lock_init(spin_lock_claim_unused(true));
}

void web_core0_ready(void) {
    // processSettings() has just set this to the mode that is running
    running_mode = settings.startupMode;
    core0_ready = true;
}

bool web_flash_try_lock(void) {
    return spin_try_lock_unsafe(flash_lock);
}

void web_flash_unlock(void) {
    spin_unlock_unsafe(flash_lock);
}

// core 1 side, nesting
static uint8_t flash_depth;

void web_flash_acquire_blocking(void) {
    if (!flash_depth) {
        spin_lock_unsafe_blocking(flash_lock);
    }
    ++flash_depth;
}

bool web_flash_try_acquire(void) {
    if (!flash_depth && !spin_try_lock_unsafe(flash_lock)) {
        return false;
    }
    ++flash_depth;
    return true;
}

void web_flash_release(void) {
    if (flash_depth && --flash_depth == 0) {
        spin_unlock_unsafe(flash_lock);
    }
}

// ---- Keep CYW43 driver waits on core 1 -------------------------------------
// In poll mode the CYW43 driver waits with sleep_until()/semaphore timeouts,
// which arm alarms on the SDK's default alarm pool. That pool's interrupt fires
// on core 0, where it would interrupt ISA bus handling. These link-time
// wrappers (-Wl,--wrap=...) turn those waits into busy waits on core 1.

void __wrap_cyw43_delay_ms(uint32_t ms) {
    busy_wait_ms(ms);
}

void __wrap_cyw43_delay_us(uint32_t us) {
    busy_wait_us_32(us);
}

void __wrap_cyw43_await_background_or_timeout_us(uint32_t timeout_us) {
    // Return early when the chip signals that it has something for us, like
    // the original does when its GPIO interrupt fires.
    uint32_t start = time_us_32();
    while (time_us_32() - start < timeout_us) {
        if (gpio_get(CYW43_PIN_WL_HOST_WAKE)) {
            break;
        }
    }
}

extern void __real_sleep_ms(uint32_t ms);
extern void __real_sleep_us(uint64_t us);

void __wrap_sleep_ms(uint32_t ms) {
    if (get_core_num() == 1) {
        busy_wait_ms(ms);
    } else {
        __real_sleep_ms(ms);
    }
}

void __wrap_sleep_us(uint64_t us) {
    if (get_core_num() == 1) {
        busy_wait_us(us);
    } else {
        __real_sleep_us(us);
    }
}

// ---- WiFi ------------------------------------------------------------------

static struct netif *sta_netif(void) {
    return &cyw43_state.netif[CYW43_ITF_STA];
}

static void wifi_connect(void) {
    // Snapshot the credentials; core 0 may be rewriting them for pgusinit
    strncpy(ssid, settings.WiFi.ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
    strncpy(password, settings.WiFi.password, sizeof(password) - 1);
    password[sizeof(password) - 1] = 0;

    last_connect_us = time_us_32();
    if (!ssid[0]) {
        return;
    }
    DBG_PRINTF("web: joining %s\n", ssid);
    cyw43_arch_wifi_connect_async(ssid, password[0] ? password : NULL,
                                  password[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
}

static void append(char *dst, const char *src) {
    size_t len = strlen(dst);
    while (*src && len < STATUS_LEN - 1) {
        dst[len++] = *src++;
    }
    dst[len] = 0;
}

static void append_int(char *dst, int32_t v) {
    char num[12];
    char *p = num + sizeof(num) - 1;
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
    *p = 0;
    do {
        *--p = '0' + (u % 10);
        u /= 10;
    } while (u);
    if (neg) {
        *--p = '-';
    }
    append(dst, p);
}

static void update_status_string(void) {
    char *s = status_buf[status_pub ^ 1];
    s[0] = 0;
    if (state == WEB_FAILED) {
        append(s, "Err: WiFi chip init failed");
    } else if (state == WEB_IDLE) {
        append(s, "Err: No SSID set (use /wifissid and /wifipass)");
    } else if (state != WEB_RUNNING) {
        append(s, "Starting WiFi...");
    } else if (!ssid[0]) {
        append(s, "Err: No SSID set (use /wifissid and /wifipass)");
    } else {
        switch (last_link) {
        case CYW43_LINK_UP:
            append(s, "Connected to SSID ");
            append(s, ssid);
            append(s, ", Signal ");
            append_int(s, rssi);
            append(s, " dB, web UI: http://");
            append(s, ip4addr_ntoa(netif_ip4_addr(sta_netif())));
            append(s, "/");
            break;
        case CYW43_LINK_JOIN:
        case CYW43_LINK_NOIP:
            append(s, "Connected to SSID ");
            append(s, ssid);
            append(s, ", waiting for IP address");
            break;
        case CYW43_LINK_FAIL:
            append(s, "Err: Connection failed");
            break;
        case CYW43_LINK_NONET:
            append(s, "Err: SSID not found");
            break;
        case CYW43_LINK_BADAUTH:
            append(s, "Err: Authentication failure");
            break;
        default:
            append(s, "Connecting to SSID ");
            append(s, ssid);
            append(s, "...");
            break;
        }
    }
    status_pub ^= 1;
}

static bool web_start_locked(void) {
#if PICO_RP2040
    // The network buffers live in the XIP cache's memory (web_xip_sram.h),
    // usable as SRAM only while the cache is off, as PicoGUS always has it
    if (xip_ctrl_hw->ctrl & XIP_CTRL_EN_BITS) {
        DBG_PUTS("web: XIP cache is enabled, no memory for the network");
        return false;
    }
#endif
    if (!web_http_init() || !web_upload_init()) {
        DBG_PUTS("web: out of XIP SRAM");
        return false;
    }

    // Never let the network stack preempt audio: the CYW43 host-wake handler
    // goes to the lowest priority on this core, same as the audio interrupt.
    irq_set_priority(IO_IRQ_BANK0, PICO_LOWEST_IRQ_PRIORITY);

    int err = cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE);
    if (!err) {
        cyw43_arch_enable_sta_mode();
    }
    if (err) {
        DBG_PUTS("web: cyw43 init failed");
        return false;
    }
    irq_set_priority(IO_IRQ_BANK0, PICO_LOWEST_IRQ_PRIORITY);

#if PICOGUS_WEB_MDNS
    mdns_resp_init();
    mdns_resp_add_netif(sta_netif(), CYW43_HOST_NAME);
    mdns_resp_add_service(sta_netif(), "PicoGUS", "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
#endif
    if (!web_http_start()) {
        DBG_PUTS("web: http start failed");
    }
    wifi_connect();
    return true;
}

static bool web_start(void) {
    // Hold off core 0's flash writes (settings save): bringing up the chip reads
    // its firmware from flash, and the buffers in the XIP cache memory must not
    // be touched while a flash write has the cache turned on
    web_flash_acquire_blocking();
    bool ok = web_start_locked();
    web_flash_release();
    return ok;
}

static void web_run(uint32_t now) {
    if (connect_requested) {
        connect_requested = false;
        reconnect_shift = 0;
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        wifi_connect();
        last_link = CYW43_LINK_DOWN;
        update_status_string();
    }

    cyw43_arch_poll();
    web_http_poll();

    if (now - last_link_check_us >= WEB_LINK_CHECK_US || status_requested) {
        last_link_check_us = now;
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link != last_link) {
            if (link == CYW43_LINK_UP) {
                reconnect_shift = 0;
            }
            if ((link == CYW43_LINK_UP) != (last_link == CYW43_LINK_UP)) {
                // WiFi LED on the Pico W shows whether the web UI is reachable
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, link == CYW43_LINK_UP);
#if PICOGUS_WEB_MDNS
                if (link == CYW43_LINK_UP) {
                    mdns_resp_announce(sta_netif());
                }
#endif
            }
            last_link = link;
            last_rssi_us = now - WEB_RSSI_REFRESH_US;
        }
        if (link == CYW43_LINK_UP && now - last_rssi_us >= WEB_RSSI_REFRESH_US) {
            last_rssi_us = now;
            if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) != 0) {
                rssi = 0;
            }
        }
        // Keep retrying while not connected; join failures, a vanished access
        // point and dropped links all end up here.
        if (link != CYW43_LINK_UP && link != CYW43_LINK_JOIN && link != CYW43_LINK_NOIP
            && now - last_connect_us >= (WEB_RECONNECT_US << reconnect_shift)) {
            if (reconnect_shift < WEB_RECONNECT_MAX_SHIFT) {
                ++reconnect_shift;
            }
            wifi_connect();
        }
        status_requested = false;
        update_status_string();
    }
}

void web_task(void) {
    uint32_t now = time_us_32();
    if ((int32_t)(now - next_run_us) < 0) {
        return;
    }
    next_run_us = now + WEB_POLL_INTERVAL_US;

    switch (state) {
    case WEB_WAIT_CORE0:
        if (core0_ready) {
            start_at_us = now + WEB_START_DELAY_US;
            state = WEB_WAIT_START;
        }
        return;
    case WEB_WAIT_START:
        if ((int32_t)(now - start_at_us) >= 0) {
            // Without a configured network, leave the WiFi chip off until
            // pgusinit /wifissid sets one: no network, no load at all
            state = settings.WiFi.ssid[0] ? WEB_RUNNING : WEB_IDLE;
            if (state == WEB_RUNNING && !web_start()) {
                state = WEB_FAILED;
            }
            update_status_string();
            window_start_us = time_us_32();
        }
        return;
    case WEB_IDLE:
        if (connect_requested) {
            connect_requested = false;
            state = web_start() ? WEB_RUNNING : WEB_FAILED;
            update_status_string();
            window_start_us = time_us_32();
        }
        return;
    case WEB_RUNNING:
        // The web page is stored in flash; hold off core 0 flash writes while
        // the network stack runs. If core 0 is saving settings right now, skip.
        if (!web_flash_try_acquire()) {
            next_run_us = now;
            return;
        }
        web_run(now);
        web_flash_release();
        break;
    case WEB_FAILED:
        return;
    }

    // Wait for a requested settings save to finish first. Core 0 does it on the
    // next ISA write; give up after a few seconds if the PC is not running.
    if (reboot_pending && (int32_t)(now - reboot_at_us) >= 0
        && (!picogus_save_pending() || (int32_t)(now - reboot_at_us) >= 3000000)) {
        // Tell the multifw bootloader which mode to start (like pgusinit /mode).
        // Always set: the register keeps its value across reboots.
        watchdog_hw->scratch[3] = reboot_mode ? reboot_mode : running_mode;
        // never in the middle of core 0 writing the settings to flash
        web_flash_acquire_blocking();
        watchdog_reboot(0, 0, 0);
        for (;;) {
            tight_loop_contents();
        }
    }

    // Timing statistics: how long a single call took, and how much of core 1
    // the web server used over the last window
    uint32_t end = time_us_32();
    uint32_t took = end - now;
    if (took > poll_max_us) {
        poll_max_us = took;
    }
    if (took > poll_max_window_us) {
        poll_max_window_us = took;
    }
    window_busy_us += took;
    uint32_t window = end - window_start_us;
    if (window >= WEB_STATS_WINDOW_US) {
        load_permille = (uint16_t)(((uint64_t)window_busy_us * 1000) / window);
        poll_max_prev_window_us = poll_max_window_us;
        poll_max_window_us = 0;
        window_busy_us = 0;
        window_start_us = end;
    }
}

void web_get_net_info(web_net_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->link = (int8_t)last_link;
    info->rssi = rssi;
    info->ssid = ssid;
    if (state == WEB_RUNNING) {
        info->ip = ip4_addr_get_u32(netif_ip4_addr(sta_netif()));
        memcpy(info->mac, cyw43_state.mac, 6);
    }
    info->poll_max_us = poll_max_us;
    info->poll_max_recent_us = poll_max_prev_window_us > poll_max_window_us ? poll_max_prev_window_us : poll_max_window_us;
    info->load_permille = load_permille;
}

void web_schedule_reboot(uint8_t mode, uint32_t delay_ms) {
    reboot_mode = mode;
    reboot_at_us = time_us_32() + delay_ms * 1000;
    reboot_pending = true;
}
