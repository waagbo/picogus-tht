// By Jeroen Taverne
// New features and mods by smymm

#include "pico/stdlib.h"
#include "hardware/structs/watchdog.h"
#include <hardware/flash.h>
#include "flash_firmware.h"
#include "system/flash_settings.h"
#include "../include/pg_debug.h"

#if PICO_RP2040
// RP2040 images start with the 256 byte boot2, followed by the vector table
#define FW_VECTOR_TABLE_OFFSET 0x100
#define VTOR_ADDR (PPB_BASE + M0PLUS_VTOR_OFFSET)
#else
// RP2350 images have no boot2: the vector table is at the start of the image
// (the bootrom-visible IMAGE_DEF block follows it)
#define FW_VECTOR_TABLE_OFFSET 0
#define VTOR_ADDR (PPB_BASE + M33_VTOR_OFFSET)
#endif

static uint32_t sStart = 0;
static const uint32_t offset[NR_OF_FIRMWARES] = {FLASH_FIRMWARE1, FLASH_FIRMWARE2, FLASH_FIRMWARE3, FLASH_FIRMWARE4, FLASH_FIRMWARE5, FLASH_FIRMWARE6};

uint8_t read_permMode(void)
{
    Settings settings;
    loadSettings(&settings, false /* don't migrate - we just need the startupMode */);
    uint8_t pModeByte = settings.startupMode;

    if (pModeByte >= 1 && pModeByte <= NR_OF_FIRMWARES)
        return (pModeByte - 1);
    else
        return 0;   // No valid value, boot 1st fw.
}


int main(void)
{
    stdio_init_all();
    DBG_PUTS("picogus bootloader");
    stdio_flush();
    uint8_t firmware_nr = (uint8_t) (0x000000FF & watchdog_hw->scratch[3]);

    if (firmware_nr > NR_OF_FIRMWARES) {
        firmware_nr = 0;
    }

    if (firmware_nr == 0)   // Try to boot from permanent storage if no change requested / cold boot / bad value
    {
        sStart = offset[read_permMode()] + XIP_BASE;
    } else {    // Mode change requested by pgusinit
    	sStart = offset[firmware_nr-1] + XIP_BASE;
    }

    // Jump to application
    asm volatile
    (
         "mov r0, %[start]\n"
         "ldr r1, =%[vtable]\n"
         "str r0, [r1]\n"
         "ldmia r0, {r0, r1}\n"
         "msr msp, r0\n"
         "bx r1\n"
         :
         : [start] "r" (sStart + FW_VECTOR_TABLE_OFFSET), [vtable] "X" (VTOR_ADDR)
         :
    );

    return 0;
}
