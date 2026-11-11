# The PGDFS drivers for Windows:
#   9x/  PGDFS.VXD  Windows 95/98/ME   (Open Watcom 2.0: make INCLUDE="$WATCOM"/h)
#   nt/  pgdfs.sys  Windows NT 4.0/2000/XP (MinGW-w64: i686-w64-mingw32-gcc)
#   test/           host tests of the shared client core (cc)
#
#   make            both drivers
#   make test       the host tests
#   make clean

.PHONY: all vxd sys test clean

all: vxd sys

vxd:
	$(MAKE) -C 9x

sys:
	$(MAKE) -C nt

test:
	$(MAKE) -C test test

clean:
	$(MAKE) -C 9x clean
	$(MAKE) -C nt clean
	$(MAKE) -C test clean
