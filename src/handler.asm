;============================================================================
; HANDLER.ASM  -  TNFSDRV INT 2Fh network redirector handler
; Assemble: wasm -2 -ms handler.asm
;
; Handles INT 2Fh AH=11h (MS-DOS network redirector API) for virtual drive T:.
;
; Stack layout after push sequence + mov bp,sp:
;   [bp+ 0] BP      [bp+10] DX
;   [bp+ 2] ES      [bp+12] CX
;   [bp+ 4] DS *    [bp+14] BX
;   [bp+ 6] DI      [bp+16] AX
;   [bp+ 8] SI      [bp+18] IP  \
;                   [bp+20] CS   > from INT instruction
;                   [bp+22] FLAGS/
;   * = caller's DS (NOT DGROUP; handler sets DS=DGROUP on entry)
;
; Subfunctions handled (AL value):
;   05h  CHDIR      — validate path via SDA->fn1
;   06h  CLOSE      — always succeed
;   08h  READ       — copy fake content to SDA->curr_dta
;   0Fh  GETATTR    — return attribute from SDA->fn1
;   16h  OPEN       — fill SFT for README.TXT
;   2Eh  SPOPNFIL   — same as OPEN + CX=1 (used by TYPE/COPY in DOS 5+/6.x)
;   0Ch  DISKSPACE  — return fake 16 MB volume info (AX/BX/CX/DX)
;   1Bh  FINDFIRST  — fn1+template-based directory entry dispatch
;   1Ch  FINDNEXT   — DTA dir_entry sequence counter
;   other — log + chain to old vector
;============================================================================

        .model  small

extrn   _old_int2f          : dword
extrn   _log_2f_call        : near   ; void __cdecl (unsigned ax, unsigned bx, unsigned cx, unsigned dx, unsigned ds, unsigned si, unsigned es, unsigned di)
extrn   _do_chdir           : near   ; unsigned __cdecl (void) — 0=ok, 3=path not found
extrn   _do_findfirst       : near   ; unsigned __cdecl (void)
extrn   _do_findnext        : near   ; unsigned __cdecl (unsigned es, unsigned di)
extrn   _do_getattr         : near   ; unsigned __cdecl (void) — 0xFFFF=not found, else attr byte
extrn   _do_open            : near   ; unsigned __cdecl (unsigned es, unsigned di) — 0=ok, else error
extrn   _do_spopen          : near   ; unsigned __cdecl (unsigned es, unsigned di) — 0=ok, else error
extrn   _do_read            : near   ; unsigned __cdecl (unsigned es, unsigned di, unsigned cx) — buf from SDA->curr_dta
extrn   _do_close           : near   ; void __cdecl (void)
extrn   _do_diskspace       : near   ; void __cdecl (void) — log only; registers set in ASM

;============================================================================
_TEXT   segment byte public 'CODE'
        assume  cs:_TEXT, ds:DGROUP, ss:DGROUP

old2f_off   dw  0           ; filled by init_int2f_ptr__
old2f_seg   dw  0

;============================================================================
; INT 2Fh handler — dispatches AH=11h subfunctions; all others log + chain.
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
        cmp     al, 0Ch
        je      handle_110C
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

        ;--- AL=0Ch: DISKSPACE — fake 16 MB (8 sec/clus * 512 B/sec * 4096 clus) -
handle_110C:
        call    _do_diskspace
        mov     word ptr [bp+16], 8     ; AX = sectors per cluster
        mov     word ptr [bp+14], 4096  ; BX = available clusters
        mov     word ptr [bp+12], 512   ; CX = bytes per sector
        mov     word ptr [bp+10], 4096  ; DX = total clusters
        jmp     do_2f_ret_ok

        ;--- AL=0Fh: GETATTR — filename from SDA->fn1 ------------------------
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

        ;--- AL=2Eh: SPOPNFIL — same SFT fill, CX=1 (TYPE/COPY in DOS 5+/6.x) -
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

        ;--- AL=1Bh: FINDFIRST — uses glob_sdaptr, classified by fn1+template -
handle_111B:
        call    _do_findfirst
        mov     word ptr [bp+16], ax
        test    ax, ax
        jnz     do_ff_nf
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

        ;--- AL=1Ch: FINDNEXT — ES:DI is the DTA from FINDFIRST ---------------
handle_111C:
        push    word ptr [bp+6]     ; di_val (2nd arg)
        push    word ptr [bp+2]     ; es_val (1st arg)
        call    _do_findnext
        add     sp, 4
        mov     word ptr [bp+16], ax
        test    ax, ax
        jnz     do_fn_eof
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

        ;--- Shared success/error return paths --------------------------------
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

        ;--- all other AH=11h calls: log + chain to old vector ---------------
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
