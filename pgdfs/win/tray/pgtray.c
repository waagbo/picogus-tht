/*
 * PGTRAY - the PicoGUS in the notification area of Windows
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see ../../LICENSE.
 *
 * Loads and unloads the CD image of the card's CD-ROM emulation, switches an
 * mt32-pi on the MIDI output between its synthesizers, switches the card
 * mode and sets up the WiFi network of the firmware with the web server. An
 * icon in the notification area with a menu, and a window that hides there.
 *
 * One program for Windows 95 / 98 / ME and Windows NT 4.0 / 2000 / XP: plain
 * Win32 as Windows 95 has it, ANSI, no C runtime. The card part is pgcard.c,
 * the way to the card backend.c.
 */

#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0400
#define _WIN32_WINNT 0x0400
#define _WIN32_IE 0x0300
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmsystem.h>
#define PICOGUS_NO_MODENAMES
#include "../../../common/picogus.h"
#include "resource.h"
#include "pgcard.h"
#include "backend.h"

#define APP_TITLE       "PicoGUS"
#define WND_CLASS       "PGTrayWnd"
#define WM_TRAY         (WM_APP + 1)
#define WM_SHOWME       (WM_APP + 2)
#define TIMER_REFRESH   1
#define TIMER_TRAY      2
#define REFRESH_MS      3000
#define TRAY_RETRY_MS   2000    /* no taskbar yet: the icon is offered again */
#define HOVER_MS        2000    /* the tip is refreshed this often while the mouse is on the icon */
#define MENU_IMAGES     30      /* CD images in the menu; the window lists all */
#define LIST_BYTES      32768
#define REG_APP         "Software\\PicoGUS\\PGTray"
#define REG_RUN         "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE       "PGTray"

enum { PAGE_CD, PAGE_MIDI, PAGE_CARD, PAGE_WEB, PAGES };

/* Windows 95 and NT 4.0 know the notification icon in its first layout only:
 * 88 bytes, a tip of 64 */
#define NID_SIZE        NOTIFYICONDATAA_V1_SIZE
#define NID_TIP         64
typedef char nid_is_v1[NID_SIZE == 88 ? 1 : -1];

static HINSTANCE inst;
static HWND wnd, tab, page[PAGES];
static HICON icon_cd, icon_empty;
static UINT wm_taskbar;
static int tray_added;
static DWORD hover_tick;

/* the card as last seen */
static struct {
  int result;                   /* PGCARD_* of the last look */
  pgcard_info info;
  unsigned char cd_index;
  char cd_name[PGCARD_TEXT];    /* ANSI */
  char list[LIST_BYTES];        /* the card's names, OEM */
  unsigned count;
  int list_result;
  char list_error[PGCARD_TEXT];
  char url[128];
} card;

static const unsigned char modes[] = { GUS_MODE, SB_MODE, ADLIB_MODE, MPU_MODE, PSG_MODE, USB_MODE };

/* gcc may call these for structure copies even without a C library */
void *memset(void *dst, int v, size_t n) {
  unsigned char *d = dst;
  while (n--) *d++ = (unsigned char)v;
  return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  while (n--) *d++ = *s++;
  return dst;
}

/* ---- small things ------------------------------------------------------------------ */

static void set_text(HWND dlg, int id, const char *text) {
  char now[512];
  now[0] = 0;
  GetDlgItemTextA(dlg, id, now, sizeof(now));
  if (lstrcmpA(now, text) != 0) SetDlgItemTextA(dlg, id, text);
}

static void status(const char *text) {
  set_text(wnd, IDC_STATUS, text);
}

/* something went wrong: in the window if it shows, else in a box */
static void complain(const char *text) {
  status(text);
  if (!IsWindowVisible(wnd)) MessageBoxA(NULL, text, APP_TITLE, MB_OK | MB_ICONEXCLAMATION | MB_SETFOREGROUND);
}

static HCURSOR busy(void) {
  return SetCursor(LoadCursorA(NULL, IDC_WAIT));
}

static void oem_to_ansi(const char *oem, char *ansi, unsigned max) {
  char tmp[PGCARD_TEXT];
  lstrcpynA(tmp, oem, sizeof(tmp));
  OemToCharA(tmp, tmp);
  lstrcpynA(ansi, tmp, max);
}

/* what to tell about a failed card request; text is the card's own (OEM) */
static void explain(int r, const char *what, const char *text, char *out) {
  char ansi[PGCARD_TEXT];
  switch (r) {
  case PGCARD_EIO:
    if (backend_kind() == BACKEND_NONE) {
      lstrcpyA(out, "The PGDFS driver (pgdfs.sys) is not running: under Windows NT, 2000 and XP it is the way to the card.");
    } else {
      lstrcpyA(out, "The PGDFS driver refused the request.");
    }
    break;
  case PGCARD_ENOCARD:
    lstrcpyA(out, "No PicoGUS answers on port 1D0h.");
    break;
  case PGCARD_ETIMEOUT:
    wsprintfA(out, "%s: the card did not answer in time.", what);
    break;
  case PGCARD_ECARD:
    oem_to_ansi(text, ansi, 200);
    wsprintfA(out, "%s: %s", what, ansi[0] ? ansi : "the card reported an error.");
    break;
  case PGCARD_ENOTSUP:
    wsprintfA(out, "%s: not in this firmware or card mode.", what);
    break;
  default:
    wsprintfA(out, "%s: not possible.", what);
    break;
  }
}

/* ---- the notification area ------------------------------------------------------------ */

/* shows the icon for the state of the card, or takes it away */
static void tray_set(int remove) {
  NOTIFYICONDATAA nid;
  char tip[160];
  int ok;
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = NID_SIZE;
  nid.hWnd = wnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = card.cd_index ? icon_cd : icon_empty;
  if (card.result != PGCARD_OK) {
    lstrcpyA(tip, backend_kind() == BACKEND_NONE ? "PicoGUS: no PGDFS driver" : "PicoGUS: no card found");
  } else if (!card.info.has_cd) {
    wsprintfA(tip, "PicoGUS: %s mode", pgcard_mode_name(card.info.mode));
  } else if (card.cd_index) {
    wsprintfA(tip, "PicoGUS CD: %.100s", card.cd_name);
  } else {
    lstrcpyA(tip, "PicoGUS CD: no image loaded");
  }
  lstrcpynA(nid.szTip, tip, NID_TIP);
  if (remove) {
    Shell_NotifyIconA(NIM_DELETE, &nid);
    tray_added = 0;
    return;
  }
  /* an icon the taskbar does not have (not yet, or no more: Explorer started
   * again) is added */
  ok = tray_added && Shell_NotifyIconA(NIM_MODIFY, &nid);
  if (!ok) ok = Shell_NotifyIconA(NIM_ADD, &nid) || Shell_NotifyIconA(NIM_MODIFY, &nid);
  tray_added = ok;
  if (ok) KillTimer(wnd, TIMER_TRAY);
  else SetTimer(wnd, TIMER_TRAY, TRAY_RETRY_MS, NULL);
}

/* ---- the card ------------------------------------------------------------------------- */

static void update_pages(void);

/* looks at the card: mode, firmware, loaded CD image */
static void look(void) {
  char name[PGCARD_TEXT];
  card.cd_index = 0;
  card.cd_name[0] = 0;
  card.result = pgcard_get_info(&card.info);
  if (card.result == PGCARD_OK && card.info.has_cd) {
    if (pgcard_cd_current(&card.cd_index, name, sizeof(name)) == PGCARD_OK) {
      oem_to_ansi(name, card.cd_name, sizeof(card.cd_name));
    } else {
      card.cd_index = 0;
    }
  }
  tray_set(0);
  update_pages();
}

/* the n-th name (0..) of the card's list, ANSI */
static void image_name(unsigned n, char *out, unsigned max) {
  const char *p = card.list;
  while (n--) p += lstrlenA(p) + 1;
  oem_to_ansi(p, out, max);
}

static void fill_list(void) {
  HWND lb = GetDlgItem(page[PAGE_CD], IDC_CD_LIST);
  char name[PGCARD_TEXT], line[PGCARD_TEXT + 16];
  unsigned i;
  SendMessageA(lb, WM_SETREDRAW, FALSE, 0);
  SendMessageA(lb, LB_RESETCONTENT, 0, 0);
  for (i = 0; i < card.count; i++) {
    image_name(i, name, sizeof(name));
    wsprintfA(line, "%u\t%s", i + 1, name);
    SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)line);
  }
  if (card.cd_index && card.cd_index <= card.count) SendMessageA(lb, LB_SETCURSEL, card.cd_index - 1, 0);
  SendMessageA(lb, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(lb, NULL, TRUE);
}

/* asks the card for its CD images */
static void list_images(void) {
  HCURSOR old = busy();
  card.count = 0;
  card.list_error[0] = 0;
  if (card.result != PGCARD_OK) {
    card.list_result = card.result;
  } else {
    card.list_result = pgcard_cd_list(card.list, sizeof(card.list), &card.count, card.list_error,
                                      sizeof(card.list_error));
  }
  SetCursor(old);
  fill_list();
}

static void load_image(unsigned index) {
  char err[PGCARD_TEXT], text[2 * PGCARD_TEXT];
  HCURSOR old = busy();
  int r = pgcard_cd_load((unsigned char)index, err, sizeof(err));
  look();
  SetCursor(old);
  if (r != PGCARD_OK) {
    explain(r, index ? "Loading the CD image" : "Unloading the CD image", err, text);
    complain(text);
  } else if (card.cd_index) {
    wsprintfA(text, "Loaded %.200s.", card.cd_name);
    status(text);
  } else {
    status("No CD image loaded.");
  }
  if (card.cd_index && card.cd_index <= card.count) {
    SendDlgItemMessageA(page[PAGE_CD], IDC_CD_LIST, LB_SETCURSEL, card.cd_index - 1, 0);
  } else {
    SendDlgItemMessageA(page[PAGE_CD], IDC_CD_LIST, LB_SETCURSEL, (WPARAM)-1, 0);
  }
}

static void switch_mode(unsigned char mode, int save) {
  char text[400];
  unsigned char now = 0;
  HCURSOR old;
  int r;
  wsprintfA(text,
            "Restart the card in %s mode?\n\nIt becomes a different device: a Windows driver loaded for "
            "the present mode has no card until you switch back.%s",
            pgcard_mode_name(mode), save ? "\n\nThe mode is saved: the card starts in it from now on." : "");
  if (MessageBoxA(IsWindowVisible(wnd) ? wnd : NULL, text, APP_TITLE, MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND) != IDOK) {
    return;
  }
  old = busy();
  status("Restarting the card...");
  UpdateWindow(wnd);
  r = pgcard_set_mode(mode, save, &now);
  look();
  SetCursor(old);
  if (r != PGCARD_OK) {
    explain(r, "Switching the card mode", "", text);
    complain(text);
  } else if (now != mode) {
    wsprintfA(text, "The card restarted in %s mode: this firmware has no %s mode.", pgcard_mode_name(now),
              pgcard_mode_name(mode));
    complain(text);
  } else {
    wsprintfA(text, "The card is in %s mode.", pgcard_mode_name(now));
    status(text);
  }
  if (card.result == PGCARD_OK && card.info.has_cd) {
    list_images();
  } else {
    card.count = 0;
    fill_list();
  }
}

/* ---- MIDI: an mt32-pi on the card's MIDI output ---------------------------------------- */

static void reg_get(const char *name, char *value, DWORD max) {
  HKEY key;
  DWORD type = 0, size = max;
  value[0] = 0;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_APP, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
  if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)value, &size) != ERROR_SUCCESS || type != REG_SZ) value[0] = 0;
  value[max - 1] = 0;
  RegCloseKey(key);
}

static void reg_set(const char *name, const char *value) {
  HKEY key;
  DWORD disp;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_APP, 0, NULL, 0, KEY_WRITE, NULL, &key, &disp) != ERROR_SUCCESS) return;
  RegSetValueExA(key, name, 0, REG_SZ, (const BYTE *)value, lstrlenA(value) + 1);
  RegCloseKey(key);
}

static int contains(const char *s, const char *word) {
  int n = lstrlenA(word), i;
  for (; *s; s++) {
    for (i = 0; i < n && s[i]; i++) {
      char a = s[i], b = word[i];
      if (a >= 'a' && a <= 'z') a = (char)(a - 32);
      if (a != b) break;
    }
    if (i == n) return 1;
  }
  return 0;
}

/* the MIDI outputs of Windows; the one used last, else the first that looks
 * like an MPU-401 */
static void fill_midi(void) {
  HWND cb = GetDlgItem(page[PAGE_MIDI], IDC_MIDI_DEV);
  MIDIOUTCAPSA caps;
  char saved[MAXPNAMELEN + 1];
  UINT n = midiOutGetNumDevs(), i;
  int pick = -1, guess = -1;
  reg_get("MidiOut", saved, sizeof(saved));
  SendMessageA(cb, CB_RESETCONTENT, 0, 0);
  for (i = 0; i < n; i++) {
    if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
    SendMessageA(cb, CB_SETITEMDATA, SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)caps.szPname), i);
    if (saved[0] && lstrcmpA(saved, caps.szPname) == 0) pick = (int)i;
    if (guess < 0 && caps.wTechnology == MOD_MIDIPORT &&
        (contains(caps.szPname, "MPU") || contains(caps.szPname, "MIDI OUT") || contains(caps.szPname, "EXTERNAL"))) {
      guess = (int)i;
    }
  }
  if (pick < 0) pick = guess;
  if (pick < 0 && n) pick = 0;
  n = (UINT)SendMessageA(cb, CB_GETCOUNT, 0, 0);
  for (i = 0; i < n; i++) {
    if ((int)SendMessageA(cb, CB_GETITEMDATA, i, 0) == pick) SendMessageA(cb, CB_SETCURSEL, i, 0);
  }
}

static void midi_send(const unsigned char *msg, unsigned len, const char *done) {
  static char data[32];
  HWND cb = GetDlgItem(page[PAGE_MIDI], IDC_MIDI_DEV);
  char text[300], name[MAXPNAMELEN + 1];
  HMIDIOUT out;
  MIDIHDR hdr;
  MMRESULT mr;
  DWORD start;
  int sel = (int)SendMessageA(cb, CB_GETCURSEL, 0, 0);
  unsigned i;

  if (sel < 0) {
    complain("Windows has no MIDI output. The mt32-pi is reached through the MIDI driver of the card's MPU-401.");
    return;
  }
  name[0] = 0;
  SendMessageA(cb, CB_GETLBTEXT, sel, (LPARAM)name);
  mr = midiOutOpen(&out, (UINT)SendMessageA(cb, CB_GETITEMDATA, sel, 0), 0, 0, CALLBACK_NULL);
  if (mr != MMSYSERR_NOERROR) {
    wsprintfA(text, "%.40s: ", name);
    if (mr == MMSYSERR_ALLOCATED) {
      lstrcatA(text, "the MIDI output is in use by another program.");
    } else {
      midiOutGetErrorTextA(mr, text + lstrlenA(text), 200);
    }
    complain(text);
    return;
  }
  for (i = 0; i < len && i < sizeof(data); i++) data[i] = (char)msg[i];
  memset(&hdr, 0, sizeof(hdr));
  hdr.lpData = data;
  hdr.dwBufferLength = len;
  hdr.dwBytesRecorded = len;
  mr = midiOutPrepareHeader(out, &hdr, sizeof(hdr));
  if (mr == MMSYSERR_NOERROR) {
    mr = midiOutLongMsg(out, &hdr, sizeof(hdr));
    if (mr == MMSYSERR_NOERROR) {
      start = GetTickCount();
      while (!(hdr.dwFlags & MHDR_DONE) && GetTickCount() - start < 2000) Sleep(5);
      if (!(hdr.dwFlags & MHDR_DONE)) {
        midiOutReset(out);
        mr = MMSYSERR_ERROR;
      }
    }
    midiOutUnprepareHeader(out, &hdr, sizeof(hdr));
  }
  midiOutClose(out);
  if (mr != MMSYSERR_NOERROR) {
    wsprintfA(text, "%.40s: the message could not be sent.", name);
    complain(text);
  } else {
    reg_set("MidiOut", name);
    wsprintfA(text, "%s (sent to %.40s)", done, name);
    status(text);
  }
}

/* mt32-pi's own messages: F0 7D <command> [<value>] F7 */
static void mt32pi(unsigned char command, unsigned char value, const char *done) {
  unsigned char msg[5];
  msg[0] = 0xF0;
  msg[1] = 0x7D;
  msg[2] = command;
  msg[3] = value;
  msg[4] = 0xF7;
  midi_send(msg, sizeof(msg), done);
}

/* ---- WiFi ----------------------------------------------------------------------------- */

static void wifi_look(void) {
  char line[PGCARD_TEXT], text[PGCARD_TEXT + 64];
  HCURSOR old = busy();
  int r = pgcard_wifi_status(line, sizeof(line));
  SetCursor(old);
  card.url[0] = 0;
  if (r == PGCARD_OK) {
    lstrcpynA(text, line, sizeof(text));
    pgcard_wifi_url(line, card.url, sizeof(card.url));
  } else if (r == PGCARD_ENOTSUP) {
    lstrcpyA(text, "This firmware has no WiFi. The web server is in the firmware for Pico W boards "
                   "(picogusw.uf2, pg2350w.uf2).");
  } else {
    explain(r, "WiFi status", "", text);
  }
  set_text(page[PAGE_WEB], IDC_WEB_STATUS, text);
  EnableWindow(GetDlgItem(page[PAGE_WEB], IDC_WEB_OPEN), card.url[0] != 0);
  EnableWindow(GetDlgItem(page[PAGE_WEB], IDC_WEB_CONNECT), r != PGCARD_ENOTSUP);
  /* the network it is on, as a start for a change */
  if (r == PGCARD_OK && GetWindowTextLengthA(GetDlgItem(page[PAGE_WEB], IDC_WEB_SSID)) == 0) {
    static const char key[] = "SSID ";
    const char *p;
    for (p = line; *p; p++) {
      int i;
      for (i = 0; key[i] && p[i] == key[i]; i++) {}
      if (!key[i]) {
        char ssid[PGCARD_SSID_MAX + 1];
        int n = 0;
        for (p += i; *p && *p != ',' && *p != '.' && n < PGCARD_SSID_MAX; p++) ssid[n++] = *p;
        ssid[n] = 0;
        SetDlgItemTextA(page[PAGE_WEB], IDC_WEB_SSID, ssid);
        break;
      }
    }
  }
}

static void wifi_connect(void) {
  char ssid[PGCARD_SSID_MAX + 8], pass[PGCARD_PASS_MAX + 8], text[300];
  HCURSOR old;
  int save = IsDlgButtonChecked(page[PAGE_WEB], IDC_WEB_SAVE) == BST_CHECKED;
  int r;
  GetDlgItemTextA(page[PAGE_WEB], IDC_WEB_SSID, ssid, sizeof(ssid));
  GetDlgItemTextA(page[PAGE_WEB], IDC_WEB_PASS, pass, sizeof(pass));
  if (!ssid[0] || lstrlenA(ssid) > PGCARD_SSID_MAX) {
    complain("The network name (SSID) is 1 to 32 characters long.");
    return;
  }
  if (lstrlenA(pass) > PGCARD_PASS_MAX) {
    complain("The password is at most 63 characters long.");
    return;
  }
  old = busy();
  r = pgcard_wifi_set(ssid, pass, save);
  SetCursor(old);
  if (r != PGCARD_OK) {
    explain(r, "Setting the WiFi network", "", text);
    complain(text);
    return;
  }
  wsprintfA(text, "The card connects to %s%s. Refresh shows how it went.", ssid,
            save ? " and keeps the network" : "");
  status(text);
  set_text(page[PAGE_WEB], IDC_WEB_STATUS, "Connecting...");
  EnableWindow(GetDlgItem(page[PAGE_WEB], IDC_WEB_OPEN), FALSE);
}

/* ---- the window ----------------------------------------------------------------------- */

static void update_pages(void) {
  char text[PGCARD_TEXT + 100];
  HWND cd = page[PAGE_CD], cp = page[PAGE_CARD], cb;
  int ok = card.result == PGCARD_OK, has_cd = ok && card.info.has_cd, i, n;
  if (!cd) return;

  /* CD-ROM */
  set_text(cd, IDC_CD_CURRENT, !has_cd ? "" : card.cd_index ? card.cd_name : "(no image)");
  if (!ok) {
    explain(card.result, "The card", "", text);
  } else if (!has_cd) {
    wsprintfA(text, "The card is in %s mode. Its CD-ROM emulation is part of Sound Blaster and USB mode.",
              pgcard_mode_name(card.info.mode));
  } else if (!card.info.cd_port) {
    lstrcpyA(text, "The CD-ROM emulation is switched off: PGUSINIT /cdport gives it its port.");
  } else if (card.list_result == PGCARD_ECARD && card.list_error[0]) {
    oem_to_ansi(card.list_error, text, sizeof(text));
  } else {
    wsprintfA(text, "CD-ROM drive at port %Xh, %u image%s on the USB drive.", card.info.cd_port, card.count,
              card.count == 1 ? "" : "s");
  }
  set_text(cd, IDC_CD_INFO, text);
  EnableWindow(GetDlgItem(cd, IDC_CD_LIST), has_cd);
  EnableWindow(GetDlgItem(cd, IDC_CD_LOAD), has_cd);
  EnableWindow(GetDlgItem(cd, IDC_CD_EJECT), has_cd && card.cd_index != 0);

  /* Card */
  set_text(cp, IDC_CARD_FW, ok ? card.info.fw : "(no card)");
  wsprintfA(text, "%s", backend_name());
  set_text(cp, IDC_CARD_ACCESS, text);
  cb = GetDlgItem(cp, IDC_CARD_MODE);
  if (ok && !SendMessageA(cb, CB_GETDROPPEDSTATE, 0, 0) && GetFocus() != cb) {
    n = (int)SendMessageA(cb, CB_GETCOUNT, 0, 0);
    for (i = 0; i < n; i++) {
      if ((unsigned char)SendMessageA(cb, CB_GETITEMDATA, i, 0) == card.info.mode &&
          (int)SendMessageA(cb, CB_GETCURSEL, 0, 0) != i) {
        SendMessageA(cb, CB_SETCURSEL, i, 0);
      }
    }
  }
  EnableWindow(cb, ok);
  EnableWindow(GetDlgItem(cp, IDC_CARD_SWITCH), ok);
}

static void select_page(int which) {
  int i;
  for (i = 0; i < PAGES; i++) ShowWindow(page[i], i == which ? SW_SHOW : SW_HIDE);
  if (which == PAGE_CD && card.result == PGCARD_OK && card.info.has_cd) {
    list_images();
    update_pages();
  } else if (which == PAGE_MIDI) {
    fill_midi();
  } else if (which == PAGE_WEB) {
    wifi_look();
  }
}

static int autostart_get(void) {
  HKEY key;
  DWORD type, size = 0;
  int on = 0;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
  on = RegQueryValueExA(key, RUN_VALUE, NULL, &type, NULL, &size) == ERROR_SUCCESS;
  RegCloseKey(key);
  return on;
}

static void autostart_set(int on) {
  char path[MAX_PATH], cmd[MAX_PATH + 16];
  HKEY key;
  DWORD disp;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_RUN, 0, NULL, 0, KEY_WRITE, NULL, &key, &disp) != ERROR_SUCCESS) return;
  if (on && GetModuleFileNameA(NULL, path, sizeof(path))) {
    wsprintfA(cmd, "\"%s\" /tray", path);
    RegSetValueExA(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, lstrlenA(cmd) + 1);
  } else {
    RegDeleteValueA(key, RUN_VALUE);
  }
  RegCloseKey(key);
}

static void show_window(void) {
  ShowWindow(wnd, IsIconic(wnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(wnd);
  SetTimer(wnd, TIMER_REFRESH, REFRESH_MS, NULL);
  look();
  select_page((int)SendMessageA(tab, TCM_GETCURSEL, 0, 0));
}

static void hide_window(void) {
  KillTimer(wnd, TIMER_REFRESH);
  ShowWindow(wnd, SW_HIDE);
}

/* ---- the menu of the icon -------------------------------------------------------------- */

static void tray_menu(void) {
  HMENU menu = CreatePopupMenu(), cd = CreatePopupMenu(), mode = CreatePopupMenu(), midi = CreatePopupMenu();
  char name[PGCARD_TEXT], line[PGCARD_TEXT + 16];
  POINT pt;
  unsigned i;
  int ok, has_cd;

  look();
  ok = card.result == PGCARD_OK;
  has_cd = ok && card.info.has_cd;
  if (has_cd) list_images();

  AppendMenuA(cd, MF_STRING | (card.cd_index ? 0 : MF_CHECKED), IDM_CD, "&No image (eject)");
  if (has_cd && card.count) AppendMenuA(cd, MF_SEPARATOR, 0, NULL);
  for (i = 0; has_cd && i < card.count && i < MENU_IMAGES; i++) {
    unsigned n;
    image_name(i, name, 200);
    /* an ampersand in a name is not a shortcut */
    for (n = 0, line[0] = 0; name[n] && lstrlenA(line) < (int)sizeof(line) - 3; n++) {
      int at = lstrlenA(line);
      line[at] = name[n];
      line[at + 1] = name[n] == '&' ? '&' : 0;
      line[at + 2] = 0;
    }
    AppendMenuA(cd, MF_STRING | (card.cd_index == i + 1 ? MF_CHECKED : 0), IDM_CD + i + 1, line);
  }
  if (has_cd && card.count > MENU_IMAGES) {
    AppendMenuA(cd, MF_SEPARATOR, 0, NULL);
    AppendMenuA(cd, MF_STRING, IDM_OPEN, "&More in the window...");
  }
  if (has_cd && !card.count) {
    oem_to_ansi(card.list_error[0] ? card.list_error : "No images", name, 200);
    AppendMenuA(cd, MF_STRING | MF_GRAYED, 0, name);
  }

  for (i = 0; i < sizeof(modes); i++) {
    AppendMenuA(mode, MF_STRING | (ok && card.info.mode == modes[i] ? MF_CHECKED : 0), IDM_MODE + modes[i],
                pgcard_mode_name(modes[i]));
  }
  AppendMenuA(midi, MF_STRING, IDC_MIDI_MT32, "&MT-32");
  AppendMenuA(midi, MF_STRING, IDC_MIDI_SF, "&SoundFont (General MIDI)");

  AppendMenuA(menu, MF_STRING, IDM_OPEN, "&Open PicoGUS");
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_POPUP | (has_cd ? 0 : MF_GRAYED), (UINT_PTR)cd, "&CD image");
  AppendMenuA(menu, MF_POPUP, (UINT_PTR)midi, "mt32-pi &synthesizer");
  AppendMenuA(menu, MF_POPUP | (ok ? 0 : MF_GRAYED), (UINT_PTR)mode, "Card &mode");
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_STRING, IDM_EXIT, "E&xit");
  SetMenuDefaultItem(menu, IDM_OPEN, FALSE);

  /* the menu of a notification icon closes on a click elsewhere only when
   * its window is the foreground window */
  GetCursorPos(&pt);
  SetForegroundWindow(wnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, wnd, NULL);
  PostMessageA(wnd, WM_NULL, 0, 0);
  DestroyMenu(menu);            /* and its submenus */
}

/* ---- commands -------------------------------------------------------------------------- */

static void on_command(int id, int code) {
  HWND focus;
  int sel;

  if (id >= IDM_CD && id <= IDM_CD + 255) {
    load_image((unsigned)(id - IDM_CD));
    return;
  }
  if (id > IDM_MODE && id <= IDM_MODE + NE2000_MODE) {
    if (card.result == PGCARD_OK && card.info.mode != id - IDM_MODE) switch_mode((unsigned char)(id - IDM_MODE), 0);
    return;
  }
  switch (id) {
  case IDM_OPEN:
    show_window();
    break;
  case IDCANCEL:
  case IDC_HIDE:
    hide_window();
    break;
  case IDM_EXIT:
  case IDC_EXIT:
    DestroyWindow(wnd);
    break;
  case IDC_AUTOSTART:
    autostart_set(IsDlgButtonChecked(wnd, IDC_AUTOSTART) == BST_CHECKED);
    break;
  case IDOK:
    /* Enter: what the control with the focus is for */
    focus = GetFocus();
    if (focus == GetDlgItem(page[PAGE_CD], IDC_CD_LIST)) on_command(IDC_CD_LOAD, 0);
    else if (focus == GetDlgItem(page[PAGE_MIDI], IDC_MIDI_SFNUM)) on_command(IDC_MIDI_SFSET, 0);
    else if (focus == GetDlgItem(page[PAGE_WEB], IDC_WEB_SSID) || focus == GetDlgItem(page[PAGE_WEB], IDC_WEB_PASS)) {
      wifi_connect();
    }
    break;

  case IDC_CD_LIST:
    if (code != LBN_DBLCLK) break;
    /* fall through */
  case IDC_CD_LOAD:
    sel = (int)SendDlgItemMessageA(page[PAGE_CD], IDC_CD_LIST, LB_GETCURSEL, 0, 0);
    if (sel < 0) status("Choose an image in the list first.");
    else load_image((unsigned)sel + 1);
    break;
  case IDC_CD_EJECT:
    load_image(0);
    break;
  case IDC_CD_REFRESH:
    look();
    if (card.result == PGCARD_OK && card.info.has_cd) list_images();
    update_pages();
    status("");
    break;

  case IDC_MIDI_DEV:
    if (code == CBN_SELCHANGE) {          /* the choice is kept */
      char name[MAXPNAMELEN + 1];
      sel = (int)SendDlgItemMessageA(page[PAGE_MIDI], IDC_MIDI_DEV, CB_GETCURSEL, 0, 0);
      name[0] = 0;
      if (sel >= 0) SendDlgItemMessageA(page[PAGE_MIDI], IDC_MIDI_DEV, CB_GETLBTEXT, sel, (LPARAM)name);
      if (name[0]) reg_set("MidiOut", name);
    }
    break;
  case IDC_MIDI_MT32:
    mt32pi(0x03, 0x00, "mt32-pi: MT-32 synthesizer.");
    break;
  case IDC_MIDI_SF:
    mt32pi(0x03, 0x01, "mt32-pi: SoundFont synthesizer.");
    break;
  case IDC_MIDI_ROM_OLD:
    mt32pi(0x01, 0x00, "mt32-pi: MT-32 (old) ROM set.");
    break;
  case IDC_MIDI_ROM_NEW:
    mt32pi(0x01, 0x01, "mt32-pi: MT-32 (new) ROM set.");
    break;
  case IDC_MIDI_ROM_CM32L:
    mt32pi(0x01, 0x02, "mt32-pi: CM-32L ROM set.");
    break;
  case IDC_MIDI_SFSET: {
    BOOL good = FALSE;
    UINT n = GetDlgItemInt(page[PAGE_MIDI], IDC_MIDI_SFNUM, &good, FALSE);
    char text[64];
    if (!good || n > 127) {
      complain("The SoundFont number is 0 to 127.");
      break;
    }
    wsprintfA(text, "mt32-pi: SoundFont %u.", n);
    mt32pi(0x02, (unsigned char)n, text);
    break;
  }

  case IDC_CARD_SWITCH:
    sel = (int)SendDlgItemMessageA(page[PAGE_CARD], IDC_CARD_MODE, CB_GETCURSEL, 0, 0);
    if (sel >= 0) {
      switch_mode((unsigned char)SendDlgItemMessageA(page[PAGE_CARD], IDC_CARD_MODE, CB_GETITEMDATA, sel, 0),
                  IsDlgButtonChecked(page[PAGE_CARD], IDC_CARD_SAVE) == BST_CHECKED);
    }
    break;

  case IDC_WEB_REFRESH:
    wifi_look();
    status("");
    break;
  case IDC_WEB_OPEN:
    if (card.url[0]) ShellExecuteA(wnd, "open", card.url, NULL, NULL, SW_SHOWNORMAL);
    break;
  case IDC_WEB_CONNECT:
    wifi_connect();
    break;
  default:
    break;
  }
}

static INT_PTR CALLBACK page_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  (void)dlg;
  (void)lp;
  if (msg == WM_INITDIALOG) return TRUE;
  if (msg == WM_COMMAND) {
    on_command(LOWORD(wp), HIWORD(wp));
    return TRUE;
  }
  return FALSE;
}

static void create_pages(void) {
  static const char *const titles[PAGES] = { "CD-ROM", "MIDI", "Card mode", "Web server" };
  static const int ids[PAGES] = { IDD_CD, IDD_MIDI, IDD_CARD, IDD_WEB };
  static int stops[1] = { 16 };
  TCITEMA item;
  RECT rc;
  HWND cb;
  unsigned i;

  tab = GetDlgItem(wnd, IDC_TAB);
  for (i = 0; i < PAGES; i++) {
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = (char *)titles[i];
    SendMessageA(tab, TCM_INSERTITEMA, i, (LPARAM)&item);
  }
  /* the pages lie in the display area of the tab control, above it */
  GetWindowRect(tab, &rc);
  MapWindowPoints(NULL, wnd, (POINT *)&rc, 2);
  SendMessageA(tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&rc);
  for (i = 0; i < PAGES; i++) {
    page[i] = CreateDialogParamA(inst, MAKEINTRESOURCEA(ids[i]), wnd, page_proc, 0);
    SetWindowPos(page[i], HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0);
  }
  SendDlgItemMessageA(page[PAGE_CD], IDC_CD_LIST, LB_SETTABSTOPS, 1, (LPARAM)stops);
  cb = GetDlgItem(page[PAGE_CARD], IDC_CARD_MODE);
  for (i = 0; i < sizeof(modes); i++) {
    SendMessageA(cb, CB_SETITEMDATA, SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)pgcard_mode_name(modes[i])), modes[i]);
  }
  SendDlgItemMessageA(page[PAGE_MIDI], IDC_MIDI_SFNUM, EM_LIMITTEXT, 3, 0);
  SetDlgItemTextA(page[PAGE_MIDI], IDC_MIDI_SFNUM, "0");
  SendDlgItemMessageA(page[PAGE_WEB], IDC_WEB_SSID, EM_LIMITTEXT, PGCARD_SSID_MAX, 0);
  SendDlgItemMessageA(page[PAGE_WEB], IDC_WEB_PASS, EM_LIMITTEXT, PGCARD_PASS_MAX, 0);
  CheckDlgButton(page[PAGE_WEB], IDC_WEB_SAVE, BST_CHECKED);
  EnableWindow(GetDlgItem(page[PAGE_WEB], IDC_WEB_OPEN), FALSE);
  fill_midi();
}

static INT_PTR CALLBACK main_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == wm_taskbar && wm_taskbar) {      /* Explorer started again: the icon is gone */
    tray_added = 0;
    tray_set(0);
    return TRUE;
  }
  switch (msg) {
  case WM_INITDIALOG:
    wnd = dlg;
    SendMessageA(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon_cd);
    create_pages();
    CheckDlgButton(dlg, IDC_AUTOSTART, autostart_get() ? BST_CHECKED : BST_UNCHECKED);
    return TRUE;
  case WM_COMMAND:
    on_command(LOWORD(wp), HIWORD(wp));
    return TRUE;
  case WM_NOTIFY:
    if (((NMHDR *)lp)->idFrom == IDC_TAB && ((NMHDR *)lp)->code == TCN_SELCHANGE) {
      status("");
      select_page((int)SendMessageA(tab, TCM_GETCURSEL, 0, 0));
    }
    return TRUE;
  case WM_SYSCOMMAND:
    /* minimizing and closing hide the window; the icon stays */
    if ((wp & 0xFFF0) == SC_MINIMIZE || (wp & 0xFFF0) == SC_CLOSE) {
      hide_window();
      return TRUE;
    }
    return FALSE;
  case WM_CLOSE:
    hide_window();
    return TRUE;
  case WM_TIMER:
    if (wp == TIMER_REFRESH && IsWindowVisible(dlg)) look();
    else if (wp == TIMER_TRAY) tray_set(0);
    return TRUE;
  case WM_TRAY:
    if (lp == WM_LBUTTONDBLCLK) {
      show_window();
    } else if (lp == WM_RBUTTONUP) {
      tray_menu();
    } else if (lp == WM_MOUSEMOVE && GetTickCount() - hover_tick >= HOVER_MS) {
      hover_tick = GetTickCount();        /* the tip about to show says what is loaded now */
      look();
    }
    return TRUE;
  case WM_SHOWME:
    show_window();
    return TRUE;
  case WM_DESTROY:
    tray_set(1);
    PostQuitMessage(0);
    return TRUE;
  default:
    return FALSE;
  }
}

/* ---- start ----------------------------------------------------------------------------- */

/* "/tray" after the program's name: start as the icon only */
static int start_hidden(const char *cmd) {
  if (*cmd == '"') {
    for (cmd++; *cmd && *cmd != '"'; cmd++) {}
    if (*cmd) cmd++;
  } else {
    while (*cmd && *cmd != ' ' && *cmd != '\t') cmd++;
  }
  return contains(cmd, "/TRAY") || contains(cmd, "-TRAY");
}

static int run(void) {
  WNDCLASSA wc;
  HWND other;
  MSG msg;
  int hidden = start_hidden(GetCommandLineA());

  inst = GetModuleHandleA(NULL);
  other = FindWindowA(WND_CLASS, NULL);
  if (other) {                                /* one icon is enough */
    if (!hidden) PostMessageA(other, WM_SHOWME, 0, 0);
    return 0;
  }
  InitCommonControls();
  icon_cd = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(IDI_CD), IMAGE_ICON, 16, 16, 0);
  icon_empty = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(IDI_EMPTY), IMAGE_ICON, 16, 16, 0);

  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = DefDlgProcA;
  wc.cbWndExtra = DLGWINDOWEXTRA;
  wc.hInstance = inst;
  wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_CD));
  wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = WND_CLASS;
  if (!RegisterClassA(&wc)) return 1;
  wm_taskbar = RegisterWindowMessageA("TaskbarCreated");
  if (!CreateDialogParamA(inst, MAKEINTRESOURCEA(IDD_MAIN), NULL, main_proc, 0)) return 1;

  look();                       /* and the icon */
  if (!hidden) show_window();

  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    if (IsDialogMessageA(wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return (int)msg.wParam;
}

void __cdecl Start(void) {
  ExitProcess((UINT)run());
}
