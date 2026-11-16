; INT13TST.COM - exercises the PGBOOT INT 13h handler from DOS and prints
; the results (redirect to a file or COM1). Test tool, not needed to boot.
;   INT13TST          read-only checks
;   INT13TST W        also write tests (last sector of A: and of the 80h disk,
;                     restored afterwards)
; Lines starting with "FAIL" mark unexpected results; the last line is
; "INT13TST DONE, n failures".
        cpu     8086
        org     100h

start:
        mov     dx, s_title
        call    puts
        ; write tests requested?
        mov     si, 81h
.arg:   lodsb
        cmp     al, 13
        je      .noarg
        or      al, 20h
        cmp     al, 'w'
        jne     .arg
        mov     byte [wtest], 1
.noarg:
        ; ---- AH=08h / 15h for 00h, 80h, 81h
        mov     dl, 00h
        call    params
        mov     dl, 80h
        call    params
        mov     dl, 81h
        call    params
        ; ---- EDD
        mov     dx, s_41
        call    puts
        mov     ah, 41h
        mov     bx, 55AAh
        mov     dl, 80h
        int     13h
        call    regs
        cmp     bx, 0AA55h
        je      .e1
        call    fail
.e1:
        mov     dx, s_48
        call    puts
        mov     word [edd], 1Eh
        mov     ah, 48h
        mov     dl, 80h
        mov     si, edd
        int     13h
        call    regs
        mov     cx, 15
        mov     si, edd
.e2:    lodsw
        call    hex16
        call    space
        loop    .e2
        call    nl
        ; ---- CHS read vs LBA read of the first 20 sectors of 80h
        mov     dx, s_cmp
        call    puts
        mov     di, buf1
        xor     bp, bp                          ; LBA
.chs:   push    di
        mov     ax, bp
        call    lba2chs80                       ; -> CX, DH
        mov     bx, di
        mov     ax, 0201h
        mov     dl, 80h
        int     13h
        pop     di
        jc      .chsf
        add     di, 512
        inc     bp
        cmp     bp, 20
        jb      .chs
        jmp     short .ext
.chsf:  call    regs
        call    fail
.ext:
        mov     word [dap], 10h
        mov     word [dap+2], 20
        mov     word [dap+4], buf2
        mov     [dap+6], ds
        mov     word [dap+8], 0
        mov     word [dap+10], 0
        mov     word [dap+12], 0
        mov     word [dap+14], 0
        mov     ah, 42h
        mov     dl, 80h
        mov     si, dap
        int     13h
        call    regs
        mov     ax, [dap+2]
        call    hex16
        call    nl
        mov     si, buf1
        mov     di, buf2
        mov     cx, 20*256
        repe cmpsw
        je      .same
        call    fail
        jmp     short .odd
.same:  mov     dx, s_same
        call    puts
        ; ---- odd, unnormalised buffer: ES:BX = (DS-1):(buf2+16+1), 3 sectors from LBA 5
.odd:
        mov     dx, s_odd
        call    puts
        mov     ax, ds
        dec     ax
        mov     es, ax
        mov     bx, buf2 + 16 + 1
        mov     ax, 5
        call    lba2chs80
        mov     ax, 0203h
        mov     dl, 80h
        int     13h
        push    ds
        pop     es
        call    regs
        mov     si, buf1 + 5*512
        mov     di, buf2 + 1
        mov     cx, 3*512
        repe cmpsb
        je      .same2
        call    fail
        jmp     short .errs
.same2: mov     dx, s_same
        call    puts
        ; ---- error cases
.errs:
        mov     dx, s_e1
        call    puts
        mov     ax, 0201h                       ; sector 0: invalid
        mov     cx, 0000h
        mov     dx, 0080h
        mov     bx, buf1
        int     13h
        call    regs
        call    expect04
        mov     dx, s_e2
        call    puts
        mov     ax, 0C00h                       ; seek to cylinder 1023
        mov     cx, 0FFC1h
        mov     dx, 0080h
        int     13h
        call    regs
        call    expect04
        mov     dx, s_e3
        call    puts
        mov     ah, 01h
        mov     dl, 80h
        int     13h
        call    regs
        mov     dx, s_e4
        call    puts
        mov     ah, 0F9h                        ; unknown
        mov     dl, 80h
        int     13h
        call    regs
        jc      .e4ok
        call    fail
.e4ok:
        mov     dx, s_e5
        call    puts
        mov     ax, 0401h                       ; verify 1 sector
        mov     cx, 0001h
        mov     dx, 0080h
        int     13h
        call    regs
        jnc     .e5ok
        call    fail
.e5ok:
        mov     dx, s_e6
        call    puts
        mov     ah, 16h
        mov     dl, 00h
        int     13h
        call    regs
        mov     dx, s_e7
        call    puts
        mov     ax, 0201h                       ; floppy read, sector 1
        mov     cx, 0001h
        mov     dx, 0000h
        mov     bx, buf1
        int     13h
        call    regs
        jnc     .e7ok
        call    fail
.e7ok:
        mov     dx, s_e8
        call    puts
        mov     ax, 0201h                       ; physical disk (81h) sector 1
        mov     cx, 0001h
        mov     dx, 0081h
        mov     bx, buf1
        int     13h
        call    regs
        cmp     word [buf1+510], 0AA55h
        je      .e8ok
        call    fail
.e8ok:
        cmp     byte [wtest], 0
        je      .done
        call    writes
.done:
        mov     dx, s_done
        call    puts
        mov     al, [fails]
        xor     ah, ah
        call    hex16
        mov     dx, s_fails
        call    puts
        mov     ax, 4C00h
        int     21h

; writes: last sector of A: (CHS from AH=08h) and of 80h (LBA from AH=48h)
writes:
        mov     dx, s_w1
        call    puts
        mov     ah, 08h
        mov     dl, 00h
        int     13h
        push    ds                              ; AH=08h returned ES:DI -> DPT
        pop     es
        ; CH = last cyl, CL = spt, DH = last head: last sector
        mov     [wchs], cx
        mov     [wchs+2], dh
        mov     bx, buf1                        ; save
        mov     ax, 0201h
        mov     dl, 0
        int     13h
        jc      .f1
        call    pattern
        mov     bx, buf2
        mov     ax, 0301h
        mov     cx, [wchs]
        mov     dh, [wchs+2]
        mov     dl, 0
        int     13h
        call    regs
        jc      .f1
        mov     bx, buf2 + 512                  ; read back
        mov     ax, 0201h
        mov     cx, [wchs]
        mov     dh, [wchs+2]
        mov     dl, 0
        int     13h
        jc      .f1
        mov     si, buf2
        mov     di, buf2 + 512
        mov     cx, 256
        repe cmpsw
        jne     .f1
        mov     bx, buf1                        ; restore
        mov     ax, 0301h
        mov     cx, [wchs]
        mov     dh, [wchs+2]
        mov     dl, 0
        int     13h
        jc      .f1
        mov     dx, s_same
        call    puts
        jmp     short .hd
.f1:    call    fail
.hd:
        mov     dx, s_w2
        call    puts
        ; last LBA = total - 1 from AH=48h (edd+16)
        mov     ax, [edd+16]
        mov     dx, [edd+18]
        sub     ax, 1
        sbb     dx, 0
        mov     [dap+8], ax
        mov     [dap+10], dx
        mov     word [dap+2], 1
        mov     word [dap+4], buf1
        mov     ah, 42h
        mov     dl, 80h
        mov     si, dap
        int     13h
        jc      .f2
        call    pattern
        mov     word [dap+4], buf2
        mov     ax, 4300h
        mov     dl, 80h
        mov     si, dap
        int     13h
        call    regs
        jc      .f2
        mov     word [dap+4], buf2 + 512
        mov     ah, 42h
        mov     dl, 80h
        mov     si, dap
        int     13h
        jc      .f2
        mov     si, buf2
        mov     di, buf2 + 512
        mov     cx, 256
        repe cmpsw
        jne     .f2
        mov     word [dap+4], buf1
        mov     ax, 4300h
        mov     dl, 80h
        mov     si, dap
        int     13h
        jc      .f2
        mov     dx, s_same
        call    puts
        ; one past the end: 04h expected
        mov     dx, s_w3
        call    puts
        mov     ax, [edd+16]
        mov     dx, [edd+18]
        mov     [dap+8], ax
        mov     [dap+10], dx
        mov     ah, 42h
        mov     dl, 80h
        mov     si, dap
        int     13h
        call    regs
        call    expect04
        ret
.f2:    call    fail
        ret

pattern:
        mov     di, buf2
        mov     cx, 512
        mov     al, 5Ah
.p:     stosb
        inc     al
        loop    .p
        ret

; lba2chs80: AX (small LBA) -> CX/DH for drive 80h using geometry from AH=08h
lba2chs80:
        push    ax
        push    bx
        mov     bl, [g80spt]
        xor     bh, bh
        xor     dx, dx
        div     bx                              ; AX = track, DX = sector-1
        mov     cl, dl
        inc     cl
        mov     bl, [g80heads]
        xor     dx, dx
        div     bx                              ; AX = cyl, DX = head
        mov     dh, dl
        mov     ch, al
        pop     bx
        pop     ax
        ret

; params: AH=08h and AH=15h for drive DL
params:
        push    dx
        mov     dx, s_08
        call    puts
        pop     dx
        push    dx
        mov     al, dl
        call    hex8
        call    space
        mov     ah, 08h
        xor     di, di
        mov     es, di
        int     13h
        pushf
        call    regs
        popf
        pop     ax
        push    ax
        jc      .t
        cmp     al, 80h
        jne     .t
        mov     al, cl
        and     al, 3Fh
        mov     [g80spt], al
        inc     dh
        mov     [g80heads], dh
.t:
        mov     dx, s_15
        call    puts
        pop     dx
        mov     ah, 15h
        int     13h
        call    regs
        push    ds
        pop     es
        ret

; regs: prints CF AX BX CX DX ES:DI (flags preserved)
regs:
        pushf
        push    ax
        push    dx
        push    ax
        mov     al, 'C'
        call    putc
        mov     al, 'F'
        call    putc
        mov     al, '0'
        mov     bp, sp
        test    byte [bp+6], 1
        jz      .c
        mov     al, '1'
.c:     call    putc
        mov     dx, s_ax
        call    puts
        pop     ax
        call    hex16
        mov     dx, s_bx
        call    puts
        mov     ax, bx
        call    hex16
        mov     dx, s_cx
        call    puts
        mov     ax, cx
        call    hex16
        mov     dx, s_dx
        call    puts
        pop     ax
        push    ax
        call    hex16
        mov     dx, s_esdi
        call    puts
        mov     ax, es
        call    hex16
        mov     al, ':'
        call    putc
        mov     ax, di
        call    hex16
        call    nl
        pop     dx
        pop     ax
        popf
        ret

expect04:
        pushf
        cmp     ah, 04h
        jne     .f
        popf
        jnc     .f2
        ret
.f:     popf
.f2:    jmp     fail

fail:
        push    dx
        mov     dx, s_fail
        call    puts
        inc     byte [fails]
        pop     dx
        ret

hex16:  push    ax
        mov     al, ah
        call    hex8
        pop     ax
hex8:   push    ax
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
.n:     and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     putc
        add     al, 7
putc:   push    ax
        push    dx
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     dx
        pop     ax
        ret
space:  push    ax
        mov     al, ' '
        call    putc
        pop     ax
        ret
nl:     push    dx
        mov     dx, s_nl
        call    puts
        pop     dx
        ret
puts:   push    ax
        mov     ah, 09h
        int     21h
        pop     ax
        ret

s_title db "INT13TST", 13, 10, "$"
s_08    db "AH=08 DL=$"
s_15    db "  AH=15: $"
s_41    db "AH=41 80: $"
s_48    db "AH=48 80: $"
s_cmp   db "20 sectors CHS (02h) vs LBA (42h): $"
s_same  db "  data identical", 13, 10, "$"
s_odd   db "odd unnormalised buffer, 3 sectors: $"
s_e1    db "read sector 0 (expect 04): $"
s_e2    db "seek cyl 1023 (expect 04): $"
s_e3    db "AH=01 status: $"
s_e4    db "AH=F9 (expect CF): $"
s_e5    db "verify 80h: $"
s_e6    db "AH=16 00h: $"
s_e7    db "read 00h sector 1: $"
s_e8    db "read 81h sector 1 (expect 55AA): $"
s_w1    db "write/readback/restore last sector of A: $"
s_w2    db "write/readback/restore last LBA of 80h: $"
s_w3    db "LBA read one past the end (expect 04): $"
s_ax    db " AX=$"
s_bx    db " BX=$"
s_cx    db " CX=$"
s_dx    db " DX=$"
s_esdi  db " ES:DI=$"
s_nl    db 13, 10, "$"
s_fail  db "FAIL", 13, 10, "$"
s_done  db "INT13TST DONE, $"
s_fails db " failures", 13, 10, "$"
fails   db 0
wtest   db 0
g80spt  db 63
g80heads db 16
wchs    dw 0, 0
        align 2
edd     times 32 db 0
dap     times 16 db 0
buf1    equ     $
buf2    equ     buf1 + 20*512
