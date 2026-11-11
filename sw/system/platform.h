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

// Small helpers that paper over the differences between RP2040 and RP2350 so
// the rest of the firmware can stay chip-agnostic. On RP2040 every helper
// compiles to exactly what the firmware did before RP2350 support existed.

#pragma once

#include "pico.h"
#include "hardware/gpio.h"
#include "hardware/structs/xip_ctrl.h"
#if PICO_RP2040
#include "hardware/regs/vreg_and_chip_reset.h"
#elif PICO_RP2350
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/powman.h"
#else
#error "PicoGUS supports RP2040 and RP2350 (Arm) only"
#endif

#if PICO_RP2350 && !PICO_RP2350A && !PICOGUS_BOARD_V21
// Board detection reads GPIO29 through the ADC, and on the 48 GPIO RP2350B
// (e.g. Pimoroni Pico Plus 2) the ADC inputs are GPIO40-47 instead. The one
// RP2350B board PicoGUS knows, v2.1 THT (boards/picogus21.h), sets its board
// type at compile time.
#error "PicoGUS supports RP2350A (30 GPIO) boards such as the Raspberry Pi Pico 2, and the RP2350B PicoGUS v2.1 (PICO_BOARD=picogus21)"
#endif

#if PICO_RP2040
#define PICOGUS_CHIP_NAME "RP2040"
// RP2040 is the original target, so its firmware string stays as it always was
#define PICOGUS_FW_STRING_SUFFIX ""
#else
#define PICOGUS_CHIP_NAME "RP2350"
#define PICOGUS_FW_STRING_SUFFIX " (" PICOGUS_CHIP_NAME ")"
#endif

// Chip reset status register, and the bits in it that record why we reset
#if PICO_RP2040
#define PLATFORM_CHIP_RESET_REG (*(io_rw_32 *)(VREG_AND_CHIP_RESET_BASE + VREG_AND_CHIP_RESET_CHIP_RESET_OFFSET))
#define PLATFORM_CHIP_RESET_HAD_POWER_ON_BITS VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS
#define PLATFORM_CHIP_RESET_HAD_RUN_PIN_BITS VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS
#define PLATFORM_CHIP_RESET_HAD_DEBUG_BITS VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS
#else
#define PLATFORM_CHIP_RESET_REG (powman_hw->chip_reset)
#define PLATFORM_CHIP_RESET_HAD_POWER_ON_BITS (POWMAN_CHIP_RESET_HAD_POR_BITS | POWMAN_CHIP_RESET_HAD_BOR_BITS)
#define PLATFORM_CHIP_RESET_HAD_RUN_PIN_BITS POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS
#define PLATFORM_CHIP_RESET_HAD_DEBUG_BITS POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS
#endif

// Turn off the XIP cache. Everything time-critical runs from SRAM
// (PICO_COPY_TO_RAM), so flash reads bypass the cache from here on.
static inline void platform_disable_xip_cache(void) {
#if PICO_RP2040
    hw_clear_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_EN_BITS);
#else
    hw_clear_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_EN_SECURE_BITS | XIP_CTRL_EN_NONSECURE_BITS);
#endif
}

// RP2350 GPIO pads come out of reset with their input buffer disabled and
// their isolation latch set, whereas RP2040 pads are input-enabled at reset.
// gpio_set_function() takes care of this, but pins that are only ever sampled
// by a PIO state machine (e.g. the ISA address/data bus, IOR/IOW, DACK, TC)
// never go through it and would otherwise always read as 0 on RP2350.
static inline void platform_gpio_enable_input(uint gpio) {
#if PICO_RP2040
    (void)gpio;
#else
    gpio_set_input_enabled(gpio, true);
    hw_clear_bits(&pads_bank0_hw->io[gpio], PADS_BANK0_GPIO0_ISO_BITS);
#endif
}
