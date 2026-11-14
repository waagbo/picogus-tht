/* Copy-to-RAM link script for a firmware in the RP2350 multifw bundle.

   This is the Pico SDK's memmap_copy_to_ram.ld (SDK 2.3 and later, where the
   script is assembled from include files) without its FLASH region: that is
   defined by the generated per-firmware script that includes this file, so
   every firmware is linked for its own slot in the bundle (see firmware.ld.in
   and generate_ld.cmake).
*/

/* Include platform memory locations */
INCLUDE "set_memory_locations.incl"

/* Include memory regions used, apart from FLASH */
INCLUDE "memory_psram.incl"
INCLUDE "memory_xip_ram.incl"
INCLUDE "memory_ram.incl"
INCLUDE "memory_scratch.incl"
INCLUDE "memory_generated.incl"
INCLUDE "memory_extra.incl"

/* Include aliases for storage memory regions */
INCLUDE "memory_aliases_flash.incl"

/* Define entry point symbol */
ENTRY(_entry_point)

/* Include default sections */
INCLUDE "sections_copy_to_ram.incl"
