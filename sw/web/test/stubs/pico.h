// Host test stub of the Pico SDK base header
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define __in_flash(group)
#define __force_inline inline __attribute__((always_inline))
#define XIP_BASE 0x10000000u
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif
