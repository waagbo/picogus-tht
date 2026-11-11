; PGDFS.VXD - the VxD frame: device descriptor block, control procedure and
; the VMM and IFS manager services the C part (pgdfs9x.c) uses, as C
; functions (cdecl: arguments on the stack, result in EAX or EDX:EAX, EBX ESI
; EDI EBP kept).
;
; Copyright (C) 2026 PicoGUS contributors
; Distributed under the MIT license, see ../../LICENSE.
;
; A VxD service call is INT 20h followed by a dword: the device id in the high
; word, the service number in the low word. C-convention services take their
; arguments on the stack like a C function and leave them there.
;
; Assembled with the Open Watcom assembler: wasm -q vxd.asm

        .386p

; segments of a Watcom flat-model module; code and data all go into
; locked, preloaded objects (see the link file in the Makefile)
_LTEXT  segment dword public use32 'CODE'
_LTEXT  ends
_LDATA  segment dword public use32 'CODE'
_LDATA  ends
CONST   segment dword public use32 'DATA'
CONST   ends
CONST2  segment dword public use32 'DATA'
CONST2  ends
_DATA   segment dword public use32 'DATA'
_DATA   ends
_BSS    segment dword public use32 'BSS'
_BSS    ends
ZZBSS   segment dword public use32 'BSS'
ZZBSS   ends
DGROUP  group CONST, CONST2, _DATA, _BSS, ZZBSS

; VMM services (device 0001h)
SVC_Get_System_Time     equ 0001003Fh
SVC_HeapAllocate        equ 0001004Fh
SVC_HeapFree            equ 00010051h
SVC_CreateMutex         equ 00010101h
SVC_EnterMutex          equ 0001012Eh
SVC_LeaveMutex          equ 0001012Fh
SVC_RegOpenKey          equ 00010148h
SVC_RegCloseKey         equ 00010149h
SVC_RegQueryValueEx     equ 00010151h
SVC_Time_Slice_Sleep    equ 00010161h
; IFS manager services (device 0040h)
SVC_IFS_Get_Version     equ 00400000h
SVC_IFS_RegisterNet     equ 00400002h
SVC_IFS_Get_DOSTime     equ 00400007h
SVC_IFS_InitUseAdd      equ 0040002Ch
SVC_IFS_Ring0GetDriveInfo equ 00400035h
SVC_IFS_DosToWin32Time  equ 00400039h
SVC_IFS_MetaMatch       equ 0040003Dh
SVC_UniToBCS            equ 00400040h
SVC_BCSToUni            equ 00400042h
SVC_UniToUpper          equ 00400043h

; control messages
SYS_CRITICAL_INIT       equ 0
DEVICE_INIT             equ 1
INIT_COMPLETE           equ 2

VXDCALL macro svc
        int     20h
        dd      svc
        endm

        extrn   _pg_device_init:near
        extrn   _pg_init_complete:near

; ---- device descriptor block ------------------------------------------------

_LDATA  segment
        public  PGDFS_DDB
PGDFS_DDB label dword
        dd      0                       ; DDB_Next
        dw      0400h                   ; DDB_SDK_Version (Windows 95)
        dw      0                       ; DDB_Req_Device_Number: Undefined_Device_ID
        db      0, 3                    ; version 0.3
        dw      0                       ; DDB_Flags
        db      'PGDFS   '              ; DDB_Name
        dd      0A0010100h              ; DDB_Init_Order: FSD_Init_Order, after IFSMgr
        dd      offset Control          ; DDB_Control_Proc
        dd      0, 0                    ; V86 / PM API procedures
        dd      0, 0                    ; V86 / PM API CS:IP
        dd      0                       ; DDB_Reference_Data
        dd      0, 0                    ; service table and size: no services
        dd      0                       ; DDB_Win32_Service_Table
        dd      50726576h               ; DDB_Prev: 'Prev'
        dd      80                      ; DDB_Size
        dd      52737631h, 52737632h, 52737633h ; 'Rsv1' 'Rsv2' 'Rsv3'
_LDATA  ends

; the limits of the uninitialized data, cleared at Sys_Critical_Init
_BSS    segment
        public  _pg_bss_begin
_pg_bss_begin label byte
_BSS    ends
ZZBSS   segment
        public  _pg_bss_end
_pg_bss_end label byte
ZZBSS   ends

; ---- control procedure --------------------------------------------------------
; EAX = message; carry set on return = failure

_LTEXT  segment
Control proc near
        cmp     eax, SYS_CRITICAL_INIT
        jne     not_crit
        push    edi
        push    ecx
        push    eax
        mov     edi, offset _pg_bss_begin
        mov     ecx, offset _pg_bss_end
        sub     ecx, edi
        xor     eax, eax
        cld
        rep     stosb
        pop     eax
        pop     ecx
        pop     edi
        clc
        ret
not_crit:
        cmp     eax, DEVICE_INIT
        jne     not_init
        pushad
        cld
        call    _pg_device_init         ; int (void): nonzero = loaded
        test    eax, eax
        popad
        jz      failed
        clc
        ret
not_init:
        cmp     eax, INIT_COMPLETE
        jne     done
        pushad
        cld
        call    _pg_init_complete       ; void (void)
        popad
done:   clc
        ret
failed: stc
        ret
Control endp

; ---- C-convention services: arguments copied, EBX ESI EDI EBP kept ------------

CSERVICE macro name, svc, nargs
        local   n
        public  name
name    proc near
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        n = nargs
        rept    nargs
        push    dword ptr [ebp + 4 + 4 * n]
        n = n - 1
        endm
        VXDCALL svc
        add     esp, 4 * nargs
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret
name    endp
        endm

CSERVICE _vx_heap_alloc,        SVC_HeapAllocate, 2      ; void *(size, flags)
CSERVICE _vx_heap_free,         SVC_HeapFree, 2          ; unsigned (p, flags)
CSERVICE _vx_create_mutex,      SVC_CreateMutex, 2       ; void *(boost, flags)
CSERVICE _vx_enter_mutex,       SVC_EnterMutex, 2        ; void (mutex, flags)
CSERVICE _vx_leave_mutex,       SVC_LeaveMutex, 1        ; void (mutex)
CSERVICE _vx_reg_open_key,      SVC_RegOpenKey, 3        ; long (hkey, subkey, &hkey)
CSERVICE _vx_reg_close_key,     SVC_RegCloseKey, 1       ; long (hkey)
CSERVICE _vx_reg_query_value,   SVC_RegQueryValueEx, 6   ; long (hkey, name, 0, &type, data, &size)
CSERVICE _vx_ifs_get_version,   SVC_IFS_Get_Version, 0   ; int (void)
CSERVICE _vx_ifs_register_net,  SVC_IFS_RegisterNet, 3   ; int (connect, version, netid)
CSERVICE _vx_ifs_get_dostime,   SVC_IFS_Get_DOSTime, 0   ; dos_time (void): date << 16 | time
CSERVICE _vx_ifs_init_use_add,  SVC_IFS_InitUseAdd, 4    ; int (use_info_2 *, proid, static, lana)
CSERVICE _vx_ifs_dos_to_win32,  SVC_IFS_DosToWin32Time, 1 ; _FILETIME in EDX:EAX (dos_time)
CSERVICE _vx_ifs_metamatch,     SVC_IFS_MetaMatch, 3     ; int (pattern, name, flags)
CSERVICE _vx_uni_to_bcs,        SVC_UniToBCS, 5          ; _QWORD (dst, src, bytes, max, charset)
CSERVICE _vx_bcs_to_uni,        SVC_BCSToUni, 4          ; _QWORD (dst, src, bytes, charset)
CSERVICE _vx_uni_to_upper,      SVC_UniToUpper, 3        ; int (dst, src, bytes)

; ---- register services ------------------------------------------------------------

; unsigned vx_get_system_time(void): milliseconds since Windows started
        public  _vx_get_system_time
_vx_get_system_time proc near
        VXDCALL SVC_Get_System_Time
        ret
_vx_get_system_time endp

; void vx_time_slice_sleep(unsigned ms): the current thread idles that long
        public  _vx_time_slice_sleep
_vx_time_slice_sleep proc near
        push    ebx
        push    esi
        push    edi
        push    ebp
        mov     eax, [esp + 20]
        VXDCALL SVC_Time_Slice_Sleep
        pop     ebp
        pop     edi
        pop     esi
        pop     ebx
        ret
_vx_time_slice_sleep endp

; int vx_drive_is_ifs(int drive): 1 when IFSMgr manages the 0-based drive
        public  _vx_drive_is_ifs
_vx_drive_is_ifs proc near
        push    ebx
        push    esi
        push    edi
        push    ebp
        mov     edx, [esp + 20]
        VXDCALL SVC_IFS_Ring0GetDriveInfo
        mov     eax, 0
        jc      not_ifs
        mov     eax, 1
not_ifs:
        pop     ebp
        pop     edi
        pop     esi
        pop     ebx
        ret
_vx_drive_is_ifs endp

; port I/O (pgc_plat_* in pgdfs9x.c), in the C calling convention
        public  _vx_inb
_vx_inb proc near
        mov     edx, [esp + 4]
        xor     eax, eax
        in      al, dx
        ret
_vx_inb endp

        public  _vx_outb
_vx_outb proc near
        mov     edx, [esp + 4]
        mov     eax, [esp + 8]
        out     dx, al
        ret
_vx_outb endp

; void vx_insw(port, dst, words): rep insw to DS:dst (ES set to DS)
        public  _vx_insw
_vx_insw proc near
        push    edi
        push    es
        push    ds
        pop     es
        mov     edx, [esp + 12]
        mov     edi, [esp + 16]
        movzx   ecx, word ptr [esp + 20]
        cld
        rep     insw
        pop     es
        pop     edi
        ret
_vx_insw endp

; void vx_outsw(port, src, words): rep outsw from DS:src
        public  _vx_outsw
_vx_outsw proc near
        push    esi
        mov     edx, [esp + 8]
        mov     esi, [esp + 12]
        movzx   ecx, word ptr [esp + 16]
        cld
        rep     outsw
        pop     esi
        ret
_vx_outsw endp

_LTEXT  ends

        end
