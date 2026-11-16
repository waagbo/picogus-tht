/*
 * Stand-in for the sw/bootdisk C sources, used only while that directory has no C
 * sources yet (the Makefile switches to the real code automatically).
 * Registers work (names and options are stored and read back); every
 * BD frame answers AX = 01h like firmware without PGBOOT.
 */
#include <string.h>
#include "bootdisk/bootdisk.h"

static char *names[BD_UNITS];
static uint8_t *opts_p;
static uint8_t idx_w[BD_UNITS], idx_r[BD_UNITS];

void bd_init(char *fd_name, char *hd_name, uint8_t *opts) {
    names[0] = fd_name;
    names[1] = hd_name;
    opts_p = opts;
}
void bd_ctl_name_select(uint8_t unit) { if (unit < BD_UNITS) idx_w[unit] = idx_r[unit] = 0; }
void bd_ctl_name_write(uint8_t unit, uint8_t c) {
    if (unit >= BD_UNITS) return;
    if (idx_w[unit] < BD_NAME_MAX) names[unit][idx_w[unit]++] = (char)c;
    if (c == 0 || idx_w[unit] >= BD_NAME_MAX) { names[unit][idx_w[unit]] = 0; idx_w[unit] = 0; }
}
uint8_t bd_ctl_name_read(uint8_t unit) {
    if (unit >= BD_UNITS) return 0;
    uint8_t c = (uint8_t)names[unit][idx_r[unit]];
    if (c == 0) idx_r[unit] = 0; else idx_r[unit]++;
    return c;
}
uint8_t bd_ctl_opts_read(void) { return (uint8_t)((*opts_p & BD_OPT_MASK) | BD_OPTS_SIGNATURE); }
void bd_ctl_opts_write(uint8_t v) { *opts_p = v & BD_OPT_MASK; }
void bd_set_boot_nonce(uint32_t nonce) { (void)nonce; }
void bd_tasks(void) {}
void bd_on_drive_mounted(void) {}
void bd_on_drive_unmounted(void) {}
uint16_t bd_process(uint8_t al, const uint8_t *req, uint16_t req_len,
                    uint8_t *answ, uint16_t maxpl, uint16_t *ax) {
    (void)al; (void)req; (void)req_len; (void)answ; (void)maxpl;
    *ax = BD_ST_BADCMD;
    return 0;
}
bool bd_path_is_open_image(const char *path) { (void)path; return false; }
bool bd_path_holds_open_image(const char *path) { (void)path; return false; }
