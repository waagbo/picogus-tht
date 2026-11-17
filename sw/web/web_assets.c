/*
 *  Copyright (C) 2026  PicoGUS contributors
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

// The files of the web page: a pack made by scripts/web_pack.py, kept in flash.
//
// In the multi-firmware image the pack is stored once, at a fixed place after
// the WiFi chip's firmware, and every mode reads it from there. Firmware built
// for a single mode carries it as an array of its own.
//
// Flash is only read through the cache-bypassing alias (the XIP cache's memory
// holds the web server's buffers, see web_xip_sram.h), and only from
// web_task(), which keeps core 0 from writing flash meanwhile.
//
// Pack format, little endian:
//   "PGWP", u16 version (1), u16 count, u32 size of the pack, u32 reserved
//   count entries: char path[28], char type[28], u32 offset, u32 length,
//                  u32 etag, u32 flags (1 = gzip compressed)
//   the files, each starting on a 4 byte boundary

#include <string.h>

#include "web_internal.h"

#ifdef PICOGUS_WEB_ASSETS_OFFSET
#include "hardware/regs/addressmap.h"
#else
#include "web_assets.h"
#endif

#define PACK_VERSION    1
#define PACK_MAX_FILES  64
#define PACK_FLAG_GZIP  1

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t count;
    uint32_t size;
    uint32_t reserved;
} pack_header_t;

typedef struct {
    char path[28];
    char type[28];
    uint32_t offset;
    uint32_t length;
    uint32_t etag;
    uint32_t flags;
} pack_entry_t;

// The pack if there is a sound one, else NULL
static const pack_header_t *pack(void) {
#ifdef PICOGUS_WEB_ASSETS_OFFSET
    const pack_header_t *p = (const pack_header_t *)(XIP_NOCACHE_NOALLOC_BASE + PICOGUS_WEB_ASSETS_OFFSET);
    uint32_t max = PICOGUS_WEB_ASSETS_SIZE;
#else
    const pack_header_t *p = (const pack_header_t *)web_flash_uncached(web_assets_pack);
    uint32_t max = sizeof(web_assets_pack);
#endif
    if (memcmp(p->magic, "PGWP", 4) || p->version != PACK_VERSION || p->count > PACK_MAX_FILES
        || p->size > max || sizeof(*p) + p->count * sizeof(pack_entry_t) > p->size) {
        return NULL;
    }
    return p;
}

int web_asset_find(const char *path) {
    const pack_header_t *p = pack();
    if (!p) {
        return -1;
    }
    if (!strcmp(path, "/index.html")) {
        path = "/";
    }
    const pack_entry_t *e = (const pack_entry_t *)(p + 1);
    for (int i = 0; i < p->count; ++i) {
        if (!strncmp(e[i].path, path, sizeof(e[i].path)) && !e[i].path[sizeof(e[i].path) - 1]
            && !e[i].type[sizeof(e[i].type) - 1]
            && e[i].offset <= p->size && e[i].length <= p->size - e[i].offset) {
            return i;
        }
    }
    return -1;
}

// False if the pack changed since web_asset_find() (a firmware upload
// rewrites it)
bool web_asset_get(int index, web_asset_t *asset) {
    const pack_header_t *p = pack();
    if (!p || index < 0 || index >= p->count) {
        return false;
    }
    const pack_entry_t *e = (const pack_entry_t *)(p + 1) + index;
    if (e->path[sizeof(e->path) - 1] || e->type[sizeof(e->type) - 1]
        || e->offset > p->size || e->length > p->size - e->offset) {
        return false;
    }
    asset->type = e->type;
    asset->data = (const uint8_t *)p + e->offset;
    asset->length = e->length;
    asset->etag = e->etag;
    asset->gzip = (e->flags & PACK_FLAG_GZIP) != 0;
    return true;
}
