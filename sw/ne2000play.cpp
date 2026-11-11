/*
 *  Copyright (C) 2022-2024  Ian Scott
 *  Copyright (C) 2024       Kevin Moonlight
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

#include <math.h>
#include <string.h>

#include "include/pg_debug.h"

#if PICO_ON_DEVICE

#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "hardware/timer.h"
#include "pico/multicore.h"

#endif

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

extern "C" {
  #include "ne2000/ne2000.h"
}

#include "system/pico_pic.h"

#include "system/flash_settings.h"
extern Settings settings;

extern uint LED_PIN;


// Bring up WiFi. The CYW43 driver then does its background work in a low
// priority IRQ on the calling core, so this must run on core 1.
void ne2000_core1_init() {
    DBG_PUTS("starting core 1 ne2000");
    PG_EnableWifi();
    PG_Wifi_Connect(settings.WiFi.ssid, settings.WiFi.password);
}

// Handle requests from core 0 and keep the WiFi connection up. Call this
// regularly from the core 1 main loop.
void ne2000_core1_task() {
    static bool flag = false;
    if (multicore_fifo_rvalid()) {
        switch(multicore_fifo_pop_blocking()) {
        case FIFO_NE2K_SEND:
            ne2000_initiate_send();
            break;
        case FIFO_WIFI_STATUS:
            PG_Wifi_GetStatus();
            break;
        default:
            break;
        }
    }
    if (((time_us_32() >> 21) & 0x1) == 0x1) {
        if (flag == false) {
            DBG_PUTCHAR('=');
            PG_Wifi_Reconnect();
            flag = true;
        }
    } else {
        flag = false;
    }
}

void play_ne2000() {
    // Init PIC on this core so it handles timers
    PIC_Init();

    ne2000_core1_init();
    while(1) {
        ne2000_core1_task();
    }
}
