# Script to generate the linker scripts for each firmware with the firmware size and offset (aligned to 1024 bytes)
# As well as the flash_firmware.h file used by the bootloader and uf2create utility

# Initial fixed offset of 32768 for the bootloader, or after the shared CYW43
# WiFi firmware blob (Pico W builds with the web server) that follows it
if(DEFINED FW_FIRST_ORIGIN)
    set(FW_ORIGIN "${FW_FIRST_ORIGIN}")
else()
    set(FW_ORIGIN "32768")
endif()

macro(generate_ld_file FW_FILE FW_LD_FILE)
    file(SIZE ${FW_FILE} FW_SIZE)
    math(EXPR FW_SIZE_ALIGNED "(${FW_SIZE} / 1024) * 1024 + 1024")
    # file(REMOVE ${FW_LD_FILE})
    configure_file(${FW_LD_IN} ${FW_LD_FILE})
    list(APPEND FW_ORIGINS "${FW_ORIGIN}")
    math(EXPR FW_ORIGIN "${FW_ORIGIN} + ${FW_SIZE_ALIGNED}")
endmacro()

generate_ld_file(${FW_1_BIN} ${FW_1_LD})
generate_ld_file(${FW_2_BIN} ${FW_2_LD})
generate_ld_file(${FW_3_BIN} ${FW_3_LD})
generate_ld_file(${FW_4_BIN} ${FW_4_LD})
generate_ld_file(${FW_5_BIN} ${FW_5_LD})
generate_ld_file(${FW_6_BIN} ${FW_6_LD})
list(GET FW_ORIGINS 0 FW_1_ORIGIN)
list(GET FW_ORIGINS 1 FW_2_ORIGIN)
list(GET FW_ORIGINS 2 FW_3_ORIGIN)
list(GET FW_ORIGINS 3 FW_4_ORIGIN)
list(GET FW_ORIGINS 4 FW_5_ORIGIN)
list(GET FW_ORIGINS 5 FW_6_ORIGIN)

# The last flash sector holds the settings. The image is kept within 2 MB on
# every chip (the RP2040's flash size, and what uf2create lays out).
if(FW_ORIGIN GREATER 2093056)
    message(FATAL_ERROR "Firmwares end at ${FW_ORIGIN}, which overlaps the settings sector of a 2 MB flash at 2093056")
endif()

file(REMOVE ${FLASH_FIRMWARE_H})
configure_file(${FLASH_FIRMWARE_H_IN} ${FLASH_FIRMWARE_H})
