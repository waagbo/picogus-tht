; ---------------------------------------------------------------------------
; PGBOOT start-up tune on the PC speaker (timer channel 2, port 61h).
;
; The card's CMD_BDTUNE register selects it: 0 silent, 1..TUNES one of the
; tunes below. Played once in the late init, when the card has answered.
; Note lengths are counted in BIOS timer ticks (55 ms), so the tempo is the
; same on every machine. A key ends the tune; the key stays in the buffer.
; ---------------------------------------------------------------------------

PIT_HZ          equ 1193182

; note Hz, ticks        rest ticks        tune_end
%macro note 2
        dw      PIT_HZ / (%1)
        db      %2
%endmacro
%macro rest 1
        dw      0
        db      %1
%endmacro
%define tune_end dw 0FFFFh

; tune_play: clobbers AX, BX, CX, DX, SI, DI, ES.
tune_play:
        mov     al, CMD_BDTUNE
        call    rd_ui
        or      al, al
        jz      .ret
        cmp     al, TUNES
        ja      .ret
        xor     ah, ah
        mov     bx, ax
        shl     bx, 1
        mov     si, [cs:tune_tab+bx-2]
        mov     cx, 1                   ; start on a tick: the first note is full length
        call    wait_ticks
        xor     ax, ax
        mov     es, ax
.note:
        cs lodsw
        cmp     ax, 0FFFFh
        je      .off
        mov     cx, ax                  ; timer divisor, 0 = rest
        cs lodsb
        mov     bl, al                  ; length in ticks
        in      al, 61h
        and     al, 0FCh
        jcxz    .gate                   ; a rest: speaker off
        push    ax
        mov     al, 0B6h                ; channel 2, low/high byte, square wave
        out     43h, al
        mov     al, cl
        out     42h, al
        mov     al, ch
        out     42h, al
        pop     ax
        or      al, 03h                 ; timer gate and speaker data on
.gate:
        out     61h, al
.tick:
        mov     di, [es:046Ch]
.hold:
        mov     ah, 1
        int     16h
        jnz     .off                    ; a key: enough
        cmp     di, [es:046Ch]
        je      .hold
        dec     bl
        jnz     .tick
        jmp     short .note
.off:
        in      al, 61h
        and     al, 0FCh
        out     61h, al
.ret:
        ret

; ---- the tunes -----------------------------------------------------------
TUNES           equ 3

tune_tab:
        dw      tune_1, tune_2, tune_3

; 1: four rising notes, half a second
tune_1:
        note    523, 2                  ; C5
        note    659, 2                  ; E5
        note    784, 2                  ; G5
        note    1047, 4                 ; C6
        tune_end

; 2: a short fanfare, about a second
tune_2:
        note    523, 2                  ; C5
        note    392, 2                  ; G4
        note    523, 2                  ; C5
        note    659, 2                  ; E5
        note    784, 3                  ; G5
        rest    1
        note    784, 1                  ; G5
        note    1047, 6                 ; C6
        tune_end

; 3: a quick sweep down and up
tune_3:
        note    1319, 1                 ; E6
        note    1047, 1                 ; C6
        note    784, 1                  ; G5
        note    659, 1                  ; E5
        note    784, 1                  ; G5
        note    1047, 1                 ; C6
        note    1319, 1                 ; E6
        note    1568, 4                 ; G6
        tune_end
