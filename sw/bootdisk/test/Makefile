# Host tests for PGBOOT (../bootdisk.c, ../bd_geometry.c), served through the
# PGDFS server (../../dfs/dfs_server.c, dfs_fs.c) on the RAM disk FatFs
# harness of ../../dfs/test.
#
#   make test     build with cc and run
#   make clean
#
# As in ../../dfs/test: FatFs resolves #include "ffconf.h" next to ff.h, so
# the build directory holds copies of the FatFs sources beside the host
# configuration (../../dfs/test/ffconf_host.h, firmware config + f_mkfs).

CC      ?= cc
B       := build
BD      := ..
SW      := ../..
DFS     := $(SW)/dfs
DT      := $(DFS)/test
FATFS   := $(SW)/fatfs/source
CFLAGS  ?= -O1 -g
WARN    := -Wall -Wextra -Wno-unused-parameter
INC     := -I$(B) -I$(BD) -I$(DFS) -I$(DT) -I$(SW) -I$(SW)/../common
DEFS    := -DBD_HOST_TEST

LINKS := $(B)/ffconf.h $(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c
OBJS  := $(B)/bootdisk.o $(B)/bd_geometry.o $(B)/dfs_server.o $(B)/dfs_fs.o \
         $(B)/ff.o $(B)/ffunicode.o $(B)/ramdisk_diskio.o $(B)/test_bootdisk.o
HDRS  := $(BD)/bootdisk.h $(BD)/bd_geometry.h $(DFS)/dfs.h $(DFS)/dfs_fs.h $(DFS)/dfs_server.h \
         $(SW)/../common/picogus.h $(DT)/ramdisk_diskio.h

.PHONY: all test clean

all: $(B)/test_bootdisk

test: $(B)/test_bootdisk
	./$(B)/test_bootdisk

$(B):
	mkdir -p $(B)

$(B)/ffconf.h: $(DT)/ffconf_host.h | $(B)
	cp $< $@

$(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c: $(B)/%: $(FATFS)/% | $(B)
	cp $< $@

$(B)/ff.o: $(B)/ff.c $(LINKS)
	$(CC) $(CFLAGS) -Wall $(INC) -c $< -o $@

$(B)/ffunicode.o: $(B)/ffunicode.c $(LINKS)
	$(CC) $(CFLAGS) -Wall $(INC) -c $< -o $@

$(B)/%.o: $(BD)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/%.o: $(DFS)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/%.o: $(DT)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/%.o: %.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/test_bootdisk: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -rf $(B)
