# The PGDFS drivers for Windows, and PGTRAY:
#   9x/   PGDFS.VXD   Windows 95/98/ME   (Open Watcom 2.0: make INCLUDE="$WATCOM"/h)
#   nt/   pgdfs.sys   Windows NT 4.0/2000/XP (MinGW-w64: i686-w64-mingw32-gcc)
#   tray/ pgtray.exe  both (MinGW-w64)
#   test/             host tests of the shared client core and PGTRAY's card part (cc)
#
#   make            the drivers and PGTRAY
#   make test       the host tests
#   make clean

.PHONY: all vxd sys tray test clean

all: vxd sys tray

vxd:
	$(MAKE) -C 9x

sys:
	$(MAKE) -C nt

tray:
	$(MAKE) -C tray

test:
	$(MAKE) -C test test

clean:
	$(MAKE) -C 9x clean
	$(MAKE) -C nt clean
	$(MAKE) -C tray clean
	$(MAKE) -C test clean
