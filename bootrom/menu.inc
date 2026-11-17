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
; The look is that of the PicoGUS web page (its Textmode design system,
; sw/web/www/textmode on the web server branch): a title bar, a framed list
; window with a shadow, a second window for the drives, a status line of
; Key=Action hints, in the colours of its three themes: Setup, Commander
; and Mono. The card keeps the choice (CMD_BDTHEME); a monochrome adapter
; always gets its own set of attributes.
;
; Screen: INT 10h AH=02h (cursor), AH=09h (a run of one character with an
; attribute) and teletype, which keeps the attribute of the cells it writes
; to. 80 columns, 25 rows.
; ---------------------------------------------------------------------------

MENU_ROWS       equ 16              ; entries per page (a power of two)
ROW_TITLE       equ 0
LIST_Y          equ 1               ; the list window: top row, left column,
LIST_X          equ 17              ; columns, rows (with its frame)
LIST_W          equ 46
LIST_H          equ MENU_ROWS + 2
MENU_Y          equ LIST_Y + 1      ; first entry row
ENT_X           equ LIST_X + 2      ; an entry: its bar,
ENT_W           equ LIST_W - 4
COL_TAG         equ ENT_X + 1       ; "A:" / "C:",
COL_SIZE        equ COL_TAG + 4     ; "1440K" / "<DIR>",
COL_NAME        equ COL_SIZE + 8    ; the name (27 characters at most)
POS_X           equ LIST_X + LIST_W - 2 - POS_W     ; "3/41" in the bottom frame
POS_W           equ 13
DRV_Y           equ 20              ; the drives window
DRV_X           equ 8
DRV_W           equ 64
DRV_H           equ 4
ROW_A           equ DRV_Y + 1
ROW_C           equ DRV_Y + 2
COL_DRV         equ DRV_X + 3       ; "A:"
COL_DNAME       equ COL_DRV + 4     ; the image in it
ROW_STATUS      equ 24
COL_TITLE       equ (80 - MENU_TITLE_LEN) / 2
COL_VERSION     equ 80 - 1 - MENU_VER_LEN
MENU_NAME_COLS  equ DRV_X + DRV_W - 3 - COL_DNAME   ; of an image path in the drives window
MENU_PATH_COLS  equ LIST_W - 8      ; of the folder path in the list window's frame

; A theme: what each part of the screen looks like (a character and/or an
; attribute). In a text, a byte 01h-0Fh gives the cells from the cursor to
; the end of the row the attribute with that index.
TH_SCR_CH       equ 0               ; the screen behind the windows: character,
TH_SCR          equ 1               ; attribute
TH_SHD_CH       equ 2               ; a window's shadow: character,
TH_SHD          equ 3               ; attribute
TH_TITLE        equ 4               ; title bar
TH_LIST         equ 5               ; list window: frame and fill
TH_LTITLE       equ 6               ; its title
TH_LTEXT        equ 7               ; an entry
TH_LKEY         equ 8               ; the A: / C: of an entry
TH_SEL          equ 9               ; the selected entry
TH_WIN          equ 10              ; drives window: frame, fill, text
TH_WTITLE       equ 11              ; its title and A: / C:
TH_STAT         equ 12              ; status line
TH_SKEY         equ 13              ; a key on the status line
TH_SIZE         equ 14
THEME_MDA       equ 3               ; index of the monochrome adapter's set

%if MENU_ROWS != 16 || (BD_LIST_HDR_LEN + MENU_ROWS * BD_LIST_ENTRY_LEN) > MENU_PAGE_BYTES
%error menu page size
%endif
%if COL_NAME + BD_LIST_DISP_LEN > ENT_X + ENT_W || POS_X < ENT_X
%error list window too narrow
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
        mov     ah, 0Fh                 ; AL = video mode
        int     10h
        mov     [m_mode], al
        cmp     al, 7                   ; 80x25 text: MDA/Hercules,
        je      .page
        cmp     al, 3                   ; colour,
        je      .page
        cmp     al, 2                   ; black and white
        je      .page
        mov     ax, 0003h               ; 40 columns, or a graphics mode a
        int     10h                     ; BIOS logo left behind: 80x25 text
.page:
        mov     ax, 0500h               ; the menu draws on page 0
        int     10h
        xor     ax, ax
        mov     [m_plen], ax
        mov     [m_top], ax
        mov     [m_sel], ax
        mov     [r_bootsel], al
.restyle:
        ; ---- everything that only changes with the theme ----------------
        mov     al, CMD_BDTHEME
        call    rd_ui                   ; 0 from firmware without themes
        cmp     byte [m_mode], 7
        jne     .theme
        mov     al, THEME_MDA
.theme:
        mov     ah, TH_SIZE
        mul     ah
        add     ax, theme_tab
        mov     [m_theme], ax
        mov     al, TH_SCR_CH
        call    th2
        xor     dx, dx
        mov     cx, 80
.screen:
        call    m_rep
        inc     dh
        cmp     dh, 25
        jb      .screen
        mov     al, TH_TITLE
        call    th
        xor     dx, dx
        call    m_fill
        mov     dl, COL_TITLE
        call    m_goto
        mov     si, msg_mtitle
        call    print
        mov     dl, COL_VERSION
        call    m_goto
        mov     si, msg_mver
        call    print
        mov     al, TH_WIN
        call    th
        mov     dx, (DRV_Y << 8) | DRV_X
        mov     cx, (DRV_H << 8) | DRV_W
        call    m_box
        mov     al, TH_WTITLE
        call    th
        mov     dl, DRV_X + (DRV_W - MENU_DRV_LEN) / 2
        mov     cx, MENU_DRV_LEN
        call    m_fill
        mov     si, msg_mdrives
        call    print
        call    m_hints
.reload:
        call    m_load
.header:
        call    m_header
        call    m_draw_all
.key:
        mov     dx, 25 << 8             ; the cursor: below the screen, out of sight
        call    m_goto
        xor     ah, ah
        int     16h
        cmp     byte [m_msgon], 0
        je      .nomsg
        call    m_hints                 ; a key takes the last message away
.nomsg:
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
        cmp     al, 'r'
        je      .rescan
        cmp     al, 't'
        je      .next_theme
        cmp     al, 'm'
        je      .next_tune
.tokey1:
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
        jc      .rescan                 ; nothing listed: look again
        cmp     byte [es:di+BD_LIST_OFF_KIND], BD_KIND_DIR
        jne     .boot
        call    m_chdir
        jc      .tokey
        xor     ax, ax
        mov     [m_top], ax
        mov     [m_sel], ax
.rescan:
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
.next_theme:
        mov     al, CMD_BDTHEME
        call    .next
        jmp     .restyle
.next_tune:
        mov     al, CMD_BDTUNE
        call    .next
        mov     cx, 3                   ; the card compiles the tune
        call    wait_ticks
        mov     si, msg_mtune
        call    m_msg
        mov     al, 1                   ; with its name
        call    tune_play
        jmp     .key
; the card moves the setting in register AL on to its next value
.next:
        call    select
        mov     dx, DATA_PORT_HIGH
        mov     al, BD_UI_NEXT
        out     dx, al
        ret
.esc:
        mov     byte [r_bootsel], 0
.exit:
        ; fall through: leave a clean screen for the boot messages

; m_cls: blank screen, cursor home. Preserves everything.
m_cls:
        pushm   ax, bx, cx, dx, si, di, bp
        mov     ax, 0600h
        mov     bh, 07h
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

; m_fill: CX blanks with attribute BL from row DH, column DL.
; m_rep: CX times character AL instead. The cursor is left at DH/DL, so that
; teletype output lands on those cells. Preserve everything.
m_fill:
        push    ax
        mov     al, ' '
        call    m_rep
        pop     ax
        ret
m_rep:
        pushm   ax, bx, cx, si, di, bp
        call    m_goto
        jcxz    .none
        mov     ah, 09h
        xor     bh, bh
        int     10h
.none:
        popm    ax, bx, cx, si, di, bp
        ret

; th2: AL = index in the theme -> AL = that byte, AH = the next one, BL = AH:
; a character and its attribute. th: AL = index -> BL = that attribute.
; Clobber AX.
th2:
        push    si
        mov     si, [m_theme]
        xor     ah, ah
        add     si, ax
        mov     ax, [cs:si]
        mov     bl, ah
        pop     si
        ret
th:
        call    th2
        mov     bl, al
        ret

; m_puts: the text at CS:SI from the cursor on. A byte 01h-0Fh gives the
; cells from the cursor to the end of the row the theme attribute with that
; index. Clobbers SI.
m_puts:
        pushm   ax, bx, cx, dx
.next:
        cs lodsb
        or      al, al
        jz      .done
        cmp     al, 10h
        jae     .char
        push    ax
        mov     ah, 03h                 ; DH, DL = where the cursor is
        xor     bh, bh
        int     10h
        pop     ax
        call    th
        mov     cx, 80
        sub     cl, dl
        call    m_fill
        jmp     short .next
.char:
        call    putc
        jmp     short .next
.done:
        popm    ax, bx, cx, dx
        ret

; m_msg: the text at CS:SI on the status line, until the next key.
; m_hints: the key hints there. Clobber SI.
m_hints:
        mov     si, msg_mhelp
        call    m_msg
        mov     byte [m_msgon], 0
        ret
m_msg:
        push    dx
        mov     dx, ROW_STATUS << 8
        call    m_goto
        call    m_puts
        mov     byte [m_msgon], 1
        pop     dx
        ret

; m_box: a window: top row DH, left column DL, CL columns, CH rows, in
; attribute BL, a single-line frame one cell inside its left and right
; edges, and the theme's shadow at its right and below. Preserves everything.
m_box:
        pushm   ax, bx, cx, dx, si, di
        mov     bh, ch                  ; rows to go
        xor     ch, ch
        mov     di, cx                  ; columns
        mov     si, box_top
.row:
        mov     cx, di
        call    m_fill
        inc     dl
        mov     cx, 1
        cs lodsb                        ; the frame: left,
        call    m_rep
        inc     dl
        mov     cx, di
        sub     cx, 4
        cs lodsb                        ; between,
        call    m_rep
        add     dl, cl
        mov     cx, 1
        cs lodsb                        ; right
        call    m_rep
        add     dl, 2                   ; the shadow starts a row down
        mov     cx, 2
        cmp     si, box_mid
        je      .lit
        call    m_shadow
.lit:
        mov     ax, di
        sub     dl, al
        inc     dh
        mov     si, box_mid
        dec     bh
        jz      .below
        cmp     bh, 1
        jne     .row
        mov     si, box_bottom
        jmp     short .row
.below:
        add     dl, 2
        mov     cx, di
        call    m_shadow
        popm    ax, bx, cx, dx, si, di
        ret

; m_shadow: CX cells of shadow from row DH, column DL. Preserves everything.
m_shadow:
        pushm   ax, bx
        mov     al, TH_SHD_CH
        call    th2
        call    m_rep
        popm    ax, bx
        ret

box_top         db 0DAh, 0C4h, 0BFh
box_mid         db 0B3h, ' ', 0B3h
box_bottom      db 0C0h, 0C4h, 0D9h

; The themes: the semantic colours of the web page (tokens.css) as text
; attributes (background << 4 | foreground). Order: Setup, Commander, Mono
; as CMD_BDTHEME numbers them, then the set for a monochrome adapter.
theme_tab:
        ;  screen     shadow     title list  ltit  ltext lkey  sel   win   wtit  stat  skey
        db 0B0h, 10h, ' ', 00h,  70h,  1Fh,  1Eh,  17h,  1Eh,  4Fh,  70h,  71h,  70h,  71h   ; Setup
        db ' ', 07h,  ' ', 07h,  30h,  30h,  30h,  30h,  31h,  0Fh,  70h,  71h,  07h,  0Fh   ; Commander
        db 0B0h, 08h, 0DBh, 08h, 70h,  07h,  0Fh,  07h,  0Fh,  70h,  07h,  0Fh,  70h,  70h   ; Mono
        db ' ', 07h,  ' ', 07h,  70h,  07h,  0Fh,  07h,  0Fh,  70h,  07h,  0Fh,  70h,  70h   ; MDA
%if ($ - theme_tab) != 4 * TH_SIZE
%error theme table
%endif

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

; m_header: the list window with the folder in its frame, and the images
; in A: and C: in the drives window. Preserves all but SI.
m_header:
        pushm   ax, bx, cx, dx
        mov     al, TH_LIST
        call    th
        mov     dx, (LIST_Y << 8) | LIST_X
        mov     cx, (LIST_H << 8) | LIST_W
        call    m_box
        ; the folder, centred in the top of the frame; "..." and its end
        ; when it is too long
        mov     si, m_path
        mov     cx, [m_plen]
        cmp     cx, MENU_PATH_COLS
        jbe     .fits
        add     si, cx
        mov     cx, MENU_PATH_COLS
        sub     si, cx
.fits:
        or      cx, cx
        jnz     .place
        inc     cx                      ; the root: a backslash
.place:
        mov     dl, LIST_W - 2
        sub     dl, cl
        shr     dl, 1
        add     dl, LIST_X
        mov     al, TH_LTITLE
        call    th
        add     cx, 2
        call    m_fill
        sub     cx, 2
        mov     al, ' '
        call    putc
        cmp     word [m_plen], 0
        je      .root
.path:
        lodsb
        cmp     word [m_plen], MENU_PATH_COLS
        jbe     .char
        cmp     cx, MENU_PATH_COLS - 3
        jbe     .char
        mov     al, '.'                 ; the first three of a cut path
.char:
        call    putc
        loop    .path
        jmp     short .drives
.root:
        mov     al, '\'
        call    putc
.drives:
        mov     al, ' '
        call    putc
        mov     dh, ROW_A
        mov     ax, ('A' << 8) | CMD_BDFDNAME
        call    .drive
        mov     dh, ROW_C
        mov     ax, ('C' << 8) | CMD_BDHDNAME
        call    .drive
        popm    ax, bx, cx, dx
        ret
; row DH: drive letter AH and the image name in register AL ("-": none)
.drive:
        push    ax
        mov     al, TH_WIN
        call    th
        mov     dl, DRV_X + 2
        mov     cx, DRV_W - 4
        call    m_fill
        mov     al, TH_WTITLE
        call    th
        mov     dl, COL_DRV
        mov     cx, 2
        call    m_fill
        pop     ax
        push    ax
        mov     al, ah
        call    putc
        mov     al, ':'
        call    putc
        mov     dl, COL_DNAME
        call    m_goto
        pop     ax
        call    select                  ; rewinds the name
        mov     cx, MENU_NAME_COLS
        mov     dx, DATA_PORT_HIGH
        in      al, dx
        or      al, al
        jnz     .name
        mov     al, '-'
        jmp     putc
.name:
        call    putc
        dec     cx
        jz      .cut
        mov     dx, DATA_PORT_HIGH
        in      al, dx
        or      al, al
        jnz     .name
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

; m_pos: " 3/41 " in the bottom of the list window's frame. Preserves everything.
m_pos:
        pushm   ax, bx, cx, dx
        mov     al, TH_LIST
        call    th
        mov     dx, ((LIST_Y + LIST_H - 1) << 8) | POS_X
        mov     cx, POS_W
        mov     al, 0C4h
        call    m_rep                   ; the frame under an older, longer number
        mov     al, ' '
        call    putc
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
        call    putc
        popm    ax, bx, cx, dx
        ret

; m_draw_row: the list row of entry AX (on the page that is loaded): the
; selection bar when it is the selected one, blank past the end of the list.
; "A:  1440K   name" / "C:  504M    name" / "    <DIR>   name".
; Preserves everything.
m_draw_row:
        pushm   ax, bx, cx, dx, si, di, es
        mov     si, ax
        sub     ax, [m_top]
        add     al, MENU_Y
        mov     dh, al
        mov     dl, ENT_X
        mov     al, TH_LTEXT
        cmp     si, [m_sel]
        jne     .attr
        cmp     word [m_total], 0
        je      .attr
        mov     al, TH_SEL
.attr:
        call    th
        mov     cx, ENT_W
        call    m_fill
        mov     ax, si
        call    m_entry
        jc      .done
        mov     al, [es:di+BD_LIST_OFF_KIND]
        or      al, al
        jnz     .image
        mov     dl, COL_SIZE
        call    m_goto
        push    si
        mov     si, msg_mdir
        call    print
        pop     si
        jmp     short .name
.image:
        mov     dl, COL_TAG
        cmp     si, [m_sel]
        je      .tag                    ; the bar keeps its own colour
        push    ax
        mov     al, TH_LKEY
        call    th
        mov     cx, 2
        call    m_fill
        pop     ax
.tag:
        call    m_goto
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
