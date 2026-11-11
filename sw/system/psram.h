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

// GUS sample RAM: PSRAM on PIO SPI (rp2040-psram, PicoGUS 1.x/2.0) or on the
// RP2350's QMI (PSRAM_QMI, PicoGUS v2.1 THT with a Core2350B2). Both provide
// the same psram_spi_inst_t / psram_read* / psram_write* API.

#pragma once

#if PSRAM_QMI
#include "system/psram_qmi.h"
#else
#include "psram_spi.h"
#endif
