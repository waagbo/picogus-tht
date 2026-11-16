; ===========================================================================
; PGBOOT.ROM - PicoGUS boot ROM (experimental)
;
; An 8 KB x86 option ROM that presents disk image files on the PicoGUS USB
; drive as BIOS drive 00h (floppy image) and 80h (hard disk image) and boots
; from them. The card serves 512-byte sectors over the PGDFS transport
; (sw/bootdisk/PROTOCOL.md, sw/dfs/PROTOCOL.md).
;
;   POST (far call to offset 3): detect the card, hook INT 19h, retf.
;   INT 19h (late init): reserve 1 KB at the top of conventional memory,
;     wait for the USB drive, open the images, hook INT 13h through a RAM
;     trampoline, boot sector 0 of the floppy (or hard disk) image.
;   INT 13h: serve 00h / 80h, shift physical hard disks up by one.
;
; 8086 code throughout, except out186/in186 (rep outsw/insw), which only run
; when the CPU check found an 80186 or later. Build: see Makefile.
; Copyright (C) 2026 PicoGUS contributors. MIT license.
; ===========================================================================

        cpu     8086
        bits    16
        org     0

%include "defs.inc"

        section .text

; ---- ROM header -----------------------------------------------------------
rom_start:
        db      55h, 0AAh, ROM_BLOCKS
        jmp     strict near post_init           ; 03h: init entry (far call)
        db      'PGBT'                          ; 06h: signature (loader)
        db      1                               ; 0Ah: loader interface version
        db      0                               ; 0Bh: flags, HDRF_RAMCOPY set by the loader
        dd      0                               ; 0Ch: original BIOS INT 13h (loader)
        dd      0                               ; 10h: previous INT 19h (RAM copy only)
        dw      code_end                        ; 14h: bytes used
        iret                                    ; 16h: IRET for the loader
        db      0                               ; 17h
        dw      0                               ; 18h: no PCI data structure
        dw      0                               ; 1Ah: no PnP header
        dw      int13_catch                     ; 1Ch: boot catcher (loader)
        db      "PGBOOT ", PGBOOT_VERSION, " PicoGUS boot ROM", 0

%if HDR_SIG != 6 || HDR_BIOS13 != 0Ch || HDR_CATCH != 1Ch
%error header layout
%endif

; ===========================================================================
; POST init: far call from the BIOS option ROM scan. Preserves all registers.
; ===========================================================================
post_init:
        pushf
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        cld
        mov     si, msg_banner
        call    print
        call    detect_card
        jnc     .found
        call    print
        or      ah, ah
        jnz     .nohook                         ; answers, but old/disabled: stay out
        test    byte [cs:HDR_FLAGS], HDRF_RAMCOPY
        jnz     .nohook                         ; PGBOOT.COM: the card had time
        ; nothing answers: the card may still be starting (fast POST);
        ; hook INT 19h anyway, the late init looks again
        mov     si, msg_later
        call    print
        jmp     short .hook19
.found:
        mov     si, msg_found
        call    print
        mov     ax, bx
        call    print_hex
        call    detect_cpu
        mov     si, msg_cpu86
        or      al, al
        jz      .c
        mov     si, msg_cpu186
.c:
        call    print
.hook19:
        ; hook INT 19h (once); interrupts stay off until the popf below
        xor     ax, ax
        mov     ds, ax
        mov     dx, cs
        cli
        cmp     word [19h*4], int19_entry
        jne     .hook
        cmp     [19h*4+2], dx
        je      .hooked
.hook:
        mov     ax, [19h*4]
        mov     bx, [19h*4+2]
        test    byte [cs:HDR_FLAGS], HDRF_RAMCOPY
        jz      .inrom
        mov     [cs:HDR_OLD19], ax              ; writable copy (PGBOOT.COM)
        mov     [cs:HDR_OLD19+2], bx
        jmp     short .set
.inrom:
        mov     [OLD19_VEC*4], ax               ; never write to the ROM itself:
        mov     [OLD19_VEC*4+2], bx             ; an unprotected EEPROM would take it
.set:
        mov     word [19h*4], int19_entry
        mov     [19h*4+2], dx
.hooked:
        jmp     short .done
.nohook:
        call    crlf
.done:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        popf
        retf

; get_old19: DX:AX = the INT 19h handler that was there before us
get_old19:
        test    byte [cs:HDR_FLAGS], HDRF_RAMCOPY
        jz      .ivt
        mov     ax, [cs:HDR_OLD19]
        mov     dx, [cs:HDR_OLD19+2]
        ret
.ivt:
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, [OLD19_VEC*4]
        mov     dx, [OLD19_VEC*4+2]
        pop     ds
        ret

; ===========================================================================
; INT 19h: late init and boot
; ===========================================================================
int19_entry:
        sti
        cld
        call    detect_card
        jnc     .card
        or      ah, ah
        jnz     .gone
        ; nothing answers: give a card that is still starting ~2 s
        push    es
        xor     ax, ax
        mov     es, ax
        mov     di, 2 * TICKS_PER_SEC
        mov     bp, [es:046Ch]
.retry:
        call    detect_card
        jnc     .late
        or      ah, ah
        jnz     .late
        mov     ax, [es:046Ch]
        cmp     ax, bp
        je      .retry
        mov     bp, ax
        dec     di
        jnz     .retry
        stc
.late:
        pop     es
        jnc     .card
.gone:
        ; card gone (or disabled since POST): unhook and continue
        call    print_pfx
        call    print
        call    crlf
        test    byte [cs:HDR_FLAGS], HDRF_CATCH
        jz      .nc
        jmp     catch_chain
.nc:
        call    get_old19
        xor     bx, bx
        mov     ds, bx
        cli
        mov     [19h*4], ax
        mov     [19h*4+2], dx
        sti
        push    dx
        push    ax
        retf

.card:
        push    ax                              ; options
        push    bx                              ; data port
        push    cx                              ; max payload
        ; ---- RAM: reuse our block if INT 13h already points at it ---------
        xor     ax, ax
        mov     ds, ax
        les     di, [13h*4]
        cmp     di, r_tramp13
        jne     .fresh
        cmp     word [es:r_sig], 'PG'
        jne     .fresh
        cmp     word [es:r_sig+2], 'BT'
        jne     .fresh
        mov     ax, cs
        cmp     [es:r_romseg], ax
        jne     .fresh
        push    es
        pop     ds
        call    uninstall
        mov     byte [r_fresh], 0
        jmp     short .ram
.fresh:
        mov     ax, 40h
        mov     ds, ax
        dec     word [13h]
        mov     ax, [13h]
        mov     cl, 6
        shl     ax, cl
        mov     es, ax
        xor     di, di
        xor     ax, ax
        mov     cx, 512
        rep stosw
        call    get_old19
        mov     [es:r_old19], ax
        mov     [es:r_old19+2], dx
        push    es
        pop     ds
        mov     byte [r_fresh], 1
.ram:
        ; DS = RAM
        pop     cx
        pop     bx
        pop     ax
        mov     word [r_sig], 'PG'
        mov     word [r_sig+2], 'BT'
        mov     [r_romseg], cs
        mov     [r_port], bx
        mov     [r_opts], al
        mov     ax, cx
        mov     al, ah
        shr     al, 1                           ; maxlen / 512
        mov     [r_maxrd], al
        sub     cx, BD_IO_HDR_LEN               ; the BD header in the payload
        mov     al, ch
        shr     al, 1                           ; (maxlen - 10) / 512
        mov     [r_maxwr], al
        call    detect_cpu
        mov     [r_cpu], al
        ; INT 13h to chain to: the loader's value, else the current vector
        mov     ax, [cs:HDR_BIOS13]
        mov     dx, [cs:HDR_BIOS13+2]
        mov     bx, ax
        or      bx, dx
        jnz     .have13
        xor     bx, bx
        mov     es, bx
        mov     ax, [es:13h*4]
        mov     dx, [es:13h*4+2]
.have13:
        mov     [r_old13], ax
        mov     [r_old13+2], dx
        ; trampoline: push cs / push cs / jmp far ROM:int13_entry
        mov     word [r_tramp13], 0E0Eh
        mov     byte [r_tramp13+2], 0EAh
        mov     word [r_tramp13+3], int13_entry
        mov     [r_tramp13+5], cs
        ; own stack at the top of the block (the BIOS boot stack can be tiny)
        cli
        mov     [r_oldss], ss
        mov     [r_oldsp], sp
        mov     ax, ds
        mov     ss, ax
        mov     sp, RAM_STACK_TOP
        sti

        ; ---- anything configured? ------------------------------------------
        mov     al, CMD_BDFDNAME
        call    rd8
        mov     ah, al
        mov     al, CMD_BDHDNAME
        call    rd8
        mov     byte [r_probe], 1               ; unit that tells us about the drive:
        or      ah, ah                          ; the floppy if configured,
        jz      .p1                             ; else the hard disk
        mov     byte [r_probe], 0
.p1:
        or      al, ah
        jnz     .configured
        mov     si, msg_noimg
        jmp     chain19_msg
.configured:
        ; ---- wait for the USB drive ------------------------------------------
        ; not mounted: CMD_DFSSTAT reads NODRIVE, or (a transaction left
        ; READY) BDINFO answers state 4. Checked once per tick, Esc skips.
        mov     byte [r_shown], 0
        mov     word [r_count], WAIT_SECONDS
        xor     ax, ax
        mov     es, ax
        mov     si, [es:046Ch]
        mov     di, TICKS_PER_SEC
.wl:
        call    is_mounted
        jnc     .gotdrive
        cmp     byte [r_shown], 0
        jne     .spin
        mov     byte [r_shown], 1
        call    print_pfx
        push    si
        mov     si, msg_wait
        call    print
        pop     si
        call    print_count
.spin:
        mov     ah, 1                           ; peek; only Esc is taken, other
        int     16h                             ; keys (F5/F8 for DOS) stay queued
        jz      .nokey
        cmp     al, 1Bh
        jne     .nokey
        xor     ah, ah
        int     16h
        call    crlf
        mov     si, msg_skipped
        jmp     chain19_msg
.nokey:
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:046Ch]
        cmp     ax, si
        je      .spin
        mov     si, ax
        dec     di
        jnz     .wl
        mov     di, TICKS_PER_SEC
        dec     word [r_count]
        jz      .nodrive
        mov     al, 8
        call    putc
        call    putc
        call    print_count
        jmp     short .wl
.nodrive:
        call    crlf
        mov     si, msg_nodrive
        jmp     chain19_msg
.gotdrive:
        cmp     byte [r_shown], 0
        je      .mounted
        call    crlf
.mounted:
        ; ---- open and present the images -------------------------------------
        mov     byte [r_units], 0
        mov     di, u_fd
        mov     ax, 0                           ; unit 0
        mov     bl, 'A'
        call    do_unit
        jc      .nofd
        or      byte [r_units], 1
.nofd:
        mov     di, u_hd
        mov     ax, 1
        mov     bl, 'C'
        call    do_unit
        jc      .nohd
        or      byte [r_units], 2
.nohd:
        test    byte [r_units], 3
        jnz     .install
        mov     si, msg_noready
        jmp     chain19_msg

.install:
        mov     ax, 40h
        mov     es, ax
        mov     ax, [es:10h]
        mov     [r_equip], ax
        mov     al, [es:75h]
        mov     [r_hdcnt], al
        add     al, 80h
        mov     [r_hdmax], al
        test    byte [r_units], 1
        jz      .eq_ok
        test    byte [es:10h], 1
        jnz     .eq_ok
        mov     ax, [es:10h]
        and     al, 03Fh                        ; one drive
        or      al, 01h                         ; floppy drives present
        mov     [es:10h], ax
.eq_ok:
        test    byte [r_units], 2
        jz      .hd_ok
        inc     byte [es:75h]
.hd_ok:
        call    build_dpt
        xor     ax, ax
        mov     es, ax
        cli
        mov     word [es:13h*4], r_tramp13
        mov     [es:13h*4+2], ds
        sti

        ; ---- boot ---------------------------------------------------------------
        mov     dl, 00h
        test    byte [r_units], 1
        jz      .hdfirst
        test    byte [r_units], 2
        jz      .order
        test    byte [r_opts], BD_OPT_BOOT_HD
        jz      .order
.hdfirst:
        mov     dl, 80h
.order:
        mov     [r_bootdrv], dl
        call    try_boot                        ; returns only on failure
        mov     dl, [r_bootdrv]
        xor     dl, 80h                         ; the other one, if present
        mov     al, 1
        or      dl, dl
        jz      .t2
        mov     al, 2
.t2:
        test    [r_units], al
        jz      .nothing
        call    try_boot
.nothing:
        mov     si, msg_bootbios
        ; fall through: the images stay installed, the BIOS boots through them

; chain19_msg: print SI (prefixed), give back the memory when nothing was
; installed, restore the caller's stack and continue with the old INT 19h.
chain19_msg:
        call    print_pfx
        call    print
        call    crlf
        test    byte [r_units], 3
        jnz     .keep
        ; nothing installed: unhook INT 19h and free the block if ours
        xor     ax, ax
        test    byte [cs:HDR_FLAGS], HDRF_CATCH
        jnz     .freemem                        ; the loader unhooked INT 19h
        mov     es, ax
        mov     ax, [r_old19]
        mov     dx, [r_old19+2]
        cli
        mov     [es:19h*4], ax
        mov     [es:19h*4+2], dx
        sti
.freemem:
        cmp     byte [r_fresh], 1
        jne     .keep
        mov     ax, 40h
        mov     es, ax
        mov     ax, ds
        mov     cl, 6
        shr     ax, cl
        cmp     ax, [es:13h]
        jne     .keep
        inc     word [es:13h]
        mov     word [r_sig], 0
.keep:
        mov     ax, [r_old19]
        mov     dx, [r_old19+2]
        cli
        mov     ss, [r_oldss]
        mov     sp, [r_oldsp]
        sti
        test    byte [cs:HDR_FLAGS], HDRF_CATCH
        jnz     catch_chain
        push    dx
        push    ax
        retf

; ---------------------------------------------------------------------------
; Boot catcher (RAM copy only). PGBOOT.COM points INT 13h here (directly and
; as the handler DOS restores at INT 19h, INT 2Fh AH=13h) and then lets DOS
; run its own INT 19h, which puts back the vectors DOS took over (STACKS=,
; ...). The BIOS INT 19h then reads the boot sector through INT 13h and lands
; here with a clean vector table: undo the catch and run the late init. If
; that does not boot, the BIOS's INT 13h call is passed on (catch_chain) to
; whatever INT 13h is now (our handler when images were installed), so the
; BIOS boot simply continues.
; ---------------------------------------------------------------------------
int13_catch:
        push    ax
        push    bx
        push    cx
        push    dx
        push    bp
        push    si
        push    di
        push    ds
        push    es
        xor     ax, ax
        mov     ds, ax
        mov     ax, [cs:HDR_BIOS13]
        mov     dx, [cs:HDR_BIOS13+2]
        cli
        mov     [13h*4], ax
        mov     [13h*4+2], dx
        sti
        or      byte [cs:HDR_FLAGS], HDRF_CATCH
        jmp     int19_entry

; catch_chain: SS:SP at the registers int13_catch saved: pass the BIOS's
; INT 13h call on to the current INT 13h vector
catch_chain:
        xor     ax, ax
        mov     ds, ax
        mov     ax, [13h*4]
        mov     [cs:HDR_OLD19], ax              ; (free in catch mode)
        mov     ax, [13h*4+2]
        mov     [cs:HDR_OLD19+2], ax
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     bp
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        jmp     far [cs:HDR_OLD19]

; is_mounted: CF=0 when the USB drive is mounted: CMD_DFSSTAT is not
; NODRIVE and BDINFO (no OPEN) for unit [r_probe] does not answer state 4.
; Clobbers AX, CX, DX (not SI, DI, ES).
is_mounted:
        mov     al, CMD_DFSSTAT
        call    rd8
        cmp     al, DFS_NODRIVE
        je      .no
        mov     al, [r_probe]
        xor     ah, ah
        call    bdinfo
        jc      .no
        cmp     byte [x_ans+BI_STATE], BD_STATE_NODRIVE
        je      .no
        clc
        ret
.no:
        stc
        ret

; print_count: [r_count] as two digits
print_count:
        push    ax
        mov     ax, [r_count]
        aam
        xchg    al, ah
        add     ax, '00'
        call    putc
        xchg    al, ah
        call    putc
        pop     ax
        ret

; do_unit: BDINFO with OPEN for card unit AL into the unit structure at DI,
; print what it is. BL = drive letter. CF=0: ready and installed.
do_unit:
        mov     [di+U_UNIT], al
        mov     ah, BD_INFO_OPEN
        push    bx
        call    bdinfo
        pop     bx
        jc      .xerr
        cmp     byte [x_ans+BI_STATE], 0
        je      .silent                         ; nothing configured for it
        call    unit_load
        pushf
        call    print_pfx
        mov     al, bl
        call    putc
        mov     al, ':'
        call    putc
        mov     al, ' '
        call    putc
        call    print_ansname
        popf
        jc      .reason
        mov     si, msg_comma
        call    print
        mov     ax, [di+U_TOTAL]
        mov     dx, [di+U_TOTAL+2]
        call    print_size
        test    byte [di+U_FLAGS], BIF_RO
        jz      .rw
        mov     si, msg_ro
        call    print
.rw:
        call    crlf
        clc
        ret
.reason:
        mov     al, [x_ans+BI_STATE]
        mov     si, msg_st_nf
        cmp     al, 2
        je      .r
        mov     si, msg_st_bad
        cmp     al, 3
        je      .r
        cmp     al, 1
        je      .r
        mov     si, msg_st_nousb
        cmp     al, 4
        je      .r
        mov     si, msg_st_frag
        cmp     al, BD_STATE_FRAG
        je      .r
        mov     si, msg_st_nr
.r:
        call    print
        call    crlf
        stc
        ret
.xerr:
        push    ax
        call    print_pfx
        mov     al, bl
        call    putc
        mov     si, msg_xerr
        call    print
        pop     ax
        mov     al, ah
        call    print_hex8
        mov     al, 'h'
        call    putc
        call    crlf
.silent:
        stc
        ret

; try_boot: read sector 0 of drive DL (00h/80h) to 0000:7C00 through our own
; code and jump to it. Returns on failure (message printed).
try_boot:
        mov     byte [r_isfd], 1
        mov     word [r_cur], u_fd
        mov     bl, 'A'
        or      dl, dl
        jz      .u
        mov     byte [r_isfd], 0
        mov     word [r_cur], u_hd
        mov     bl, 'C'
.u:
        push    dx
        push    bx
        xor     ax, ax
        mov     [io_lba], ax
        mov     [io_lba+2], ax
        mov     [io_seg], ax
        mov     word [io_off], 7C00h
        mov     word [io_count], 1
        mov     byte [io_op], IO_READ
        call    disk_io
        pop     bx
        pop     dx
        call    print_pfx
        mov     al, bl
        or      ah, ah
        jz      .read
        push    ax
        call    putc
        mov     si, msg_rderr
        call    print
        pop     ax
        mov     al, ah
        call    print_hex8
        mov     al, 'h'
        call    putc
        call    crlf
        ret
.read:
        xor     cx, cx
        mov     es, cx
        cmp     word [es:7DFEh], 0AA55h
        je      .go
        or      dl, dl
        jnz     .nosig
        mov     ah, [es:7C00h]                  ; floppies without a signature:
        cmp     ah, 0EBh                        ; accept a jump
        je      .go
        cmp     ah, 0E9h
        je      .go
.nosig:
        call    putc
        mov     si, msg_nosig
        call    print
        call    crlf
        ret
.go:
        push    ax
        mov     si, msg_booting
        call    print
        pop     ax
        call    putc
        mov     al, ':'
        call    putc
        call    crlf
        cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 7C00h
        mov     ds, ax
        mov     es, ax
        sti
        xor     dh, dh
        xor     bx, bx
        xor     cx, cx
        xor     si, si
        xor     di, di
        xor     bp, bp
        jmp     0000h:7C00h

; uninstall: undo a previous late init (INT 19h ran again). DS = RAM.
uninstall:
        xor     ax, ax
        mov     es, ax
        mov     ax, [r_old13]
        mov     dx, [r_old13+2]
        cli
        mov     [es:13h*4], ax
        mov     [es:13h*4+2], dx
        sti
        mov     ax, 40h
        mov     es, ax
        test    byte [r_units], 1
        jz      .1
        mov     ax, [r_equip]
        mov     [es:10h], ax
.1:
        test    byte [r_units], 2
        jz      .2
        mov     al, [r_hdcnt]
        mov     [es:75h], al
.2:
        mov     byte [r_units], 0
        mov     byte [r_fdchg], 0
        ret

; build_dpt: diskette parameter table for AH=08h/18h, SPT from the image
build_dpt:
        push    si
        push    di
        push    es
        push    ds
        pop     es
        mov     si, dpt_template
        mov     di, r_dpt
        mov     cx, 11
.c:
        cs lodsb
        stosb
        loop    .c
        mov     al, [u_fd+U_SPT]
        mov     [r_dpt+4], al
        pop     es
        pop     di
        pop     si
        ret

dpt_template:
        db      0DFh, 02h, 25h, 02h, 12h, 1Bh, 0FFh, 6Ch, 0F6h, 0Fh, 08h

%include "xport.inc"
%include "int13.inc"
%include "print.inc"

; ---- messages -----------------------------------------------------------------
msg_banner      db "PGBOOT ", PGBOOT_VERSION, ": ", 0
msg_found       db "PicoGUS found, data port ", 0
%ifdef FORCE8088
msg_cpu86       db "h (8088 forced)", 13, 10, 0
%else
msg_cpu86       db "h (8088)", 13, 10, 0
%endif
msg_cpu186      db "h (186+)", 13, 10, 0
msg_nocard      db "PicoGUS not found", 0
msg_oldfw       db "PicoGUS firmware without PGBOOT support", 0
msg_nodfs       db "PGDFS disabled or unsupported on the card", 0
msg_romoff      db "disabled (pgusinit /bdopts bit 2)", 0
msg_pfx         db "PGBOOT: ", 0
msg_crlf        db 13, 10, 0
msg_noimg       db "no disk image configured", 0
msg_wait        db "waiting for the USB drive (Esc skips)... ", 0
msg_skipped     db "skipped", 0
msg_nodrive     db "no USB drive", 0
msg_noready     db "no image ready", 0
msg_bootbios    db "continuing with the BIOS boot", 0
msg_comma       db ", ", 0
msg_ro          db " (read-only)", 0
msg_st_nf       db " - file not found", 0
msg_st_bad      db " - unusable image", 0
msg_st_nousb    db " - no USB drive", 0
msg_st_nr       db " - not ready", 0
msg_st_frag     db " - too fragmented (copy it to a freshly formatted drive)", 0
msg_later       db ", will look again at boot", 13, 10, 0
msg_xerr        db ": card error ", 0
msg_rderr       db ": boot sector read error ", 0
msg_nosig       db ": not bootable (no 55AAh signature)", 0
msg_booting     db "booting ", 0

code_end:

%if ram_vars_end > RAM_STACK_TOP - 256
%error RAM variables leave too little stack
%endif

        times   ROM_SIZE - 1 - ($ - $$) db 0
        db      0                               ; checksum, set by mkrom.py
