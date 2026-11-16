# pgcard-sim: PicoGUS card simulator for the QEMU pgbridge device.
#
#   make                 build $(B)/pgcard-sim
#   make BD_STUB=1       force the bootdisk stub even if sw/bootdisk has the server
#   make DEBUG=1         also enable DFS_LOG() output of the file server
#   make clean
#
# Compiles the real card-side sources: sw/dfs/dfs_transport.c (core 0 + the
# core 1 dfs_tasks()), dfs_server.c, dfs_fs.c, sw/bootdisk/*.c and FatFs from
# sw/fatfs/source with the host configuration of sw/dfs/test (ffconf_host.h:
# read/write, LFN, fast seek). Like sw/dfs/test/Makefile, the build directory
# holds copies of ff.c/ff.h/diskio.h/ffunicode.c next to ffconf_host.h named
# ffconf.h, because ff.h includes "ffconf.h" relative to its own directory.
#
# The build directory lives outside the repository (EMU_CACHE).

EMU_CACHE ?= $(HOME)/pico/emu-cache
B         ?= $(EMU_CACHE)/sim-build
ROOT      := $(abspath ../../..)
SW        := $(ROOT)/sw
DFS       := $(SW)/dfs
BD        := $(SW)/bootdisk
FATFS     := $(SW)/fatfs/source
CC        ?= cc
CFLAGS    ?= -O1 -g
WARN      := -Wall -Wextra -Wno-unused-parameter
DEFS      := -DDFS_HOST_TEST
INC       := -I$(B) -I. -I$(DFS) -I$(SW) -I$(ROOT)/common
ifdef DEBUG
DEFS      += -DDFS_DEBUG
endif

# The real bootdisk server once sw/bootdisk defines bd_init(); the stub before.
ifndef BD_STUB
BD_REAL   := $(shell grep -l '^void *bd_init' $(BD)/*.c 2>/dev/null)
endif
ifneq ($(BD_REAL),)
BD_SRCS   := $(wildcard $(BD)/*.c)
BD_OBJS   := $(patsubst $(BD)/%.c,$(B)/bd_%.o,$(BD_SRCS))
else
BD_OBJS   := $(B)/bd_stub.o
endif

LINKS := $(B)/ffconf.h $(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c
OBJS  := $(B)/main.o $(B)/file_diskio.o \
         $(B)/dfs_transport.o $(B)/dfs_server.o $(B)/dfs_fs.o \
         $(B)/ff.o $(B)/ffunicode.o $(BD_OBJS)
HDRS  := $(wildcard $(DFS)/*.h $(BD)/*.h) $(ROOT)/common/picogus.h $(SW)/usb_msc/msc_app.h sim.h

.PHONY: all clean info
all: $(B)/pgcard-sim

info:
	@echo "bootdisk: $(if $(BD_REAL),real ($(notdir $(BD_SRCS))),stub)"

$(B):
	mkdir -p $(B)

$(B)/ffconf.h: $(DFS)/test/ffconf_host.h | $(B)
	cp $< $@

$(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c: $(B)/%: $(FATFS)/% | $(B)
	cp $< $@

$(B)/ff.o: $(B)/ff.c $(LINKS)
	$(CC) $(CFLAGS) -w $(INC) -c $< -o $@

$(B)/ffunicode.o: $(B)/ffunicode.c $(LINKS)
	$(CC) $(CFLAGS) -w $(INC) -c $< -o $@

$(B)/%.o: $(DFS)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/bd_%.o: $(BD)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/%.o: %.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/pgcard-sim: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)
	@echo "pgcard-sim built with the bootdisk $(if $(BD_REAL),server from sw/bootdisk,STUB)"

clean:
	rm -rf $(B)
