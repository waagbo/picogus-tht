// Board config file for PicoGUS v2.1 THT: the PicoGUS 2.0 circuit on a
// Waveshare Core2350B2 module (RP2350B, 16 MB flash, 8 MB PSRAM on QMI CS1).
// Same GPIO map as PicoGUS 2.0; the GUS sample RAM is the module's PSRAM
// (PSRAM_QMI) instead of v2.0's APS6404L on GPIO0-3.

// The below lines aren't just comments - they're directives to the Pico SDK cmake system
// pico_cmake_set PICO_PLATFORM = rp2350
// pico_cmake_set PICO_CYW43_SUPPORTED = 1
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (16 * 1024 * 1024)
// PICO_RP2350_A2_SUPPORTED is deliberately not set, as for pico2_fast.h.

#ifndef _BOARDS_PICOGUS21_H
#define _BOARDS_PICOGUS21_H

// For board detection: the firmware can't tell this board apart at runtime
// (v2.0 is recognised by GPIO29 on the ADC, and the RP2350B ADC is GPIO40-47)
#define PICOGUS_BOARD_V21 1

// RP2350B: 48 GPIOs
#define PICO_RP2350A 0

// Waveshare Core2350B: 12 MHz crystal, 16 MB W25Q128-class flash
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// PSRAM on the module: chip select is QMI CS1 on GPIO47 (Core2350B1/B2)
#define PICOGUS_PSRAM_QMI_CS_PIN 47

// The carrier's activity LED is GPIO23, as on PicoGUS 2.0. The module's own
// LED is on GPIO39.
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 23
#endif

// --- UART: MIDI/debug out on GPIO28, as on every PicoGUS ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 28
#endif

// --- Radio Module 2 (U14, CYW43439), optional: wired as on the Pico W, on four
// of the module's free GPIOs. Data and clock share one PIO, so both are in the
// GPIO16-47 window. BT_ON is tied to WL_ON on the card; the RM2's own GPIO0-2
// (on a Pico W: LED, SMPS mode, VBUS sense) are not connected.
#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif
#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif
#ifndef CYW43_PIN_WL_DYNAMIC
#define CYW43_PIN_WL_DYNAMIC 0
#endif
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 40u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 44u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 44u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 44u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 45u
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 42u
#endif

// --- Boot ROM (experimental; U15 flash in a PLCC-32 socket, U16 74HCT688): ROM_EN.
// High = the option ROM answers at its base address (solder jumpers JP1-JP3,
// default D0000h-D7FFFh). A 5.1k pull-down (R35) keeps it off until the firmware
// drives the pin; the firmware only drives it high when built with
// -DBOOTROM_ENABLE=ON.
#define PICOGUS_BOOTROM_EN_PIN 2

// The flash clock divider for the 370MHz overclock is set at runtime by
// overclock_370mhz() (RP2350 has no boot2 to take PICO_FLASH_SPI_CLKDIV)

#endif
