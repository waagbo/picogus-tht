// Host test stub: the activity LED does nothing here
#pragma once
#include "pico.h"
typedef unsigned int uint;
static inline void gpio_xor_mask(uint32_t mask) { (void)mask; }
