; ---------------------------------------------------------------------------
; PGBOOT console output through INT 10h AH=0Eh (teletype). Some video BIOSes
; clobber BP/SI/DI in teletype, so putc saves everything it can.
; ---------------------------------------------------------------------------

; putc: character AL
putc:
        push    ax
        push    bx
        push    si
        push    di
        push    bp
        mov     ah, 0Eh
        mov     bx, 0007h
        int     10h
        pop     bp
        pop     di
        pop     si
        pop     bx
        pop     ax
        ret

; print: zero-terminated string at CS:SI. Preserves all but SI (points past).
print:
        push    ax
.l:
        cs lodsb
        or      al, al
        jz      .e
        call    putc
        jmp     short .l
.e:
        pop     ax
        ret

; print_pfx: "PGBOOT: "
print_pfx:
        push    si
        mov     si, msg_pfx
        call    print
        pop     si
        ret

crlf:
        push    si
        mov     si, msg_crlf
        call    print
        pop     si
        ret

; print_hex: AX as 3 or 4 hex digits (ports: leading zero nibble dropped)
print_hex:
        push    ax
        push    cx
        push    dx
        mov     dx, ax
        mov     cx, 4
.l:
        push    cx
        mov     cl, 4
        rol     dx, cl
        pop     cx
        mov     al, dl
        and     al, 0Fh
        jnz     .dig
        cmp     cx, 4
        je      .skip                   ; drop a leading zero nibble
.dig:
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 'A' - '0' - 10
.out:
        call    putc
.skip:
        loop    .l
        pop     dx
        pop     cx
        pop     ax
        ret

; print_hex8: AL as 2 hex digits
print_hex8:
        push    ax
        push    cx
        mov     ah, al
        mov     cl, 4
        shr     al, cl
        call    .nib
        mov     al, ah
        call    .nib
        pop     cx
        pop     ax
        ret
.nib:
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .o
        add     al, 'A' - '0' - 10
.o:
        jmp     putc

; print_dec: DX:AX unsigned decimal
print_dec:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        xor     cx, cx
        mov     bx, 10
.div:
        push    ax
        mov     ax, dx
        xor     dx, dx
        div     bx                      ; high word / 10
        mov     si, ax
        pop     ax
        div     bx                      ; (rem:low) / 10
        push    dx                      ; digit
        inc     cx
        mov     dx, si
        or      si, ax
        jnz     .div
.out:
        pop     ax
        add     al, '0'
        call    putc
        loop    .out
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; print_size: DX:AX sectors as "1440K" or "504M"
print_size:
        push    ax
        push    cx
        push    dx
        cmp     dx, 0
        jne     .mb
        cmp     ax, 20480               ; below 10 MB: kilobytes
        jae     .mb
        shr     ax, 1
        call    print_dec
        mov     al, 'K'
        jmp     short .unit
.mb:
        mov     cx, 11
.sh:
        shr     dx, 1
        rcr     ax, 1
        loop    .sh
        call    print_dec
        mov     al, 'M'
.unit:
        call    putc
        pop     dx
        pop     cx
        pop     ax
        ret

; print_ansname: the display name in the BDINFO answer (x_ans, DS=RAM),
; at most 48 characters
print_ansname:
        push    ax
        push    cx
        push    si
        mov     cl, [x_ans+BI_NAMELEN]
        xor     ch, ch
        cmp     cx, 48
        jbe     .ok
        mov     cx, 48
.ok:
        mov     si, x_ans+BI_NAME
        jcxz    .e
.l:
        lodsb
        call    putc
        loop    .l
.e:
        pop     si
        pop     cx
        pop     ax
        ret
