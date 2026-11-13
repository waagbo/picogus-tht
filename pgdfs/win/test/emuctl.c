/*
 * The setup registers of the emulated PicoGUS, see emuctl.h.
 *
 * Copyright (C) 2026 PicoGUS contributors. MIT license, see ../../LICENSE.
 */
#include <string.h>
#define PICOGUS_NO_MODENAMES
#include "picogus.h"
#include "emuctl.h"

emuctl_t emuctl;

enum { CD_NONE, CD_LIST, CD_LOAD_INDEX, CD_LOAD };

static uint32_t cur_read, cur_write;
static uint8_t  port_low;
static int      cur_read_idx;

/* cdrom_t */
static int8_t   image_status;
static int      image_command, image_data, command_age;
static char     image_path[256];
static char     error_str[256];
static bool     have_list;          /* image_list allocated */
static int      image_count;
static uint8_t  current_index;

/* WiFi */
static const char *status_read;
static uint32_t status_idx;
static int      status_busy;

void emuctl_reset(void) {
    memset(&emuctl, 0, sizeof(emuctl));
    emuctl.fw = "PicoGUS v4.2.0+pgdfs";
    emuctl.mode = SB_MODE;
    emuctl.boot_request = SB_MODE;
    emuctl.has_cd = true;
    emuctl.cd_port = 0x250;
    emuctl.drive = true;
    emuctl.wifi_status = "";
    cur_read = cur_write = 0;
    cur_read_idx = 0;
    image_status = CD_STATUS_IDLE;
    image_command = CD_NONE;
    image_path[0] = error_str[0] = 0;
    have_list = false;
    image_count = 0;
    current_index = 0;
    status_read = NULL;
}

bool emuctl_select(uint8_t reg) {
    switch (reg) {
    case CMD_FWSTRING:
        cur_read = 0;
        return true;
    case CMD_BOOTMODE: case CMD_CDSTATUS: case CMD_CDLOAD: case CMD_CDAUTOADV:
    case CMD_WIFIAPPLY: case CMD_WIFISTAT: case CMD_SAVE: case CMD_REBOOT: case CMD_DEFAULTS:
    case CMD_HWTYPE: case CMD_CHIPTYPE: case CMD_MAINVOL:
        return true;
    case CMD_CDPORT:
        port_low = 0;
        return true;
    case CMD_WIFISSID:
        memset(emuctl.ssid, 0, sizeof(emuctl.ssid));
        cur_write = 0;
        return true;
    case CMD_WIFIPASS:
        memset(emuctl.pass, 0, sizeof(emuctl.pass));
        cur_write = 0;
        return true;
    case CMD_CDLIST:
        if (emuctl.has_cd) {
            if (image_status == CD_STATUS_IDLE || image_status == CD_STATUS_ERROR) {
                image_status = CD_STATUS_BUSY;
                image_command = CD_LIST;
                command_age = 0;
            }
            cur_read = 0;
            cur_read_idx = 0;
        }
        return true;
    case CMD_CDNAME:
        cur_write = 0;
        cur_read = 0;
        return true;
    case CMD_CDERROR:
        cur_read = 0;
        return true;
    default:
        return false;
    }
}

void emuctl_write_low(uint8_t reg, uint8_t v) {
    if (reg == CMD_CDPORT) port_low = v;
}

void emuctl_write_high(uint8_t reg, uint8_t v) {
    switch (reg) {
    case CMD_BOOTMODE:
        emuctl.boot_request = v;
        break;
    case CMD_SAVE:
        emuctl.saves++;
        break;
    case CMD_REBOOT:
        emuctl.reboots++;
        emuctl.mode = emuctl.boot_request;
        emuctl.has_cd = emuctl.mode == SB_MODE || emuctl.mode == USB_MODE;
        image_status = CD_STATUS_IDLE;
        image_command = CD_NONE;
        have_list = false;
        current_index = 0;
        image_path[0] = 0;
        if (emuctl.on_reboot) emuctl.on_reboot();
        break;
    case CMD_WIFISSID:
        if (cur_write < sizeof(emuctl.ssid)) emuctl.ssid[cur_write++] = (char)v;
        break;
    case CMD_WIFIPASS:
        if (cur_write < sizeof(emuctl.pass)) emuctl.pass[cur_write++] = (char)v;
        break;
    case CMD_WIFIAPPLY:
        emuctl.applies++;
        break;
    case CMD_WIFISTAT:
        if (emuctl.has_wifi) {
            status_read = emuctl.wifi_status;
            status_idx = 0;
            status_busy = emuctl.wifi_busy_reads;
        }
        break;
    case CMD_CDPORT:
        emuctl.cd_port = (v || port_low) ? (uint16_t)((v << 8) | port_low) : 0xFFFF;
        break;
    case CMD_CDLOAD:
        if (!emuctl.has_cd) break;
        image_data = v;
        image_status = CD_STATUS_BUSY;
        image_command = CD_LOAD_INDEX;
        command_age = 0;
        break;
    case CMD_CDNAME:
        if (!emuctl.has_cd) break;
        if (!cur_write) memset(image_path, 0, sizeof(image_path));
        if (cur_write < sizeof(image_path)) image_path[cur_write++] = (char)v;
        if (!v) {
            cur_write = 0;
            image_status = CD_STATUS_BUSY;
            image_command = CD_LOAD;
            command_age = 0;
        }
        break;
    case CMD_CDAUTOADV:
        emuctl.autoadvance = v;
        break;
    default:
        break;
    }
}

uint8_t emuctl_read_low(uint8_t reg) {
    if (reg == CMD_CDPORT) return emuctl.cd_port == 0xFFFF ? 0 : (uint8_t)(emuctl.cd_port & 0xFF);
    return 0;
}

static uint8_t string_read(const char *s) {
    uint8_t ret = (uint8_t)s[cur_read++];
    if (ret == 0) cur_read = 0;
    return ret;
}

uint8_t emuctl_read_high(uint8_t reg) {
    uint8_t ret;
    switch (reg) {
    case CMD_MAGIC:     return 0xDD;
    case CMD_PROTOCOL:  return PICOGUS_PROTOCOL_VER;
    case CMD_FWSTRING:  return string_read(emuctl.fw);
    case CMD_BOOTMODE:  return emuctl.boot_request;     /* settings.startupMode */
    case CMD_WIFISTAT:
        if (!status_read) return 0;
        if (status_busy > 0) {
            status_busy--;
            return 0xFF;
        }
        ret = (uint8_t)status_read[status_idx];
        if (ret == 0) status_idx = 0;
        else status_idx++;
        return ret;
    case CMD_CDPORT:    return emuctl.cd_port == 0xFFFF ? 0 : (uint8_t)(emuctl.cd_port >> 8);
    case CMD_CDAUTOADV: return emuctl.autoadvance;
    case CMD_HWTYPE:    return 0;
    case CMD_CHIPTYPE:  return 0;
    default:
        break;
    }
    if (!emuctl.has_cd) return 0xFF;
    switch (reg) {
    case CMD_CDSTATUS:
        return (uint8_t)image_status;
    case CMD_CDLIST:
        if (!have_list) {
            emuctl.list_overruns++;     /* the firmware reads freed memory here */
            return 0xEE;
        }
        if (cur_read_idx == image_count) {
            cur_read_idx = 0;
            cur_read = 0;
            image_status = CD_STATUS_IDLE;
            have_list = false;
            return 0x04;
        }
        ret = (uint8_t)emuctl.image[cur_read_idx][cur_read++];
        if (ret == 0) {
            ++cur_read_idx;
            cur_read = 0;
        }
        return ret;
    case CMD_CDLOAD:
        return current_index;
    case CMD_CDNAME:
        return string_read(image_path);
    case CMD_CDERROR:
        return string_read(error_str);
    default:
        return 0xFF;
    }
}

static bool list_images(void) {
    emuctl.list_scans++;
    if (!emuctl.drive) {
        strcpy(error_str, "No USB disk or error mounting it");
        return false;
    }
    if (!emuctl.images) {
        strcpy(error_str, "No image files on USB disk");
        return false;
    }
    return true;
}

void emuctl_task(void) {
    if (image_command == CD_NONE) return;
    if (command_age++ < emuctl.busy_polls) return;
    switch (image_command) {
    case CD_LIST:
        error_str[0] = 0;
        have_list = list_images();
        image_count = emuctl.images;
        image_command = CD_NONE;
        image_status = have_list ? CD_STATUS_READY : CD_STATUS_ERROR;
        break;
    case CD_LOAD_INDEX:
        if (image_data == 0) {
            image_path[0] = 0;
            image_command = CD_LOAD;
            current_index = 0;
        } else {
            if (!list_images()) {
                image_command = CD_NONE;
                image_status = CD_STATUS_ERROR;
                return;
            }
            if (image_data > emuctl.images) image_data = 1;
            strcpy(image_path, emuctl.image[image_data - 1]);
            image_command = CD_LOAD;
            current_index = (uint8_t)image_data;
        }
        /* cdrom_tasks() serves CD_COMMAND_IMAGE_LOAD on its next round */
        command_age = emuctl.busy_polls;
        break;
    case CD_LOAD:
        error_str[0] = 0;
        if (image_path[0]) {
            if (emuctl.bad_image && strcmp(image_path, emuctl.bad_image) == 0) {
                strcpy(error_str, "Error loading iso '");
                strncat(error_str, image_path, 200);
                strcat(error_str, "'");
                image_command = CD_NONE;
                image_status = CD_STATUS_ERROR;
                current_index = 0;
                break;
            }
            current_index = 0;
            for (int i = 0; i < emuctl.images; i++) {
                if (strcmp(image_path, emuctl.image[i]) == 0) current_index = (uint8_t)(i + 1);
            }
        }
        image_command = CD_NONE;
        image_status = CD_STATUS_IDLE;
        break;
    }
}
