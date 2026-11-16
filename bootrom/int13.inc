; ---------------------------------------------------------------------------
; PGBOOT INT 13h handler
;
; Reached through the RAM trampoline r_tramp13 (push cs / push cs /
; jmp far ROM:int13_entry), so on entry the stack holds
;   [sp] RAMSEG, [sp+2] RAMSEG, [sp+4] IP, [sp+6] CS, [sp+8] FLAGS
; The two RAMSEG words tell us where our state is; they are replaced by the
; old handler's address when we chain (retf), and dropped on return (iret).
; ---------------------------------------------------------------------------

; frame after "push bp / mov bp, sp / push ds" and the register saves in own13
%define F_RAMSEG bp+2
%define F_FL     bp+10
%define F_DS     bp-2
%define F_ES     bp-4
%define F_DI     bp-6
%define F_SI     bp-8
%define F_DX     bp-10
%define F_DL     bp-10
%define F_DH     bp-9
%define F_CX     bp-12
%define F_CL     bp-12
%define F_CH     bp-11
%define F_BX     bp-14
%define F_BL     bp-14
%define F_BH     bp-13
%define F_AX     bp-16
%define F_AL     bp-16
%define F_AH     bp-15

int13_entry:
        sti
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [F_RAMSEG]
        test    byte [r_units], 1
        jz      .nofd
        or      dl, dl
        jnz     .nofd
        jmp     fd_req
.nofd:
        test    byte [r_units], 2
        jz      chain13
        cmp     dl, 80h
        jb      chain13
        jne     .xl
        jmp     hd_req
.xl:
        cmp     dl, [r_hdmax]
        ja      chain13
        jmp     xlate13

; chain13: pass the request on unchanged (DS = RAM, frame as on entry)
chain13:
        push    ax
        mov     ax, [r_old13]
        mov     [bp+2], ax
        mov     ax, [r_old13+2]
        mov     [bp+4], ax
        pop     ax
        pop     ds
        pop     bp
        retf                                    ; -> old handler, IRET frame intact

; xlate13: physical hard disk 81h..80h+n -> DL-1 for the old handler, then
; fix up the answer (DL restored, AH=08h: DL = hard disk count incl. image)
xlate13:
        push    ax                              ; [bp-4] caller AX
        push    dx                              ; [bp-6] caller DX
        dec     dl
        pushf                                   ; simulated INT: flags,
        push    cs                              ; CS,
        call    .set                            ; IP = .back
        jmp     short .back
.set:
        push    word [r_old13+2]
        push    word [r_old13]
        mov     ds, [F_DS]                      ; the caller's DS (AH=42h etc. use DS:SI)
        retf
.back:
        mov     bp, sp                          ; no flag changes from here to lahf
        lea     bp, [bp+6]
        push    ax                              ; [bp-8] result AX
        lahf
        mov     [F_FL], ah                      ; result SF ZF AF PF CF for the caller
        mov     ah, [bp-3]                      ; the caller's AH
        cmp     ah, 08h
        je      .f08
        cmp     ah, 15h
        je      .out                            ; DX is an output
        mov     dl, [bp-6]
        jmp     short .out
.f08:
        test    byte [F_FL], 1
        jnz     .out
        push    es
        mov     es, [F_RAMSEG]
        mov     dl, [es:r_hdcnt]                ; physical disks + the image
        inc     dl
        pop     es
.out:
        pop     ax
        add     sp, 4
        pop     ds                              ; (the caller's DS already)
        pop     bp
        add     sp, 4
        iret

; ---- our own drives ------------------------------------------------------------
fd_req:
        mov     byte [r_isfd], 1
        mov     word [r_cur], u_fd
        jmp     short own13
hd_req:
        mov     byte [r_isfd], 0
        mov     word [r_cur], u_hd
own13:
        push    es
        push    di
        push    si
        push    dx
        push    cx
        push    bx
        push    ax
        cld
        cmp     byte [r_busy], 0
        je      .free
        ; reentered (from an interrupt handler while we wait for the card):
        ; the transport is in use, answer "drive not ready"
        mov     byte [F_AH], 0AAh
        or      byte [F_FL], 1
        jmp     exit_nobusy
.free:
        mov     byte [r_busy], 1
        mov     si, [r_cur]
        mov     bl, ah
        xor     bh, bh
        cmp     byte [r_isfd], 0
        je      .hd
        cmp     bl, 18h
        ja      .bad
        shl     bx, 1
        jmp     [cs:fd_table+bx]
.hd:
        cmp     bl, 41h
        jb      .hdlo
        sub     bl, 41h - 16h                   ; 41h..48h -> 16h..1Dh
        cmp     bl, 1Dh
        ja      .bad
        jmp     short .go
.hdlo:
        cmp     bl, 15h
        ja      .bad
.go:
        shl     bx, 1
        jmp     [cs:hd_table+bx]
.bad:
        jmp     fn_bad

fd_table:
        dw      fn_fdreset, fn_status, fn_read, fn_write        ; 00-03
        dw      fn_verify, fn_format, fn_bad, fn_bad            ; 04-07
        dw      fn_params, fn_bad, fn_bad, fn_bad               ; 08-0B
        dw      fn_bad, fn_bad, fn_bad, fn_bad                  ; 0C-0F
        dw      fn_bad, fn_bad, fn_bad, fn_bad                  ; 10-13
        dw      fn_bad, fn_type, fn_change, fn_ok               ; 14-17
        dw      fn_setmedia                                     ; 18

hd_table:
        dw      fn_ok, fn_status, fn_read, fn_write             ; 00-03
        dw      fn_verify, fn_bad, fn_bad, fn_bad               ; 04-07
        dw      fn_params, fn_ok, fn_bad, fn_bad                ; 08-0B
        dw      fn_seek, fn_ok, fn_bad, fn_bad                  ; 0C-0F
        dw      fn_ok, fn_ok, fn_bad, fn_bad                    ; 10-13
        dw      fn_ok, fn_type                                  ; 14-15
        dw      fn_echeck, fn_eread, fn_ewrite, fn_everify      ; 41-44
        dw      fn_bad, fn_bad, fn_eseek, fn_eparams            ; 45-48

; ---- exits ---------------------------------------------------------------------
; exit_st: AH = status; stores it as the caller's AH and in the BIOS data
; area (40:41h floppy, 40:74h hard disk); CF = (AH != 0)
exit_st:
        mov     [F_AH], ah
        mov     bx, 40h
        mov     es, bx
        mov     bx, 41h
        cmp     byte [r_isfd], 0
        jne     .fd
        mov     bl, 74h
.fd:
        mov     [es:bx], ah
exit_cf:
        and     byte [F_FL], 0FEh
        or      ah, ah
        jz      exit_pop
        or      byte [F_FL], 1
exit_pop:
        mov     byte [r_busy], 0
exit_nobusy:
        pop     ax
        pop     bx
        pop     cx
        pop     dx
        pop     si
        pop     di
        pop     es
        pop     ds
        pop     bp
        add     sp, 4                           ; the trampoline's RAMSEG words
        iret
; exit_raw: CF clear, caller's registers as stored in the frame
exit_raw:
        and     byte [F_FL], 0FEh
        jmp     short exit_pop

fn_bad:
        mov     ah, 01h
        jmp     short exit_st

fn_fdreset:
        call    fd_refresh
fn_ok:
        xor     ah, ah
        jmp     short exit_st

; AH=01h: last status (AH and AL, as BIOSes differ), not updated
fn_status:
        mov     bx, 40h
        mov     es, bx
        mov     bx, 41h
        cmp     byte [r_isfd], 0
        jne     .fd
        mov     bl, 74h
.fd:
        mov     ah, [es:bx]
        mov     al, ah
        mov     [F_AX], ax
        jmp     short exit_cf

; AH=02h/03h/04h: CHS read / write / verify, AL sectors, ES:BX buffer
fn_read:
        mov     byte [io_op], IO_READ
        jmp     short fn_rw
fn_write:
        mov     byte [io_op], IO_WRITE
        jmp     short fn_rw
fn_verify:
        mov     byte [io_op], IO_VERIFY
fn_rw:
        mov     al, [F_AL]
        mov     byte [F_AL], 0                  ; sectors done, set below
        xor     ah, ah
        or      al, al
        jz      fn_bad
        mov     [io_count], ax
        mov     cx, [F_CX]
        mov     dh, [F_DH]
        call    chs2lba
        jc      fn_nosec
        mov     [io_lba], ax
        mov     [io_lba+2], dx
        mov     ax, [F_BX]
        mov     [io_off], ax
        mov     ax, [F_ES]
        mov     [io_seg], ax
fn_io:
        call    disk_io
        mov     al, [io_done]
        mov     [F_AL], al
        jmp     exit_st
fn_nosec:
        mov     ah, 04h
        jmp     exit_st

; AH=05h (floppy): format track CH, head DH: fill its sectors with F6h
fn_format:
        mov     ah, 03h
        test    byte [si+U_FLAGS], BIF_RO
        jnz     .st
        mov     cx, [F_CX]
        and     cl, 0C0h
        or      cl, 1                           ; sector 1 of the track
        mov     dh, [F_DH]
        call    chs2lba
        jc      fn_nosec
        mov     [io_lba], ax
        mov     [io_lba+2], dx
        mov     ax, [si+U_SPT]
        mov     [io_count], ax
        mov     byte [io_op], IO_FILL
        call    disk_io
.st:
        jmp     exit_st

; AH=08h: drive parameters
fn_params:
        cmp     byte [r_isfd], 0
        je      .geo
        call    fd_refresh
        mov     si, u_fd
.geo:
        mov     ax, [si+U_CYL]
        dec     ax                              ; last cylinder
        mov     [F_CH], al
        mov     cl, 6
        shl     ah, cl
        or      ah, [si+U_SPT]
        mov     [F_CL], ah
        mov     al, [si+U_HEADS]
        dec     al
        mov     [F_DH], al
        mov     byte [F_AL], 0
        mov     bx, 40h
        mov     es, bx
        cmp     byte [r_isfd], 0
        je      .hd
        ; floppy: count from the equipment word, BL type, ES:DI -> DPT
        mov     al, [es:10h]
        mov     ah, 0
        test    al, 1
        jz      .cnt
        mov     cl, 6
        shr     al, cl
        inc     al
        mov     ah, al
.cnt:
        mov     [F_DL], ah
        mov     al, [si+U_FDTYPE]
        mov     ah, 0
        mov     [F_BX], ax
        mov     [F_ES], ds
        mov     word [F_DI], r_dpt
        jmp     fn_ok
.hd:
        mov     al, [r_hdcnt]                   ; physical disks + the image
        inc     al
        mov     [F_DL], al
        jmp     fn_ok

; AH=0Ch (hard disk): seek, only the bounds check
fn_seek:
        mov     cx, [F_CX]
        mov     dh, [F_DH]
        call    chs2lba
        jc      fn_nosec
        jmp     fn_ok

; AH=15h: drive type. Floppy: 02h (change line). Hard disk: 03h, CX:DX sectors
fn_type:
        cmp     byte [r_isfd], 0
        je      .hd
        mov     byte [F_AH], 02h
        jmp     exit_raw
.hd:
        mov     byte [F_AH], 03h
        mov     ax, [si+U_TOTAL]
        mov     [F_DX], ax
        mov     ax, [si+U_TOTAL+2]
        mov     [F_CX], ax
        jmp     exit_raw

; AH=16h (floppy): media change -> 06h + CF once per change
fn_change:
        call    fd_refresh
        cmp     ah, 80h
        je      .st
        xor     ah, ah
        cmp     byte [r_fdchg], 0
        je      .st
        mov     byte [r_fdchg], 0
        mov     ah, 06h
.st:
        jmp     exit_st

; AH=18h (floppy): set media type for format; must match the image
fn_setmedia:
        mov     ax, [F_CX]
        mov     bl, al
        and     bl, 3Fh
        xor     bh, bh
        cmp     bx, [si+U_SPT]
        jne     .no
        mov     cl, 6
        shr     al, cl
        xchg    al, ah                          ; AX = last cylinder
        inc     ax
        cmp     ax, [si+U_CYL]
        jne     .no
        mov     [F_ES], ds
        mov     word [F_DI], r_dpt
        jmp     fn_ok
.no:
        mov     ah, 0Ch
        jmp     exit_st

; ---- EDD ---------------------------------------------------------------------------
; AH=41h: installation check -> EDD 1.1, extended disk access (42h-44h, 47h, 48h)
fn_echeck:
        cmp     word [F_BX], 55AAh
        je      .ok
        jmp     fn_bad
.ok:
        mov     word [F_BX], 0AA55h
        mov     byte [F_AH], 21h
        mov     word [F_CX], 0001h
        jmp     exit_raw

; AH=42h/43h/44h: extended read / write / verify, DS:SI -> disk address packet
fn_eread:
        mov     byte [io_op], IO_READ
        jmp     short fn_ext
fn_ewrite:
        mov     byte [io_op], IO_WRITE
        jmp     short fn_ext
fn_everify:
        mov     byte [io_op], IO_VERIFY
fn_ext:
        call    dap_lba
        jc      .ret
        mov     ax, [es:di+2]
        mov     [io_count], ax
        mov     word [es:di+2], 0
        mov     ax, [es:di+4]
        mov     [io_off], ax
        mov     dx, [es:di+6]
        mov     [io_seg], dx
        and     ax, dx
        inc     ax
        jz      .flat                           ; FFFF:FFFF: 64-bit flat address
        xor     ax, ax
        cmp     [io_count], ax
        je      .ret                            ; nothing to do: AH = 0
        call    disk_io
        mov     es, [F_DS]
        mov     di, [F_SI]
        mov     cx, [io_done]
        mov     [es:di+2], cx
.ret:
        jmp     exit_st
.flat:
        jmp     fn_bad

; AH=47h: extended seek, only the bounds check
fn_eseek:
        call    dap_lba
        jc      .ret
        mov     ax, [io_lba]
        mov     dx, [io_lba+2]
        sub     ax, [si+U_TOTAL]
        sbb     dx, [si+U_TOTAL+2]
        mov     ah, 04h
        jnc     .ret
        xor     ah, ah
.ret:
        jmp     exit_st

; dap_lba: ES:DI = the caller's DS:SI (DAP), io_lba = its LBA.
; CF=1 with AH = 01h (bad packet) or 04h (LBA beyond 32 bits)
dap_lba:
        mov     es, [F_DS]
        mov     di, [F_SI]
        mov     ah, 01h
        cmp     byte [es:di], 10h
        jb      .fail
        mov     ax, [es:di+12]
        or      ax, [es:di+14]
        mov     ah, 04h                         ; (flags from the OR)
        jnz     .fail
        mov     ax, [es:di+8]
        mov     [io_lba], ax
        mov     ax, [es:di+10]
        mov     [io_lba+2], ax
        clc
        ret
.fail:
        stc
        ret

; AH=48h: drive parameters, DS:SI -> result buffer (size word first)
fn_eparams:
        mov     es, [F_DS]
        mov     di, [F_SI]
        mov     ax, [es:di]
        cmp     ax, 1Ah
        jae     .ok
        jmp     fn_bad
.ok:
        mov     bx, 1Ah
        cmp     ax, 1Eh
        jb      .sz
        mov     bx, 1Eh
.sz:
        mov     ax, bx
        stosw                                   ; +0 size
        mov     ax, 0002h                       ; +2 flags: CHS information valid
        stosw
        xor     dx, dx
        mov     ax, [si+U_CYL]
        stosw                                   ; +4 cylinders
        xchg    ax, dx
        stosw
        mov     ax, [si+U_HEADS]
        stosw                                   ; +8 heads
        xchg    ax, dx
        stosw
        mov     ax, [si+U_SPT]
        stosw                                   ; +12 sectors per track
        xchg    ax, dx
        stosw
        mov     ax, [si+U_TOTAL]
        stosw                                   ; +16 total sectors (qword)
        mov     ax, [si+U_TOTAL+2]
        stosw
        xchg    ax, dx
        stosw
        stosw
        mov     ax, 512
        stosw                                   ; +24 bytes per sector
        cmp     bx, 1Eh
        jb      .done
        mov     ax, 0FFFFh                      ; +26 no DPTE
        stosw
        stosw
.done:
        jmp     fn_ok

; ---- helpers --------------------------------------------------------------------------
; fd_refresh: BDINFO for the floppy unit (no OPEN): picks up a swapped image
; (geometry, read-only flag) and latches the media change for AH=16h.
; out: AH = 0, or 80h when the card did not answer or the unit is not ready.
; Clobbers AX, BX, CX, DX, DI.
fd_refresh:
        mov     ax, 0000h
        call    bdinfo
        jc      .nr
        mov     di, u_fd
        test    byte [x_ans+BI_FLAGS], BIF_CHANGED
        jnz     .chg
        mov     al, [x_ans+BI_GEN]
        cmp     al, [di+U_GEN]
        je      .load
.chg:
        mov     byte [r_fdchg], 1
.load:
        call    unit_load
        jc      .nr
        mov     al, [di+U_SPT]
        mov     [r_dpt+4], al
        xor     ah, ah
        ret
.nr:
        mov     ah, 80h
        ret

; chs2lba: CX/DH (INT 13h CHS) -> DX:AX LBA for the unit at SI.
; CF=1: outside the geometry. Clobbers BX, CX, DI.
chs2lba:
        mov     al, cl
        and     ax, 003Fh
        jz      .bad
        cmp     ax, [si+U_SPT]
        ja      .bad
        dec     ax
        mov     di, ax                          ; sector - 1
        mov     bl, dh
        xor     bh, bh                          ; head
        cmp     bx, [si+U_HEADS]
        jae     .bad
        mov     al, cl
        mov     cl, 6
        shr     al, cl
        mov     ah, al
        mov     al, ch                          ; cylinder
        cmp     ax, [si+U_CYL]
        jae     .bad
        mul     word [si+U_HEADS]
        add     ax, bx
        adc     dx, 0
        mov     bx, [si+U_SPT]
        call    mul32x16
        add     ax, di
        adc     dx, 0
        clc
        ret
.bad:
        stc
        ret

; mul32x16: DX:AX *= BX (result must fit in 32 bits)
mul32x16:
        push    cx
        mov     cx, ax
        mov     ax, dx
        mul     bx
        xchg    ax, cx
        mul     bx
        add     dx, cx
        pop     cx
        ret

; disk_io: io_count sectors from io_lba of unit [r_cur], buffer io_seg:io_off,
; operation io_op. Split into frames of at most r_maxrd (read, verify) or
; r_maxwr (write, fill) sectors. The buffer is tracked as a linear address
; and every frame gets a seg:off that reaches it without the offset wrapping:
; the segment is the address / 16 but at most FFFFh, so buffers in the HMA
; (FFFF:xxxx with A20 on) work and never wrap to low memory; a frame is cut
; short at the end of that last segment (09h when not even a sector fits).
; Stops at the end of the image with 04h. A floppy answer 06h (the image was
; swapped: token mismatch) refreshes the unit so the next call works and
; latches the change for AH=16h.
; out: AH = status, io_done = sectors transferred. Clobbers all but BP, DS.
disk_io:
        mov     ax, [io_seg]
        mov     dx, ax
        mov     cl, 4
        shl     ax, cl
        mov     cl, 12
        shr     dx, cl                          ; DX:AX = segment * 16
        add     ax, [io_off]
        adc     dx, 0
        mov     [io_lin], ax
        mov     [io_lin+2], dx
        mov     word [io_done], 0
.next:
        mov     si, [r_cur]
        mov     cx, [io_count]
        sub     cx, [io_done]
        jnz     .more
        xor     ah, ah
        ret
.more:
        mov     ax, [io_lba]
        mov     dx, [io_lba+2]
        sub     ax, [si+U_TOTAL]
        sbb     dx, [si+U_TOTAL+2]
        jc      .inside
        mov     ah, 04h                         ; LBA at or past the end
        ret
.inside:
        neg     dx                              ; DX:AX = sectors left in the image
        neg     ax
        sbb     dx, 0
        jnz     .lim
        cmp     ax, cx
        jae     .lim
        mov     cx, ax
.lim:
        mov     al, [r_maxrd]
        test    byte [io_op], 1                 ; IO_WRITE, IO_FILL
        jz      .rd
        mov     al, [r_maxwr]
.rd:
        xor     ah, ah
        or      al, al
        jnz     .max
        mov     ah, 01h
        ret
.max:
        cmp     cx, ax
        jbe     .buf
        mov     cx, ax
.buf:
        ; seg:off for the linear buffer address
        mov     ax, [io_lin]
        mov     dx, [io_lin+2]
        cmp     dx, 10h
        jae     .hma
        mov     bx, ax
        and     bx, 0Fh                         ; offset
        push    cx
        mov     cl, 4
        shr     ax, cl
        mov     cl, 12
        shl     dx, cl
        or      ax, dx                          ; segment
        pop     cx
        jmp     short .seg
.hma:
        sub     ax, 0FFF0h                      ; address - FFFF0h
        sbb     dx, 0Fh
        jnz     .wrap                           ; beyond FFFF:FFFF
        mov     bx, ax
        mov     ax, 0FFFFh
        ; sectors that fit before the offset wraps: (10000h - off) / 512
        mov     dx, bx
        neg     dx
        jz      .seg                            ; offset 0: a whole segment
        push    cx
        mov     cl, 9
        shr     dx, cl
        pop     cx
        or      dx, dx
        jz      .wrap
        cmp     cx, dx
        jbe     .seg
        mov     cx, dx
.seg:
        mov     [x_dptr], bx
        mov     [x_aptr], bx
        mov     [x_dptr+2], ax
        mov     [x_aptr+2], ax
        ; frame header: UU NN LBA TOKEN
        mov     [io_n], cl
        mov     al, [si+U_UNIT]
        mov     ah, cl
        mov     [x_hdr+4], ax
        mov     ax, [io_lba]
        mov     [x_hdr+6], ax
        mov     ax, [io_lba+2]
        mov     [x_hdr+8], ax
        mov     ax, [si+U_TOKEN]
        mov     [x_hdr+10], ax
        mov     ax, [si+U_TOKEN+2]
        mov     [x_hdr+12], ax
        mov     word [x_hlen], 4 + BD_IO_HDR_LEN
        mov     ah, cl
        xor     al, al
        shl     ax, 1                           ; bytes = n * 512
        mov     bx, ax
        xor     ax, ax
        mov     [x_fill], al
        mov     [x_dlen], ax
        mov     [x_alen], ax
        mov     dl, BD_AL_READ
        mov     ax, TMO_READ
        mov     cl, [io_op]
        cmp     cl, IO_READ
        jne     .nr
        mov     [x_alen], bx
        jmp     short .hdr
.nr:
        cmp     cl, IO_VERIFY
        je      .hdr
        mov     dl, BD_AL_WRITE
        mov     ax, TMO_WRITE
        mov     [x_dlen], bx
        cmp     cl, IO_FILL
        jne     .hdr
        mov     byte [x_fill], 1
.hdr:
        mov     byte [x_hdr+2], 0
        mov     [x_hdr+3], dl
        call    set_tmo
        call    xact                            ; preserves BX
        jc      .ret                            ; AH = transport status
        or      ax, ax
        jz      .cardok
        mov     ah, al                          ; the card's INT 13h status
        or      ah, ah
        jnz     .st
        mov     ah, 20h
.st:
        cmp     ah, BD_ST_CHANGED
        jne     .ret
        cmp     byte [r_isfd], 0
        je      .ret
        call    fd_refresh                      ; new token and geometry
        mov     byte [r_fdchg], 1
        mov     ah, BD_ST_CHANGED
.ret:
        ret
.wrap:
        mov     ah, 09h                         ; buffer runs past FFFF:FFFF
        ret
.cardok:
        test    byte [io_op], 1
        jnz     .adv                            ; writes: no payload
        cmp     cx, bx
        je      .adv
        mov     ah, 20h                         ; short answer
        ret
.adv:
        mov     al, [io_n]
        xor     ah, ah
        add     [io_done], ax
        add     [io_lba], ax
        adc     word [io_lba+2], 0
        add     [io_lin], bx                    ; + n * 512
        adc     word [io_lin+2], 0
        jmp     .next
