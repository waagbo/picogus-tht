/*
 * pgbridge: forwards the PicoGUS ISA I/O ports to an external card simulator
 * (tools/emu/sim/pgcard-sim in the PicoGUS repository) over a UNIX socket.
 *
 * Claimed ports:
 *   control 1D0h-1D2h (CONTROL_PORT, DATA_PORT_LOW, DATA_PORT_HIGH)
 *   data window: 2 ports at "dataport" (default 1D4h, 0 = not claimed)
 *
 * The PicoGUS is an 8-bit ISA card, so it only ever sees 8-bit bus cycles:
 * a 16-bit CPU access to it is split by the bus into two byte cycles, base
 * (low byte) first, then base+1 (high byte). impl.max_access_size = 1 makes
 * the QEMU memory core do exactly that split before the callbacks run, so
 * the simulator only ever receives byte accesses, in bus order.
 *
 * Wire format (both directions, 8 bytes, little-endian):
 *   u8 op    'r' read, 'w' write            (QEMU -> sim)
 *            'R' read reply                  (sim -> QEMU)
 *   u8 size  always 1
 *   u16 port absolute I/O port
 *   u32 value
 * Writes are posted (no reply); reads block until the reply arrives. The
 * simulator handles messages strictly in order, so a read always observes
 * every earlier write, like on the bus.
 *
 * Properties:
 *   path      UNIX socket of the simulator (required)
 *   dataport  data window base (default 0x1d4, 0 = none)
 *   timeout   seconds to wait for the simulator socket at realize (default 10)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include <sys/socket.h>
#include <sys/un.h>

#define TYPE_PGBRIDGE "pgbridge"
OBJECT_DECLARE_SIMPLE_TYPE(PGBridgeState, PGBRIDGE)

#define PGB_CTRL_BASE 0x1d0
#define PGB_CTRL_SIZE 3

typedef struct {
    uint16_t base;
    PGBridgeState *s;
} PGBridgeRegion;

struct PGBridgeState {
    ISADevice parent_obj;

    char *path;
    uint32_t dataport;
    uint32_t timeout;

    int fd;
    bool dead;
    MemoryRegion ctrl_io;
    MemoryRegion data_io;
    PGBridgeRegion ctrl_region;
    PGBridgeRegion data_region;
};

static bool pgb_xfer(PGBridgeState *s, uint8_t *msg, size_t len, bool send)
{
    size_t done = 0;

    if (s->dead) {
        return false;
    }
    while (done < len) {
        ssize_t n = send ? write(s->fd, msg + done, len - done)
                         : read(s->fd, msg + done, len - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error_report("pgbridge: simulator connection lost (%s)",
                         n < 0 ? strerror(errno) : "EOF");
            s->dead = true;
            return false;
        }
        done += n;
    }
    return true;
}

static void pgb_pack(uint8_t *m, uint8_t op, uint16_t port, uint32_t val)
{
    m[0] = op;
    m[1] = 1;
    m[2] = port & 0xff;
    m[3] = port >> 8;
    m[4] = val & 0xff;
    m[5] = (val >> 8) & 0xff;
    m[6] = (val >> 16) & 0xff;
    m[7] = val >> 24;
}

static uint64_t pgb_read(void *opaque, hwaddr addr, unsigned size)
{
    PGBridgeRegion *r = opaque;
    PGBridgeState *s = r->s;
    uint16_t port = r->base + addr;
    uint8_t m[8];

    pgb_pack(m, 'r', port, 0);
    if (!pgb_xfer(s, m, sizeof(m), true) || !pgb_xfer(s, m, sizeof(m), false)) {
        return 0xff;
    }
    if (m[0] != 'R' || (m[2] | (m[3] << 8)) != port) {
        error_report("pgbridge: bad reply op=%02x port=%04x for read of %04x",
                     m[0], m[2] | (m[3] << 8), port);
        s->dead = true;
        return 0xff;
    }
    return m[4];
}

static void pgb_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    PGBridgeRegion *r = opaque;
    uint8_t m[8];

    pgb_pack(m, 'w', r->base + addr, val & 0xff);
    pgb_xfer(r->s, m, sizeof(m), true);
}

static const MemoryRegionOps pgb_ops = {
    .read = pgb_read,
    .write = pgb_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    /* in ax, 1D1h is a legal (odd) word access on the ISA bus */
    .valid.unaligned = true,
    /* 8-bit card: the memory core splits wider accesses into byte cycles,
     * lowest address first, like the ISA bus does. */
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pgb_realize(DeviceState *d, Error **errp)
{
    ISADevice *dev = ISA_DEVICE(d);
    PGBridgeState *s = PGBRIDGE(d);
    struct sockaddr_un sa;
    uint32_t waited_ms = 0;

    if (!s->path || !*s->path) {
        error_setg(errp, "pgbridge: property 'path' (simulator socket) is required");
        return;
    }
    if (strlen(s->path) >= sizeof(sa.sun_path)) {
        error_setg(errp, "pgbridge: socket path too long");
        return;
    }
    if (s->dataport && ((s->dataport & 1) || s->dataport > 0x3fe ||
                        (s->dataport + 1 >= PGB_CTRL_BASE &&
                         s->dataport < PGB_CTRL_BASE + PGB_CTRL_SIZE))) {
        error_setg(errp, "pgbridge: bad dataport 0x%x", s->dataport);
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, s->path);
    for (;;) {
        s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s->fd < 0) {
            error_setg_errno(errp, errno, "pgbridge: socket");
            return;
        }
        if (connect(s->fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            break;
        }
        close(s->fd);
        s->fd = -1;
        if (waited_ms >= s->timeout * 1000) {
            error_setg_errno(errp, errno, "pgbridge: cannot connect to %s", s->path);
            return;
        }
        g_usleep(100 * 1000);
        waited_ms += 100;
    }
    s->dead = false;

    s->ctrl_region.base = PGB_CTRL_BASE;
    s->ctrl_region.s = s;
    memory_region_init_io(&s->ctrl_io, OBJECT(dev), &pgb_ops, &s->ctrl_region,
                          "pgbridge-ctrl", PGB_CTRL_SIZE);
    memory_region_add_subregion(isa_address_space_io(dev), PGB_CTRL_BASE,
                                &s->ctrl_io);
    if (s->dataport) {
        s->data_region.base = s->dataport;
        s->data_region.s = s;
        memory_region_init_io(&s->data_io, OBJECT(dev), &pgb_ops,
                              &s->data_region, "pgbridge-data", 2);
        memory_region_add_subregion(isa_address_space_io(dev), s->dataport,
                                    &s->data_io);
    }
}

static const Property pgb_properties[] = {
    DEFINE_PROP_STRING("path", PGBridgeState, path),
    DEFINE_PROP_UINT32("dataport", PGBridgeState, dataport, 0x1d4),
    DEFINE_PROP_UINT32("timeout", PGBridgeState, timeout, 10),
};

static void pgb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pgb_realize;
    dc->desc = "PicoGUS I/O port bridge to an external card simulator";
    device_class_set_props(dc, pgb_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pgb_info = {
    .name          = TYPE_PGBRIDGE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(PGBridgeState),
    .class_init    = pgb_class_init,
};

static void pgb_register_types(void)
{
    type_register_static(&pgb_info);
}

type_init(pgb_register_types)
