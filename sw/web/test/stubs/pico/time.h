// Host test stub: time is simulated by the test driver
#pragma once
#include "pico.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef uint64_t absolute_time_t;
extern uint64_t test_now_us;
static inline absolute_time_t get_absolute_time(void) { return test_now_us; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
static inline uint32_t time_us_32(void) { return (uint32_t)test_now_us; }
#ifdef __cplusplus
}
#endif
