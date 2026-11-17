; ---------------------------------------------------------------------------
; PGBOOT start-up tune on the PC speaker (timer channel 2, port 61h).
;
; The tunes live on the card (sw/bootdisk/bd_tune.c: a set that comes with
; the firmware, and one of the user's own). The card's CMD_BDTUNE setting
; selects one, and CMD_BDTUNEDATA reads it as a stream:
;     n, n name characters, then per note: timer divisor (word, 0 = silence)
;     and length (word), and FFFFh at the end.
; A tune set to "off" has a name and no notes; firmware without tunes reads
; FFh, which is no name length.
;
; A length counts units of 1/32 of a BIOS timer tick (1.7 ms). The ROM takes
; the time from timer channel 0, which the tick comes from: its counter's high
; byte, latched. A BIOS runs that counter in mode 3 (it counts down by two,
; twice per tick: 512 high-byte steps a tick) or in mode 2 (once: 256), so
; the ROM first counts the steps of one tick. A key ends the tune; the key
; stays in the buffer.
; ---------------------------------------------------------------------------

TUNE_NAME_MAX   equ 24

; tune_play: AL != 0: the tune's name is printed at the cursor first.
; Clobbers AX, BX, CX, DX, SI, DI, ES.
tune_play:
        mov     ch, al
        mov     al, CMD_BDTUNEDATA
        call    select                  ; rewinds the stream
        call    .byte
        cmp     al, TUNE_NAME_MAX
        ja      .ret
        mov     cl, al
        or      cl, cl
        jz      .named
.name:
        call    .byte
        or      ch, ch
        jz      .quiet
        call    putc
.quiet:
        dec     cl
        jnz     .name
.named:
        call    .word                   ; the first note, or the end
        cmp     ax, 0FFFFh
        je      .ret                    ; off: no time spent
        push    ax
        ; ---- timer steps in one tick: 256 or 512 --------------------------
        xor     ax, ax
        mov     es, ax
        mov     di, [es:046Ch]
.edge:
        cmp     di, [es:046Ch]
        je      .edge
        mov     di, [es:046Ch]
        call    .timer
        mov     bl, al
        xor     si, si
.count:
        call    .steps
        add     si, ax
        cmp     di, [es:046Ch]
        je      .count
        mov     bh, 3                   ; a unit is 8 steps,
        cmp     si, 384
        jb      .mode
        inc     bh                      ; or 16
.mode:
        pop     ax
.note:
        mov     di, ax                  ; divisor
        call    .word
        mov     cl, bh
        shl     ax, cl
        mov     si, ax                  ; steps this note lasts
        in      al, 61h
        and     al, 0FCh                ; silence: speaker off
        or      di, di
        jz      .gate
        push    ax
        mov     al, 0B6h                ; channel 2, low/high byte, square wave
        out     43h, al
        mov     ax, di
        out     42h, al
        mov     al, ah
        out     42h, al
        pop     ax
        or      al, 03h                 ; timer gate and speaker data on
.gate:
        out     61h, al
.hold:
        mov     ah, 1
        int     16h
        jnz     .off                    ; a key: enough
        call    .steps
        sub     si, ax
        ja      .hold
        call    .word
        cmp     ax, 0FFFFh
        jne     .note
.off:
        in      al, 61h
        and     al, 0FCh
        out     61h, al
.ret:
        ret
; AL = the next byte of the stream
.byte:
        mov     dx, DATA_PORT_HIGH
        in      al, dx
        ret
; AX = its next word
.word:
        call    .byte
        mov     ah, al
        call    .byte
        xchg    al, ah
        ret
; AL = high byte of timer channel 0, counting down
.timer:
        xor     al, al                  ; latch channel 0
        out     43h, al
        in      al, 40h
        in      al, 40h
        ret
; AX = timer steps since the last call (BL keeps the last value seen)
.steps:
        call    .timer
        mov     ah, bl
        sub     ah, al
        mov     bl, al
        mov     al, ah
        xor     ah, ah
        ret
