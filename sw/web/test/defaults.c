// Host test stand-in for getDefaultSettings() in system/flash_settings.c,
// which needs too much of the Pico SDK to build on the host.
#include <string.h>
#include "system/flash_settings.h"

void getDefaultSettings(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->magic = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;
    s->startupMode = 1;
    s->Joy.basePort = 0xffff;
    s->GUS.basePort = 0x240;
    s->GUS.audioBuffer = 4;
    s->SB.basePort = 0x220;
    s->SB.oplBasePort = 0x388;
    s->MPU.basePort = 0x330;
    s->CMS.basePort = 0x220;
    s->Tandy.basePort = 0x2c0;
    s->Mouse.basePort = 0xffff;
    s->Mouse.reportRate = 60;
    s->Mouse.sensitivity = 0x100;
    s->CD.basePort = 0x250;
    s->Volume.mainVol = 100;
    s->Volume.sbVol = 90;
    s->SB16.irq = 5;
    s->SB16.dma = 1;
    s->SB16.sbType = 6;
}
