;============================================================================
; HANDLER.ASM  -  TNFSDRV interrupt handlers
; Assemble: wasm -2 -ms handler.asm
;
; Contains two separate interrupt handlers:
;
;   new_int21__  (LEGACY)
;     Intercepts INT 21h AH=4Eh/4Fh/43h for C:\TNFS path-based virtual FS.
;     Logs many other AH= values as diagnostics.
;     All other calls chain to the original DOS INT 21h vector.
;
;   new_int2f__  (ACTIVE: network redirector)
;     Handles INT 2Fh AH=11h (MS-DOS network redirector API).
;     Dispatches on AL:
;       AL=05h  CHDIR      — return AX=0, CF=0
;       AL=1Bh  FINDFIRST  — calls do_findfirst() in tsr.c
;       AL=1Ch  FINDNEXT   — calls do_findnext() in tsr.c
;     All other AL values: log + chain to old vector.
;
; Stack layout (both handlers, same push sequence → mov bp,sp):
;   [bp+ 0] BP      [bp+10] DX
;   [bp+ 2] ES      [bp+12] CX
;   [bp+ 4] DS *    [bp+14] BX
;   [bp+ 6] DI      [bp+16] AX
;   [bp+ 8] SI      [bp+18] IP  \
;                   [bp+20] CS   > from INT instruction
;                   [bp+22] FLAGS/
;   * = caller's DS (NOT DGROUP; handler sets DS=DGROUP on entry)
;============================================================================

        .model  small

;--- Macro: redirect IRET target of the chained handler to ret_label --------
SETUP_RETURN_HOOK MACRO ret_label
    mov     ax, [bp+18]
    mov     cs:orig_ip, ax
    mov     ax, [bp+20]
    mov     cs:orig_cs, ax
    mov     ax, [bp+22]
    mov     cs:orig_flags, ax
    mov     word ptr [bp+18], offset ret_label
    mov     ax, cs
    mov     word ptr [bp+20], ax
ENDM

;--- Macro: return to original caller via saved orig_* triplet --------------
IRET_TO_CALLER MACRO
    mov     word ptr cs:orig_bx, bx
    mov     bx, word ptr cs:orig_flags
    push    bx
    mov     bx, word ptr cs:orig_cs
    push    bx
    mov     bx, word ptr cs:orig_ip
    push    bx
    mov     bx, word ptr cs:orig_bx
    iret
ENDM

extrn   _old_int21          : dword
extrn   _in_handler         : byte
extrn   _find_state         : byte
extrn   _last_search_attr   : byte
extrn   _log_int21          : near   ; void __cdecl (unsigned ax)
extrn   _log_29             : near   ; void __cdecl (unsigned ax, unsigned ds, unsigned si)
extrn   _log_1a             : near   ; void __cdecl (unsigned ds, unsigned dx)
extrn   _log_2f_return      : near   ; void __cdecl (unsigned es, unsigned bx)
extrn   _log_3b             : near   ; void __cdecl (unsigned ds, unsigned dx)
extrn   _log_47             : near   ; void __cdecl (unsigned dx)
extrn   _log_4e_pre         : near   ; void __cdecl (unsigned ds, unsigned dx, unsigned cx)
extrn   _log_4e_dta         : near   ; void __cdecl (unsigned es, unsigned bx)
extrn   _log_dta_dump       : near   ; void __cdecl (unsigned es, unsigned bx)
extrn   _log_4f_pre         : near   ; void __cdecl (unsigned state)
extrn   _log_handled_4e     : near   ; void __cdecl (void)
extrn   _log_handled_4f     : near   ; void __cdecl (unsigned entry)
extrn   _log_3d             : near   ; void __cdecl (unsigned ds, unsigned dx)
extrn   _log_43             : near   ; void __cdecl (unsigned ax, unsigned ds, unsigned dx)
extrn   _log_43_result      : near   ; void __cdecl (unsigned result)
extrn   _log_60             : near   ; void __cdecl (unsigned ds, unsigned si)
extrn   _log_11_fcb_raw     : near   ; void __cdecl (unsigned ds, unsigned dx)
extrn   _log_12_pre         : near   ; void __cdecl (void)
extrn   _log_12_chain_return : near  ; void __cdecl (unsigned ax)
extrn   _log_fcb_result_raw  : near  ; void __cdecl (unsigned es, unsigned bx)
extrn   _log_59_return      : near   ; void __cdecl (unsigned ax, unsigned bx, unsigned cx)
extrn   _log_6c             : near   ; void __cdecl (unsigned ds, unsigned si, unsigned bx, unsigned dx)
extrn   _log_42             : near   ; void __cdecl (unsigned ax, unsigned bx, unsigned cx, unsigned dx)
extrn   _log_3f             : near   ; void __cdecl (unsigned bx, unsigned cx)
extrn   _log_3e             : near   ; void __cdecl (unsigned bx)
extrn   _fill_dta_new       : near   ; void __cdecl (unsigned es, unsigned bx, unsigned entry)
extrn   _fill_dta_fcb_ext   : near   ; void __cdecl (unsigned es, unsigned bx, unsigned entry)
extrn   _classify_4e_content : near  ; int  __cdecl (unsigned ds, unsigned dx)
extrn   _handle_43          : near   ; int  __cdecl (unsigned ax, unsigned ds, unsigned dx)
extrn   _match_base         : near   ; int  __cdecl (unsigned ds, unsigned dx)
extrn   _match_tnfs_tail    : near   ; int  __cdecl (unsigned ds, unsigned dx)
extrn   _fcb_set_state      : near   ; void __cdecl (unsigned ds, unsigned dx, unsigned state)
extrn   _fcb_set_readme_size : near  ; void __cdecl (unsigned ds, unsigned dx)
extrn   _fcb_get_state      : near   ; unsigned __cdecl (unsigned ds, unsigned dx)
extrn   _log_fcb_bytes      : near   ; void __cdecl (unsigned ds, unsigned dx, unsigned tag)
extrn   _log_3b_set         : near   ; void __cdecl (unsigned flag)
extrn   _in_tnfs_dir        : byte   ; set 1 by AH=3B CD-into, cleared by CD-away
extrn   _last_tnfs_fcb_eof  : byte   ; set 1 by do_12_eof; consumed by AH=59 intercept
extrn   _log_handled_11     : near   ; void __cdecl (unsigned entry)
extrn   _log_handled_12     : near   ; void __cdecl (unsigned entry)
extrn   _fcb_find_state     : byte
extrn   _old_int2f          : dword
extrn   _log_2f_call        : near   ; void __cdecl (unsigned ax, unsigned bx, unsigned cx, unsigned dx, unsigned ds, unsigned si, unsigned es, unsigned di)
extrn   _do_chdir           : near   ; unsigned __cdecl (void) — 0=ok, 3=path not found
extrn   _do_findfirst       : near   ; unsigned __cdecl (void)
extrn   _do_findnext        : near   ; unsigned __cdecl (unsigned es, unsigned di)
extrn   _do_getattr         : near   ; unsigned __cdecl (void) — 0xFFFF=not found, else attr
extrn   _do_open            : near   ; unsigned __cdecl (unsigned es, unsigned di) — 0=ok, else error
extrn   _do_spopen          : near   ; unsigned __cdecl (unsigned es, unsigned di) — 0=ok, else error
extrn   _do_read            : near   ; unsigned __cdecl (unsigned es, unsigned di, unsigned cx) — buf from SDA->curr_dta
extrn   _do_close           : near   ; void __cdecl (void)

;============================================================================
_TEXT   segment byte public 'CODE'
        assume  cs:_TEXT, ds:DGROUP, ss:DGROUP

old21_off   dw  0           ; filled by init_cs_ptr__
old21_seg   dw  0
old2f_off   dw  0           ; filled by init_int2f_ptr__
old2f_seg   dw  0

; Shared return-hook storage (serialised: in_handler flag prevents nesting)
orig_ip     dw  0
orig_cs     dw  0
orig_flags  dw  0
orig_bx     dw  0

; DTA address captured inside AH=4Eh handler
dta_seg     dw  0
dta_off     dw  0

;============================================================================
public  new_int21__
new_int21__ proc far

        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        push    bp
        mov     bp, sp

        mov     ax, DGROUP
        mov     ds, ax

        cmp     _in_handler, 0
        jne     do_chain_reentrant
        mov     _in_handler, 1

        mov     ax, [bp+16]

        ;--- AH=29h  Parse Filename: log DS:SI input, chain -----------------
        cmp     ah, 29h
        jne     not_29
        push    word ptr [bp+8]         ; si_val  (3rd arg - rightmost)
        push    word ptr [bp+4]         ; ds_val  (2nd arg, caller's DS)
        push    word ptr [bp+16]        ; ax_val  (1st arg, AL = parse flags)
        call    _log_29
        add     sp, 6
        jmp     do_chain

        ;--- AH=1Ah  Set DTA: log new DTA address DS:DX, chain --------------
not_29:
        cmp     ah, 1Ah
        jne     not_1a
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg, caller's DS)
        call    _log_1a
        add     sp, 4
        jmp     do_chain

        ;--- AH=2Fh  Get DTA: return hook captures ES:BX from DOS ----------
not_1a:
        cmp     ah, 2Fh
        jne     not_2f
        SETUP_RETURN_HOOK do_2f_return
        jmp     do_chain_reentrant      ; keep in_handler=1 for the return hook

        ;--- AH=3Bh  Change Directory: set/clear in_tnfs_dir, log decision ----
not_2f:
        cmp     ah, 3Bh
        jne     not_3b
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg, caller's DS)
        call    _log_3b
        add     sp, 4
        ; 1) Absolute path: "C:\TNFS" or any sub-path inside it
        push    word ptr [bp+10]
        push    word ptr [bp+4]
        call    _match_base
        add     sp, 4
        test    ax, ax
        jnz     do_3b_tnfs
        ; 2) Relative path: bare last component "TNFS", ".\TNFS", etc.
        push    word ptr [bp+10]
        push    word ptr [bp+4]
        call    _match_tnfs_tail
        add     sp, 4
        test    ax, ax
        jnz     do_3b_tnfs
        ; 3) Neither → leaving TNFS
        mov     byte ptr _in_tnfs_dir, 0
        push    0
        call    _log_3b_set             ; "AH=3B SET in_tnfs_dir=0"
        add     sp, 2
        jmp     do_chain
do_3b_tnfs:
        mov     byte ptr _in_tnfs_dir, 1
        push    1
        call    _log_3b_set             ; "AH=3B SET in_tnfs_dir=1"
        add     sp, 2
        jmp     do_chain

        ;--- AH=47h  Get Current Directory: log DL, chain -------------------
not_3b:
        cmp     ah, 47h
        jne     not_47
        push    word ptr [bp+10]        ; dx_val (DL = drive, 1-based)
        call    _log_47
        add     sp, 2
        jmp     do_chain

        ;--- AH=43h  Get/Set File Attributes --------------------------------
        ; Log all; intercept when path is within TNFS_BASE.
not_47:
        cmp     ah, 43h
        jne     not_43

        push    word ptr [bp+10]        ; dx_val (3rd arg)
        push    word ptr [bp+4]         ; ds_val (2nd arg, caller's DS)
        push    word ptr [bp+16]        ; ax_val (1st arg)
        call    _log_43
        add     sp, 6

        push    word ptr [bp+10]        ; dx_val (3rd arg)
        push    word ptr [bp+4]         ; ds_val (2nd arg)
        push    word ptr [bp+16]        ; ax_val (1st arg, AL=0 GET / AL=1 SET)
        call    _handle_43              ; 0=chain  0x10=dir  0x20=file  0x100=SET ok
        add     sp, 6

        push    ax
        call    _log_43_result
        pop     ax

        test    ax, ax
        je      do_chain
        cmp     ax, 100h
        je      do_43_nocx
        mov     word ptr [bp+12], ax    ; CX = attribute
do_43_nocx:
        mov     word ptr [bp+16], 0
        jmp     ret_ok

        ;--- AH=4Eh  FindFirst -----------------------------------------------
        ; Intercept when DS:DX starts with TNFS_BASE; chain everything else.
not_43:
        cmp     ah, 4Eh
        jne     not_4e

        push    word ptr [bp+12]        ; cx_val (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg, caller's DS)
        call    _log_4e_pre
        add     sp, 6

        push    word ptr [bp+10]        ; dx_val
        push    word ptr [bp+4]         ; ds_val
        call    _classify_4e_content    ; 0=chain 1=root 2=GAMES(empty)
        add     sp, 4

        cmp     ax, 0
        je      do_chain
        cmp     ax, 2
        je      do_4e_games

        ; AX=1: enumerate root - GAMES only (README.TXT deferred)
        mov     al, byte ptr [bp+12]
        mov     byte ptr _last_search_attr, al
        mov     byte ptr _find_state, 0
        mov     ah, 2Fh
        int     21h                     ; ES:BX = current DTA
        mov     ax, DGROUP
        mov     ds, ax
        mov     cs:dta_seg, es
        mov     cs:dta_off, bx
        ; Log DTA address before fill
        push    cs:dta_off
        push    cs:dta_seg
        call    _log_4e_dta
        add     sp, 4
        ; Fill DTA with GAMES
        push    0                       ; entry=0
        push    cs:dta_off
        push    cs:dta_seg
        call    _fill_dta_new
        add     sp, 6
        push    cs:dta_off
        push    cs:dta_seg
        call    _log_dta_dump
        add     sp, 4
        mov     byte ptr _find_state, 1
        mov     byte ptr _in_tnfs_dir, 1    ; signal to AH=11h interceptor
        call    _log_handled_4e
        mov     word ptr [bp+16], 0
        jmp     ret_ok

do_4e_games:
        call    _log_handled_4e
        mov     byte ptr _find_state, 0
        mov     word ptr [bp+16], 12h
        jmp     ret_err

        ;--- AH=4Fh  FindNext ------------------------------------------------
        ; find_state=0: not our search → chain to DOS
        ; find_state=1: GAMES already returned → return README.TXT, set state=2
        ; find_state=2: README.TXT already returned → EOF (CF=1, AX=12h)
not_4e:
        cmp     ah, 4Fh
        jne     not_4f

        xor     ax, ax
        mov     al, byte ptr _find_state
        push    ax
        call    _log_4f_pre
        pop     ax

        test    al, al
        je      do_chain                ; find_state=0: not our search
        cmp     al, 2
        jae     do_4f_eof               ; find_state=2: EOF

        ; find_state=1: return README.TXT
        mov     ah, 2Fh
        int     21h                     ; ES:BX = current DTA
        mov     ax, DGROUP
        mov     ds, ax
        mov     cs:dta_seg, es
        mov     cs:dta_off, bx
        push    1                       ; entry=1 README.TXT (3rd arg)
        push    cs:dta_off              ; bx_val (2nd arg)
        push    cs:dta_seg              ; es_val (1st arg)
        call    _fill_dta_new
        add     sp, 6
        push    cs:dta_off
        push    cs:dta_seg
        call    _log_dta_dump
        add     sp, 4
        mov     byte ptr _find_state, 2
        push    1                       ; entry=1
        call    _log_handled_4f         ; "HANDLED AH=4F ENTRY=1"
        add     sp, 2
        mov     word ptr [bp+16], 0
        jmp     ret_ok

do_4f_eof:
        mov     byte ptr _find_state, 0
        push    0FFh
        call    _log_handled_4f         ; "HANDLED AH=4F EOF"
        add     sp, 2
        mov     word ptr [bp+16], 12h
        jmp     ret_err

        ;--- AH=60h  Canonicalize Path: log, chain ---------------------------
not_4f:
        cmp     ah, 60h
        jne     not_60
        push    word ptr [bp+8]         ; si_val
        push    word ptr [bp+4]         ; ds_val (caller's DS)
        call    _log_60
        add     sp, 4
        jmp     do_chain

        ;--- AH=3Dh  Open File: log, chain (TODO: intercept for TNFS files) -
not_60:
        cmp     ah, 3Dh
        jne     not_3d
        push    word ptr [bp+10]        ; dx_val
        push    word ptr [bp+4]         ; ds_val (caller's DS)
        call    _log_3d
        add     sp, 4
        jmp     do_chain

        ;--- AH=11h  FCB FindFirst -------------------------------------------
        ; Log the raw FCB and current DTA for diagnostics.
        ; Intercept when: FCB byte[0]==FFh (extended FCB) AND in_tnfs_dir==1.
        ; Return GAMES in extended FCB result format; set fcb_find_state=1.
        ; All other AH=11h calls chain to DOS unchanged.
not_3d:
        cmp     ah, 11h
        jne     not_11
        ; Get current DTA (chains silently: in_handler=1)
        mov     ah, 2Fh
        int     21h
        mov     ax, DGROUP
        mov     ds, ax
        mov     cs:dta_seg, es
        mov     cs:dta_off, bx
        ; Log raw FCB bytes
        push    word ptr [bp+10]
        push    word ptr [bp+4]
        call    _log_11_fcb_raw
        add     sp, 4
        ; Log DTA contents before any intercept
        push    cs:dta_off
        push    cs:dta_seg
        call    _log_dta_dump
        add     sp, 4
        ; Intercept only for extended FCB (byte[0]==FFh) when in TNFS dir,
        ; and only when the search attribute does NOT include volume label (0x08).
        mov     es, [bp+4]              ; caller's DS (FCB segment)
        mov     di, [bp+10]             ; caller's DX (FCB offset)
        cmp     byte ptr es:[di], 0FFh  ; extended FCB marker?
        jne     do_chain                ; no: chain to DOS
        test    byte ptr es:[di+6], 08h ; search attr: volume label bit set?
        jnz     do_chain                ; yes: chain - do not fake a volume label
        cmp     byte ptr _in_tnfs_dir, 0
        je      do_chain                ; not in TNFS dir: chain
        ; Log caller's FCB before we touch it (11PRE)
        push    0                       ; tag=0 → "11PRE" (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _log_fcb_bytes
        add     sp, 6
        ; Fill DTA with GAMES in extended FCB result format
        push    0                       ; entry=0 GAMES (3rd arg)
        push    cs:dta_off              ; bx_val (2nd arg)
        push    cs:dta_seg              ; es_val (1st arg)
        call    _fill_dta_fcb_ext
        add     sp, 6
        ; Write TNFS marker with state=1 into caller's FCB reserved bytes [1..5]
        push    1                       ; state=1 → next=README (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _fcb_set_state
        add     sp, 6
        mov     byte ptr _fcb_find_state, 1
        push    0
        call    _log_handled_11         ; "HANDLED AH=11 ENTRY=0"
        add     sp, 2
        ; Log caller's FCB after modification (11POST)
        push    1                       ; tag=1 → "11POST" (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _log_fcb_bytes
        add     sp, 6
        mov     byte ptr [bp+16], 0     ; AL=0 success; preserve AH (FCB: only AL used)
        jmp     ret_ok

        ;--- AH=12h  FCB FindNext --------------------------------------------
        ; State is read from caller's FCB reserved bytes [1..5] (TNFS marker):
        ;   state 0 (no marker) → not our search, chain to DOS
        ;   state 1             → return README.TXT, set state=2
        ;   state 2             → clear marker, return AL=FFh (no DOS chain)
not_11:
        cmp     ah, 12h
        jne     not_12
        call    _log_12_pre
        ; Log caller's FCB before we touch it (12PRE)
        push    2                       ; tag=2 → "12PRE" (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _log_fcb_bytes
        add     sp, 6
        ; Read TNFS state from caller's FCB bytes [1..5]
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _fcb_get_state
        add     sp, 4
        ; AX: 0=not ours → chain; 1=README next; 2+=EOF
        cmp     ax, 0
        je      do_chain                ; no TNFS marker: not our search
        cmp     ax, 1
        je      do_12_readme
        jmp     do_12_eof

        ;--- state=1: return README.TXT, advance marker to state=2 ----------
do_12_readme:
        mov     ah, 2Fh
        int     21h                     ; ES:BX = current DTA
        mov     ax, DGROUP
        mov     ds, ax
        mov     cs:dta_seg, es
        mov     cs:dta_off, bx
        push    1                       ; entry=1 README.TXT (3rd arg)
        push    cs:dta_off              ; bx_val (2nd arg)
        push    cs:dta_seg              ; es_val (1st arg)
        call    _fill_dta_fcb_ext
        add     sp, 6
        push    cs:dta_off              ; bx_val (2nd arg)
        push    cs:dta_seg              ; es_val (1st arg)
        call    _log_fcb_result_raw     ; DTA hex dump
        add     sp, 4
        push    2                       ; state=2 → EOF next (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _fcb_set_state
        add     sp, 6
        mov     byte ptr _fcb_find_state, 2
        push    1                       ; entry=1
        call    _log_handled_12         ; "HANDLED AH=12 ENTRY=1"
        add     sp, 2
        ; Write size 123 (0x7B) at documented extended FCB file-size field +0x17
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _fcb_set_readme_size
        add     sp, 4
        ; Log caller FCB after all writes (shows marker + size field together)
        push    3                       ; tag=3 → "12POST" (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _log_fcb_bytes
        add     sp, 6
        mov     byte ptr [bp+16], 0     ; AL=0 success; preserve AH
        jmp     ret_ok

        ;--- state>=2: clear marker, return EOF directly (no DOS chain) ------
do_12_eof:
        push    0                       ; state=0 → clear TNFS marker (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _fcb_set_state
        add     sp, 6
        mov     byte ptr _fcb_find_state, 0
        push    0FFh
        call    _log_handled_12         ; "HANDLED AH=12 EOF"
        add     sp, 2
        push    3                       ; tag=3 → "12POST" (3rd arg)
        push    word ptr [bp+10]        ; dx_val (2nd arg)
        push    word ptr [bp+4]         ; ds_val (1st arg)
        call    _log_fcb_bytes
        add     sp, 6
        mov     byte ptr _last_tnfs_fcb_eof, 1  ; arm AH=59 intercept for next call
        mov     byte ptr [bp+16], 0FFh          ; AL=FFh; preserve AH (FCB: only AL used)
        jmp     ret_ok

        ;--- AH=59h  Get Extended Error: intercept after our fake FCB EOF ------
not_12:
        cmp     ah, 59h
        jne     not_59
        cmp     byte ptr _last_tnfs_fcb_eof, 0
        je      do_59_normal            ; not our EOF: chain to DOS as usual
        ; Our AH=12 EOF was the last FCB call. Return "no more files" (0x0012)
        ; so COMMAND.COM sees the same result as when DOS handles real FCB EOF.
        mov     byte ptr _last_tnfs_fcb_eof, 0
        mov     word ptr [bp+16], 0012h ; AX = 0012h (no more files)
        mov     word ptr [bp+14], 0803h ; BH=08 (out-of-resource), BL=03 (not retryable)
        mov     word ptr [bp+12], 0200h ; CH=02 (disk), CL=00
        jmp     ret_ok
do_59_normal:
        SETUP_RETURN_HOOK do_59_return
        jmp     do_chain_reentrant

        ;--- AH=6Ch  Extended Open/Create: log DS:SI path, BX mode, DX action ----
not_59:
        cmp     ah, 6Ch
        jne     not_6c
        push    word ptr [bp+10]        ; dx_val (4th arg - action flags)
        push    word ptr [bp+14]        ; bx_val (3rd arg - access/sharing mode)
        push    word ptr [bp+8]         ; si_val (2nd arg - path offset; path is DS:SI)
        push    word ptr [bp+4]         ; ds_val (1st arg - caller's DS)
        call    _log_6c
        add     sp, 8
        jmp     do_chain

        ;--- AH=42h  Seek: log handle, origin, CX:DX offset -----------------
not_6c:
        cmp     ah, 42h
        jne     not_42
        push    word ptr [bp+10]        ; dx_val (4th arg - offset low word)
        push    word ptr [bp+12]        ; cx_val (3rd arg - offset high word)
        push    word ptr [bp+14]        ; bx_val (2nd arg - file handle)
        push    word ptr [bp+16]        ; ax_val (1st arg - AL=origin)
        call    _log_42
        add     sp, 8
        jmp     do_chain

        ;--- AH=3Fh  Read from File: log handle and byte count ---------------
not_42:
        cmp     ah, 3Fh
        jne     not_3f
        push    word ptr [bp+12]        ; cx_val (2nd arg - bytes to read)
        push    word ptr [bp+14]        ; bx_val (1st arg - file handle)
        call    _log_3f
        add     sp, 4
        jmp     do_chain

        ;--- AH=3Eh  Close File: log handle ----------------------------------
not_3f:
        cmp     ah, 3Eh
        jne     not_3e
        push    word ptr [bp+14]        ; bx_val - file handle
        call    _log_3e
        add     sp, 2
        jmp     do_chain

        ;--- Catch-all: log AH only ------------------------------------------
not_3e:
        push    word ptr [bp+16]
        call    _log_int21
        add     sp, 2

do_chain:
        mov     _in_handler, 0
do_chain_reentrant:
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        db      02Eh, 0FFh, 02Eh        ; jmp cs:[old21_off]
        dw      offset old21_off

;--- ret_ok: clear CF --------------------------------------------------------
ret_ok:
        mov     _in_handler, 0
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        and     word ptr [bp+6], 0FFFEh
        pop     bp
        iret

;--- ret_err: set CF ---------------------------------------------------------
ret_err:
        mov     _in_handler, 0
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        or      word ptr [bp+6], 1
        pop     bp
        iret

;=============================================================================
; Return hooks
; Entered via IRET from DOS after SETUP_RETURN_HOOK redirected the return.
; in_handler=1 on entry; DS unknown; set DS=DGROUP; log; clear in_handler.
;=============================================================================

;--- do_2f_return: AH=2Fh returned. ES:BX = DTA address. -------------------
do_2f_return:
        push    ax
        push    bx
        push    es
        push    ds
        push    bp
        mov     bp, sp      ; [bp+0]=BP [bp+2]=DS [bp+4]=ES [bp+6]=BX [bp+8]=AX

        mov     ax, DGROUP
        mov     ds, ax

        push    word ptr [bp+6]         ; bx_val (2nd arg, pushed first)
        push    word ptr [bp+4]         ; es_val (1st arg)
        call    _log_2f_return
        add     sp, 4

        mov     _in_handler, 0
        pop     bp
        pop     ds
        pop     es
        pop     bx
        pop     ax
        IRET_TO_CALLER

;--- do_59_return: AH=59h returned. AX=error BH=class BL=action CH=locus. --
do_59_return:
        push    ax
        push    bx
        push    cx
        push    ds
        push    bp
        mov     bp, sp      ; [bp+0]=BP [bp+2]=DS [bp+4]=CX [bp+6]=BX [bp+8]=AX

        mov     ax, DGROUP
        mov     ds, ax

        push    word ptr [bp+4]         ; cx_val (3rd arg, pushed first)
        push    word ptr [bp+6]         ; bx_val (2nd arg)
        push    word ptr [bp+8]         ; ax_val (1st arg)
        call    _log_59_return
        add     sp, 6

        mov     _in_handler, 0
        pop     bp
        pop     ds
        pop     cx
        pop     bx
        pop     ax
        IRET_TO_CALLER

;--- do_12_return: AH=12h chained; log DOS result, force AL=FFh, IRET. -------
do_12_return:
        push    ax
        push    ds
        push    bp
        mov     bp, sp      ; [bp+0]=BP [bp+2]=DS [bp+4]=AX

        mov     ax, DGROUP
        mov     ds, ax

        push    word ptr [bp+4]         ; ax_val (only arg; AL = what DOS returned)
        call    _log_12_chain_return
        add     sp, 2

        mov     byte ptr [bp+4], 0FFh   ; force AL=FFh; AH preserved as DOS left it

        mov     _in_handler, 0
        pop     bp
        pop     ds
        pop     ax                      ; AX = AH(DOS) : AL(FFh forced)
        IRET_TO_CALLER

new_int21__ endp

;============================================================================
public  init_cs_ptr__
init_cs_ptr__ proc near
        push    ds
        mov     ax, DGROUP
        mov     ds, ax
        mov     ax, word ptr _old_int21
        mov     cs:old21_off, ax
        mov     ax, word ptr _old_int21+2
        mov     cs:old21_seg, ax
        pop     ds
        ret
init_cs_ptr__ endp

;============================================================================
; INT 2Fh stub — log AH=11h (network redirector) calls and chain.
; Stack layout identical to INT 21h handler (same push sequence).
;============================================================================
public  new_int2f__
new_int2f__ proc far
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        push    bp
        mov     bp, sp

        mov     ax, DGROUP
        mov     ds, ax

        mov     ax, [bp+16]
        cmp     ah, 11h
        jne     do_2f_pass

        ;--- dispatch on AL ---------------------------------------------------
        cmp     al, 05h
        je      handle_1105
        cmp     al, 06h
        je      handle_1106
        cmp     al, 08h
        je      handle_1108
        cmp     al, 0Fh
        je      handle_110F
        cmp     al, 16h
        je      handle_1116
        cmp     al, 2Eh
        je      handle_112E
        cmp     al, 1Bh
        je      handle_111B
        cmp     al, 1Ch
        je      handle_111C
        jmp     do_2f_log

        ;--- AL=05h: CHDIR — validate path via SDA->fn1 ----------------------
handle_1105:
        call    _do_chdir
        test    ax, ax
        jnz     do_chdir_fail
        mov     word ptr [bp+16], 0
        jmp     do_2f_ret_ok
do_chdir_fail:
        mov     word ptr [bp+16], ax    ; AX = 3 (path not found)
        jmp     do_2f_ret_err

        ;--- AL=1Bh: FINDFIRST — uses glob_sdaptr, no args from caller --------
handle_111B:
        call    _do_findfirst
        mov     word ptr [bp+16], ax
        test    ax, ax
        jnz     do_ff_nf
        ; success: AX=0, CF clear
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        and     word ptr [bp+6], 0FFFEh
        pop     bp
        iret
do_ff_nf:
        ; not found: AX=0x12, CF set
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        or      word ptr [bp+6], 0001h
        pop     bp
        iret

        ;--- AL=1Ch: FINDNEXT — ES:DI is the DTA set up by FINDFIRST ----------
handle_111C:
        push    word ptr [bp+6]     ; di_val (2nd arg)
        push    word ptr [bp+2]     ; es_val (1st arg)
        call    _do_findnext
        add     sp, 4
        mov     word ptr [bp+16], ax
        test    ax, ax
        jnz     do_fn_eof
        ; success: AX=0, CF clear
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        and     word ptr [bp+6], 0FFFEh
        pop     bp
        iret
do_fn_eof:
        ; EOF: AX=0x12, CF set
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        or      word ptr [bp+6], 0001h
        pop     bp
        iret

        ;--- AL=06h: CLOSE — return success ----------------------------------
handle_1106:
        call    _do_close
        mov     word ptr [bp+16], 0
        jmp     do_2f_ret_ok

        ;--- AL=08h: READ — ES:DI=SFT, CX=bytes, buf from SDA->curr_dta ------
handle_1108:
        push    word ptr [bp+12]    ; cx_val   (3rd arg)
        push    word ptr [bp+6]     ; di_val   (2nd arg)
        push    word ptr [bp+2]     ; es_val   (1st arg)
        call    _do_read
        add     sp, 6
        mov     word ptr [bp+12], ax    ; CX = bytes read
        mov     word ptr [bp+16], 0     ; AX = 0
        jmp     do_2f_ret_ok

        ;--- AL=0Fh: GETATTR — filename in SDA->fn1 --------------------------
handle_110F:
        call    _do_getattr
        cmp     ax, 0FFFFh
        je      do_110F_nf
        mov     word ptr [bp+12], ax    ; CX = attribute
        mov     word ptr [bp+16], 0     ; AX = 0
        jmp     do_2f_ret_ok
do_110F_nf:
        mov     word ptr [bp+16], 2     ; AX = 2 (file not found)
        jmp     do_2f_ret_err

        ;--- AL=16h: OPEN — ES:DI=SFT ----------------------------------------
handle_1116:
        push    word ptr [bp+6]     ; di_val (2nd arg)
        push    word ptr [bp+2]     ; es_val (1st arg)
        call    _do_open
        add     sp, 4
        test    ax, ax
        jnz     do_open_err
        mov     word ptr [bp+16], 0
        jmp     do_2f_ret_ok
do_open_err:
        mov     word ptr [bp+16], ax
        jmp     do_2f_ret_err

        ;--- AL=2Eh: SPOPNFIL (Special Open) — same SFT fill, CX=1 ----------
        ; Used by TYPE and COPY in MS-DOS 5.0/6.x instead of regular OPEN.
handle_112E:
        push    word ptr [bp+6]     ; di_val (2nd arg)
        push    word ptr [bp+2]     ; es_val (1st arg)
        call    _do_spopen
        add     sp, 4
        test    ax, ax
        jnz     do_spopen_err
        mov     word ptr [bp+12], 1     ; CX = 1 (action: file opened)
        mov     word ptr [bp+16], 0     ; AX = 0
        jmp     do_2f_ret_ok
do_spopen_err:
        mov     word ptr [bp+16], ax
        jmp     do_2f_ret_err

        ;--- Shared return paths for the above handlers -----------------------
do_2f_ret_ok:
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        and     word ptr [bp+6], 0FFFEh ; CF = 0
        pop     bp
        iret
do_2f_ret_err:
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        push    bp
        mov     bp, sp
        or      word ptr [bp+6], 0001h  ; CF = 1
        pop     bp
        iret

        ;--- all other AH=11h calls: log and chain ---------------------------
do_2f_log:
        push    word ptr [bp+6]     ; di_val  (8th arg, rightmost)
        push    word ptr [bp+2]     ; es_val  (7th arg)
        push    word ptr [bp+8]     ; si_val  (6th arg)
        push    word ptr [bp+4]     ; ds_val  (5th arg)
        push    word ptr [bp+10]    ; dx_val  (4th arg)
        push    word ptr [bp+12]    ; cx_val  (3rd arg)
        push    word ptr [bp+14]    ; bx_val  (2nd arg)
        push    ax                  ; ax_val  (1st arg, leftmost)
        call    _log_2f_call
        add     sp, 16

do_2f_pass:
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        db      02Eh, 0FFh, 02Eh    ; jmp cs:[old2f_off]
        dw      offset old2f_off
new_int2f__ endp

;============================================================================
public  init_int2f_ptr__
init_int2f_ptr__ proc near
        push    ds
        mov     ax, DGROUP
        mov     ds, ax
        mov     ax, word ptr _old_int2f
        mov     cs:old2f_off, ax
        mov     ax, word ptr _old_int2f+2
        mov     cs:old2f_seg, ax
        pop     ds
        ret
init_int2f_ptr__ endp

_TEXT   ends
        end
