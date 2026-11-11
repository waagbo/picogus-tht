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

// PSRAM on the RP2350's QMI (chip select CS1), memory mapped, for boards
// whose PSRAM sits on the RP2350 itself (PicoGUS v2.1 THT: Waveshare
// Core2350B2, CS1 = GPIO47). It provides the subset of rp2040-psram's
// psram_spi.h API that the GUS emulation uses, so the GUS code does not
// change: the "SPI instance" just records where the PSRAM is mapped, and
// every read or write is a plain memory access.

#pragma once

#include <string.h>
#include "pico.h"
#include "hardware/pio.h"

#if !PICO_RP2350
#error "QMI PSRAM needs an RP2350"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// XIP window for QMI chip select 1. The firmware turns the XIP cache off
// (platform_disable_xip_cache()), so use the uncached alias: every access goes
// straight to the PSRAM and nothing needs flushing.
#define PSRAM_QMI_BASE ((uintptr_t)0x15000000u)

typedef struct psram_spi_inst {
    volatile uint8_t *base;
    size_t size;            // bytes, from the PSRAM's ID; 0 if no PSRAM answered
} psram_spi_inst_t;

// Detect and set up the PSRAM on CS1 (GPIO PICOGUS_PSRAM_QMI_CS_PIN): reset
// it, switch it to QPI and program QMI window 1 for quad reads and writes at
// the current clk_sys. Returns size 0 if no AP Memory PSRAM answers.
psram_spi_inst_t psram_qmi_init(void);

// Re-program QMI window 1's timing and command formats for the current
// clk_sys. Call after any clk_sys change and after flash writes (the SDK's
// flash functions keep CS1's timing and read setup, but reset its write
// format). Does nothing until psram_qmi_init() found a PSRAM.
void psram_qmi_reconfigure(void);

// Write/read test of the GUS memory (first 1MB), reading every 'increment'
// bytes; returns 0 if it passed, 1 if it failed, as rp2040-psram's
// test_psram(), which the call sites use by that name (rp2040-psram is still
// linked, so this one has its own).
int psram_qmi_test(psram_spi_inst_t *spi, int increment);
#define test_psram psram_qmi_test

// --- psram_spi.h compatible API ---

static inline psram_spi_inst_t psram_spi_init_clkdiv(PIO pio, int sm, float clkdiv, bool fudge) {
    (void)pio; (void)sm; (void)clkdiv; (void)fudge;
    return psram_qmi_init();
}

static inline psram_spi_inst_t psram_spi_init(PIO pio, int sm) {
    return psram_spi_init_clkdiv(pio, sm, 1.0f, false);
}

static inline void psram_spi_uninit(psram_spi_inst_t spi, bool fudge) {
    (void)spi; (void)fudge;
}

__force_inline static uint8_t psram_read8(psram_spi_inst_t *spi, uint32_t addr) {
    return spi->base[addr];
}

__force_inline static uint16_t psram_read16(psram_spi_inst_t *spi, uint32_t addr) {
    uint16_t v;
    memcpy(&v, (const void *)(spi->base + addr), sizeof(v));
    return v;
}

__force_inline static uint32_t psram_read32(psram_spi_inst_t *spi, uint32_t addr) {
    uint32_t v;
    memcpy(&v, (const void *)(spi->base + addr), sizeof(v));
    return v;
}

__force_inline static void psram_read(psram_spi_inst_t *spi, const uint32_t addr, uint8_t *dst, const size_t count) {
    memcpy(dst, (const void *)(spi->base + addr), count);
}

__force_inline static void psram_write8(psram_spi_inst_t *spi, uint32_t addr, uint8_t val) {
    spi->base[addr] = val;
}

__force_inline static void psram_write8_async(psram_spi_inst_t *spi, uint32_t addr, uint8_t val) {
    spi->base[addr] = val;
}

// rp2040-psram's cross-core variants (DRAM peek/poke from core 0 while core 1
// writes asynchronously): here every access is a single load or store through
// the QMI, which serialises the two cores, and the "async" writes above are
// already complete when they return, so these are the plain accesses
__force_inline static uint8_t psram_read8_xcore(psram_spi_inst_t *spi, uint32_t addr) {
    return spi->base[addr];
}

__force_inline static void psram_write8_xcore(psram_spi_inst_t *spi, uint32_t addr, uint8_t val) {
    spi->base[addr] = val;
}

__force_inline static void psram_write16(psram_spi_inst_t *spi, uint32_t addr, uint16_t val) {
    memcpy((void *)(spi->base + addr), &val, sizeof(val));
}

__force_inline static void psram_write32(psram_spi_inst_t *spi, uint32_t addr, uint32_t val) {
    memcpy((void *)(spi->base + addr), &val, sizeof(val));
}

__force_inline static void psram_write32_async(psram_spi_inst_t *spi, uint32_t addr, uint32_t val) {
    memcpy((void *)(spi->base + addr), &val, sizeof(val));
}

__force_inline static void psram_write(psram_spi_inst_t *spi, const uint32_t addr, const uint8_t *src, const size_t count) {
    memcpy((void *)(spi->base + addr), src, count);
}

__force_inline static void psram_write_async_fast(psram_spi_inst_t *spi, uint32_t addr, uint8_t *val, const size_t count) {
    memcpy((void *)(spi->base + addr), val, count);
}

#ifdef __cplusplus
}
#endif
