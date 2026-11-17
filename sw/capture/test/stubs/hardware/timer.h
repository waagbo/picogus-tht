// Host stand-in for the Pico SDK's timer: the tests set the time
#pragma once
#include <stdint.h>

extern uint64_t test_clock_us;

static inline uint32_t time_us_32(void) { return (uint32_t)test_clock_us; }
static inline uint64_t time_us_64(void) { return test_clock_us; }
