// Board config file for overclocked Pico 2 (RP2350A), for Pico-based PicoGUS boards

// The below lines aren't just comments - they're directives to the Pico SDK cmake system
// pico_cmake_set PICO_PLATFORM = rp2350
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (4 * 1024 * 1024)
// PICO_RP2350_A2_SUPPORTED is deliberately not set: it makes picotool prepend
// an RP2350-E10 workaround block to UF2s, which pgusinit /flash can't handle
// and which erases the last flash sector (the PicoGUS settings) on A2 chips.
// E10 only affects flash with a partition table, which PicoGUS doesn't use.

#ifndef _BOARDS_PICO2_FAST_H
#define _BOARDS_PICO2_FAST_H

// Allow extra time for xosc to start.
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

// The flash clock divider for the 370MHz overclock is set at runtime by
// overclock_370mhz() (RP2350 has no boot2 to take PICO_FLASH_SPI_CLKDIV)

#include "boards/pico2.h"
#endif
