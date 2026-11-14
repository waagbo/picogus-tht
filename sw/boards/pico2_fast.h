// Board config file for overclocked Pico 2 (RP2350A), for Pico-based PicoGUS boards

// The below lines aren't just comments - they're directives to the Pico SDK cmake system
// pico_cmake_set PICO_PLATFORM = rp2350
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (4 * 1024 * 1024)
// pico_cmake_set PICO_RP2350_A2_SUPPORTED = 0
// PICO_RP2350_A2_SUPPORTED is turned off for cmake (boards/pico2.h would turn it
// on): it makes picotool add an RP2350-E10 workaround block to UF2s, which
// pgusinit /flash can't handle and which erases the last flash sector (the
// PicoGUS settings) on A2 chips. E10 only affects flash with a partition table,
// which PicoGUS doesn't use. The C code keeps its A2 workarounds: the #define
// of the same name still comes from boards/pico2.h.

#ifndef _BOARDS_PICO2_FAST_H
#define _BOARDS_PICO2_FAST_H

// Allow extra time for xosc to start.
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

// Slower flash for running at 370MHz: 370MHz / 6 = 61.7MHz. The firmware embeds
// the SDK's XIP setup function (PICO_EMBED_XIP_SETUP, the RP2350 counterpart of
// the RP2040's boot2), which applies these at startup and after every flash
// erase or program. RXDELAY is in half clk_sys cycles: 4 samples the flash's
// data 13.5ns after the clock edge that launches it, the same as the SDK's
// default (CLKDIV 2, RXDELAY 2) does at the stock 150MHz.
#define PICO_FLASH_SPI_CLKDIV 6
#define PICO_FLASH_SPI_RXDELAY 4

#include "boards/pico2.h"
#endif
