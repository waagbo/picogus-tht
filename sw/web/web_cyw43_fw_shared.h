// Replacement for the CYW43 chipset firmware include (CYW43_CHIPSET_FIRMWARE_INCLUDE_FILE)
// used by the multi-firmware ("multifw") image on Pico W boards.
//
// Every mode in the multifw image would otherwise carry its own ~225KB copy of
// the WiFi firmware, which does not fit in 2MB of flash. Instead, one copy is
// stored at a fixed flash offset (right after the bootloader) by uf2create and
// all modes point the driver there. The driver checks the blob's signature
// before using it, so a missing blob just means WiFi stays off.

#include "hardware/regs/addressmap.h"

#if !defined(PICOGUS_CYW43_FW_OFFSET) || !defined(PICOGUS_CYW43_WIFI_FW_LEN) || !defined(PICOGUS_CYW43_CLM_LEN)
#error "Shared CYW43 firmware location/size not defined"
#endif

#define CYW43_WIFI_FW_LEN (PICOGUS_CYW43_WIFI_FW_LEN)
#define CYW43_CLM_LEN (PICOGUS_CYW43_CLM_LEN)
// read through the cache-bypassing alias: the XIP cache memory holds the web
// server's buffers (see web_xip_sram.h)
static const uintptr_t fw_data = XIP_NOCACHE_NOALLOC_BASE + PICOGUS_CYW43_FW_OFFSET;
