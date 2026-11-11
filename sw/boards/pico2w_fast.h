// Board config file for overclocked Pico 2 W (RP2350A + CYW43439)

// The below lines aren't just comments - they're directives to the Pico SDK cmake system
// pico_cmake_set PICO_PLATFORM = rp2350
// pico_cmake_set PICO_CYW43_SUPPORTED = 1
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (4 * 1024 * 1024)
// pico_cmake_set PICO_RP2350_A2_SUPPORTED = 0
// (no RP2350-E10 workaround block in UF2s, see pico2_fast.h)

#ifndef _BOARDS_PICO2W_FAST_H
#define _BOARDS_PICO2W_FAST_H

// Allow extra time for xosc to start.
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

// Slower flash for running at 370MHz, see pico2_fast.h
#define PICO_FLASH_SPI_CLKDIV 6
#define PICO_FLASH_SPI_RXDELAY 4

#include "boards/pico2_w.h"
#endif
