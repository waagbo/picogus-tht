/*
 * pgcard-sim: PicoGUS card simulator for the QEMU pgbridge device.
 *
 * Listens on a UNIX socket for the byte-wide I/O port accesses the bridge
 * forwards (see tools/emu/qemu/pgbridge.patch for the wire format) and
 * answers them like the card does:
 *
 *   core 0  register logic of sw/picogus.cpp reimplemented here for the
 *           registers the ROM, pgusinit and PGUSDFS use (knock/select on
 *           1D0h, DATA_PORT_LOW/HIGH, the PGDFS data window), delegating to
 *           the real dfs_ctl_* / dfs_data_* (sw/dfs/dfs_transport.c) and
 *           bd_ctl_* (sw/bootdisk) functions.
 *   core 1  the real dfs_tasks() -> dfs_server/dfs_fs/FatFs (+ bootdisk),
 *           run between socket messages and when the ROM polls the status,
 *           so a poll sees BUSY first and READY later (--busy-polls).
 *   USB     a raw image file (--stick), mounted after --mount-delay-ms
 *           (NODRIVE until then), with the same mount notifications as
 *           usb_msc/msc_app.c.
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "picogus.h"
#include "dfs.h"
#include "dfs_server.h"
#include "bootdisk/bootdisk.h"
#include "sim.h"

/* Register numbers the other agents add to common/picogus.h; the values are
 * fixed by sw/bootdisk/PROTOCOL.md. */
#ifndef CMD_BDFDNAME
#define CMD_BDFDNAME 0x88
#endif
#ifndef CMD_BDHDNAME
#define CMD_BDHDNAME 0x89
#endif
#ifndef CMD_BDOPTS
#define CMD_BDOPTS 0x8A
#endif

#define SIM_FWSTRING "pgcard-sim v1.0 (PicoGUS card simulator)"

int sim_log_level;
static FILE *logf;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    va_list ap;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(logf, "[%5ld.%03ld] ", (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

/* ---- time ------------------------------------------------------------------ */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t t_start;
uint32_t dfs_platform_millis(void) {
    return (uint32_t)(now_ms() - t_start);
}

/* ---- options / persisted settings ----------------------------------------- */

static const char *opt_socket = "/tmp/pgcard.sock";
static const char *opt_stick;
static long opt_mount_delay_ms;
static int  opt_busy_polls = 1;
static bool opt_persist;

/* Settings.BootDisk stand-ins, bound to the server with bd_init() */
static char bd_fd_name[BD_NAME_BUF];
static char bd_hd_name[BD_NAME_BUF];
static uint8_t bd_opts;

/* ---- core 0 state (mirrors sw/picogus.cpp) ---------------------------------- */

static bool control_active;
static uint8_t sel_reg;
static uint8_t cur_read;
static uint8_t basePort_low;
static uint16_t dfs_base = DFS_DEFAULT_DATA_PORT;   /* settings.DFS.basePort */
static uint8_t startup_mode = USB_MODE;
static uint16_t base_ports[256];                     /* other *PORT registers, stored only */
static uint8_t plain_regs[256];                      /* other byte registers, stored only */

static inline bool dfs_port_valid(uint16_t base) {
    return base >= 0x100 && base <= 0x3FE && !(base + 1 >= 0x1D0 && base <= 0x1D3);
}

static bool is_port_reg(uint8_t r) {
    switch (r) {
    case CMD_GUSPORT: case CMD_OPLPORT: case CMD_SBPORT: case CMD_MPUPORT:
    case CMD_TANDYPORT: case CMD_CMSPORT: case CMD_MOUSEPORT: case CMD_NE2KPORT:
    case CMD_CDPORT:
        return true;
    }
    return false;
}

/* The registers select_picogus() knows; anything else drops control_active. */
static bool is_known_reg(uint8_t r) {
    switch (r) {
    case CMD_MAGIC: case CMD_PROTOCOL: case CMD_FWSTRING: case CMD_BOOTMODE:
    case CMD_GUSPORT: case CMD_OPLPORT: case CMD_SBPORT: case CMD_MPUPORT:
    case CMD_TANDYPORT: case CMD_CMSPORT: case CMD_JOYEN:
    case CMD_GUSBUF: case CMD_GUSDMA: case CMD_GUS44K:
    case CMD_WTVOL: case CMD_MPUDELAY: case CMD_MPUFAKE: case CMD_OPLWAIT:
    case CMD_MOUSEPORT: case CMD_MOUSEPROTO: case CMD_MOUSERATE: case CMD_MOUSESEN:
    case CMD_NE2KPORT: case CMD_WIFISSID: case CMD_WIFIPASS: case CMD_WIFIAPPLY: case CMD_WIFISTAT:
    case CMD_CDPORT: case CMD_CDLIST: case CMD_SBTYPE: case CMD_SBIRQ: case CMD_SBDMA: case CMD_SBOPTS:
    case CMD_CDSTATUS: case CMD_CDLOAD: case CMD_CDAUTOADV:
    case CMD_MAINVOL: case CMD_OPLVOL: case CMD_SBVOL: case CMD_CDVOL: case CMD_GUSVOL: case CMD_PSGVOL:
    case CMD_CDNAME: case CMD_CDERROR:
    case CMD_SAVE: case CMD_REBOOT: case CMD_DEFAULTS: case CMD_HWTYPE: case CMD_FLASH:
    case CMD_DFSSTAT: case CMD_DFSREQ: case CMD_DFSEXEC: case CMD_DFSRESP:
    case CMD_DFSINFO: case CMD_DFSMAXLEN: case CMD_DFSTIME: case CMD_DFSPORT:
    case CMD_BDFDNAME: case CMD_BDHDNAME: case CMD_BDOPTS:
        return true;
    }
    return false;
}

static const char *reg_name(uint8_t r) {
    switch (r) {
    case CMD_MAGIC: return "MAGIC";
    case CMD_PROTOCOL: return "PROTOCOL";
    case CMD_FWSTRING: return "FWSTRING";
    case CMD_BOOTMODE: return "BOOTMODE";
    case CMD_HWTYPE: return "HWTYPE";
    case CMD_SAVE: return "SAVE";
    case CMD_REBOOT: return "REBOOT";
    case CMD_DEFAULTS: return "DEFAULTS";
    case CMD_DFSSTAT: return "DFSSTAT";
    case CMD_DFSREQ: return "DFSREQ";
    case CMD_DFSEXEC: return "DFSEXEC";
    case CMD_DFSRESP: return "DFSRESP";
    case CMD_DFSINFO: return "DFSINFO";
    case CMD_DFSMAXLEN: return "DFSMAXLEN";
    case CMD_DFSTIME: return "DFSTIME";
    case CMD_DFSPORT: return "DFSPORT";
    case CMD_BDFDNAME: return "BDFDNAME";
    case CMD_BDHDNAME: return "BDHDNAME";
    case CMD_BDOPTS: return "BDOPTS";
    }
    return is_known_reg(r) ? "other" : "UNKNOWN";
}

static const char *al_name(uint8_t al) {
    switch (al) {
    case 0x01: return "RMDIR"; case 0x03: return "MKDIR"; case 0x05: return "CHDIR";
    case 0x06: return "CLOSE"; case 0x08: return "READ"; case 0x09: return "WRITE";
    case 0x0A: return "LOCK"; case 0x0B: return "UNLOCK"; case 0x0C: return "DISKSPACE";
    case 0x0E: return "SETATTR"; case 0x0F: return "GETATTR"; case 0x11: return "RENAME";
    case 0x13: return "DELETE"; case 0x16: return "OPEN"; case 0x17: return "CREATE";
    case 0x1B: return "FINDFIRST"; case 0x1C: return "FINDNEXT"; case 0x21: return "SEEKEND";
    case 0x24: return "SETTIME"; case 0x2E: return "SPOPNFIL";
    case 0xF0: return "ECHO"; case 0xF1: return "LONGNAME"; case 0xF2: return "DIAG";
    case 0xF3: return "BDINFO"; case 0xF4: return "BDREAD"; case 0xF5: return "BDWRITE";
    }
    return "?";
}

/* ---- frame tracing ---------------------------------------------------------- */

#define CAP 48
static uint8_t  req_cap[CAP];
static uint32_t req_bytes;          /* data window writes since CMD_DFSREQ */
static uint8_t  ans_cap[CAP];
static uint32_t ans_bytes;          /* data window reads since CMD_DFSRESP */
static bool     ans_open;
static uint32_t frame_no;
static uint8_t  last_status_logged = 0xAA;
static int      busy_reads;         /* status reads that saw BUSY since EXEC */

static void hexdump(char *out, size_t outsz, const uint8_t *p, uint32_t n) {
    size_t o = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < n && i < CAP && o + 4 < outsz; i++) {
        o += (size_t)snprintf(out + o, outsz - o, "%02x ", p[i]);
    }
}

static void log_request(void) {
    char hx[CAP * 3 + 8];
    uint16_t len = (uint16_t)(req_cap[0] | (req_cap[1] << 8));
    uint8_t al = req_cap[3];
    const uint8_t *pl = req_cap + 4;
    frame_no++;
    if (sim_log_level < 1) return;
    if (al == 0xF4 || al == 0xF5) {
        uint32_t lba = (uint32_t)(pl[2] | (pl[3] << 8) | (pl[4] << 16) | ((uint32_t)pl[5] << 24));
        uint32_t tok = (uint32_t)(pl[6] | (pl[7] << 8) | (pl[8] << 16) | ((uint32_t)pl[9] << 24));
        logmsg("REQ #%u %-9s len=%u unit=%u count=%u lba=%u token=%08x (%u bytes streamed)",
               frame_no, al_name(al), len, pl[0], pl[1], lba, tok, req_bytes);
    } else if (al == 0xF3) {
        logmsg("REQ #%u %-9s len=%u unit=%u flags=%02x (%u bytes streamed)",
               frame_no, al_name(al), len, pl[0], pl[1], req_bytes);
    } else {
        uint32_t n = req_bytes > 4 ? req_bytes - 4 : 0;
        hexdump(hx, sizeof(hx), req_cap + 4, n > CAP - 4 ? CAP - 4 : n);
        logmsg("REQ #%u %-9s len=%u drive=%02x (%u bytes streamed) %s",
               frame_no, al_name(al), len, req_cap[2], req_bytes, hx);
    }
}

static void flush_answer_log(void) {
    char hx[CAP * 3 + 8];
    if (!ans_open) return;
    ans_open = false;
    if (sim_log_level < 1) return;
    uint16_t len = (uint16_t)(ans_cap[0] | (ans_cap[1] << 8));
    uint16_t ax = (uint16_t)(ans_cap[2] | (ans_cap[3] << 8));
    uint32_t shown = ans_bytes > 4 ? ans_bytes - 4 : 0;
    if (shown > 24) shown = 24;
    hexdump(hx, sizeof(hx), ans_cap + 4, shown);
    logmsg("ANS #%u len=%u AX=%04x (%u bytes read) %s", frame_no, len, ax, ans_bytes, hx);
}

void sim_trace_disk(char dir, uint32_t lba, unsigned count, int res) {
    if (sim_log_level >= 3) logmsg("disk %c lba=%u n=%u res=%d", dir, lba, count, res);
}

/* ---- core 1 ------------------------------------------------------------------ */

static bool mounted;
static uint64_t t_connect;          /* the "power on": first bridge connection */

static void core1_mount_check(void) {
    if (mounted || !opt_stick || !t_connect) return;
    if (now_ms() - t_connect < (uint64_t)opt_mount_delay_ms) return;
    stick_set_present(true);
    FRESULT fr = f_mount(sim_fatfs(), "", 1);
    mounted = true;                 /* one attempt, like one USB attach */
    if (fr != FR_OK) {
        logmsg("USB: f_mount failed (FRESULT %d): drive stays NODRIVE", fr);
        return;
    }
    dfs_on_drive_mounted();
#ifdef SIM_CALL_BD_HOOKS
    bd_on_drive_mounted();
#endif
    logmsg("USB: stick mounted after %lu ms, info \"%s\"",
           (unsigned long)(now_ms() - t_connect), dfs_server_info_string());
}

/* One pass of the core 1 loop. serve_busy = false leaves a BUSY frame for
 * later so that the ROM's status poll can observe BUSY. */
static void core1_tick(bool serve_busy) {
    core1_mount_check();
    if (!serve_busy && dfs_ctl_status() == DFS_STATUS_BUSY) return;
    dfs_tasks();
}

/* ---- core 0: register handlers (mirror select_picogus/write_picogus_* /
 *      read_picogus_* in sw/picogus.cpp) ---------------------------------- */

static void select_reg(uint8_t v) {
    flush_answer_log();
    sel_reg = v;
    if (sim_log_level >= 2 || (sim_log_level >= 1 && v != CMD_DFSSTAT && v != CMD_DFSREQ &&
                               v != CMD_DFSRESP && v != CMD_DFSEXEC))
        logmsg("select %02x %s", v, reg_name(v));
    if (is_port_reg(v)) { basePort_low = 0; return; }
    switch (v) {
    case CMD_FWSTRING: cur_read = 0; break;
    case CMD_DFSREQ: dfs_ctl_select_req(); req_bytes = 0; memset(req_cap, 0, sizeof(req_cap)); break;
    case CMD_DFSRESP: dfs_ctl_select_resp(); ans_bytes = 0; ans_open = true; memset(ans_cap, 0, sizeof(ans_cap)); break;
    case CMD_DFSINFO: dfs_ctl_info_rewind(); break;
    case CMD_DFSTIME: dfs_ctl_time_rewind(); break;
    case CMD_DFSPORT: basePort_low = 0; break;
    case CMD_BDFDNAME: bd_ctl_name_select(BD_UNIT_FD); break;
    case CMD_BDHDNAME: bd_ctl_name_select(BD_UNIT_HD); break;
    case CMD_FLASH:
        logmsg("CMD_FLASH selected: firmware flashing is not simulated");
        break;
    default:
        if (!is_known_reg(v)) control_active = false;
        break;
    }
}

static void write_low(uint8_t v) {
    if (is_port_reg(sel_reg) || sel_reg == CMD_DFSPORT) basePort_low = v;
}

static void write_high(uint8_t v) {
    if (sim_log_level >= 2 || (sim_log_level >= 1 && sel_reg != CMD_DFSEXEC && sel_reg != CMD_DFSTIME &&
                               sel_reg != CMD_BDFDNAME && sel_reg != CMD_BDHDNAME))
        logmsg("write %02x %s <- %02x", sel_reg, reg_name(sel_reg), v);
    if (is_port_reg(sel_reg)) {
        base_ports[sel_reg] = (v || basePort_low) ? (uint16_t)((v << 8) | basePort_low) : 0xFFFF;
        return;
    }
    switch (sel_reg) {
    case CMD_BOOTMODE: startup_mode = v; break;
    case CMD_SAVE: logmsg("CMD_SAVE: settings saved (fd=\"%s\" hd=\"%s\" opts=%02x dfsport=%03x)",
                          bd_fd_name, bd_hd_name, bd_opts, dfs_base); break;
    case CMD_REBOOT: logmsg("CMD_REBOOT: card reboot is not simulated"); break;
    case CMD_DEFAULTS:              /* getDefaultSettings() + the floppy eject of picogus.cpp */
        logmsg("CMD_DEFAULTS: settings reset");
        memset(bd_fd_name, 0, sizeof(bd_fd_name));
        memset(bd_hd_name, 0, sizeof(bd_hd_name));
        bd_opts = 0;
        dfs_base = DFS_DEFAULT_DATA_PORT;
        bd_ctl_name_select(BD_UNIT_FD);
        bd_ctl_name_write(BD_UNIT_FD, 0);
        break;
    case CMD_FLASH: break;
    case CMD_DFSSTAT: dfs_ctl_abort(); logmsg("DFSSTAT write: abort"); break;
    case CMD_DFSEXEC:
        log_request();
        dfs_ctl_exec();
        busy_reads = 0;
        if (sim_log_level >= 1 && dfs_ctl_status() != DFS_STATUS_BUSY)
            logmsg("EXEC rejected: status %02x (declared len vs %u bytes streamed)", dfs_ctl_status(), req_bytes);
        break;
    case CMD_DFSTIME: dfs_ctl_time_write(v); break;
    case CMD_DFSPORT: {
        uint16_t base = (uint16_t)(((v << 8) | basePort_low) & ~1u);
        if (base == 0 || dfs_port_valid(base)) dfs_base = base;
        break;
    }
    case CMD_BDFDNAME: bd_ctl_name_write(BD_UNIT_FD, v); if (!v && sim_log_level >= 1) logmsg("BDFDNAME commit"); break;
    case CMD_BDHDNAME: bd_ctl_name_write(BD_UNIT_HD, v); if (!v && sim_log_level >= 1) logmsg("BDHDNAME commit"); break;
    case CMD_BDOPTS: bd_ctl_opts_write(v); break;
    default: plain_regs[sel_reg] = v; break;
    }
}

static uint8_t read_low(void) {
    if (is_port_reg(sel_reg)) {
        uint16_t p = base_ports[sel_reg];
        return p == 0xFFFF ? 0 : (uint8_t)(p & 0xFF);
    }
    switch (sel_reg) {
    case CMD_DFSMAXLEN: return dfs_base ? (uint8_t)(dfs_ctl_max_payload() & 0xFF) : 0;
    case CMD_DFSPORT: return (uint8_t)(dfs_base & 0xFF);
    }
    return 0;
}

static uint8_t read_high(void) {
    uint8_t ret;
    if (is_port_reg(sel_reg)) {
        uint16_t p = base_ports[sel_reg];
        return p == 0xFFFF ? 0 : (uint8_t)(p >> 8);
    }
    switch (sel_reg) {
    case CMD_MAGIC: return 0xDD;
    case CMD_PROTOCOL: return PICOGUS_PROTOCOL_VER;
    case CMD_FWSTRING:
        ret = (uint8_t)SIM_FWSTRING[cur_read++];
        if (!ret) cur_read = 0;
        return ret;
    case CMD_BOOTMODE: return startup_mode;
    case CMD_HWTYPE: return PICOGUS_2;
    case CMD_FLASH: return 0;
    case CMD_DFSSTAT: {
        uint8_t s = dfs_ctl_status();
        if (s == DFS_STATUS_BUSY && ++busy_reads > opt_busy_polls) {
            core1_tick(true);        /* core 1 finishes the request */
            s = dfs_ctl_status();
        }
        if (sim_log_level >= 2 || (sim_log_level >= 1 && s != last_status_logged))
            logmsg("status -> %02x", s);
        last_status_logged = s;
        return s;
    }
    case CMD_DFSINFO: return dfs_ctl_info_read();
    case CMD_DFSMAXLEN: return dfs_base ? (uint8_t)(dfs_ctl_max_payload() >> 8) : 0;
    case CMD_DFSPORT: return (uint8_t)(dfs_base >> 8);
    case CMD_BDFDNAME: return bd_ctl_name_read(BD_UNIT_FD);
    case CMD_BDHDNAME: return bd_ctl_name_read(BD_UNIT_HD);
    case CMD_BDOPTS: return bd_ctl_opts_read();
    case CMD_DFSREQ: case CMD_DFSEXEC: case CMD_DFSRESP: case CMD_DFSTIME:
        return 0xFF;
    }
    if (is_known_reg(sel_reg)) return plain_regs[sel_reg];
    return 0xFF;
}

/* One byte-wide bus cycle. Returns the value for reads. */
static uint8_t bus_cycle(bool is_write, uint16_t port, uint8_t v) {
    if (dfs_base && (port & ~1u) == dfs_base) {
        if (is_write) {
            if (req_bytes < CAP) req_cap[req_bytes] = v;
            req_bytes++;
            dfs_data_write(v);
            return 0;
        }
        v = dfs_data_read();
        if (ans_bytes < CAP) ans_cap[ans_bytes] = v;
        ans_bytes++;
        return v;
    }
    switch (port) {
    case CONTROL_PORT:
        if (!is_write) return sel_reg;
        if (v == 0xCC) control_active = true;
        else if (control_active) select_reg(v);
        return 0;
    case DATA_PORT_LOW:
        if (!is_write) return read_low();
        if (control_active) write_low(v);
        return 0;
    case DATA_PORT_HIGH:
        if (!is_write) return read_high();
        if (control_active) write_high(v);
        return 0;
    }
    /* not decoded by the card: the bus floats */
    return 0xFF;
}

/* ---- socket loop ------------------------------------------------------------- */

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static bool send_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void serve_client(int cfd) {
    static uint8_t buf[65536];
    size_t have = 0;
    for (;;) {
        if (stop) return;
        int timeout = 20;
        if (dfs_ctl_status() == DFS_STATUS_BUSY) timeout = 1;
        if (opt_stick && !mounted && t_connect) {
            int64_t left = (int64_t)opt_mount_delay_ms - (int64_t)(now_ms() - t_connect);
            if (left < timeout) timeout = left < 0 ? 0 : (int)left;
        }
        struct pollfd pfd = { .fd = cfd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0 && errno == EINTR) continue;
        if (pr == 0) {                      /* bus idle: core 1 catches up */
            core1_tick(true);
            continue;
        }
        ssize_t n = read(cfd, buf + have, sizeof(buf) - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            logmsg("bridge disconnected");
            return;
        }
        have += (size_t)n;
        size_t off = 0;
        while (have - off >= 8) {
            uint8_t *m = buf + off;
            uint16_t port = (uint16_t)(m[2] | (m[3] << 8));
            uint8_t val = m[4];
            off += 8;
            if (m[0] == 'w') {
                if (sim_log_level >= 4) logmsg("out %03x, %02x", port, val);
                bus_cycle(true, port, val);
                /* core 1 runs concurrently on the card: anything but a
                 * request being served (time, committed names) happens now */
                if (port != (dfs_base & ~1u) && port != (dfs_base | 1u)) core1_tick(false);
            } else if (m[0] == 'r') {
                uint8_t r = bus_cycle(false, port, 0);
                if (sim_log_level >= 4) logmsg("in  %03x -> %02x", port, r);
                uint8_t rep[8] = { 'R', 1, m[2], m[3], r, 0, 0, 0 };
                if (!send_all(cfd, rep, sizeof(rep))) {
                    logmsg("bridge write failed");
                    return;
                }
            } else {
                logmsg("protocol error: op %02x", m[0]);
                return;
            }
        }
        memmove(buf, buf + off, have - off);
        have -= off;
    }
}

static void usage(void) {
    fprintf(stderr,
        "usage: pgcard-sim --stick IMAGE [options]\n"
        "  --stick FILE         raw image of the USB drive (FAT12/16/32, superfloppy or MBR)\n"
        "  --fd NAME            floppy image path on the stick (Settings.BootDisk fd name)\n"
        "  --hd NAME            hard disk image path on the stick\n"
        "  --opts N             CMD_BDOPTS value (bit0 fd ro, bit1 hd ro, bit2 rom off, bit3 boot hd)\n"
        "  --mount-delay-ms N   report NODRIVE for N ms after the bridge connects\n"
        "  --dfsport N          PGDFS data window base (default 0x1d4, 0 = disabled)\n"
        "  --busy-polls N       status polls that see BUSY before a request is served (default 1)\n"
        "  --socket PATH        UNIX socket to listen on (default /tmp/pgcard.sock)\n"
        "  --persist            keep listening after the bridge disconnects\n"
        "  --log[=N]            trace: 1 registers+frames, 2 +status polls, 3 +disk I/O, 4 +every port access\n"
        "  --logfile FILE       trace destination (default stderr)\n");
}

int main(int argc, char **argv) {
    static const struct option lo[] = {
        { "stick", required_argument, 0, 's' },
        { "fd", required_argument, 0, 'f' },
        { "hd", required_argument, 0, 'h' },
        { "opts", required_argument, 0, 'o' },
        { "mount-delay-ms", required_argument, 0, 'd' },
        { "dfsport", required_argument, 0, 'p' },
        { "busy-polls", required_argument, 0, 'b' },
        { "socket", required_argument, 0, 'S' },
        { "persist", no_argument, 0, 'P' },
        { "log", optional_argument, 0, 'l' },
        { "logfile", required_argument, 0, 'L' },
        { "help", no_argument, 0, '?' },
        { 0, 0, 0, 0 }
    };
    int c;
    logf = stderr;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 's': opt_stick = optarg; break;
        case 'f': snprintf(bd_fd_name, sizeof(bd_fd_name), "%s", optarg); break;
        case 'h': snprintf(bd_hd_name, sizeof(bd_hd_name), "%s", optarg); break;
        case 'o': bd_opts = (uint8_t)strtoul(optarg, NULL, 0); break;
        case 'd': opt_mount_delay_ms = strtol(optarg, NULL, 0); break;
        case 'p': dfs_base = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'b': opt_busy_polls = atoi(optarg); break;
        case 'S': opt_socket = optarg; break;
        case 'P': opt_persist = true; break;
        case 'l': sim_log_level = optarg ? atoi(optarg) : 1; break;
        case 'L':
            logf = fopen(optarg, "w");
            if (!logf) { perror(optarg); return 2; }
            break;
        default: usage(); return 2;
        }
    }
    if (dfs_base && !dfs_port_valid(dfs_base)) {
        fprintf(stderr, "pgcard-sim: invalid --dfsport\n");
        return 2;
    }
    if (opt_stick && !stick_open(opt_stick)) return 1;

    t_start = now_ms();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* card boot, core 0: settings, then the servers (before core 1 starts) */
    bd_init(bd_fd_name, bd_hd_name, &bd_opts);
    /* per-boot nonce for the image tokens: a new sim process is a new card boot */
    bd_set_boot_nonce((uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ (uint32_t)now_ms());
    dfs_init();

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(opt_socket) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "pgcard-sim: socket path too long\n");
        return 2;
    }
    strcpy(sa.sun_path, opt_socket);
    unlink(opt_socket);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(lfd, 1) != 0) {
        perror("pgcard-sim: socket");
        return 1;
    }
    logmsg("pgcard-sim listening on %s (stick %s, fd \"%s\", hd \"%s\", opts %02x, mount delay %ld ms, data window %03x)",
           opt_socket, opt_stick ? opt_stick : "(none)", bd_fd_name, bd_hd_name, bd_opts,
           opt_mount_delay_ms, dfs_base);

    while (!stop) {
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        if (!t_connect) t_connect = now_ms();
        logmsg("bridge connected");
        serve_client(cfd);
        close(cfd);
        if (!opt_persist) break;
    }
    close(lfd);
    unlink(opt_socket);
    if (mounted) f_unmount("");
    stick_close();
    logmsg("pgcard-sim exit (%u frames)", frame_no);
    return 0;
}
