// Host test stub: single threaded
#pragma once
#include "pico.h"
typedef struct { bool init; } critical_section_t;
static inline void critical_section_init(critical_section_t *c) { c->init = true; }
static inline bool critical_section_is_initialized(critical_section_t *c) { return c->init; }
static inline void critical_section_enter_blocking(critical_section_t *c) { (void)c; }
static inline void critical_section_exit(critical_section_t *c) { (void)c; }
