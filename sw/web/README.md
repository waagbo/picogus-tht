# PicoGUS web server (Pico W, Pico 2 W)

PicoGUS boards with a **Raspberry Pi Pico W or Pico 2 W** (PicoGUS v1.x and
PicoGUS Femto with one fitted) can run a small web server next to the sound
card emulation. Open the card's address in a browser to:

* see what the card is doing: mode, firmware, WiFi, loaded CD image, and how
  much time the web server takes from the sound card
* change volumes, live
* change settings (ports, IRQ/DMA, GUS/SB/MPU/mouse/CD options) and save them
* switch the card mode (GUS, Sound Blaster, AdLib, MPU-401, Tandy/CMS, USB)
* pick a CD image from the USB drive
* control an [mt32-pi](https://github.com/dwhinham/mt32-pi) connected to the
  MIDI out: MT-32/SoundFont synth, ROM set, SoundFont, reversed stereo, reboot,
  text on the MT-32 display, GM/GS/MT-32 resets, all notes off, raw MIDI bytes
* upload files: a SysEx file (`.syx`) to send to MIDI out, or a firmware
  update (`.uf2`)

The page is a text-mode setup program, as sound cards came with: one 80x25
screen in the 16 VGA colours, worked with the keyboard or the mouse (see
[The page](#the-page)).

All sound modes include it when built for a Pico W board (`PICO_BOARD=picow_fast`,
or `pico2w_fast` for the RP2350-based Pico 2 W). The NE2000 firmware does not:
it hands the WiFi chip to DOS. PicoGUS 2.0 and v2.1 boards have no WiFi chip.

The release files are `picogusw.uf2` for a Pico W and `pg2350w.uf2` for a
Pico 2 W (`sw/scripts/build_variants.sh web`). The web server is an
experimental feature (`sw/experimental/README.md`), so they are part of the
experimental set of release files.

## Setting up

WiFi uses the same settings as the NE2000 firmware. From DOS:

```
pgusinit /wifissid MyNetwork /wifipass MyPassword /save
pgusinit /wifistatus
```

`/wifistatus` prints the address of the web page, e.g. `Connected to SSID
MyNetwork, Signal -55 dB, web UI: http://192.168.1.42/`. The card also registers
the host name `picogus` with your router's DHCP server, which many home routers
make reachable as `http://picogus/`. While the web page can be reached, the
LED on the Pico W is lit.

Without a configured network the WiFi chip stays off and nothing changes for
the sound card.

**There is no password.** Anyone on your network can change the card's
settings or install firmware. Only use it on a network you trust.

## The page

The page is built from the Textmode design system (`www/textmode/README.md`):
an 80x25 character screen with the IBM VGA font, drawn at 1, 2 or 3 times its
size, whichever fits the browser window. A phone fits the screen to its width.

Everything can be done from the keyboard:

| Key | |
| --- | --- |
| Arrow keys | move the bar; change a value (volume, radio buttons); left leaves a submenu |
| Enter | select the command under the bar, or press the dialog's default button |
| Esc | back to the previous window |
| Tab, Shift+Tab | next and previous control of a dialog |
| Letters | the highlighted letter of a command runs it. While the cursor is in a text field, letters are typed and Alt+letter is the hotkey. |
| Space | tick a check box |
| Home, End, PgUp, PgDn | first and last command; in lists and volume bars, larger steps |
| F1, F2 | help; save the settings to the card |

and with the mouse: the bar follows the pointer, a click selects, the volume
bars can be clicked and dragged, lists scroll with the wheel, and the key
hints on the bottom line are buttons. Only one window takes input at a time.

Display in the main menu switches between the design system's three colour
sets (Setup, Commander, Mono); the browser remembers the choice.

To work on the page without a card, `make -C sw/web/test card` runs a mock
card on `http://127.0.0.1:8080/` that serves the page from `www/` as the
firmware would, and `make -C sw/web/test ui` runs the page through a browser,
once with the keyboard only and once with the mouse only.

### How the page is stored

`scripts/web_pack.py` packs the files of the page when the firmware is built:

* `www/index.html` with its style sheets, script and font put inside it
  (`data-inline`), so the card serves the page as one document and one
  request. It is 79KB, 29KB as stored (gzip).
* every file under `www/static/`, served at `/static/...`: the place for
  anything that is better loaded separately. There are none at present.

The pack is kept in flash and sent from there; it takes no RAM. In the
multi-mode firmware file it is stored once for all six modes, after the WiFi
chip's firmware, in a region of 64KB (`WEB_ASSETS_KB`). Each file has an
`ETag`: the browser keeps its copy and the card answers `304 Not Modified`
until a firmware update changes the file.

## Limits

What bounds the web server, most limiting first:

* **RAM for code, on the RP2040.** PicoGUS runs entirely from RAM (flash is too
  slow and unpredictable for ISA bus timing), so every byte of *code* costs RAM.
  The network stack is about 55KB of it (see [Memory](#memory)), and Sound
  Blaster mode, the largest, is left with about 22KB of the RP2040's 264KB.
  This is why there is no TLS and no password check (the libraries would not
  fit), why mDNS is optional, and why new server-side features must stay small.
  The RP2350 has over 270KB free in every mode.
* **Buffers: 16KB.** lwIP's heap and the HTTP and upload buffers share the
  16KB that the RP2040's unused XIP cache provides. That allows 3 connections
  at a time, 2 x 1460 bytes of TCP window each way, and API answers of up to
  about 2KB (the status JSON is about 1KB; a long list of CD images is cut).
* **Time.** The server runs at most every 2ms, at the lowest priority, and in
  Sound Blaster and AdLib mode only while the FM buffer is half full. It sends
  at most two packets per run, which is the ceiling on speed; a game that keeps
  core 1 busy lowers it further.
* **Flash is not a limit.** The page's files are not copied to RAM. A Pico W
  (2MB) has about 680KB of flash free with all modes and the web server, a
  Pico 2 W (4MB) about 2.7MB. `WEB_ASSETS_KB` sets how much of it the
  multi-mode firmware reserves for the page: a larger script or a second font
  only needs a larger number there. What a bigger page costs is the time of
  its first load (the 29KB page is 20 packets); after that the browser has it.

## Staying out of the way of the sound card

The ISA bus and the sound emulation always come first:

* **Core 0**, which serves the ISA bus, does no network work at all. It only
  sets a few flags for pgusinit's WiFi commands, which take no time.
* **Everything else runs on core 1, at the lowest priority.** The web server is
  polled from the core 1 main loop of each mode (`web_task()`), in thread mode.
  Audio rendering, the emulated interrupt controller and USB run in interrupts,
  so they always preempt it. lwIP runs in "poll" mode: the WiFi chip's
  interrupt only marks work as pending and runs at the lowest interrupt
  priority, so it never delays the audio interrupt.
* **Bounded work per poll.** `web_task()` runs at most every 2ms. Small TCP
  windows (2 × 1460 bytes) limit how much data one poll can process. Uploads
  are throttled through TCP flow control: MIDI is fed at wire speed, and
  firmware is written one 512-byte UF2 block per poll.
* **Audio buffers come first.** In Sound Blaster and AdLib modes, whose OPL
  synthesis runs in the main loop, the web server only runs while at least
  half of the OPL output buffer (~2.9ms of audio) is filled.
* **No interrupts on core 0.** The WiFi driver's waits are busy-waits on
  core 1 (`--wrap` in `webserver.cmake`); in poll mode they would otherwise set
  alarms whose interrupts fire on core 0.
* **Flash is shared safely.** The WiFi firmware and the web page are read from
  flash, and firmware updates write it. A hardware spinlock keeps core 0 from
  saving settings (which erases flash) at the same time: `pgusinit /save`
  normally saves at once, and waits for a later ISA write if core 1 has the
  flash just then. A save asked for on the web page is done by core 0 on its
  next ISA write as well. The boot ROM's flash routines turn the XIP cache
  back on, so it is turned off again after every flash write, and flash data
  is read through the cache-bypassing alias (see Memory below).
* **WiFi reconnects are rare.** Joining a network keeps core 1 busy for a few
  milliseconds. While the network can't be reached, retries back off from
  every 20 seconds to every ~5 minutes.
* **Settings changes use pgusinit's code.** The web UI goes through the same
  code that handles pgusinit's control port writes (`picogus_web_write()` in
  `picogus.cpp`), and the flash write of a save happens on core 0, as for
  pgusinit.
* **MIDI from the web UI never splits a message of the running program.**
  Messages from the web UI (`MIDI_Inject()` in `mpu401/midi.c`) are sent from
  the core 1 main loop only:
  * only when the DOS program's MIDI stream is between messages and not inside
    a SysEx. In UART mode the program's bytes go to the MIDI output as it
    writes them, so the firmware keeps track of whether it is part way through
    a message. A message the program never finishes holds web MIDI up for one
    second at most;
  * after a web message the program's running status is no longer what is on
    the wire, so the status byte of its next message is sent again;
  * SysEx is paced like the MPU-401's SysEx delay;
  * while a program uses MPU-401 intelligent mode, they wait.

  SysEx messages from files may be up to 512 bytes.

The status page shows how much of core 1 the web server used over the last
10 seconds, and its longest single run.

### Memory

PicoGUS runs entirely from RAM. The web server adds about 55KB of code and data
to each mode. On the RP2040 that leaves Sound Blaster mode, the largest, with
about 22KB free for CD images and the USB stack; the other modes keep 65KB or
more, and on the RP2350 every mode has over 260KB free. Because of this, the
RP2040's Sound Blaster mode is built without the PGDFS file service when a
source tree has both features (the other modes, and the RP2350, have both).

| Part | RAM |
| --- | --- |
| lwIP (TCP, UDP, DHCP, ARP, ICMP) | ~22KB |
| CYW43 WiFi driver and SDK glue | ~20KB |
| web server and MIDI queue | ~13KB |

On the RP2040 all network *buffers* (lwIP's heap, HTTP connections, upload
buffers; 16KB in total) live in the XIP cache memory. PicoGUS turns the flash
cache off at boot, after which that memory can be used as ordinary SRAM at
`0x15000000` (see `web_xip_sram.h`).

A cached flash read would overwrite those buffers, so in these firmwares
everything that reads flash at run time uses the cache-bypassing alias
(`XIP_NOCACHE_NOALLOC_BASE`) instead: the files of the web page, the shared
WiFi firmware, and AdLib mode's OPL level table.

The RP2350's XIP cache can't be used that way, and it has twice the RAM: there
the buffers are ordinary memory.

The ~225KB WiFi chip firmware stays in flash, as do the files of the web page.
In the multi-mode firmware file all modes share a single copy of each, stored
right after the bootloader (`web_cyw43_fw_shared.h`, `web_assets.c`,
`webserver.cmake`).

## Firmware update

Upload `picogusw.uf2` (Pico W) or `pg2350w.uf2` (Pico 2 W) to keep the web
page. The file is checked block by block and written straight to flash.
Blocks for the other chip are skipped, so a file for it is refused with
nothing written, and the settings sector is never touched. Sound stutters
while flash is erased, and the card restarts into the same mode when done.

If the upload breaks off half way, the old firmware is already partly
overwritten. The card keeps running from RAM, so upload again before
restarting. As a last resort, the Pico W can always be reflashed over USB by
holding BOOTSEL.

`pgusinit /flash` works as on every PicoGUS; it stops the web server for the
duration.

## Build options

| CMake option | Default | |
| --- | --- | --- |
| `WEBSERVER` | `ON` on Pico W and Pico 2 W boards | include the web server in the sound card firmwares |
| `WEB_MDNS` | `OFF` | also answer mDNS for `http://picogus.local/`. Costs ~12KB of RAM, which Sound Blaster mode needs for CD images made of many track files. |
| `WEB_ASSETS_KB` | `64` | flash reserved for the files of the web page in the multi-mode firmware file. The build fails, saying so, if the page outgrows it. |

## HTTP API

The web page uses this API; scripts can too. Forms are
`application/x-www-form-urlencoded`, answers are JSON.

| Request | |
| --- | --- |
| `GET /api/status` | firmware, mode, network, all settings (`s`), upload progress, CD image |
| `POST /api/set` | any of the settings keys from `s`, e.g. `sbvol=80&sbirq=7`. Applied immediately, all or nothing. |
| `POST /api/save` | save settings to flash |
| `POST /api/defaults` | load default settings (not saved). The WiFi network is kept. |
| `POST /api/reboot` | restart the card |
| `POST /api/mode` | `mode=1..6` (GUS, AdLib, MPU, PSG, SB, USB), `save=1` to make it the default |
| `POST /api/wifi` | `ssid=...&pass=...`: join another network (save to keep it) |
| `POST /api/midi` | `hex=F07D0300F7`: complete MIDI messages to send to MIDI out |
| `POST /api/upload/syx` | body: a `.syx` file |
| `POST /api/upload/firmware` | body: a `.uf2` file |
| `GET /api/cd` | list CD images on the USB drive (answers `{"busy":true}` until the list is ready) |
| `POST /api/cd/load` | `index=n` from the list, 0 to eject |

## Tests

`web/test` runs the real lwIP stack and the web server on a PC over a
loopback interface. It sends requests in awkward pieces and checks them:
headers, forms, settings, MIDI checks, SysEx and firmware uploads with
backpressure, broken and aborted uploads, connection limits, and a 10 minute
polling run. It also tests the MIDI injection in `mpu401/midi.c`, and that
the page is served from its pack, with `304` for a copy the browser has.

```
make -C sw/web/test PICO_SDK_PATH=/path/to/pico-sdk test
```

`make -C sw/web/test ui` tests the page itself in a browser (Node.js with
Playwright) against a mock card: every command once by keyboard and once by
mouse, that all windows sit on whole character cells inside the 80x25 screen,
and that there is never more than one bar.

## Files

| File | |
| --- | --- |
| `web_main.c` | startup, WiFi, pgusinit `/wifistatus`, flash lock, timing statistics |
| `web_http.c` | HTTP server on the lwIP raw API |
| `web_api.cpp` | API handlers |
| `web_upload.c` | firmware and SysEx uploads, MIDI message checks |
| `web_assets.c` | the files of the web page, read from their pack in flash |
| `www/index.html`, `www/app.js`, `www/app.css` | the web page |
| `www/textmode/` | the Textmode design system: colours, components, font |
| `scripts/web_pack.py` | packs the page for the firmware |
| `test/mock_card.py`, `test/ui_test.js` | a mock card and the browser test of the page |
| `lwipopts.h`, `web_cyw43_config.h` | lwIP and WiFi driver configuration |
| `webserver.cmake` | build integration |
