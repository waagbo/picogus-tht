# Song recorder: FM and MIDI music to files on the USB drive

Experimental feature, branch `sw/capture`. For RP2350 boards (Pico 2, Pico 2 W, PicoGUS v2.1), in every mode that has FM or the MPU-401 and the USB stack. It builds on the `pgdfs` (writing to the USB drive) and `webserver` features, which its branch contains.

**It has not run on a card yet.** It builds for all three platforms and passes its tests on a PC (see Tests below).

- The card can record what a program sends to the FM chip and to the MIDI output, as the raw commands with their timing, into files on the USB drive, for playing or studying them later:
  - FM: every OPL2/OPL3 register write, in DOSBox raw OPL files (`.DRO`, version 2.0), as DOSBox's own capture makes them. They play in AdPlug-based players and on OPL hardware players.
  - MIDI: every byte the program sends, from the MPU-401 (UART and intelligent mode) and from the Sound Blaster's MIDI port, in standard MIDI files (`.MID`, format 0, 1 ms per tick). What the web page sends to an mt32-pi is not recorded.
- Each song gets its own file: `OPL00001.DRO`, `MID00001.MID`, ... in the `CAPTURE` folder of the drive. The numbers continue after the highest one on the drive.
  - A song starts with its first note. The file begins with what the program set up before: for FM the registers as they are at that moment, for MIDI the programs, the usual controllers and the pitch bend of each channel, followed by what the program sent since the song before (SysEx included), without the waiting in between.
  - A song ends when no note has sounded for a while, 3 seconds unless set otherwise. The file ends one second after its last sound. What a program writes while nothing sounds, such as the instruments of the next song, goes to the next file.
  - Songs shorter than 5 seconds (also a setting) are not kept, which leaves out most sound effects. A MIDI file with SysEx in it is always kept, as later songs may need what it sets up.
  - One file per song can be switched off: then a file runs from the first note until the recorder is switched off.
  - Two songs with less silence between them than the pause setting end up in one file, and a song with a longer rest in it in two. *New Song* on the web page ends the files at that moment.
- On a Pico 2 W the web page controls it: *Song Recorder* in the main menu switches FM and MIDI recording on and off, sets the pause and the shortest song, shows what is being recorded, and starts a new song. *Song List* there lists the recorded songs, newest first, to download or delete them.
- The settings are kept on the USB drive, in `CAPTURE\CAPTURE.CFG`, and read when the drive is mounted: recording carries on after a restart or a change of mode. The file is one line of text, such as `opl=1 midi=1 split=1 gap=3 min=5`, and can be written on a PC. That is also how the recorder is switched on where there is no web page (Pico 2, PicoGUS v2.1).
- The files are also there for DOS and Windows through PGDFS. Leave the file that is being recorded alone until its song has ended.
- Switch the recorder on before the program starts: what a program set up earlier is not known to it, so the first song could miss its instruments.
- A file is brought up to date on the drive every 5 seconds, so a power cut costs at most the last seconds of a song. A MIDI file left that way has no end-of-track event.
- While it records, or one of its files is downloaded, FM synthesis is kept going during the waits for the USB drive, which would otherwise interrupt it for a few milliseconds each time. The recorder allocates its memory (about 10KB per stream) when it is first switched on.
- The RP2040 firmware does not have the recorder: its code, with a FatFs that can write, takes more RAM than Sound Blaster mode with the web server has left. `-DCAPTURE=ON` builds it in regardless; `-DCAPTURE=OFF` leaves it out on the RP2350.
- HTTP API (see `sw/web/README.md`): `POST /api/rec`, `GET /api/rec/files`, `GET /api/rec/file`, `POST /api/rec/delete`, and `rec` in `GET /api/status`. The web server gained responses that are read from the drive as they are sent, and query strings.

## Tests

- `make -C sw/capture/test test` runs the recorder and FatFs on a RAM disk with a simulated clock: songs are found and split, the files are read back and checked as a player would read them, across timer wrap-arounds, a pulled drive and write errors.
- `make -C sw/web/test test` covers the HTTP API, downloads included, and the tap in the MIDI output; `make -C sw/web/test ui` the page in a browser.
- The MIDI files of the tests were also read with the `mido` library, and the `.DRO` layout checked against its description on the ModdingWiki.
