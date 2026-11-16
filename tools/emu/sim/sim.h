/* pgcard-sim internal interfaces */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ff.h"

/* file_diskio.c */
bool   stick_open(const char *path);
void   stick_close(void);
void   stick_set_present(bool on);
FATFS *sim_fatfs(void);

/* main.c */
extern int sim_log_level;
void sim_trace_disk(char dir, uint32_t lba, unsigned count, int res);
