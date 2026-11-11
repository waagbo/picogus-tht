# Web page for Pico W and Pico 2 W boards

Experimental feature, branch `sw/webserver`.

PicoGUS v1.x and Femto boards with a Pico W or Pico 2 W get a small web page:
- Status of the card.
- Live volume control.
- Settings, and switching modes.
- Choosing CD images.
- Controls for an mt32-pi on the MIDI out: synth, ROM set, SoundFont, MT-32 display text, resets.
- Sending SysEx files to MIDI out.
- Firmware updates.

Set up WiFi with `pgusinit /wifissid` and `/wifipass`; `pgusinit /wifistatus` shows the address. The web server runs only on the second core, at the lowest priority, and stays out of the way of the ISA bus and audio. See `sw/web/README.md`.

- Release files: `picogusw.uf2` for a Pico W and `pg2350w.uf2` for a Pico 2 W. They are all modes with the web server added; `picogus.uf2` and `pgus2350.uf2` are the same firmware without it and work on these boards too. The NE2000 firmware has no web server.
- On the RP2040 the network buffers are in the XIP cache's memory; on the RP2350, which can't use it that way, in ordinary RAM.
- On the RP2040 (Pico W), Sound Blaster mode with the web server has about 23 KB of RAM left. That mode is therefore built without the PGDFS file service in firmware that has both features; the other modes and the Pico 2 W have both.
- MIDI from the web page follows the MPU-401 changes of firmware v4.2.0: in UART mode the DOS program's bytes go out as they are written, so a web message waits until the program is between messages, and the program's running status is restored after it.
- The new MPU-401 "extra wait" setting is on the web page.
- Firmware uploads accept files for the card's own chip only.
