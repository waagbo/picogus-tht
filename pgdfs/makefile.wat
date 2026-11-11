# wmake makefile for PGDFS (OpenWatcom 2.0): wmake -f makefile.wat
# See Makefile (GNU make) for the meaning of the options.

CC = wcl
AS = wasm
CFLAGS = -bcl=dos -0 -s -d0 -ms -os -wx -we -dPICOGUS_NO_MODENAMES
TSRFLAGS = $(CFLAGS) -k1024 -fm=pgdfs.map

all: pgdfs.exe dfsdiag.exe .symbolic

chint.obj: chint086.asm
	$(AS) -0 chint086.asm -fo=chint.obj -ms

pgdfs.exe: pgdfs.c xport.c xport.h globals.h dosstruc.h chint.h version.h chint.obj ../common/picogus.h
	$(CC) $(TSRFLAGS) chint.obj pgdfs.c -fe=pgdfs.exe

dfsdiag.exe: dfsdiag.c xport.c xport.h version.h ../common/picogus.h
	$(CC) $(CFLAGS) -za99 dfsdiag.c xport.c -fe=dfsdiag.exe

clean: .symbolic
	if exist pgdfs.exe del pgdfs.exe
	if exist dfsdiag.exe del dfsdiag.exe
	if exist pgdfs.map del pgdfs.map
	if exist *.obj del *.obj
	if exist *.err del *.err
