; ===========================================================================
; PGFLASH.COM - write a ROM image into a 28C64 / 28C256 EEPROM (experimental)
;
;   PGFLASH <segment> <file> [/256] [/NOSDP] [/BYTE] [/Y]
;   e.g. PGFLASH D000 PGBOOT.ROM
;
;   /256    SDP command addresses of a 28C256 (5555h/2AAAh; needs the
;           EEPROM's whole 32 KB decoded at the segment). Default: 28C64
;           (1555h/0AAAh, inside the 8 KB window).
;   /NOSDP  plain writes, no software data protection sequence
;   /BYTE   byte writes (EEPROMs without page mode); default 64-byte pages
;   /Y      do not ask for confirmation
;
; Every page (or byte) is preceded by the SDP unlock (AAh, 55h, A0h), which
; also leaves the chip protected afterwards. Pages that already hold the
; right data are skipped. The chip's data polling (bit 7) ends each write
; cycle (interrupts stay off from the unlock to the end of the cycle);
; everything is verified at the end. Nothing is written before the user
; confirms. Refuses segments that read back as RAM (shadow RAM, UMBs) and
; reports writes that do not stick (write protect jumper, wrong SDP variant,
; shadowing). Warns about other option ROMs in the same 32 KB window.
; Copyright (C) 2026 PicoGUS contributors. MIT license.
; ===========================================================================

        cpu     8086
        bits    16
        org     100h

%include "defs.inc"

        section .text

PAGE_SIZE       equ 64
MAX_SIZE        equ 32768

start:
        cld
        xor     ax, ax
        mov     [opt_256], al
        mov     [opt_nosdp], al
        mov     [opt_byte], al
        mov     [opt_yes], al
        mov     [romseg], ax
        mov     [fname], al
        mov     dx, s_banner
        call    puts
        call    parse_args

        ; ---- V86 mode: the ROM area may be remapped ------------------------------
        push    sp
        pop     ax
        cmp     ax, sp
        jne     .real
        cpu     286
        smsw    ax
        cpu     8086
        test    al, 1
        jz      .real
        mov     dx, s_v86
        jmp     die
.real:
        ; ---- refuse to rewrite a PGBOOT that is running from that EEPROM ---------
        xor     ax, ax
        mov     es, ax
        mov     ax, [romseg]
        cmp     [es:19h*4+2], ax
        je      .active
        les     di, [es:13h*4]
        cmp     di, r_tramp13
        jne     .inactive
        cmp     word [es:r_sig], 'PG'
        jne     .inactive
        cmp     word [es:r_sig+2], 'BT'
        jne     .inactive
        cmp     [es:r_romseg], ax
        jne     .inactive
.active:
        mov     dx, s_active
        jmp     die
.inactive:
        push    ds
        pop     es
        ; ---- read the image --------------------------------------------------------
        mov     dx, fname
        mov     ax, 3D00h
        int     21h
        jnc     .op
        mov     dx, s_noopen
        jmp     die
.op:
        mov     bx, ax
        mov     dx, image
        mov     cx, MAX_SIZE
        mov     ah, 3Fh
        int     21h
        pushf
        push    ax
        mov     ah, 3Eh
        int     21h
        pop     ax
        popf
        jc      .bads
        mov     [size], ax
        test    ax, 511
        jnz     .bads
        or      ax, ax
        jnz     .sized
.bads:
        mov     dx, s_badsize
        jmp     die
.sized:
        ; a ROM image with a bad checksum is refused
        cmp     word [image], 0AA55h
        jne     .nohdr
        mov     si, image
        mov     cx, [size]
        xor     ah, ah
.sum:
        lodsb
        add     ah, al
        loop    .sum
        or      ah, ah
        jz      .nohdr
        mov     dx, s_badsum
        jmp     die
.nohdr:
        ; the image must fit below F000h
        mov     ax, [size]
        mov     cl, 4
        shr     ax, cl
        add     ax, [romseg]
        cmp     ax, 0F000h
        jbe     .fits
        mov     dx, s_badseg
        jmp     die
.fits:
        ; ---- what is there now? ------------------------------------------------------
        mov     es, [romseg]
        mov     dx, s_at
        call    puts
        mov     ax, es
        call    hex16
        mov     dx, s_colon
        call    puts
        cmp     word [es:0], 0AA55h
        jne     .empty
        mov     dx, s_hasrom
        call    puts
        mov     al, [es:2]
        xor     ah, ah
        shr     ax, 1
        call    dec16
        mov     dx, s_kb
        call    puts
        jmp     short .ramtest
.empty:
        mov     dx, s_norom
        call    puts

        ; ---- other option ROMs in the same 32 KB window: a 28C256 or a board
        ; that decodes 32 KB may hold them in the same chip -------------------
        call    scan_window

        ; ---- confirm ----------------------------------------------------------------
        cmp     byte [opt_yes], 0
        jne     .go
        mov     dx, s_confirm1
        call    puts
        mov     ax, [size]
        xor     dx, dx
        call    dec16
        mov     dx, s_confirm2
        call    puts
        mov     ax, es
        call    hex16
        mov     dx, s_confirm3
        call    puts
        mov     ah, 08h
        int     21h
        push    ax
        call    putc
        call    crlf
        pop     ax
        or      al, 20h
        cmp     al, 'y'
        je      .go
        mov     dx, s_abort
        jmp     die
.go:
        ; ---- RAM test: a write that reads back at once is RAM, not an EEPROM
        ; (an EEPROM in its write cycle returns the inverted bit 7) -------------
.ramtest:  ; (only after the confirmation: it writes)
        cli
        mov     al, [es:0]
        mov     ah, al
        not     al
        mov     [es:0], al
        mov     bl, [es:0]
        sti
        cmp     bl, al
        jne     .notram
        mov     [es:0], ah                      ; put it back
        mov     dx, s_isram
        jmp     die
.notram:
        call    wait_idle                       ; an unprotected chip is now writing

        ; ---- program ----------------------------------------------------------------
        mov     dx, s_writing
        call    puts
        mov     bx, PAGE_SIZE
        cmp     byte [opt_byte], 0
        je      .ps
        mov     bx, 1
.ps:
        mov     [pagesz], bx
        xor     di, di                          ; offset in the chip
.page:
        cmp     di, [size]
        jae     .verify
        ; skip pages that are already right
        mov     si, image
        add     si, di
        push    di
        mov     cx, [pagesz]
        repe cmpsb
        pop     di
        je      .next
        mov     si, image
        add     si, di
        mov     cx, [pagesz]
        cli                                     ; no interrupt handler may touch
        call    sdp_unlock                      ; the bus until the cycle ends
.wr:
        lodsb
        mov     [es:di], al
        inc     di
        loop    .wr
        dec     di
        call    poll_done                       ; data polling on the last byte
        sti
        inc     di
        jnc     .next0
        mov     dx, s_timeout
        call    puts
        mov     ax, di
        dec     ax
        call    hex16
        mov     dx, s_hint
        jmp     die
.next:
        add     di, [pagesz]
        jmp     short .prog
.next0:
.prog:
        test    di, 1023
        jnz     .page
        mov     al, '.'
        call    putc
        jmp     short .page

        ; ---- verify -----------------------------------------------------------------
.verify:
        call    crlf
        mov     si, image
        xor     di, di
        mov     cx, [size]
.v:
        lodsb
        cmp     al, [es:di]
        jne     .vbad
        inc     di
        loop    .v
        mov     dx, s_ok
        call    puts
        call    range
        mov     dx, s_ok2
        call    puts
        call    range
        mov     dx, s_ok3
        call    puts
        mov     ax, 4C00h
        int     21h
.vbad:
        push    ax
        mov     dx, s_vfail
        call    puts
        mov     ax, di
        call    hex16
        mov     dx, s_vwrote
        call    puts
        pop     ax
        call    hex8
        mov     dx, s_vread
        call    puts
        mov     al, [es:di]
        call    hex8
        call    crlf
        mov     dx, s_hint
        jmp     die

; range: prints "D000-D1FF" for the programmed area
range:
        mov     ax, [romseg]
        call    hex16
        mov     al, '-'
        call    putc
        mov     ax, [size]
        mov     cl, 4
        shr     ax, cl
        add     ax, [romseg]
        dec     ax
        jmp     hex16

; sdp_unlock: the software data protection sequence (interrupts off)
sdp_unlock:
        cmp     byte [opt_nosdp], 0
        jne     .r
        push    bx
        mov     bx, 1555h
        mov     dx, 0AAAh
        cmp     byte [opt_256], 0
        je      .a
        mov     bx, 5555h
        mov     dx, 2AAAh
.a:
        mov     byte [es:bx], 0AAh
        xchg    bx, dx
        mov     byte [es:bx], 55h
        xchg    bx, dx
        mov     byte [es:bx], 0A0h
        pop     bx
.r:
        ret

; poll_done: wait until ES:DI reads back the image byte (write cycle over).
; Runs with interrupts off, so the timeout is a read count: 4 x 65536 reads
; of the chip (>= 0.7 us each: ~180 ms or more; a write cycle takes <= 10 ms).
; CF=1 on timeout.
poll_done:
        push    ax
        push    bx
        push    cx
        push    si
        mov     si, image
        add     si, di
        mov     bl, [si]                        ; expected
        mov     bh, 4
        xor     cx, cx
.p:
        mov     al, [es:di]
        cmp     al, bl
        jne     .busy
        mov     al, [es:di]                     ; twice: data polling can
        cmp     al, bl                          ; glitch on the last read
        je      .ok
.busy:
        loop    .p
        dec     bh
        jnz     .p
        stc
        jmp     short .ret
.ok:
        clc
.ret:
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

; scan_window: warn about option ROM signatures in the 32 KB window around
; the target that lie outside the range about to be written (reads only)
scan_window:
        push    es
        mov     bx, [romseg]
        and     bx, 0F800h                      ; 32 KB aligned
        mov     cx, 16                          ; 2 KB steps
        mov     ax, [size]
        push    cx
        mov     cl, 4
        shr     ax, cl
        pop     cx
        add     ax, [romseg]
        mov     [endseg], ax
.s:
        cmp     bx, [romseg]
        jb      .chk
        cmp     bx, [endseg]
        jb      .nx                             ; our own range
.chk:
        mov     es, bx
        cmp     word [es:0], 0AA55h
        jne     .nx
        mov     dx, s_other
        call    puts
        mov     ax, bx
        call    hex16
        mov     dx, s_other2
        call    puts
.nx:
        add     bx, 80h
        loop    .s
        pop     es
        ret

; wait_idle: wait until ES:0 reads the same twice in a row (~20 ms max)
wait_idle:
        push    cx
        mov     cx, 0FFFFh
.w:
        mov     al, [es:0]
        cmp     al, [es:0]
        je      .d
        loop    .w
.d:
        pop     cx
        ret

; parse_args: <segment> <file> [options]
parse_args:
        mov     si, 81h
.tok:
        lodsb
        cmp     al, ' '
        je      .tok
        cmp     al, 9
        je      .tok
        cmp     al, 13
        je      .end
        cmp     al, '/'
        je      .opt
        cmp     word [romseg], 0
        jne     .file
        ; segment: hex digits
        dec     si
        xor     bx, bx
.hx:
        lodsb
        call    hexdig
        jc      .hend
        mov     cl, 4
        shl     bx, cl
        or      bl, al
        jmp     short .hx
.hend:
        dec     si
        mov     [romseg], bx
        test    bx, 7Fh                         ; 2 KB boundary
        jnz     .badseg
        cmp     bx, 0C800h
        jb      .badseg
        cmp     bx, 0EE00h
        ja      .badseg
        jmp     short .tok
.badseg:
        mov     dx, s_badseg
        jmp     die
.file:
        cmp     byte [fname], 0
        jne     .usage
        mov     di, fname
.fc:
        stosb
        lodsb
        cmp     al, ' '
        je      .fe
        cmp     al, 9
        je      .fe
        cmp     al, 13
        je      .fe
        cmp     di, fname + 126
        jb      .fc
.fe:
        mov     byte [di], 0
        dec     si
        jmp     short .tok
.opt:
        mov     di, si
        mov     bx, opts
.o1:
        mov     si, di
        mov     cx, [bx]                        ; option text
        jcxz    .usage
        push    bx
        mov     bx, cx
.o2:
        mov     ah, [bx]
        or      ah, ah
        jz      .match
        lodsb
        cmp     al, 'a'
        jb      .up
        sub     al, 20h
.up:
        cmp     al, ah
        jne     .nom
        inc     bx
        jmp     short .o2
.nom:
        pop     bx
        add     bx, 4
        jmp     short .o1
.match:
        pop     bx
        mov     bx, [bx+2]
        mov     byte [bx], 1
        jmp     .tok
.end:
        cmp     word [romseg], 0
        je      .usage
        cmp     byte [fname], 0
        je      .usage
        ret
.usage:
        mov     dx, s_usage
        jmp     die

; hexdig: AL ASCII -> AL value, CF=1 if not a hex digit
hexdig:
        cmp     al, '0'
        jb      .no
        cmp     al, '9'
        jbe     .d
        or      al, 20h
        cmp     al, 'a'
        jb      .no
        cmp     al, 'f'
        ja      .no
        sub     al, 'a' - 10
        clc
        ret
.d:
        sub     al, '0'
        clc
        ret
.no:
        stc
        ret

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
; dec16: AX unsigned decimal
dec16:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx, 10
        xor     cx, cx
.d:
        xor     dx, dx
        div     bx
        push    dx
        inc     cx
        or      ax, ax
        jnz     .d
.o:
        pop     ax
        add     al, '0'
        call    putc
        loop    .o
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

opts:
        dw      o_256, opt_256
        dw      o_nosdp, opt_nosdp
        dw      o_byte, opt_byte
        dw      o_y, opt_yes
        dw      0
o_256   db "256", 0
o_nosdp db "NOSDP", 0
o_byte  db "BYTE", 0
o_y     db "Y", 0

s_banner   db "PGFLASH ", PGBOOT_VERSION, " - 28C64/28C256 EEPROM writer for PGBOOT.ROM (experimental)", 13, 10, "$"
s_usage    db "usage: PGFLASH <segment> <file> [/256] [/NOSDP] [/BYTE] [/Y]", 13, 10
           db "  e.g. PGFLASH D000 PGBOOT.ROM", 13, 10
           db "  /256   28C256 SDP addresses (5555h/2AAAh, 32 KB decoded)", 13, 10
           db "  /NOSDP no software data protection sequence", 13, 10
           db "  /BYTE  byte writes instead of 64-byte pages", 13, 10
           db "  /Y     no confirmation", 13, 10, "$"
s_v86      db "The CPU is in virtual 8086 mode (EMM386/QEMM): boot clean first.", 13, 10, "$"
s_noopen   db "Cannot open the image file.", 13, 10, "$"
s_active   db "PGBOOT is running from that segment (INT 13h/19h point there). Boot without", 13, 10
           db "it first (Esc at the USB wait, or pgusinit /bdopts with bit 2), then retry.", 13, 10, "$"
s_badsize  db "The image must be a multiple of 512 bytes, at most 32 KB.", 13, 10, "$"
s_badsum   db "The image has an option ROM header but a bad checksum.", 13, 10, "$"
s_badseg   db "Segment must be C800..EE00 on a 2 KB boundary, the image must end below F000.", 13, 10, "$"
s_at       db "Segment $"
s_colon    db ": $"
s_hasrom   db "option ROM present, $"
s_kb       db " KB", 13, 10, "$"
s_norom    db "no option ROM signature", 13, 10, "$"
s_isram    db "That segment is RAM (shadow RAM, UMB or a RAM board), not an EEPROM.", 13, 10
           db "Disable ROM shadowing for it / remove the memory manager and retry.", 13, 10, "$"
s_confirm1 db "Write $"
s_confirm2 db " bytes to segment $"
s_confirm3 db "h (y/N)? $"
s_abort    db "Aborted.", 13, 10, "$"
s_writing  db "Writing $"
s_timeout  db 13, 10, "The write cycle did not complete at offset $"
s_hint     db 13, 10, "The write did not stick: check the write-protect jumper, try /256, /NOSDP or /BYTE,", 13, 10
           db "and that the segment is not shadowed. The EEPROM may now hold a partial image.", 13, 10, "$"
s_vfail    db "Verify failed at offset $"
s_vwrote   db ": wrote $"
s_vread    db ", read $"
s_ok       db "Written and verified at $"
s_ok2      db ". Reboot to use it; with EMM386 add X=$"
s_ok3      db ".", 13, 10, "$"
s_crlf     db 13, 10, "$"
s_other    db "Warning: another option ROM at $"
s_other2   db "h, in the same 32 KB window. If it shares this EEPROM or the board", 13, 10
           db "decodes 32 KB, check the chip size and the address jumpers before writing.", 13, 10, "$"

        section .bss
        alignb  16
image      resb MAX_SIZE
fname      resb 128
size       resw 1
romseg     resw 1
pagesz     resw 1
endseg     resw 1
opt_256    resb 1
opt_nosdp  resb 1
opt_byte   resb 1
opt_yes    resb 1
