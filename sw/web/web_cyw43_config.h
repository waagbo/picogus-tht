// CYW43 driver configuration for the PicoGUS web server (CYW43_CONFIG_FILE)
#include <cyw43_configport.h>

// All firmware runs from RAM (PICO_COPY_TO_RAM), keep the ~225KB WiFi firmware
// blob and NVRAM data in flash. They are only read while the chip is brought up.
#define CYW43_RESOURCE_ATTRIBUTE __in_flash()

// DHCP host name; also used for mDNS
#undef CYW43_HOST_NAME
#define CYW43_HOST_NAME "picogus"

#ifndef PGDEBUG
#define CYW43_PRINTF(...) (void)0
#endif
// cyw43_config.h only includes <stdio.h> inside its #ifndef CYW43_PRINTF
// fallback, so we must provide it for cyw43 sources that call printf directly
// (e.g. cyw43_stats.c:cyw43_dump_stats — dead code, stripped by --gc-sections).
#include <stdio.h>
