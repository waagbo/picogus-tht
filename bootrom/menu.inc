; ---------------------------------------------------------------------------
; PGBOOT boot menu: pick the disk images on the USB drive, and set up the
; card, at boot.
;
; Runs inside the late init (INT 19h), on the ROM's own stack, DS = RAM block.
; The menu is a list browser, and the card says what is in the lists (BDLIST,
; sw/bootdisk/PROTOCOL.md): the main menu, a folder of the USB drive with
; its floppy or its hard disk images, the start-up tunes, the card's modes,
; the colours. 14 entries at a time are loaded into the kilobyte below the
; RAM block, which nothing uses before the boot sector is loaded. What Enter
; does is in the entry's kind:
;   a folder        the list of that folder
;   a disk image    its path goes to the card's image name register
;                   (CMD_BDFDNAME for a floppy image, CMD_BDHDNAME for a
;                   hard disk image: what pgusinit /fdimage and /hdimage
;                   write), and the menu returns to the main menu
;   a menu          the list it names, with the entry's name as its title
;   a choice        the value it carries is written to the control register
;                   it names (the tune, the colours, "no image", save; the
;                   mode, followed by save and restart as pgusinit /mode
;                   does); the list is then read again
;   boot            the menu ends
; The late init then opens and boots what the image name registers hold. New
; firmware can add lists and choices without a new ROM.
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

MENU_ROWS       equ 14              ; entries per page
ROW_TITLE       equ 0
LIST_Y          equ 2               ; the list window: top row (a row below
LIST_X          equ 17              ; the title bar), left column, columns
LIST_W          equ 46
MENU_Y          equ LIST_Y + 1      ; first entry row
ENT_X           equ LIST_X + 2      ; an entry: its bar,
ENT_W           equ LIST_W - 4
COL_TAG         equ ENT_X + 2       ; (the check mark is at ENT_X) "A:" / "C:",
COL_ITEM        equ ENT_X + 3       ; the name of a menu entry,
COL_SIZE        equ COL_TAG + 4     ; "1440K" / "<DIR>",
COL_NAME        equ COL_SIZE + 8    ; the name of a file (27 characters at most)
COL_MORE        equ ENT_X + ENT_W - 2   ; the arrow of an entry that opens a list
POS_X           equ LIST_X + LIST_W - 2 - POS_W     ; "3/41" in the bottom frame
POS_W           equ 13
DRV_Y           equ 19              ; the drives window: a row is left
DRV_X           equ 8               ; between it and the status line
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

%if (BD_LIST_HDR_LEN + MENU_ROWS * BD_LIST_ENTRY_LEN) > MENU_PAGE_BYTES
%error menu page size
%endif
%if LIST_Y + MENU_ROWS + 2 >= DRV_Y || DRV_Y + DRV_H >= ROW_STATUS
%error the windows overlap
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

; boot_menu: the menu itself. Returns when the user leaves it (Esc in the
; main menu, or its Boot entry); what the image name registers then hold is
; what boots. Clobbers AX, BX, CX, DX, SI, DI, BP, ES.
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
.main:
        ; ---- the main menu, with entry AX selected -------------------------
        mov     [m_sel], ax
        push    ds
        pop     es
        mov     si, msg_mmain
        mov     di, m_title
        mov     cx, MENU_MAIN_LEN
        mov     [m_tlen], cl
.title:
        cs lodsb
        stosb
        loop    .title
        mov     al, BD_LIST_MAIN
.list:
        ; ---- list AL from its top: [m_sel] and the title are set -----------
        mov     [m_list], al
        xor     ax, ax
        mov     [m_plen], ax
        mov     [m_top], ax
.restyle:
        ; ---- everything but the list window --------------------------------
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
        mov     bx, [m_sel]
        cmp     al, 1Bh
        je      .esc
        cmp     al, 0Dh
        je      .enter
        or      al, 20h
        cmp     al, 'r'
        je      .reload                 ; read the list again
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
        push    ax                      ; the entry it left
        mov     ax, bx
        xor     dx, dx
        mov     cx, MENU_ROWS
        div     cx
        mul     cx                      ; the top of its page
        cmp     ax, [m_top]
        je      .samepage
        mov     [m_top], ax
        pop     ax
        jmp     .reload
.samepage:
        pop     ax
        call    m_draw_row              ; the row it left
        mov     ax, bx
        call    m_draw_row
        call    m_pos
.tokey:
        jmp     .key

.enter:
        mov     ax, bx
        call    m_entry
        jc      .reload                 ; nothing listed: look again
        mov     al, [es:di+BD_LIST_OFF_KIND]
        cmp     al, BD_KIND_DIR
        je      .folder
        cmp     al, BD_KIND_HARDDISK
        jbe     .image
        cmp     al, BD_KIND_MENU
        je      .open
        cmp     al, BD_KIND_CHOICE
        je      .choice
        jmp     .exit                   ; boot (or what newer firmware means by it)
.folder:
        call    m_chdir
        jc      .tokey
        xor     ax, ax
        mov     [m_top], ax
        mov     [m_sel], ax
.toreload:
        jmp     .reload
.image:
        call    m_insert                ; into its drive, and back to the main menu
.back:
        mov     ax, [m_back]
        jmp     .main
.open:
        ; the list the entry names; its name is the title
        mov     [m_back], bx
        mov     cl, [es:di+BD_LIST_OFF_DISPLEN]
        xor     ch, ch
        cmp     cl, BD_LIST_DISP_LEN
        jbe     .named
        mov     cl, BD_LIST_DISP_LEN
.named:
        mov     [m_tlen], cl
        lea     si, [di+BD_LIST_OFF_DISP]
        mov     al, [es:di+BD_LIST_OFF_ARG]
        mov     di, m_title
        push    ds
        push    es
        pop     ds                      ; DS = the page, ES = the RAM block
        pop     es
        rep movsb
        push    es
        pop     ds
        mov     [m_sel], cx             ; 0
        jmp     .list
.choice:
        ; the value goes to the register the entry names. The card may be
        ; storing its settings or compiling a tune after it: give it time
        mov     bp, [es:di+BD_LIST_OFF_ARG]
        mov     ax, bp
        call    .put
        mov     ax, bp
        cmp     al, CMD_BOOTMODE
        jne     .settle
        ; the mode: as pgusinit /mode does, store it and restart the card.
        ; The card forgets what is not stored when it restarts.
        mov     al, CMD_SAVE
        call    .put
        mov     al, CMD_REBOOT
        call    .put
.settle:
        mov     di, 10 * TICKS_PER_SEC  ; until the card answers (again)
.card:
        call    detect_card
        jnc     .answers
        or      ah, ah
        jnz     .answers
        mov     cx, 1
        call    wait_ticks
        dec     di
        jnz     .card
.answers:
        mov     ax, bp
        cmp     al, CMD_SAVE
        jne     .played
        mov     si, msg_msaved
        call    m_msg
        jmp     .key
.played:
        cmp     al, CMD_BDTUNE
        jne     .chosen
        xor     al, al
        call    tune_play               ; the tune that was chosen
.chosen:
        jmp     .restyle                ; the colours may be new, the list's mark has moved
; control register AL gets the value AH, and the card a quarter of a second
.put:
        call    select
        mov     dx, DATA_PORT_HIGH
        mov     al, ah
        out     dx, al
        mov     cx, 5
        jmp     wait_ticks
.esc:
        cmp     byte [m_list], BD_LIST_MAIN
        jne     .back
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
        cmp     byte [m_list], BD_LIST_MAIN
        jne     .show
        mov     si, msg_mhelp0
.show:
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
        mov     byte [m_rows], MENU_ROWS
        mov     ax, [m_top]
        mov     cl, MENU_ROWS
        mov     ch, [m_list]
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
        jz      .empty
        ; a menu's window is as tall as the menu; a folder's keeps its size
        cmp     byte [m_list], BD_LIST_MAIN
        jb      .ret
        cmp     ax, MENU_ROWS
        jae     .ret
        mov     [m_rows], al
        jmp     short .ret
.empty:
        mov     si, msg_mempty
.say:
        call    m_msg
.ret:
        popm    ax, cx, dx, es
        ret

; m_header: the list window with its title in its frame (the folder when
; the list is of one below the root, "..." and its end when that is too
; long), and the images in A: and C: in the drives window. Preserves all but SI.
m_header:
        pushm   ax, bx, cx, dx, di
        mov     al, TH_LIST
        call    th
        mov     dx, (LIST_Y << 8) | LIST_X
        mov     cl, LIST_W
        mov     ch, [m_rows]
        add     ch, 2
        call    m_box
        mov     si, m_path
        mov     cx, [m_plen]
        or      cx, cx
        jnz     .long
        mov     si, m_title
        mov     cl, [m_tlen]
.long:
        mov     di, cx                  ; its whole length
        cmp     cx, MENU_PATH_COLS
        jbe     .place
        add     si, cx
        mov     cx, MENU_PATH_COLS
        sub     si, cx
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
        jcxz    .drives
.path:
        lodsb
        cmp     di, MENU_PATH_COLS
        jbe     .char
        cmp     cx, MENU_PATH_COLS - 3
        jbe     .char
        mov     al, '.'                 ; the first three of a cut path
.char:
        call    putc
        loop    .path
.drives:
        mov     al, ' '
        call    putc
        mov     dh, ROW_A
        mov     ax, ('A' << 8) | CMD_BDFDNAME
        call    .drive
        mov     dh, ROW_C
        mov     ax, ('C' << 8) | CMD_BDHDNAME
        call    .drive
        popm    ax, bx, cx, dx, di
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
        mov     cl, [m_rows]
        xor     ch, ch
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
        mov     dh, [m_rows]
        add     dh, LIST_Y + 1
        mov     dl, POS_X
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
        ; a check mark: the choice in effect, the image that is in the drive
        test    byte [es:di+BD_LIST_OFF_FLAGS], BD_LIST_FLAG_CURRENT
        jz      .kind
        mov     al, 0FBh
        call    putc
.kind:
        mov     al, [es:di+BD_LIST_OFF_KIND]
        cmp     al, BD_KIND_HARDDISK
        ja      .item
        or      al, al
        jnz     .image
        mov     dl, COL_SIZE
        call    m_goto
        push    si
        mov     si, msg_mdir
        call    print
        pop     si
        jmp     short .name
.item:
        ; a menu entry: an arrow when it opens a list
        cmp     al, BD_KIND_MENU
        jne     .text
        mov     dl, COL_MORE
        call    m_goto
        mov     al, 10h
        call    putc
.text:
        mov     dl, COL_ITEM
        jmp     short .at
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
.at:
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
