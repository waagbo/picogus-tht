; ===========================================================================
; PGBOOT.COM - load PGBOOT.ROM into RAM and boot through it (experimental)
;
;   PGBOOT [path\PGBOOT.ROM] [/Y] [/C]
;
; For machines without a ROM board. Reads the ROM image (default PGBOOT.ROM
; in the current directory), checks its header and checksum, copies it to
; the top of conventional memory (40:13h lowered by 8 KB, so the image is
; KB and therefore paragraph aligned), tells it where the original BIOS
; INT 13h handler is, calls its init entry (offset 3) and issues INT 19h.
;
; Loader <-> ROM convention (header fields, see defs.inc):
;   0Bh flags, bit 0 (HDRF_RAMCOPY): the image is a writable RAM copy. The
;       ROM then keeps the previous INT 19h in its header (10h) instead of
;       borrowing an interrupt vector, and never writes to itself otherwise.
;   0Ch dword: the INT 13h handler the ROM chains to (0:0 = whatever the
;       vector holds at INT 19h time). The loader stores the BIOS handler
;       from INT 2Fh AH=13h here, so the ROM does not chain into DOS.
;   16h: an IRET, used for INT 1Bh (Ctrl-Break) so the BIOS keyboard handler
;       does not jump into the DOS that is about to be replaced.
;   1Ch dw: offset of the boot catcher (/C only). After init the loader gives INT 19h
;       back to DOS and points INT 13h at the catcher (directly and through
;       INT 2Fh AH=13h ES:BX), then issues INT 19h: DOS restores the vectors
;       it took over and calls the BIOS INT 19h, whose first INT 13h call
;       lands in the catcher, which runs the ROM's late init from there.
; The loader fixes the checksum byte after patching.
;
; Caveat: INT 19h does not undo what DOS and TSRs hooked. Run it from a clean
; boot: no TSRs, no EMM386/QEMM (refused), no HIMEM if possible, STACKS=0,0.
; Copyright (C) 2026 PicoGUS contributors. MIT license.
; ===========================================================================

        cpu     8086
        bits    16
        org     100h

%include "defs.inc"

        section .text

LOAD_KB         equ ROM_SIZE / 1024

start:
        cld
        mov     byte [yes], 0
        mov     byte [catchmode], 0
        mov     dx, s_banner
        call    puts
        call    parse_args

        ; ---- refuse V86 mode (EMM386, QEMM, Windows DOS box) ---------------
        push    sp
        pop     ax
        cmp     ax, sp
        jne     .real                           ; 8086/186: no protected mode
        cpu     286
        smsw    ax
        cpu     8086
        test    al, 1
        jz      .real
        mov     dx, s_v86
        jmp     die
.real:
        ; ---- already there? ----------------------------------------------------
        xor     ax, ax
        mov     es, ax
        mov     es, [es:19h*4+2]
        cmp     word [es:HDR_SIG], 'PG'
        jne     .notyet
        cmp     word [es:HDR_SIG+2], 'BT'
        jne     .notyet
        mov     dx, s_already
        jmp     die
.notyet:
        push    ds
        pop     es

        ; ---- read and check the image ------------------------------------------
        mov     dx, path
        mov     ax, 3D00h
        int     21h
        jnc     .opened
        mov     dx, s_noopen
        call    puts
        mov     si, path
.pn:
        lodsb
        or      al, al
        jz      .pe
        call    putc
        jmp     short .pn
.pe:
        mov     dx, s_crlf
        jmp     die
.opened:
        mov     bx, ax
        mov     dx, image
        mov     cx, ROM_SIZE + 1
        mov     ah, 3Fh
        int     21h
        pushf
        push    ax
        mov     ah, 3Eh
        int     21h
        pop     ax
        popf
        jc      .badsize
        cmp     ax, ROM_SIZE
        je      .sized
.badsize:
        mov     dx, s_badsize
        jmp     die
.sized:
        mov     dx, s_badhdr
        cmp     word [image], 0AA55h
        jne     .dieh
        cmp     byte [image+2], ROM_BLOCKS
        jne     .dieh
        cmp     word [image+HDR_SIG], 'PG'
        jne     .dieh
        cmp     word [image+HDR_SIG+2], 'BT'
        jne     .dieh
        cmp     byte [image+HDR_IFVER], 1
        jne     .dieh
        mov     si, image
        mov     cx, ROM_SIZE
        xor     ah, ah
.sum:
        lodsb
        add     ah, al
        loop    .sum
        or      ah, ah
        jz      .hdrok
        mov     dx, s_badsum
.dieh:
        jmp     die
.hdrok:

        ; ---- hardware interrupt vectors that DOS/TSRs took over ------------------
        call    check_vectors

        ; ---- the BIOS INT 13h handler ------------------------------------------------
        ; INT 2Fh AH=13h sets DS:DX (handler the DOS disk driver calls) and
        ; ES:BX (handler DOS restores at INT 19h) and returns the old pair.
        ; Called twice: once to read, once to put the old values back.
        push    ds
        mov     ax, cs
        mov     es, ax
        mov     dx, dummy13
        mov     bx, dummy13
        mov     ah, 13h
        int     2Fh
        mov     ax, ds
        mov     cx, es
        pop     ds
        mov     [dos13], dx
        mov     [dos13+2], ax
        mov     [halt13], bx
        mov     [halt13+2], cx
        push    ds
        mov     es, cx
        mov     ds, ax
        mov     ah, 13h
        int     2Fh                             ; restore
        pop     ds
        push    ds
        pop     es
        ; pick ES:BX (the original ROM handler), else DS:DX, else the vector
        mov     byte [have2f13], 0
        mov     ax, [halt13]
        mov     dx, [halt13+2]
        call    valid13
        jc      .nohalt
        mov     byte [have2f13], 1
        jmp     short .got13
.nohalt:
        mov     ax, [dos13]
        mov     dx, [dos13+2]
        call    valid13
        jnc     .got13
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:13h*4]
        mov     dx, [es:13h*4+2]
        pop     es
        push    dx
        mov     dx, s_no2f13
        call    puts
        pop     dx
.got13:
        cmp     byte [catchmode], 0
        je      .cm
        cmp     byte [have2f13], 0
        jne     .cm
        mov     dx, s_nocatch
        jmp     die
.cm:
        mov     [bios13], ax
        mov     [bios13+2], dx
        push    dx
        mov     dx, s_int13
        call    puts
        pop     dx
        mov     ax, dx
        call    hex16
        mov     al, ':'
        call    putc
        mov     ax, [bios13]
        call    hex16
        call    crlf

        mov     dx, s_int19
        call    puts
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:19h*4+2]
        call    hex16
        mov     al, ':'
        call    putc
        mov     ax, [es:19h*4]
        call    hex16
        pop     es
        call    crlf

        ; ---- place the image at the top of conventional memory -------------------
        push    es
        mov     ax, 40h
        mov     es, ax
        mov     ax, [es:13h]
        pop     es
        mov     [oldtop], ax
        sub     ax, LOAD_KB
        mov     [newtop], ax
        mov     cl, 6
        shl     ax, cl
        mov     [romseg], ax
        mov     bx, cs
        add     bx, 1000h + 400h                ; our 64 KB + 16 KB margin
        cmp     ax, bx
        ja      .room
        mov     dx, s_nomem
        jmp     die
.room:
        ; patch the copy in our buffer, fix the checksum, then move it
        or      byte [image+HDR_FLAGS], HDRF_RAMCOPY
        mov     ax, [bios13]
        mov     [image+HDR_BIOS13], ax
        mov     ax, [bios13+2]
        mov     [image+HDR_BIOS13+2], ax
        mov     byte [image+ROM_SIZE-1], 0
        mov     si, image
        mov     cx, ROM_SIZE
        xor     ah, ah
.sum2:
        lodsb
        add     ah, al
        loop    .sum2
        neg     ah
        mov     [image+ROM_SIZE-1], ah

        ; keep what DOS has there, in case the ROM does not install and we
        ; return to DOS (the top of memory belongs to its last MCB)
        push    ds
        mov     di, saved
        mov     ds, [romseg]
        xor     si, si
        mov     cx, ROM_SIZE / 2
        rep movsw
        pop     ds
        cli
        mov     es, [romseg]
        xor     di, di
        mov     si, image
        mov     cx, ROM_SIZE / 2
        rep movsw
        mov     ax, 40h
        mov     es, ax
        mov     ax, [newtop]
        mov     [es:13h], ax
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:1Bh*4]
        mov     [old1b], ax
        mov     ax, [es:1Bh*4+2]
        mov     [old1b+2], ax
        mov     word [es:1Bh*4], HDR_IRET
        mov     ax, [romseg]
        mov     [es:1Bh*4+2], ax
        sti
        push    ds
        pop     es

        ; ---- ROM init (as the BIOS would call it at POST) -----------------------------
        mov     word [initptr], 3
        mov     ax, [romseg]
        mov     [initptr+2], ax
        call    far [initptr]
        push    ds
        pop     es
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:19h*4+2]
        cmp     ax, [romseg]
        je      .hooked
        ; not installed (no card, ...): undo
        cli
        mov     ax, [old1b]
        mov     [es:1Bh*4], ax
        mov     ax, [old1b+2]
        mov     [es:1Bh*4+2], ax
        mov     ax, 40h
        mov     es, ax
        mov     ax, [oldtop]
        mov     [es:13h], ax
        sti
        mov     es, [romseg]
        xor     di, di
        mov     si, saved
        mov     cx, ROM_SIZE / 2
        rep movsw
        mov     dx, s_notinst
        jmp     die
.hooked:
        ; Catch mode: INT 19h goes back to DOS (its handler restores the
        ; vectors DOS took over, then calls the BIOS INT 19h), and INT 13h
        ; points at the ROM's boot catcher, which runs the late init when
        ; the BIOS reads the boot sector.
        mov     dx, s_boot
        call    puts
        cmp     byte [catchmode], 0
        jne     .catch
        int     19h                             ; direct: the ROM's late init
        jmp     short .back
.catch:
        mov     es, [romseg]
        mov     ax, [es:HDR_CATCH]
        mov     [catch], ax
        mov     [catch+2], es
        cmp     byte [have2f13], 0
        je      .no2f
        push    ds
        mov     dx, [dos13]
        mov     ds, [dos13+2]
        mov     bx, ax                          ; ES:BX = catcher
        mov     ah, 13h
        int     2Fh                             ; DOS restores INT 13h = catcher
        pop     ds
.no2f:
        mov     es, [romseg]
        mov     ax, [es:HDR_OLD19]
        mov     dx, [es:HDR_OLD19+2]
        xor     bx, bx
        mov     es, bx
        cli
        mov     [es:19h*4], ax
        mov     [es:19h*4+2], dx
        mov     ax, [catch]
        mov     [es:13h*4], ax
        mov     ax, [catch+2]
        mov     [es:13h*4+2], ax
        int     19h
.back:
        ; not reached: INT 19h boots or chains to the BIOS
        mov     ax, 4C03h
        int     21h

; valid13: CF=0 when DX:AX looks like a handler (not 0:0, not our dummy)
valid13:
        push    ax
        or      ax, dx
        pop     ax
        jz      .no
        cmp     ax, dummy13
        jne     .ok
        push    ax
        mov     ax, cs
        cmp     dx, ax
        pop     ax
        je      .no
.ok:
        clc
        ret
.no:
        stc
        ret

dummy13:
        iret

; check_vectors: warn about hardware interrupt vectors that point into
; conventional memory or the HMA (DOS STACKS=, TSRs, HIMEM): after INT 19h
; that memory is overwritten while the vector still points there.
check_vectors:
        mov     si, vec_list
        mov     byte [warned], 0
.next:
        lodsb
        cmp     al, 0FFh
        je      .done
        mov     bl, al
        xor     bh, bh
        shl     bx, 1
        shl     bx, 1
        push    es
        xor     cx, cx
        mov     es, cx
        mov     cx, [es:bx]
        mov     dx, [es:bx+2]
        push    es
        mov     di, 40h
        mov     es, di
        mov     di, [es:13h]
        pop     es
        pop     es
        push    cx
        mov     cl, 6
        shl     di, cl
        pop     cx
        cmp     dx, 0FFFFh
        je      .bad
        cmp     dx, di
        jae     .next
.bad:
        push    dx
        mov     dx, s_vecwarn
        call    puts
        call    hex8
        mov     dx, s_vecat
        call    puts
        pop     ax
        call    hex16
        mov     al, ':'
        call    putc
        mov     ax, cx
        call    hex16
        call    crlf
        mov     byte [warned], 1
        jmp     short .next
.done:
        cmp     byte [warned], 0
        je      .ret
        mov     dx, s_vechelp
        call    puts
        cmp     byte [yes], 0
        jne     .ret
        mov     dx, s_ask
        call    puts
        mov     ah, 08h
        int     21h
        push    ax
        call    putc
        call    crlf
        pop     ax
        or      al, 20h
        cmp     al, 'y'
        je      .ret
        mov     dx, s_abort
        jmp     die
.ret:
        ret

; parse_args: first word = path (default PGBOOT.ROM), /Y = no questions
parse_args:
        mov     si, 81h
        mov     di, path
        mov     byte [di], 0
.skip:
        lodsb
        cmp     al, ' '
        je      .skip
        cmp     al, 9
        je      .skip
        cmp     al, 13
        je      .end
        cmp     al, '/'
        jne     .word
        lodsb
        or      al, 20h
        cmp     al, 'c'
        jne     .noc
        mov     byte [catchmode], 1
        jmp     short .skip
.noc:
        cmp     al, 'y'
        jne     .usage
        mov     byte [yes], 1
        jmp     short .skip
.word:
        cmp     byte [path], 0
        jne     .usage
.cp:
        stosb
        lodsb
        cmp     al, ' '
        je      .wend
        cmp     al, 9
        je      .wend
        cmp     al, 13
        je      .wend
        cmp     di, path + 127
        jb      .cp
.wend:
        mov     byte [di], 0
        dec     si
        jmp     short .skip
.end:
        cmp     byte [path], 0
        jne     .ret
        push    si
        mov     si, defpath
        mov     di, path
.dc:
        lodsb
        stosb
        or      al, al
        jnz     .dc
        pop     si
.ret:
        ret
.usage:
        mov     dx, s_usage
        jmp     die

die:
        call    puts
        mov     ax, 4C01h
        int     21h

puts:
        push    ax
        mov     ah, 09h
        int     21h
        pop     ax
        ret
crlf:
        push    dx
        mov     dx, s_crlf
        call    puts
        pop     dx
        ret
putc:
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     dx
        pop     ax
        ret
hex16:
        push    ax
        mov     al, ah
        call    hex8
        pop     ax
hex8:
        push    ax
        push    cx
        mov     cl, 4
        shr     al, cl
        call    .n
        pop     cx
        pop     ax
        push    ax
        call    .n
        pop     ax
        ret
.n:
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     putc
        add     al, 7
        jmp     putc

s_banner  db "PGBOOT.COM ", PGBOOT_VERSION, " - PicoGUS boot ROM loader (experimental)", 13, 10, "$"
s_usage   db "usage: PGBOOT [path\PGBOOT.ROM] [/Y] [/C]", 13, 10
          db "Loads the ROM image below the top of conventional memory and boots through it.", 13, 10
          db "/Y: do not ask when interrupt vectors are hooked by DOS or TSRs.", 13, 10
          db "/C: catch mode (MS-DOS 3.2+): let DOS's INT 19h restore its vectors first.", 13, 10, "$"
s_nocatch db "/C needs INT 2Fh AH=13h (MS-DOS 3.2 or later); this DOS does not have it.", 13, 10, "$"
s_v86     db "The CPU is in virtual 8086 mode (EMM386, QEMM, Windows?)."
          db " Boot DOS without a memory manager.", 13, 10, "$"
s_already db "PGBOOT is already installed (ROM or an earlier PGBOOT.COM).", 13, 10, "$"
s_noopen  db "Cannot open $"
s_badsize db "The ROM image must be exactly 8192 bytes.", 13, 10, "$"
s_badhdr  db "Not a PGBOOT ROM image (header).", 13, 10, "$"
s_badsum  db "ROM image checksum error.", 13, 10, "$"
s_no2f13  db "Warning: INT 2Fh AH=13h unsupported, chaining to the current INT 13h.", 13, 10, "$"
s_int13   db "BIOS INT 13h handler: $"
s_int19   db "INT 19h handler: $"
s_nomem   db "Not enough conventional memory.", 13, 10, "$"
s_notinst db "PGBOOT did not install (see the message above); nothing changed.", 13, 10, "$"
s_boot    db "Starting the boot (INT 19h)...", 13, 10, "$"
s_vecwarn db "Warning: INT $"
s_vecat   db "h points into DOS/TSR memory at $"
s_vechelp db "The boot overwrites that memory while the vector still points there and may", 13, 10
          db "crash. Boot clean first: no TSRs or drivers that hook hardware interrupts,", 13, 10
          db "STACKS=0,0 in CONFIG.SYS, no HIMEM/EMM386.", 13, 10, "$"
s_ask     db "Continue anyway (y/N)? $"
s_abort   db "Aborted.", 13, 10, "$"
s_crlf    db 13, 10, "$"
defpath   db "PGBOOT.ROM", 0
vec_list  db 08h, 09h, 0Eh, 15h, 1Ch, 70h, 74h, 76h, 0FFh

        section .bss
        alignb  16
image     resb ROM_SIZE + 16
saved     resb ROM_SIZE
path      resb 130
initptr   resd 1
bios13    resd 1
dos13     resd 1
halt13    resd 1
old1b     resd 1
romseg    resw 1
oldtop    resw 1
newtop    resw 1
yes       resb 1
catchmode resb 1
have2f13  resb 1
catch     resd 1
warned    resb 1
