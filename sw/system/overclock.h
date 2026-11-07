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
#if !PICO_RP2040
#include "hardware/structs/qmi.h"
#endif
#if PSRAM_QMI
#include "system/psram_qmi.h"
#endif

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

#if !PICO_RP2040
// On RP2040 the flash clock divider is set by boot2 (PICO_FLASH_SPI_CLKDIV).
// RP2350 has no boot2: the bootrom leaves the QMI in the fastest mode and
// divider it found to work at boot, normally CLKDIV 3 (50MHz at the stock
// 150MHz clk_sys), which would clock the flash at 123MHz at 370MHz. Flash is
// still read after the overclock (settings, .flashdata tables, CYW43
// firmware), so slow it down first. RXDELAY is in half clk_sys cycles; 2
// matches the bootrom's default.
#ifndef PICOGUS_FLASH_QMI_CLKDIV
#define PICOGUS_FLASH_QMI_CLKDIV 6   // 370MHz / 6 = 61.7MHz
#endif
#ifndef PICOGUS_FLASH_QMI_RXDELAY
#define PICOGUS_FLASH_QMI_RXDELAY 2
#endif

static inline void overclock_set_flash_timing(void) {
    // Keep the read mode the bootrom selected; only change clock and sample delay.
    // This must run from RAM with no flash access in flight, which holds as the
    // firmware is built with PICO_COPY_TO_RAM.
    uint32_t timing = qmi_hw->m[0].timing;
    timing &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    timing |= (PICOGUS_FLASH_QMI_CLKDIV << QMI_M0_TIMING_CLKDIV_LSB) |
              (PICOGUS_FLASH_QMI_RXDELAY << QMI_M0_TIMING_RXDELAY_LSB);
    qmi_hw->m[0].timing = timing;
}
#endif

// Call right after flash_range_erase()/flash_range_program(), before anything reads flash or PSRAM
// again. On RP2350 a flash write puts the QMI back to the bootrom's flash divider (123MHz at 370MHz)
// and resets the QMI PSRAM's write format (v2.1); this puts both back. Nothing to do on RP2040, where
// the SDK re-runs boot2 and so keeps PICO_FLASH_SPI_CLKDIV.
static inline void overclock_flash_write_done(void) {
#if !PICO_RP2040
    overclock_set_flash_timing();
#endif
#if PSRAM_QMI
    psram_qmi_reconfigure();
#endif
}

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

#if !PICO_RP2040
    // Slow the flash down before clk_sys goes up. Flash writes return the QMI to
    // the bootrom's divider, which is why this is redone on every call.
    overclock_set_flash_timing();
#endif

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

#if PSRAM_QMI
    // QMI PSRAM (v2.1): timing for the new clk_sys, and the write format that
    // the SDK's flash functions reset (this also runs after settings saves)
    psram_qmi_reconfigure();
#endif
}
