# PGBOOT: PicoGUS boot ROM, RAM loader and EEPROM flasher.
#
#   make            PGBOOT.ROM PGBOOT88.ROM PGBOOT.COM PGFLASH.COM test/INT13TST.COM
#   make check      verify the ROM images (size, header, checksum) and that the
#                   8086-only code contains no 186+ instructions
#   make clean
#
# Needs nasm (2.15 or later) and python3.

NASM    ?= nasm
PYTHON  ?= python3
# all warnings as errors, minus the relocation notes NASM 3 prints for every
# absolute address in a flat binary
NASMFLAGS = -f bin -w+all -w-reloc-abs-word -w-reloc-abs-byte -w-reloc-abs-dword \
            -w-reloc-rel-word -w-reloc-rel-dword -Werror

ROM_SRCS = pgboot.asm defs.inc xport.inc int13.inc print.inc

all: PGBOOT.ROM PGBOOT88.ROM PGBOOT.COM PGFLASH.COM test/INT13TST.COM

PGBOOT.ROM: $(ROM_SRCS) mkrom.py
	$(NASM) $(NASMFLAGS) -l pgboot.lst -o pgboot.bin pgboot.asm
	$(PYTHON) mkrom.py pgboot.bin $@

# same code, 8088 transfer loops forced (tests the path an XT takes)
PGBOOT88.ROM: $(ROM_SRCS) mkrom.py
	$(NASM) $(NASMFLAGS) -DFORCE8088 -l pgboot88.lst -o pgboot88.bin pgboot.asm
	$(PYTHON) mkrom.py pgboot88.bin $@

PGBOOT.COM: pgbootld.asm defs.inc
	$(NASM) $(NASMFLAGS) -l pgbootld.lst -o $@ pgbootld.asm

PGFLASH.COM: pgflash.asm defs.inc
	$(NASM) $(NASMFLAGS) -l pgflash.lst -o $@ pgflash.asm

# INT 13h exerciser for the emulator / real machine tests (not needed to boot)
test/INT13TST.COM: test/int13tst.asm
	$(NASM) $(NASMFLAGS) -o $@ test/int13tst.asm

check: PGBOOT.ROM PGBOOT88.ROM
	$(PYTHON) mkrom.py --check PGBOOT.ROM
	$(PYTHON) mkrom.py --check PGBOOT88.ROM
	$(PYTHON) cpucheck.py pgboot.lst out186 in186

clean:
	rm -f *.bin *.lst PGBOOT.ROM PGBOOT88.ROM PGBOOT.COM PGFLASH.COM test/INT13TST.COM

.PHONY: all check clean
