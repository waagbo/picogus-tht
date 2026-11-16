; ---------------------------------------------------------------------------
; PGBOOT transport: PicoGUS control port + PGDFS data window
; (sw/dfs/PROTOCOL.md, register sequences as in pgusdfs/xport.c)
; ---------------------------------------------------------------------------

; select: knock and select register AL. Clobbers DX.
select:
        mov     dx, CONTROL_PORT
        push    ax
        mov     al, KNOCK
        out     dx, al
        pop     ax
        out     dx, al
        ret

; rd8: AL = byte register AL (read through DATA_PORT_HIGH). Clobbers DX.
rd8:
        call    select
        mov     dx, DATA_PORT_HIGH
        in      al, dx
        ret

; rd16: AX = 16-bit register AL (word read at DATA_PORT_LOW, as pgusinit and
; PGUSDFS do: the bus splits it into 1D1h (low) and 1D2h (high)). Clobbers DX.
rd16:
        call    select
        mov     dx, DATA_PORT_LOW
        in      ax, dx
        ret

; detect_card: checks for a PicoGUS that can serve PGBOOT. No RAM needed.
; out: CF=0: BX = data window base, CX = max payload, AL = options (low nibble)
;      CF=1: SI = message (CS-relative) saying why not; AH = 0: nothing
;      answers (the card may still be starting: worth another look at
;      INT 19h), AH = 1: the card answers but cannot or must not serve
; clobbers AX, BX, CX, DX, SI
detect_card:
        mov     si, msg_nocard
        mov     al, CMD_MAGIC
        call    rd8
        cmp     al, MAGIC_VALUE
        jne     .absent
        mov     si, msg_oldfw
        mov     al, CMD_PROTOCOL
        call    rd8
        cmp     al, MIN_PROTOCOL
        jb      .fail
        mov     si, msg_nodfs
        mov     al, CMD_DFSMAXLEN
        call    rd16
        cmp     ax, 512
        jb      .fail
        cmp     ax, 32768
        ja      .fail
        mov     cx, ax
        mov     al, CMD_DFSPORT
        call    rd16
        and     ax, 0FFFEh
        jz      .fail
        mov     bx, ax
        mov     si, msg_oldfw
        mov     al, CMD_BDOPTS
        call    rd8
        mov     ah, al
        and     ah, 0C0h
        cmp     ah, 80h
        jne     .fail
        mov     si, msg_romoff
        test    al, BD_OPT_ROM_OFF
        jnz     .fail
        and     al, 0Fh
        clc
        ret
.absent:
        xor     ah, ah
        stc
        ret
.fail:
        mov     ah, 1
        stc
        ret

; detect_cpu: AL = 1 on an 80186 or later (shift counts are masked to 5
; bits there; an 8086/8088 shifts 33 times), 0 on an 8086/8088 (and on a
; NEC V20/V30, which then simply uses the loops). PGBOOT88 forces 0.
detect_cpu:
%ifdef FORCE8088
        xor     al, al
%else
        push    cx
        mov     al, 1
        mov     cl, 33
        shl     al, cl                  ; 186+: AL = 2; 8086: AL = 0
        shr     al, 1
        pop     cx
%endif
        ret

; out_raw: CX bytes from DS:SI to the data window at DX. BL = 1: 186+.
; Clobbers AX, CX, SI.
out_raw:
        shr     cx, 1
        pushf                           ; CF = odd trailing byte
        jcxz    .odd
        or      bl, bl
        jz      .loop
        call    out186
        jmp     short .odd
.loop:
        lodsw
        out     dx, ax                  ; AL -> DX, AH -> DX+1: stream order
        loop    .loop
.odd:
        popf
        jnc     .ret
        lodsb
        out     dx, al
.ret:
        ret

; in_raw: CX bytes from the data window at DX to ES:DI. BL = 1: 186+.
; Clobbers AX, CX, DI.
in_raw:
        shr     cx, 1
        pushf
        jcxz    .odd
        or      bl, bl
        jz      .loop
        call    in186
        jmp     short .odd
.loop:
        in      ax, dx                  ; DX -> AL, DX+1 -> AH
        stosw
        loop    .loop
.odd:
        popf
        jnc     .ret
        in      al, dx
        stosb
.ret:
        ret

; ---- 80186+ only: reached only when r_cpu = 1 ------------------------------
        cpu     186
out186:
        rep outsw
        ret
in186:
        rep insw
        ret
        cpu     8086

; poll: waits until CMD_DFSSTAT (already selected) reads READY, ABORTED or
; NODRIVE. Times out when [x_tmo] BIOS ticks have passed (counted as tick
; changes, so midnight and wrap-around do not matter) AND at least [x_pmin]
; x 65536 status reads were made (a sped-up timer cannot shorten it), or
; after 2 x [x_pmin] x 65536 reads whatever the ticks say (interrupts off).
; out: CF=0, AL = status; CF=1: timeout. Clobbers AX, BX, CX, DX, SI, DI.
poll:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     [x_units], ax
        mov     si, [es:046Ch]
        mov     di, [x_tmo]
        xor     cx, cx
        mov     dx, DATA_PORT_HIGH
.p:
        in      al, dx
        cmp     al, DFS_READY
        je      .fin
        cmp     al, DFS_ABORTED
        jae     .fin
        mov     ax, [es:046Ch]
        cmp     ax, si
        je      .same
        mov     si, ax
        or      di, di
        jz      .same
        dec     di
.same:
        loop    .p
        mov     ax, [x_units]
        inc     ax
        mov     [x_units], ax
        cmp     ax, [x_pmin]
        jb      .p                              ; too few reads for a timeout
        or      di, di
        jz      .tmo                            ; ticks and reads both done
        shr     ax, 1
        cmp     ax, [x_pmin]
        jb      .p
.tmo:
        pop     es
        stc
        ret
.fin:
        pop     es
        clc
        ret

; set_tmo: [x_tmo] = AX ticks, [x_pmin] to match (AX * 1.2). Clobbers AX, DX.
set_tmo:
        mov     [x_tmo], ax
        mov     dx, ax
        shr     dx, 1
        shr     dx, 1
        shr     dx, 1                           ; ticks / 8 ...
        add     ax, dx
        shr     dx, 1
        shr     dx, 1                           ; ... + ticks / 32 + ticks / 64
        add     ax, dx
        shr     dx, 1
        add     ax, dx
        mov     [x_pmin], ax
        ret

; xact: one PGDFS transaction, parameters in the x_* RAM variables (DS=RAM).
;   x_hdr/x_hlen: frame header and BD payload header (the length word is
;   filled in here); x_dlen bytes of data follow from x_dptr (or F6h filler
;   when x_fill); the answer payload goes to x_aptr, at most x_alen bytes
;   (the rest stays unread; the next CMD_DFSREQ discards it).
; out: CF=1: transport failure, AH = INT 13h status (80h timeout/no drive,
;      20h rejected twice / bad answer)
;      CF=0: AX = answer AX, CX = answer payload length
; Preserves BX, SI, DI, BP, DS, ES.
xact:
        push    bx
        push    si
        push    di
        push    es
        mov     byte [x_try], 2
.send:
        ; another client's request in flight (PGUSDFS interrupted by us):
        ; let it finish first, the card ignores a new request while BUSY
        mov     al, CMD_DFSSTAT
        call    rd8
        cmp     al, DFS_BUSY
        jne     .idle
        call    poll
        jc      .timeout
.idle:
        mov     al, CMD_DFSREQ
        call    select
        mov     ax, [x_hlen]
        add     ax, [x_dlen]
        mov     [x_hdr], ax
        mov     dx, [r_port]
        mov     bl, [r_cpu]
        mov     si, x_hdr
        mov     cx, [x_hlen]
        call    out_raw
        mov     cx, [x_dlen]
        jcxz    .exec
        cmp     byte [x_fill], 0
        jne     .fill
        push    ds
        lds     si, [x_dptr]
        call    out_raw
        pop     ds
        jmp     short .exec
.fill:
        shr     cx, 1                   ; always whole sectors: even
        mov     ax, 0F6F6h
.fl:
        out     dx, ax
        loop    .fl
.exec:
        mov     al, CMD_DFSEXEC
        call    select
        mov     dx, DATA_PORT_HIGH
        mov     al, 1
        out     dx, al
        mov     al, CMD_DFSSTAT
        call    select
        call    poll
        jc      .timeout
        cmp     al, DFS_READY
        je      .ready
        cmp     al, DFS_ABORTED
        jne     .nodrive
        dec     byte [x_try]
        jnz     .send
        mov     ah, 20h                 ; rejected twice
        jmp     short .fail
.nodrive:
        mov     ah, 80h
        jmp     short .fail
.timeout:
        mov     al, CMD_DFSSTAT
        call    select
        mov     dx, DATA_PORT_HIGH
        xor     al, al
        out     dx, al                  ; any write aborts
        mov     ah, 80h
        jmp     short .fail
.ready:
        mov     al, CMD_DFSRESP
        call    select
        mov     dx, [r_port]
        in      ax, dx                  ; total length
        mov     cx, ax
        in      ax, dx                  ; AX result
        mov     [x_ax], ax
        sub     cx, 4
        jb      .badlen
        mov     [x_plen], cx
        cmp     cx, [x_alen]
        jbe     .store
        mov     cx, [x_alen]
.store:
        jcxz    .done
        mov     bl, [r_cpu]
        les     di, [x_aptr]
        call    in_raw
.done:
        mov     ax, [x_ax]
        mov     cx, [x_plen]
        clc
        jmp     short .ret
.badlen:
        mov     ah, 20h
.fail:
        stc
.ret:
        pop     es
        pop     di
        pop     si
        pop     bx
        ret

; bdinfo: BDINFO for unit AL with flags AH, answer record into x_ans.
; out: CF=0 and the record valid (AX = 0, length >= 32); CF=1 otherwise.
; Clobbers AX, CX, DX.
bdinfo:
        mov     [x_hdr+4], ax           ; UU FF
        mov     word [x_hdr+2], (BD_AL_INFO << 8) | 0
        mov     word [x_hlen], 6
        xor     cx, cx
        mov     [x_dlen], cx
        mov     [x_fill], cl
        mov     word [x_alen], BI_MAXLEN
        mov     word [x_aptr], x_ans
        mov     [x_aptr+2], ds
        push    ax
        test    ah, BD_INFO_OPEN
        mov     ax, TMO_READ
        jz      .go
        mov     ax, TMO_OPEN
.go:
        call    set_tmo
        pop     ax
        call    xact
        jc      .ret
        or      ax, ax
        jnz     .bad
        cmp     cx, BI_NAME
        jb      .bad
        clc
        ret
.bad:
        stc
.ret:
        ret

; unit_load: takes the BDINFO record in x_ans for the unit at DI. The
; state, flags and generation are always copied; geometry, drive type and
; token only when the record describes a usable image (so an ejected floppy
; keeps answering AH=08h with the last geometry). Heads are capped at 255.
; out: CF=0 ready and usable, CF=1 not (token cleared). Clobbers AX.
unit_load:
        mov     al, [x_ans+BI_STATE]
        mov     [di+U_STATE], al
        mov     al, [x_ans+BI_FLAGS]
        mov     [di+U_FLAGS], al
        mov     al, [x_ans+BI_GEN]
        mov     [di+U_GEN], al
        cmp     byte [x_ans+BI_STATE], BD_STATE_READY
        jne     .bad
        mov     ax, [x_ans+BI_SPT]
        dec     ax
        cmp     ax, 63
        jae     .bad                            ; spt 1..63
        mov     ax, [x_ans+BI_HEADS]
        or      ax, ax
        jz      .bad
        mov     ax, [x_ans+BI_CYL]
        dec     ax
        cmp     ax, 1024
        jae     .bad                            ; cylinders 1..1024
        mov     ax, [x_ans+BI_TOTAL]
        or      ax, [x_ans+BI_TOTAL+2]
        jz      .bad
        mov     ax, [x_ans+BI_HEADS]
        cmp     ax, 255
        jbe     .h
        mov     ax, 255
.h:
        mov     [di+U_HEADS], ax
        mov     ax, [x_ans+BI_CYL]
        mov     [di+U_CYL], ax
        mov     ax, [x_ans+BI_SPT]
        mov     [di+U_SPT], ax
        mov     ax, [x_ans+BI_TOTAL]
        mov     [di+U_TOTAL], ax
        mov     ax, [x_ans+BI_TOTAL+2]
        mov     [di+U_TOTAL+2], ax
        mov     al, [x_ans+BI_FDTYPE]
        mov     [di+U_FDTYPE], al
        mov     ax, [x_ans+BI_TOKEN]
        mov     [di+U_TOKEN], ax
        mov     ax, [x_ans+BI_TOKEN+2]
        mov     [di+U_TOKEN+2], ax
        clc
        ret
.bad:
        xor     ax, ax
        mov     [di+U_TOKEN], ax
        mov     [di+U_TOKEN+2], ax
        stc
        ret
