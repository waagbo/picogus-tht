# pgtray.exe, the PicoGUS in the notification area of Windows 95/98/ME and
# Windows NT 4.0/2000/XP, built with the MinGW-w64 cross compiler
# (Debian/Ubuntu: gcc-mingw-w64-i686; its windres compiles the resources):
#
#   make            pgtray.exe
#   make imports    what it needs from Windows (to check against Windows 95)
#   make demo       pgtray-demo.exe: the same program with an emulated card
#                   inside, to work on the window without a PicoGUS
#   make clean
#
# The program is linked without a C runtime (MinGW's needs a newer Windows
# than 95): its entry point is Start(), and it imports only what Windows 95
# and NT 4.0 export. mkicon.py draws the icons.

CROSS   ?= i686-w64-mingw32-
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
OBJDUMP := $(CROSS)objdump
PYTHON  ?= python3

CFLAGS  := -std=gnu99 -Os -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -march=i486 -mtune=pentium
LDFLAGS := -nostdlib -nostartfiles -s -Wl,--subsystem,windows -Wl,--entry,_Start \
           -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
           -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
           -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-seh
LIBS    := -lkernel32 -luser32 -lshell32 -lcomctl32 -ladvapi32 -lwinmm

CORE    := ../common
TEST    := ../test
HDRS    := pgcard.h backend.h resource.h $(CORE)/pgctl.h ../../../common/picogus.h
OBJS    := pgtray.o pgcard.o backend.o pgtray.res.o
DEMO    := pgtray.o pgcard.o demo.o emuctl.demo.o pgtray.res.o

.PHONY: all clean imports demo

all: pgtray.exe

cd.ico empty.ico: mkicon.py
	$(PYTHON) mkicon.py cd.ico empty.ico

pgtray.res.o: pgtray.rc resource.h cd.ico empty.ico
	$(WINDRES) pgtray.rc -O coff -o $@

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

pgtray.exe: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

imports: pgtray.exe
	$(OBJDUMP) -p $< | sed -n '/DLL Name/,/^$$/p'

demo: pgtray-demo.exe

emuctl.demo.o: $(TEST)/emuctl.c $(TEST)/emuctl.h
	$(CC) $(CFLAGS) -I../../../common -c $< -o $@

demo.o: demo.c $(HDRS) $(TEST)/emuctl.h
	$(CC) $(CFLAGS) -I$(TEST) -c $< -o $@

pgtray-demo.exe: $(DEMO)
	$(CC) $(LDFLAGS) -o $@ $(DEMO) $(LIBS)

clean:
	rm -f *.o *.ico pgtray.exe pgtray-demo.exe
