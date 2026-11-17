/* PGTRAY resource identifiers. MIT license, see ../../LICENSE. */
#ifndef PGTRAY_RESOURCE_H
#define PGTRAY_RESOURCE_H

#define IDI_CD              1
#define IDI_EMPTY           2

#define IDD_MAIN            100
#define IDD_CD              101
#define IDD_MIDI            102
#define IDD_CARD            103
#define IDD_WEB             104

#define IDC_TAB             1000
#define IDC_STATUS          1001
#define IDC_AUTOSTART       1002
#define IDC_HIDE            1003
#define IDC_EXIT            1004

#define IDC_CD_CURRENT      1100
#define IDC_CD_LIST         1101
#define IDC_CD_LOAD         1102
#define IDC_CD_EJECT        1103
#define IDC_CD_REFRESH      1104
#define IDC_CD_INFO         1105

#define IDC_MIDI_DEV        1200
#define IDC_MIDI_MT32       1201
#define IDC_MIDI_SF         1202
#define IDC_MIDI_ROM_OLD    1203
#define IDC_MIDI_ROM_NEW    1204
#define IDC_MIDI_ROM_CM32L  1205
#define IDC_MIDI_SFNUM      1206
#define IDC_MIDI_SFSET      1207

#define IDC_CARD_FW         1300
#define IDC_CARD_ACCESS     1301
#define IDC_CARD_MODE       1302
#define IDC_CARD_SWITCH     1303
#define IDC_CARD_SAVE       1304

#define IDC_WEB_STATUS      1400
#define IDC_WEB_REFRESH     1401
#define IDC_WEB_OPEN        1402
#define IDC_WEB_SSID        1403
#define IDC_WEB_PASS        1404
#define IDC_WEB_SAVE        1405
#define IDC_WEB_CONNECT     1406

/* tray menu */
#define IDM_OPEN            2000
#define IDM_EXIT            2001
#define IDM_MODE            2100    /* + card_mode_t */
#define IDM_CD              3000    /* + image index, 0 = unload */

#endif
