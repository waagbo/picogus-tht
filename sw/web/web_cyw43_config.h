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

// Keep the bus to the WiFi chip awake. By default the driver lets it sleep
// after ~2.5s without traffic, and waking it up again (for the next packet, or
// any query) makes the driver busy wait for the chip in 1ms steps, on core 1,
// a few ms at a time. The chip draws a little more power; the ISA slot has
// plenty. cyw43_sleep counts down every 50ms: this is years.
#define CYW43_SLEEP_MAX 0xffffffffu

// Run the mode's audio work between the blocks of the WiFi firmware while the
// chip is started, ~250KB over SPI (see web_set_wait_hook())
#ifdef __cplusplus
extern "C" void web_cyw43_event_poll_hook(void);
#else
void web_cyw43_event_poll_hook(void);
#endif
#define CYW43_EVENT_POLL_HOOK web_cyw43_event_poll_hook()
