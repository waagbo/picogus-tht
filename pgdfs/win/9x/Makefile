# PGDFS.VXD, the PGDFS driver for Windows 95 / 98 / ME, built with
# Open Watcom 2.0 (wcc386, wasm, wlink) as in ../../Makefile. The CI runs:
#   make INCLUDE="$WATCOM"/h
#
# The VxD is linked with wlink's "format windows vxd" into locked, preloaded
# objects; fixle.py then applies the LE header fixes the VMM expects (see
# there). No C library is linked: ring 0 code has none.
#
# -mf      flat model            -s       no stack checks
# -zl -zls no default library references
# -ei      enums are ints        -zp4     4-byte structure packing (ifs9x.h packs
#                                         the DOS search record itself)
# -nt -nc  code in _LTEXT (locked code), class CODE

CC      = wcc386
AS      = wasm
LD      = wlink
PYTHON ?= python3
CFLAGS  = -q -bt=windows -mf -s -zl -zls -ox -wx -we -zp4 -ei -nt=_LTEXT -nc=CODE
CORE    = ../common

all: pgdfs.vxd

vxd.obj: vxd.asm
	$(AS) -q -fo=$@ vxd.asm

pgdfs9x.obj: pgdfs9x.c ifs9x.h $(CORE)/pgc.h ../../../common/picogus.h
	$(CC) $(CFLAGS) -fo=$@ pgdfs9x.c

pgc.obj: $(CORE)/pgc.c $(CORE)/pgc.h ../../../common/picogus.h
	$(CC) $(CFLAGS) -fo=$@ $(CORE)/pgc.c

pgdfs.lnk: Makefile
	echo "format windows vxd" > $@
	echo "name pgdfs.vxd" >> $@
	echo "option nodefaultlibs, quiet, offset=0, map=pgdfs.map" >> $@
	echo "option description 'PicoGUS PGDFS file system driver for Windows 95/98/ME'" >> $@
	for s in _LTEXT _LDATA CONST CONST2 _DATA _BSS ZZBSS; do echo "segment $$s preload nondiscardable" >> $@; done
	echo "export PGDFS_DDB.1" >> $@
	echo "file vxd.obj, pgdfs9x.obj, pgc.obj" >> $@

pgdfs.vxd: vxd.obj pgdfs9x.obj pgc.obj pgdfs.lnk fixle.py
	$(LD) @pgdfs.lnk
	$(PYTHON) fixle.py $@

clean:
	rm -f *.obj *.err *.map pgdfs.lnk pgdfs.vxd
