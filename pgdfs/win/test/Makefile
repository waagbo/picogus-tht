# Host test for the PGDFS client core of the Windows drivers (../common/pgc.c)
# against the real firmware transport and file server on a FatFs RAM disk.
#
#   make test     build with cc and run
#   make clean
#
# FatFs resolves #include "ffconf.h" relative to the directory ff.h lives in,
# so the build directory gets copies of the FatFs sources next to the host
# configuration of sw/dfs/test (as in sw/dfs/test/Makefile).

CC      ?= cc
B       := build
SW      := ../../../sw
DFS     := $(SW)/dfs
FATFS   := $(SW)/fatfs/source
CORE    := ../common
CFLAGS  ?= -O1 -g
WARN    := -Wall -Wextra -Wno-unused-parameter
INC     := -I$(B) -I$(DFS) -I$(DFS)/test -I$(SW) -I$(SW)/../common -I$(CORE)
DEFS    := -DDFS_HOST_TEST -DDFS_MAX_PAYLOAD=4096

LINKS := $(B)/ffconf.h $(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c
OBJS  := $(B)/dfs_server.o $(B)/dfs_fs.o $(B)/dfs_transport.o $(B)/ff.o $(B)/ffunicode.o \
         $(B)/ramdisk_diskio.o $(B)/pgc.o $(B)/test_pgc.o
HDRS  := $(CORE)/pgc.h $(DFS)/dfs.h $(DFS)/dfs_fs.h $(DFS)/dfs_server.h $(SW)/../common/picogus.h

.PHONY: all test clean

all: $(B)/test_pgc

test: $(B)/test_pgc
	./$(B)/test_pgc

$(B):
	mkdir -p $(B)

$(B)/ffconf.h: $(DFS)/test/ffconf_host.h | $(B)
	cp $< $@

$(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c: $(B)/%: $(FATFS)/% | $(B)
	cp $< $@

$(B)/ff.o: $(B)/ff.c $(LINKS)
	$(CC) $(CFLAGS) -Wall $(INC) -c $< -o $@

$(B)/ffunicode.o: $(B)/ffunicode.c $(LINKS)
	$(CC) $(CFLAGS) -Wall $(INC) -c $< -o $@

$(B)/%.o: $(DFS)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/ramdisk_diskio.o: $(DFS)/test/ramdisk_diskio.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/pgc.o: $(CORE)/pgc.c $(HDRS)
	$(CC) $(CFLAGS) $(WARN) -std=c99 -pedantic $(INC) -c $< -o $@

$(B)/test_pgc.o: test_pgc.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INC) -c $< -o $@

$(B)/test_pgc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -rf $(B)
