# pgdfs.sys, the PGDFS driver for Windows NT 4.0 / 2000 / XP (x86), built
# with the MinGW-w64 cross compiler (Debian/Ubuntu: gcc-mingw-w64-i686) and
# its DDK headers:
#
#   make            pgdfs.sys
#   make clean
#
# The driver is linked as a native image without any C runtime or libgcc; it
# imports only from ntoskrnl.exe and hal.dll (exports Windows NT 4.0 has).
# -shared gives it base relocations; pefix.py then clears the DLL flag and
# writes the PE checksum the kernel checks when it loads a driver.

CROSS   ?= i686-w64-mingw32-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
PYTHON  ?= python3
DDKINC  ?= $(shell $(CC) -print-sysroot 2>/dev/null)/include/ddk
ifeq ($(wildcard $(DDKINC)/ntifs.h),)
DDKINC  := /usr/i686-w64-mingw32/include/ddk
endif

CFLAGS  := -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Werror \
           -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
           -fno-tree-loop-distribute-patterns -march=i486 -mtune=pentium -I$(DDKINC)
LDFLAGS := -shared -nostdlib -nostartfiles -Wl,--subsystem,native -Wl,--image-base,0x10000 \
           -Wl,--entry,_DriverEntry@8 -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 \
           -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
           -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
           -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-seh -Wl,--exclude-all-symbols
LIBS    := -lntoskrnl -lhal

CORE    := ../common
OBJS    := pgdfs.o pgc.o

.PHONY: all clean imports

all: pgdfs.sys

pgdfs.o: pgdfs.c $(CORE)/pgc.h ../../../common/picogus.h
	$(CC) $(CFLAGS) -c $< -o $@

pgc.o: $(CORE)/pgc.c $(CORE)/pgc.h ../../../common/picogus.h
	$(CC) $(CFLAGS) -c $< -o $@

pgdfs.sys: $(OBJS) pefix.py
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	$(PYTHON) pefix.py $@

# what the driver needs from the kernel (to check against an NT 4.0 export list)
imports: pgdfs.sys
	$(OBJDUMP) -p $< | sed -n '/DLL Name/,/^$$/p'

clean:
	rm -f *.o pgdfs.sys
