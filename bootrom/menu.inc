; ---------------------------------------------------------------------------
; PGBOOT boot menu: pick the disk images on the USB drive at boot.
;
; Runs inside the late init (INT 19h), on the ROM's own stack, DS = RAM block.
; The card lists a folder with BDLIST (sw/bootdisk/PROTOCOL.md): its folders
; first, then its disk images, 16 entries at a time into the kilobyte below
; the RAM block, which nothing uses before the boot sector is loaded. An
; image is selected by writing its path to the card's image name register
; (CMD_BDFDNAME for a floppy-sized image, CMD_BDHDNAME for any other), the
; same registers pgusinit /fdimage and /hdimage write; the late init then
; opens and boots what the registers hold.
;
; Screen: INT 10h AH=02h (cursor), AH=09h (a row of blanks with an
; attribute) and teletype, which keeps the attribute of the cells it writes
; to. 80 columns, 25 rows, normal and reverse video only (works on MDA).
; ---------------------------------------------------------------------------

MENU_ROWS       equ 16              ; entries per page (a power of two)
MENU_Y          equ 5               ; first list row
ROW_TITLE       equ 0
ROW_A           equ 1
ROW_C           equ 2
ROW_DIR         equ 3
ROW_HELP        equ 22
ROW_MSG         equ 23
COL_SIZE        equ 5
COL_NAME        equ 13
COL_POS         equ 66
ATTR_NORMAL     equ 07h
ATTR_BAR        equ 70h
MENU_NAME_COLS  equ 72              ; of an image path shown in the header

%if MENU_ROWS != 16 || (BD_LIST_HDR_LEN + MENU_ROWS * BD_LIST_ENTRY_LEN) > MENU_PAGE_BYTES
%error menu page size
%endif

%macro pushm 1-*
%rep %0
        push    %1
%rotate 1
%endrep
%endmacro

%macro popm 1-*
%rep %0
%rotate -1
        pop     %1
%endrep
%endmacro

; menu_prompt: offers the boot menu for the configured number of seconds.
; out: AL = 0 time is up, or another key is waiting (it stays in the buffer
;      for the system that boots: F5/F8), 1 = M was pressed, 2 = Esc
; Clobbers AX, SI, DI, ES.
menu_prompt:
        call    print_pfx
        mov     si, msg_mprompt
        call    print
        mov     al, [r_menu]
        shr     al, 1
        shr     al, 1
        xor     ah, ah
        mov     [r_count], ax
        call    print_count
        xor     ax, ax
        mov     es, ax
        mov     si, [es:046Ch]
.sec:
        mov     di, TICKS_PER_SEC
.spin:
        mov     ah, 1
        int     16h
        jz      .nokey
        cmp     al, 1Bh
        je      .esc
        or      al, 20h
        cmp     al, 'm'
        je      .menu
        xor     al, al
        jmp     short .out
.nokey:
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:046Ch]
        cmp     ax, si
        je      .spin
        mov     si, ax
        dec     di
        jnz     .spin
        dec     word [r_count]
        jz      .tmo
        mov     al, 8
        call    putc
        call    putc
        call    print_count
        jmp     short .sec
.tmo:
        xor     al, al
        jmp     short .out
.esc:
        call    .take
        mov     al, 2
        jmp     short .out
.menu:
        call    .take
        mov     al, 1
.out:
        call    crlf
        ret
.take:
        xor     ah, ah
        int     16h
        ret

; boot_menu: the menu itself. Returns when the user leaves it: [r_bootsel] =
; BD_KIND_FLOPPY / BD_KIND_HARDDISK when an image was picked to boot from,
; 0 otherwise (Esc: boot what is configured, in the usual order).
; Clobbers AX, BX, CX, DX, SI, DI, ES.
boot_menu:
        mov     ax, 40h
        mov     es, ax
        cmp     byte [es:4Ah], 80
        jae     .cols
        mov     ax, 0003h               ; a 40-column mode: 80x25 text
        int     10h
.cols:
        xor     ax, ax
        mov     [m_plen], ax
        mov     [m_top], ax
        mov     [m_sel], ax
        mov     [r_bootsel], al
        call    m_cls
        mov     dh, ROW_TITLE
        mov     bl, ATTR_BAR
        mov     si, msg_mtitle
        call    m_text
        mov     dh, ROW_HELP
        mov     bl, ATTR_NORMAL
        mov     si, msg_mhelp
        call    m_text
.reload:
        call    m_load
.header:
        call    m_header
        call    m_draw_all
.key:
        xor     ah, ah
        int     16h
        push    ax
        xor     si, si
        call    m_msg                   ; a key takes the last message away
        pop     ax
        or      al, al
        jz      .ext
        cmp     al, 1Bh
        je      .esc
        cmp     al, 0Dh
        je      .enter
        cmp     al, ' '
        je      .space
        or      al, 20h
        cmp     al, 'a'
        je      .ejfd
        cmp     al, 'c'
        je      .ejhd
        cmp     al, 's'
        je      .save
        jmp     short .key
.ext:
        mov     bx, [m_sel]
        mov     al, ah
        cmp     al, 48h                 ; up
        je      .up
        cmp     al, 50h                 ; down
        je      .down
        cmp     al, 49h                 ; PgUp
        je      .pgup
        cmp     al, 51h                 ; PgDn
        je      .pgdn
        cmp     al, 47h                 ; Home
        je      .home
        cmp     al, 4Fh                 ; End
        je      .end
        jmp     short .key
.up:
        or      bx, bx
        jz      .key
        dec     bx
        jmp     short .move
.down:
        inc     bx
        jmp     short .move
.pgup:
        sub     bx, MENU_ROWS
        jnc     .move
.home:
        xor     bx, bx
        jmp     short .move
.pgdn:
        add     bx, MENU_ROWS
        jnc     .move
.end:
        mov     bx, 0FFFFh
.move:
        ; BX = the entry wanted: keep it inside the list
        mov     ax, [m_total]
        or      ax, ax
        jz      .key
        dec     ax
        cmp     bx, ax
        jbe     .inlist
        mov     bx, ax
.inlist:
        mov     ax, [m_sel]
        cmp     ax, bx
        je      .key
        mov     [m_sel], bx
        mov     dx, bx
        and     dl, 0F0h                ; its page (MENU_ROWS = 16)
        cmp     dx, [m_top]
        je      .samepage
        mov     [m_top], dx
        jmp     .reload
.samepage:
        call    m_draw_row              ; the row it left
        mov     ax, bx
        call    m_draw_row
        call    m_pos
        jmp     .key

.enter:
        mov     ax, [m_sel]
        call    m_entry
        jc      .tokey
        cmp     byte [es:di+BD_LIST_OFF_KIND], BD_KIND_DIR
        jne     .boot
        call    m_chdir
        jc      .tokey
        xor     ax, ax
        mov     [m_top], ax
        mov     [m_sel], ax
        jmp     .reload
.boot:
        call    m_insert
        mov     al, [es:di+BD_LIST_OFF_KIND]
        mov     [r_bootsel], al
        jmp     short .exit
.space:
        mov     ax, [m_sel]
        call    m_entry
        jc      .tokey
        cmp     byte [es:di+BD_LIST_OFF_KIND], BD_KIND_DIR
        je      .tokey
        call    m_insert
        jmp     .header
.ejfd:
        mov     al, CMD_BDFDNAME
        jmp     short .eject
.ejhd:
        mov     al, CMD_BDHDNAME
.eject:
        call    select                  ; rewinds the name
        mov     dx, DATA_PORT_HIGH
        xor     al, al
        out     dx, al                  ; an empty name: no image
        jmp     .header
.save:
        mov     al, CMD_SAVE
        call    select
        mov     dx, DATA_PORT_HIGH
        out     dx, al
        mov     cx, 5                   ; the card is busy writing its flash
        call    wait_ticks
        mov     si, msg_msaved
        call    m_msg
.tokey:
        jmp     .key
.esc:
        mov     byte [r_bootsel], 0
.exit:
        ; fall through: leave a clean screen for the boot messages

; m_cls: blank screen, cursor home. Preserves everything.
m_cls:
        pushm   ax, bx, cx, dx, si, di, bp
        mov     ax, 0600h
        mov     bh, ATTR_NORMAL
        xor     cx, cx
        mov     dx, 184Fh
        int     10h
        xor     dx, dx
        call    m_goto
        popm    ax, bx, cx, dx, si, di, bp
        ret

; m_goto: cursor to row DH, column DL. Preserves everything.
m_goto:
        pushm   ax, bx, si, di, bp
        mov     ah, 02h
        xor     bh, bh
        int     10h
        popm    ax, bx, si, di, bp
        ret

; m_line: row DH becomes blanks with attribute BL; the cursor is left at its
; first column. Preserves everything.
m_line:
        pushm   ax, bx, cx, dx, si, di, bp
        xor     dl, dl
        call    m_goto
        mov     ax, 0920h
        xor     bh, bh
        mov     cx, 80
        int     10h
        popm    ax, bx, cx, dx, si, di, bp
        ret

; m_msg: the message at CS:SI on the message row; SI = 0 clears the row.
; Clobbers SI.
m_msg:
        pushm   bx, dx
        mov     dh, ROW_MSG
        mov     bl, ATTR_NORMAL
        or      si, si
        jnz     .text
        call    m_line
        jmp     short .done
.text:
        call    m_text
.done:
        popm    bx, dx
        ret

; m_text: row DH with attribute BL, the text at CS:SI from its first column.
; Clobbers SI.
m_text:
        call    m_line
        jmp     print

; m_entry: ES:DI = the entry with index AX in the page that is loaded.
; CF=1: there is none. Clobbers AX.
m_entry:
        sub     ax, [m_top]
        cmp     ax, MENU_ROWS
        jae     .none
        cmp     al, [m_cnt]
        jae     .none
        mov     ah, BD_LIST_ENTRY_LEN
        mul     ah
        add     ax, BD_LIST_HDR_LEN
        mov     di, ax
        mov     es, [m_seg]
        clc
        ret
.none:
        stc
        ret

; m_load: fetch the page at [m_top] of the folder [m_path]. A folder that
; cannot be listed (the stick was swapped) sends the menu back to the root.
; Preserves everything but SI.
m_load:
        pushm   ax, cx, dx, es
.again:
        mov     ax, [m_top]
        mov     cx, MENU_ROWS
        call    bdlist
        jnc     .ok
        xor     ax, ax
        mov     [m_total], ax
        mov     [m_cnt], al
        mov     [m_top], ax
        mov     [m_sel], ax
        cmp     [m_plen], ax
        mov     [m_plen], ax
        jne     .again
        mov     si, msg_mlisterr
        jmp     short .say
.ok:
        mov     es, [m_seg]
        mov     al, [es:2]
        mov     [m_cnt], al
        mov     ax, [es:0]
        mov     [m_total], ax
        or      ax, ax
        jnz     .ret
        mov     si, msg_mempty
.say:
        call    m_msg
.ret:
        popm    ax, cx, dx, es
        ret

; m_header: the images in A: and C: and the folder shown. Preserves all but SI.
m_header:
        pushm   ax, bx, cx, dx
        mov     bl, ATTR_NORMAL
        mov     dh, ROW_A
        mov     ax, ('A' << 8) | CMD_BDFDNAME
        call    .drive
        mov     dh, ROW_C
        mov     ax, ('C' << 8) | CMD_BDHDNAME
        call    .drive
        mov     dh, ROW_DIR
        mov     si, msg_mfolder
        call    m_text
        mov     si, m_path
        mov     cx, [m_plen]
        jcxz    .root
.path:
        lodsb
        call    putc
        loop    .path
        jmp     short .done
.root:
        mov     al, '\'
        call    putc
.done:
        popm    ax, bx, cx, dx
        ret
; row DH: drive letter AH and the image name in register AL ("-": none)
.drive:
        call    m_line
        push    ax
        mov     al, ' '
        call    putc
        mov     al, ah
        call    putc
        mov     al, ':'
        call    putc
        mov     al, ' '
        call    putc
        pop     ax
        call    select                  ; rewinds the name
        mov     cx, MENU_NAME_COLS
        mov     dx, DATA_PORT_HIGH
        in      al, dx
        or      al, al
        jnz     .char
        mov     al, '-'
        jmp     putc
.char:
        call    putc
        dec     cx
        jz      .cut
        mov     dx, DATA_PORT_HIGH
        in      al, dx
        or      al, al
        jnz     .char
.cut:
        ret

; m_draw_all: the page, then the position. Preserves everything.
m_draw_all:
        pushm   ax, cx
        mov     ax, [m_top]
        mov     cx, MENU_ROWS
.row:
        call    m_draw_row
        inc     ax
        loop    .row
        popm    ax, cx
        ; fall through

; m_pos: "3/41" at the end of the folder row. Preserves everything.
m_pos:
        pushm   ax, cx, dx
        mov     dx, (ROW_DIR << 8) | COL_POS
        call    m_goto
        xor     dx, dx
        mov     ax, [m_sel]
        cmp     [m_total], dx
        je      .empty
        inc     ax
.empty:
        call    print_dec
        mov     al, '/'
        call    putc
        mov     ax, [m_total]
        call    print_dec
        mov     al, ' '
        mov     cx, 4                   ; over a longer number that was there
.pad:
        call    putc
        loop    .pad
        popm    ax, cx, dx
        ret

; m_draw_row: the list row of entry AX (on the page that is loaded): reverse
; video when it is the selected one, blank past the end of the list.
; "A:  1440K  name" / "C:  504M  name" / "    <DIR>  name". Preserves everything.
m_draw_row:
        pushm   ax, bx, cx, dx, si, di, es
        mov     si, ax
        sub     ax, [m_top]
        add     al, MENU_Y
        mov     dh, al
        mov     ax, si
        call    m_entry
        mov     bl, ATTR_NORMAL
        jc      .blank
        cmp     si, [m_sel]
        jne     .attr
        mov     bl, ATTR_BAR
.attr:
        call    m_line
        mov     dl, 1
        call    m_goto
        mov     al, [es:di+BD_LIST_OFF_KIND]
        or      al, al
        jnz     .image
        mov     dl, COL_SIZE
        call    m_goto
        mov     si, msg_mdir
        call    print
        jmp     short .name
.image:
        shl     al, 1                   ; 1 -> 'A', 2 -> 'C'
        add     al, 'A' - 2
        call    putc
        mov     al, ':'
        call    putc
        mov     dl, COL_SIZE
        call    m_goto
        push    dx
        mov     ax, [es:di+BD_LIST_OFF_SECTORS]
        mov     dx, [es:di+BD_LIST_OFF_SECTORS+2]
        call    print_size
        pop     dx
.name:
        mov     dl, COL_NAME
        call    m_goto
        mov     cl, [es:di+BD_LIST_OFF_DISPLEN]
        xor     ch, ch
        cmp     cx, BD_LIST_DISP_LEN
        ja      .done
        jcxz    .done
        lea     si, [di+BD_LIST_OFF_DISP]
.char:
        mov     al, [es:si]
        inc     si
        call    putc
        loop    .char
        jmp     short .done
.blank:
        call    m_line
.done:
        popm    ax, bx, cx, dx, si, di, es
        ret

; m_insert: the image of the entry at ES:DI goes into its drive: the folder,
; a backslash and its 8.3 name are written to CMD_BDFDNAME or CMD_BDHDNAME.
; Preserves everything.
m_insert:
        pushm   ax, cx, dx, si
        mov     al, CMD_BDFDNAME
        cmp     byte [es:di+BD_LIST_OFF_KIND], BD_KIND_FLOPPY
        je      .reg
        mov     al, CMD_BDHDNAME
.reg:
        call    select                  ; rewinds the name
        mov     dx, DATA_PORT_HIGH
        mov     si, m_path
        mov     cx, [m_plen]
        jcxz    .sep
.path:
        lodsb
        out     dx, al
        loop    .path
.sep:
        mov     al, '\'
        out     dx, al
        lea     si, [di+BD_LIST_OFF_SFN]
.name:
        mov     al, [es:si]
        inc     si
        out     dx, al
        or      al, al
        jnz     .name                   ; the terminator commits the name
        popm    ax, cx, dx, si
        ret

; m_chdir: [m_path] follows the folder entry at ES:DI: ".." drops the last
; folder, any other name is added. CF=1: the path would get too long for
; an image name below it. Preserves everything.
m_chdir:
        pushm   ax, bx, si
        mov     bx, [m_plen]
        lea     si, [di+BD_LIST_OFF_SFN]
        cmp     word [es:si], '..'
        jne     .down
        cmp     byte [es:si+2], 0
        jne     .down
.up:
        or      bx, bx
        jz      .set
        dec     bx
        cmp     byte [m_path+bx], '\'
        jne     .up
        jmp     short .set
.down:
        cmp     bx, MENU_PATH_MAX - BD_LIST_SFN_LEN
        ja      .long
        mov     byte [m_path+bx], '\'
        inc     bx
.copy:
        mov     al, [es:si]
        or      al, al
        jz      .set
        mov     [m_path+bx], al
        inc     si
        inc     bx
        jmp     short .copy
.set:
        mov     [m_plen], bx
        clc
        jmp     short .ret
.long:
        stc
.ret:
        popm    ax, bx, si
        ret

; wait_ticks: CX timer ticks (55 ms each). Clobbers CX.
wait_ticks:
        pushm   ax, es
        xor     ax, ax
        mov     es, ax
.tick:
        mov     ax, [es:046Ch]
.same:
        cmp     ax, [es:046Ch]
        je      .same
        loop    .tick
        popm    ax, es
        ret
