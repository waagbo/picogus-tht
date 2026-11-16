; ---------------------------------------------------------------------------
; PGBOOT - PicoGUS boot ROM: constants, ROM header fields, RAM layout
; See sw/bootdisk/PROTOCOL.md, sw/dfs/PROTOCOL.md, common/picogus.h.
; ---------------------------------------------------------------------------

%define PGBOOT_VERSION  "0.1"

ROM_SIZE        equ 8192
ROM_BLOCKS      equ ROM_SIZE / 512

; ---- PicoGUS control protocol (common/picogus.h) --------------------------
CONTROL_PORT    equ 1D0h
DATA_PORT_LOW   equ 1D1h
DATA_PORT_HIGH  equ 1D2h
KNOCK           equ 0CCh
MAGIC_VALUE     equ 0DDh
MIN_PROTOCOL    equ 5

CMD_MAGIC       equ 00h
CMD_PROTOCOL    equ 01h
CMD_DFSSTAT     equ 80h
CMD_DFSREQ      equ 81h
CMD_DFSEXEC     equ 82h
CMD_DFSRESP     equ 83h
CMD_DFSMAXLEN   equ 85h
CMD_DFSPORT     equ 87h
CMD_BDFDNAME    equ 88h
CMD_BDHDNAME    equ 89h
CMD_BDOPTS      equ 8Ah

DFS_READY       equ 03h
DFS_ABORTED     equ 0FEh
DFS_NODRIVE     equ 0FFh

; ---- PGBOOT frames (sw/bootdisk/PROTOCOL.md) ------------------------------
BD_AL_INFO      equ 0F3h
BD_AL_READ      equ 0F4h
BD_AL_WRITE     equ 0F5h
BD_INFO_OPEN    equ 01h

BD_OPT_FD_RO    equ 01h
BD_OPT_HD_RO    equ 02h
BD_OPT_ROM_OFF  equ 04h
BD_OPT_BOOT_HD  equ 08h

; BDINFO record offsets
BI_VERSION      equ 0
BI_STATE        equ 1
BI_TYPE         equ 2
BI_FLAGS        equ 3
BI_CYL          equ 4
BI_HEADS        equ 6
BI_SPT          equ 8
BI_TOTAL        equ 12
BI_FDTYPE       equ 16
BI_GEN          equ 17
BI_NAMELEN      equ 18
BI_TOKEN        equ 20              ; u32 image token, echoed in BDREAD/BDWRITE
BI_NAME         equ 32
BI_MAXLEN       equ 32 + 63

BIF_RO          equ 01h
BD_STATE_READY  equ 1
BD_STATE_NODRIVE equ 4
BD_STATE_FRAG   equ 5
BD_ST_CHANGED   equ 06h             ; floppy token mismatch: media changed
BD_IO_HDR_LEN   equ 10              ; UU NN LLLL TTTT
DFS_BUSY        equ 02h
BIF_CHANGED     equ 04h

; ---- timing ------------------------------------------------------------------
; Poll timeouts in BIOS ticks. The card's USB write deadline is 10 s per
; operation and one BDWRITE (write + sync) can take several, so writes get
; 30 s. poll also requires a minimum number of status reads (one ISA read
; takes >= ~0.7 us), so a program that speeds up the timer cannot shorten
; the timeout: PMIN units of 65536 reads = ticks * 55 ms / 0.7 us / 65536.
TMO_READ        equ 182         ; ~10 s: reads, verify, BDINFO
TMO_WRITE       equ 546         ; ~30 s: writes
TMO_OPEN        equ 546         ; ~30 s: BDINFO with OPEN (fast-seek map build)
WAIT_SECONDS    equ 15          ; USB drive wait at late init
TICKS_PER_SEC   equ 18

; interrupt vector that holds the pre-PGBOOT INT 19h between POST and late
; init when the ROM runs from real ROM (it cannot write to itself). Vectors
; 60h-6Fh are free between POST and boot; 6Bh is unassigned in the IBM list.
%ifndef OLD19_VEC
OLD19_VEC       equ 6Bh
%endif

; ---- ROM header (fixed offsets, the loader PGBOOT.COM relies on them) ------
HDR_SIG         equ 06h         ; 'PGBT'
HDR_IFVER       equ 0Ah         ; loader interface version (1)
HDR_FLAGS       equ 0Bh         ; bit 0: image is a writable RAM copy (loader)
HDR_BIOS13      equ 0Ch         ; dd: original BIOS INT 13h (0:0 = use the vector)
HDR_OLD19       equ 10h         ; dd: previous INT 19h (only written in a RAM copy)
HDR_USED        equ 14h         ; dw: bytes of the image in use (info)
HDR_IRET        equ 16h         ; db 0CFh: an IRET the loader may point vectors at
HDR_CATCH       equ 1Ch         ; dw: offset of the boot catcher (loader)
HDRF_RAMCOPY    equ 01h
HDRF_CATCH      equ 02h         ; set by the catcher: late init runs inside the BIOS INT 13h call

; unit structure (U_SIZE bytes)
U_SIZE          equ 24
U_CYL           equ 0           ; dw cylinders (CHS view)
U_HEADS         equ 2           ; dw
U_SPT           equ 4           ; dw
U_TOTAL         equ 6           ; dd total sectors
U_FDTYPE        equ 10          ; db INT 13h/08h BL
U_FLAGS         equ 11          ; db BDINFO flags (bit 0 read-only)
U_STATE         equ 12          ; db BDINFO state (1 = ready)
U_UNIT          equ 13          ; db card unit number
U_GEN           equ 14          ; db media generation
U_TOKEN         equ 16          ; dd image token from the last BDINFO

; ---- RAM segment (1 KB at the top of conventional memory) ------------------
; All mutable state lives here; DS points at it inside the handlers.
struc ram
r_sig           resb 4          ; 'PGBT'
r_old13         resd 1          ; INT 13h handler we chain to
r_old19         resd 1          ; INT 19h handler we chain to
r_romseg        resw 1          ; our code segment
r_port          resw 1          ; data window base
r_maxrd         resb 1          ; sectors per read frame
r_maxwr         resb 1          ; sectors per write frame
r_cpu           resb 1          ; 1 = 186+ (rep insw/outsw)
r_units         resb 1          ; bit 0 floppy image at 00h, bit 1 hard disk image at 80h
r_hdmax         resb 1          ; 80h + number of physical hard disks (last translated DL)
r_equip         resw 1          ; 40:10h before we changed it
r_hdcnt         resb 1          ; 40:75h before we changed it
r_opts          resb 1          ; CMD_BDOPTS & 0Fh
r_fdchg         resb 1          ; floppy media change seen, not yet reported by AH=16h
r_fresh         resb 1          ; memory was reserved by this late init
r_cur           resw 1          ; current unit structure (offset)
r_isfd          resb 1          ; current request is for the floppy image
r_bootdrv       resb 1
r_oldss         resw 1          ; stack of the INT 19h caller while late init runs
r_oldsp         resw 1
r_count         resw 1          ; scratch
r_shown         resb 1          ; wait message on screen
r_probe         resb 1          ; unit to ask whether the drive is mounted
r_busy          resb 1          ; INT 13h request in progress (reentry guard)
                alignb 16
u_fd            resb U_SIZE     ; unit structures, see U_*
u_hd            resb U_SIZE
r_dpt           resb 16         ; diskette parameter table returned by AH=08h
r_tramp13       resb 8          ; push cs / push cs / jmp far ROM:int13_entry
; transport parameters (xact)
x_hdr           resb 16         ; request frame header + BD payload header
x_hlen          resw 1          ; bytes of x_hdr to send
x_dlen          resw 1          ; data bytes after the header
x_dptr          resd 1          ; where they come from
x_alen          resw 1          ; answer payload bytes to store (rest discarded)
x_aptr          resd 1          ; where they go
x_ax            resw 1          ; answer AX
x_plen          resw 1          ; answer payload length
x_tmo           resw 1          ; timeout in ticks
x_pmin          resw 1          ; minimum status reads before a timeout, units of 65536
x_units         resw 1          ; status reads so far, units of 65536
x_try           resb 1
x_fill          resb 1          ; data out is F6h filler, not x_dptr
; disk_io parameters
io_lba          resd 1
io_count        resw 1
io_done         resw 1
io_off          resw 1          ; caller's buffer (seg:off as given) ...
io_seg          resw 1
io_lin          resd 1          ; ... as a linear address, advanced per frame
io_op           resb 1          ; IO_READ / IO_WRITE / IO_VERIFY / IO_FILL
io_n            resb 1          ; sectors in the current chunk
                alignb 16
x_ans           resb 128        ; BDINFO answer
ram_vars_end:
endstruc

RAM_STACK_TOP   equ 400h        ; late init runs on a stack at the top of the 1 KB

IO_READ         equ 0
IO_WRITE        equ 1
IO_VERIFY       equ 2
IO_FILL         equ 3
