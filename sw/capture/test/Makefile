# Host tests for the song recorder (../capture*.c): the real code and the real
# FatFs on a RAM disk, with the time set by the test.
#
#   make test
#   make clean
#
# FatFs and its RAM disk are set up as for the PGDFS tests (../../dfs/test):
# the build directory holds copies of the FatFs sources next to a host
# configuration named ffconf.h (the firmware's, with FF_USE_MKFS 1).

CC      ?= cc
B       := build
CAP     := ..
SW      := ../..
FATFS   := $(SW)/fatfs/source
DFSTEST := $(SW)/dfs/test
CFLAGS  ?= -O1 -g -fsanitize=address,undefined
WARN    := -Wall -Wextra -Wno-unused-parameter
DEFS    := -DCAPTURE=1 -DSOUND_OPL=1 -DSOUND_MPU=1
INC     := -I$(B) -Istubs -I$(CAP) -I$(SW) -I$(SW)/usb_msc -I$(DFSTEST)

LINKS := $(B)/ffconf.h $(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c
SRC   := $(CAP)/capture.c $(CAP)/capture_opl.c $(CAP)/capture_midi.c
HDRS  := $(CAP)/capture.h $(CAP)/capture_internal.h stubs/hardware/timer.h

.PHONY: test clean

test: $(B)/test_capture
	./$(B)/test_capture

$(B):
	mkdir -p $(B)

$(B)/ffconf.h: $(DFSTEST)/ffconf_host.h | $(B)
	cp $< $@

$(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c: $(B)/%: $(FATFS)/% | $(B)
	cp $< $@

$(B)/ff.o: $(B)/ff.c $(LINKS)
	$(CC) $(CFLAGS) -w $(INC) -c $< -o $@

$(B)/ffunicode.o: $(B)/ffunicode.c $(LINKS)
	$(CC) $(CFLAGS) -w $(INC) -c $< -o $@

$(B)/test_capture: test_capture.c $(SRC) $(HDRS) $(DFSTEST)/ramdisk_diskio.c $(B)/ff.o $(B)/ffunicode.o
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) test_capture.c $(SRC) $(DFSTEST)/ramdisk_diskio.c \
		$(B)/ff.o $(B)/ffunicode.o -o $@

clean:
	rm -rf $(B)
