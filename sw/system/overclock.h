/*
 *  Copyright (C) 2022-2024  Ian Scott
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

#pragma once

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"

// Core voltage while overclocked
#ifndef PICOGUS_VREG_VOLTAGE
#if PICO_RP2040
#define PICOGUS_VREG_VOLTAGE VREG_VOLTAGE_1_25
#else
// RP2350 is specified for 150MHz at 1.10V, so it gets further from its rated
// clock than RP2040 does. 1.30V is the highest voltage the regulator allows
// without vreg_disable_voltage_limit().
#define PICOGUS_VREG_VOLTAGE VREG_VOLTAGE_1_30
#endif
#endif

// Flash is still read after the overclock (settings, .flashdata tables, CYW43
// firmware), so its clock has to stay within what the flash chip can do. On
// both chips the divider comes from the board header (PICO_FLASH_SPI_CLKDIV,
// checked in pico_reflash.c): RP2040 gets it from boot2, RP2350 from the
// embedded XIP setup function, and either one runs at startup and again after
// every flash erase or program.
#if !PICO_RP2040 && !PICO_EMBED_XIP_SETUP
#error "RP2350 builds need PICO_EMBED_XIP_SETUP=1 for PICO_FLASH_SPI_CLKDIV to take effect"
#endif

// 370MHz PLL configuration:
// 12MHz XOSC / 2 (REFDIV) = 6MHz reference
// 6MHz * 185 (FBDIV) = 1110MHz VCO
// 1110MHz / 3 (POSTDIV1) / 1 (POSTDIV2) = 370MHz
//
// 370MHz is extremely closely evenly divisible by 44100Hz, making it ideal for
// audio sample rate generation. This frequency cannot be achieved with the
// default REFDIV of 1 since 370 is not evenly divisible by 12.
static inline void overclock_370mhz(void) {
    // Bump voltage for stability at 370MHz
    if (vreg_get_voltage() < PICOGUS_VREG_VOLTAGE) {
        vreg_set_voltage(PICOGUS_VREG_VOLTAGE);
        busy_wait_us_32(10000);  // 10ms for voltage to settle
    }

    // Switch clk_sys to USB PLL (48MHz) while reconfiguring PLL_SYS
    clock_configure_undivided(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    USB_CLK_HZ);

    // Init PLL_SYS with REFDIV=2 for 370MHz
    pll_init(pll_sys, 2, 1110000000u, 3, 1);

    // Configure clk_ref to XOSC
    clock_configure_undivided(clk_ref,
                    CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
                    0,
                    XOSC_HZ);

    // Switch clk_sys to PLL_SYS at 370MHz
    clock_configure_undivided(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    370000000u);

    // Peripheral clock stays on USB PLL (48MHz)
    clock_configure_undivided(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    USB_CLK_HZ);
}
