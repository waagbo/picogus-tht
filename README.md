# PicoGUS THT

<img src="picogus-logo.svg" width="500" alt="PicoGUS" />

PicoGUS is a Raspberry Pi Pico based ISA sound card for DOS and Win9x retro computers.
It is a highly versatile clone card that provides near perfect emulation of the Gravis Ultrasound, Sound Blaster-series, AdLib, Tandy 3-voice and CMS/Game Blaster.

Additional features like CD-ROM (Panasonic MKE), MPU-401 and USB game controllers build further on the cards versatility and the wavetable header will further extend the capacity with a card like [X2GS from Serdaco](https://serdashop.com/products/x2gs-te) to get the original Roland magic or even a [WavetablePi](https://github.com/scrapcomputing/WavetablePiHoriz) for the ultimate all-in-one experience for MS-DOS games. 

**PicoGUS THT is a through-hole conversion of the [PicoGUS project](https://github.com/polpo/picogus).**<br />
The conversion introduces through-hole components where possible in addition to embedding new features and ideas while still keeping the card compatible with the upstream project. The design goal is to make the project be easier to solder by hand as well as giving it a higher level of retro asthetics.

PicoGUS THT exists in multiple variants based on different PicoGUS revisions. Each variant is found in their separate folder (or branch if still experimental). The different *hw* designs feature additional documentation including ready to order BOM CSVs in their respective *docs* folders.

The designs still have some TSSOP SMD chips as they do not have THT equivalents, namely the two *SN74CB3T3257PWR* level shifters (U3/U4) as well as the PCM510xA DAC. PicoGUS THT features a redesigned footprint to ease debugging in case something goes wrong.

**Note!** The hardware revisions are intentionally kept aligned with the *major* revision as the original PicoGUS boards. This is done to indicate hardware feature compatibility and to prevent confusion in case of any future major revisions of the original PicoGUS design.

### Version 1.3

<img src="hw-tht-v1.3/docs/renders/PicoGUS-THT-v1.3.2-purple-front.jpg" alt="PicoGUS THT v1.3.2 Render" width="350" align="right" />

This version is an extension of the PicoGUS 1.2 design while implementing the *wavetable header* with manual volume control from the *hw-hand386* branch. Together with the addition of a full size USB-A connector accessible from the computers backside this card is now a real competitor to the v2.0 SMD design.

In addition, the THT boards have also been slightly resized to optimize for component placement and JCLPCB-pricing.
The v1.3.2 is currently under quality assurance. The following features have been tested and verified.
* [x] Card Assembly ➔ Minor adjustments to DigiKey BOM.
* [x] Flash, boot and detect card ➔ OK (v4.2.0-beta2).
* [x] Audio Playback ➔ OK (v4.2.0-beta2, SB Mode).
* [x] Wavetable MIDI Playback  ➔ OK (WavetablePi)
* [ ] External MIDI Playback
* [x] USB-A Connector (Both CD-ROM and experimental PGDFS filesystem)

The release version v1.3.2 is still pending build/test.

Assembly should be *EASY to MEDIUM* with the appropriate quality tools and consumables and a good project for beginners that want a little more challenge. Here is a list of recommended supplies with some personal recommendations:
* [Low Temperature Solder Paste](https://www.digikey.com/en/products/detail/chip-quik-inc/NC191LTA10/13531404) for easy soldering of SMD components.
* [Flux](https://www.digikey.com/en/products/detail/chip-quik-inc/NC191-2CC6/11480375) as it makes solder behave like magic when you need it the most.
* [Solder Wick](https://www.digikey.com/en/products/detail/chemtronics/SW14035/5177685) as it will help you when flux can't. Remember to **NEVER** pull a stuck wick.
* [Soldering Iron with J-Tip (Alientek T90B)](https://www.aliexpress.com/item/1005010015046346.html) will make the SMD components a breeze to solder. 
* [Tweezers](https://www.aliexpress.com/item/1005007298021096.html) for handling the SMD components.
* Digital Microscope of decent quality from AliExpress is very useful for visual inspection of SMD chips.
* Multimeter with good continuity tester.

### Version 2.1

<img src="hw-tht-v2.1/docs/renders/PicoGUS-THT-v2.1.0-purple-front.jpg" alt="PicoGUS THT v2.1.0 Render" width="350" align="right" />

This is an experimental extension of the PicoGUS 2.0 SMD design, made possible using a Waveshare Core2350B board to free enough I/O ports for the 2.0 feature set. This is still in the design phase.
<br/>

### Experimental Firmware
We have added experimental features that adds upon community ideas on top of the original firmware and software components. The features are submitted as Pull Requests to the original PicoGUS repository as well but there is no guarantee that they will ever be included in the official software releases:
* DFS drive support inspired by PicoMEM so the USB drive can be accessed from MS-DOS.

<br/>


**A big thank you to [Ian Scott (polpo)](https://github.com/polpo/) for the PicoGUS design and his contributions to the retro community.**<br/>**May his work set an example for future open-source hardware projects.**

--<br/>
Vidar Waagbø, Retro City AS

## PicoGUS

The original PicoGUS project is found at [polpo/pigocus](https://github.com/polpo/picogus) or www.picogus.com.<br/>
Here are the original README.md to give some more information about the great features of the PicoGUS:

ISA sound card and CD-ROM drive emulation on the Raspberry Pi Pico's RP2040 microcontroller. PicoGUS can emulate:

* [Gravis Ultrasound (GUS)](https://en.wikipedia.org/wiki/Gravis_Ultrasound) - the primary focus of PicoGUS, hence the name
* [Sound Blaster 16](https://en.wikipedia.org/wiki/Sound_Blaster_16) (OPL3) / [Sound Blaster Pro 2](https://en.wikipedia.org/wiki/Sound_Blaster#Sound_Blaster_Pro_2,_CT1600) (OPL3) / [AdLib](https://en.wikipedia.org/wiki/Ad_Lib,_Inc.) (OPL2)
* [MPU-401 (with intelligent mode)](https://en.wikipedia.org/wiki/MPU-401) - outputs MIDI data on 3.5mm MIDI TRS connector
* [Tandy 3-voice](http://www.vgmpf.com/Wiki/index.php?title=Tandy_3_Voice)
* [CMS/Game Blaster](http://nerdlypleasures.blogspot.com/2012/10/all-you-ever-wanted-to-know-about.html)
* [Game port joystick](https://en.wikipedia.org/wiki/Game_port)
* [Panasonic/MKE CD-ROM](https://en.wikipedia.org/wiki/Panasonic_CD_interface)

Current status: perpetual beta! See the [main wiki page](https://github.com/polpo/picogus/wiki) for current status and the [compatibility list](https://github.com/polpo/picogus/wiki/Compatibility-list) for support status of various DOS programs and other system compatibility notes. This project has a heavy demoscene focus due to the GUS's history so that's what I've concentrated on, but GUS support in games is very good to excellent.

Want to buy a PicoGUS? Fully assembled PicoGUS 2.0 sound cards are available from these sources, all of whom ship worldwide:

* [Joe's Computer Museum Shop](https://jcm-1.com/product/picogus-v2/) - in US 🇺🇸
* [Serdashop](https://www.serdashop.com/PicoGUS) - in EU 🇪🇺
* [Flamelily IT](https://shop.flamelily.co.uk/picogus) - in UK 🇬🇧

Want to make your own PicoGUS? See the [build guide](https://github.com/polpo/picogus/wiki/Building-your-PicoGUS). Note that the more DIY friendly v1.1.1 hardware has some documented issues with reset – a forthcoming v1.2 revision will fix these issues. **Important caveat**: due to the specs of the Pico, assumptions made by programs written to use the GUS, the imprecise nature of emulation, and the varying specs of retro DOS PC hardware, some things will likely never be perfect. **This is still a work in progress.**

Have a PicoGUS and want to use it? See the [configuring and using your PicoGUS guide](https://github.com/polpo/picogus/wiki/Configuring-and-using-your-PicoGUS).

Want to support PicoGUS? I have a limited number of machines to test PicoGUS in and donating either money or motherboards would be greatly appreciated and help increase the compatibility of PicoGUS.

You can donate via [PayPal](https://paypal.me/ianpolpo) or Ko-Fi: [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/U7U6IZTCB)

![PicoGUS 2.0 PCB](https://github.com/polpo/picogus/assets/1544908/821f109e-6e85-4230-8ad6-4359dd04f539)

![PicoGUS v1.1 beta PCB](https://user-images.githubusercontent.com/1544908/215666529-fc694b8f-aec3-4679-87df-b53d0c406c99.jpg)

See/hear PicoGUS in action on YouTube:

Videos by others:

[<img src="https://i3.ytimg.com/vi/08IPnmvuatw/maxresdefault.jpg" width=400>](https://www.youtube.com/watch?v=08IPnmvuatw)
[<img src="https://i3.ytimg.com/vi/bBYUTwKRyNk/maxresdefault.jpg" width=400>](https://youtu.be/bBYUTwKRyNk)
[<img src="https://i3.ytimg.com/vi/oEHVB0FITqU/maxresdefault.jpg" width=400>](https://youtu.be/oEHVB0FITqU)
[<img src="https://i3.ytimg.com/vi/aeejxbaAQ4g/maxresdefault.jpg" width=400>](https://youtu.be/aeejxbaAQ4g)
[<img src="https://i3.ytimg.com/vi/okSBZJwqVb8/maxresdefault.jpg" width=400>](https://youtu.be/okSBZJwqVb8)

My videos:

[<img src="https://i3.ytimg.com/vi/h4iWSnTc9Ag/maxresdefault.jpg" alt="September 2022 update" width=400>](https://youtu.be/h4iWSnTc9Ag)
[<img src="https://i3.ytimg.com/vi/CkJvkJVRscQ/maxresdefault.jpg" alt="October 2022 update" width=400>](https://youtu.be/CkJvkJVRscQ)
[<img src="https://i3.ytimg.com/vi/F5Zk_hHHkTg/maxresdefault.jpg" alt="December 2022 update" width=400>](https://youtu.be/F5Zk_hHHkTg)
[<img src="https://i3.ytimg.com/vi/sOHTagrWcIE/maxresdefault.jpg" alt="January 2023 update" width=400>](https://youtu.be/sOHTagrWcIE)
[<img src="https://i3.ytimg.com/vi/2LBXzy4Fus0/maxresdefault.jpg" alt="March 2023 update" width=400>](https://youtu.be/2LBXzy4Fus0)

## Open Source Credits

* [Raspberry Pi Pico C/C++ SDK](https://github.com/raspberrypi/pico-sdk)
* `pico_audio_i2s` from [pico-extras](https://github.com/raspberrypi/pico-extras)
* `stdio_async_uart` from [PicoCart64](https://github.com/kbeckmann/PicoCart64)
* `gus-x.cpp` adapted from [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
* OPL/AdLib emulation from [rp2040-doom](https://github.com/kilograham/rp2040-doom) (based on [emu8950](https://github.com/digital-sound-antiques/emu8950))
* MPU-401 emulation adapted from [HardMPU](https://github.com/ab0tj/HardMPU) and [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
* Tandy 3-voice emulation based on code contributed by [Aaron Giles](https://aarongiles.com/), adapted from [DREAMM](https://aarongiles.com/dreamm/) 
* CMS emulation based on code contributed by [Aaron Giles](https://aarongiles.com/), adapted from [DREAMM](https://aarongiles.com/dreamm/)
* USB support uses [TinyUSB](https://github.com/hathach/tinyusb) and [tusb_xinput](https://github.com/Ryzee119/tusb_xinput) with USB mass storage improvements by rppicomidi and wbcbz7.
* FAT drive support by [FatFS](https://elm-chan.org/fsw/ff/)

## License

The hardware portions of this repository (hw/ directory) are licensed under the CERN OHL version 2, permissive.

The software portions of this repository (sw/, pgusinit/ directories) as a collection are licensed under the GNU GPL version 2. Some files are individually dual-licensed under BSD or MIT licenses – see the license in the file headers for details.
