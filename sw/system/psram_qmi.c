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

// QMI (CS1) PSRAM setup for RP2350 boards, see psram_qmi.h. The command
// sequence and the 370MHz timing follow the ISA analyzer's PSRAM setup
// (isa_analyzer.cpp, from eightycc's PSRAM gist): AP Memory APS6404L-class
// QSPI PSRAM, reset, QPI mode, 0xEB quad read with 6 wait cycles, 0x38 quad
// write. The clock divider follows clk_sys, and the read sample delay is
// calibrated against a test pattern at init, as the PSRAM part on a given
// module is not always documented.

#include "system/psram_qmi.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/time.h"

#ifdef PGDEBUG
#include <stdio.h>
#define PSRAM_QMI_DBG(...) printf(__VA_ARGS__)
#else
#define PSRAM_QMI_DBG(...) do {} while (0)
#endif

#ifndef PICOGUS_PSRAM_QMI_CS_PIN
#define PICOGUS_PSRAM_QMI_CS_PIN 47
#endif
// APS6404L: 133MHz in QPI mode (84MHz across page boundaries, handled by
// PAGEBREAK). 109MHz keeps a margin; 370MHz / 4 = 92.5MHz.
#ifndef PICOGUS_PSRAM_MAX_SCK_HZ
#define PICOGUS_PSRAM_MAX_SCK_HZ 109000000u
#endif

static bool psram_found;
static uint32_t psram_rxdelay = 2;

static void __no_inline_not_in_flash_func(direct_cmd)(uint8_t cmd, bool quad) {
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        (quad ? QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB : 0) |
                        cmd;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
        ;
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (int i = 0; i < 20; i++) {
        __asm volatile("nop");
    }
}

// Window 1 timing and command formats for the current clk_sys
static void __no_inline_not_in_flash_func(set_window)(uint32_t rxdelay) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t clkdiv = (sys_hz + PICOGUS_PSRAM_MAX_SCK_HZ - 1) / PICOGUS_PSRAM_MAX_SCK_HZ;
    if (clkdiv < 2) {
        clkdiv = 2;
    }
    // Chip select low for at most ~2us (the PSRAM needs a refresh gap within
    // 8us), in units of 64 clk_sys cycles: 11 at 370MHz
    uint32_t max_select = (uint32_t)(((uint64_t)sys_hz * 2) / (64u * 1000000u));
    if (max_select > 63) {
        max_select = 63;
    }
    // At least 50ns high between transfers, in clk_sys cycles
    uint32_t min_deselect = (uint32_t)(((uint64_t)sys_hz * 50 + 999999999u) / 1000000000u);
    if (min_deselect > 31) {
        min_deselect = 31;
    }
    qmi_hw->m[1].timing =
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        3 << QMI_M1_TIMING_SELECT_HOLD_LSB |
        1 << QMI_M1_TIMING_COOLDOWN_LSB |
        rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
        max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
        clkdiv << QMI_M1_TIMING_CLKDIV_LSB;
    // Quad read 0xEB: command, address, 6 wait cycles (24 bits) and data in quad
    qmi_hw->m[1].rfmt =
        QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
        QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
        QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
        QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB |
        QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
        QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB << QMI_M1_RCMD_PREFIX_LSB;
    // Quad write 0x38: no wait cycles
    qmi_hw->m[1].wfmt =
        QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
        QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
        QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
        QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
        QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38 << QMI_M1_WCMD_PREFIX_LSB;
    __dmb();
}

void __no_inline_not_in_flash_func(psram_qmi_reconfigure)(void) {
    if (!psram_found) {
        return;
    }
    uint32_t save = save_and_disable_interrupts();
    set_window(psram_rxdelay);
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    restore_interrupts(save);
}

// Pick the read sample delay: write a pattern with the (delay independent)
// write path, read it back at each delay and take the middle of the window
// that works
static bool __no_inline_not_in_flash_func(calibrate_rxdelay)(void) {
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_QMI_BASE;
    const uint32_t words = 256;
    for (uint32_t i = 0; i < words; i++) {
        p[i] = (i * 0x9E3779B9u) ^ 0xA5C3F00Fu;
    }
    int first = -1, last = -1;
    for (uint32_t rx = 0; rx < 8; rx++) {
        set_window(rx);
        bool ok = true;
        for (uint32_t i = 0; i < words && ok; i++) {
            ok = p[i] == ((i * 0x9E3779B9u) ^ 0xA5C3F00Fu);
        }
        if (ok) {
            if (first < 0) {
                first = (int)rx;
            }
            last = (int)rx;
        } else if (first >= 0) {
            break;
        }
    }
    if (first < 0) {
        return false;
    }
    psram_rxdelay = (uint32_t)(first + last) / 2;
    set_window(psram_rxdelay);
    PSRAM_QMI_DBG("PSRAM read delay window %d-%d, using %lu\n", first, last, (unsigned long)psram_rxdelay);
    return true;
}

psram_spi_inst_t __no_inline_not_in_flash_func(psram_qmi_init)(void) {
    psram_spi_inst_t inst = { .base = (volatile uint8_t *)PSRAM_QMI_BASE, .size = 0 };

    gpio_set_function(PICOGUS_PSRAM_QMI_CS_PIN, GPIO_FUNC_XIP_CS1);
    uint32_t save = save_and_disable_interrupts();

    // Direct mode with a slow clock for the ID read
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
        ;
    // Leave QPI mode in case a previous run (warm reset) left it there
    direct_cmd(0xF5, true);

    // Read ID: command, 3 address bytes, then MF ID, KGD, EID
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t mfid = 0, kgd = 0, eid = 0;
    for (int i = 0; i < 12; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9F : 0xFF;
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
            ;
        uint8_t rx = (uint8_t)qmi_hw->direct_rx;
        if (i == 4) mfid = rx;
        if (i == 5) kgd = rx;
        if (i == 6) eid = rx;
    }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    if (kgd != 0x5D) {
        // No PSRAM (Core2350B0), or not an APS6404L-compatible part
        qmi_hw->direct_csr = 0;
        restore_interrupts(save);
        PSRAM_QMI_DBG("No QMI PSRAM (ID %02x %02x %02x)\n", mfid, kgd, eid);
        return inst;
    }

    // Reset enable, reset, QPI enable. The PSRAM stays in its default linear
    // 1K-page burst mode (no 0xC0 wrap toggle), which QMI's PAGEBREAK expects.
    direct_cmd(0x66, false);
    direct_cmd(0x99, false);
    direct_cmd(0x35, false);
    qmi_hw->direct_csr = 0;

    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    psram_found = calibrate_rxdelay();
    restore_interrupts(save);
    if (!psram_found) {
        PSRAM_QMI_DBG("QMI PSRAM does not read back at any delay\n");
        return inst;
    }

    // Density from the EID's top 3 bits: 0 = 2MB (16Mbit), 1 = 4MB, 2 = 8MB
    switch (eid >> 5) {
    case 0: inst.size = 2u * 1024 * 1024; break;
    case 1: inst.size = 4u * 1024 * 1024; break;
    default: inst.size = 8u * 1024 * 1024; break;
    }
    PSRAM_QMI_DBG("QMI PSRAM: ID %02x %02x %02x, %u KB\n", mfid, kgd, eid, (unsigned)(inst.size / 1024));
    return inst;
}

int psram_qmi_test(psram_spi_inst_t *spi, int increment) {
    if (!spi->size) {
        return 1;
    }
    // The GUS uses the first 1MB
    const uint32_t len = 1024 * 1024;
    for (uint32_t addr = 0; addr < len; addr += 4) {
        uint32_t value = (((addr + 3) & 0xFF) << 24) | (((addr + 2) & 0xFF) << 16) |
                         (((addr + 1) & 0xFF) << 8) | (addr & 0xFF);
        psram_write32(spi, addr, value);
    }
    uint32_t begin = time_us_32();
    for (uint32_t addr = 0; addr < len; addr += (uint32_t)increment) {
        if (psram_read8(spi, addr) != (uint8_t)(addr & 0xFF)) {
            PSRAM_QMI_DBG("PSRAM failure at address %lx\n", (unsigned long)addr);
            return 1;
        }
    }
    for (uint32_t addr = 0; addr + 1 < len; addr += 2u * (uint32_t)increment) {
        uint16_t expect = (uint16_t)((((addr + 1) & 0xFF) << 8) | (addr & 0xFF));
        if (psram_read16(spi, addr) != expect) {
            PSRAM_QMI_DBG("PSRAM failure at address %lx (16 bit)\n", (unsigned long)addr);
            return 1;
        }
    }
    PSRAM_QMI_DBG("PSRAM test passed in %lu us\n", (unsigned long)(time_us_32() - begin));
    // Clear the GUS memory again
    for (uint32_t addr = 0; addr < len; addr += 4) {
        psram_write32(spi, addr, 0);
    }
    return 0;
}
