;============================================================================
; HANDLER.ASM  -  TNFSDRV INT 2Fh network redirector handler
; Assemble: wasm -2 -ms handler.asm
;
; Stack layout after push sequence + mov bp,sp (still on CALLER's SS:SP):
;   [bp+ 0] BP      [bp+10] DX
;   [bp+ 2] ES      [bp+12] CX
;   [bp+ 4] DS *    [bp+14] BX
;   [bp+ 6] DI      [bp+16] AX
;   [bp+ 8] SI      [bp+18] IP  \
;                   [bp+20] CS   > from INT instruction
;                   [bp+22] FLAGS/
;   * = caller's DS (NOT DGROUP; handler sets DS=DGROUP on entry)
;
; PRIVATE STACK:
;   All C function calls (do_*, log_2f_call) run on tsr_stack in DGROUP.
;   Before each C call the handler saves the caller SS:BP, switches to
;   tsr_tos, and restores them before the final pop/iret sequence.
;   [bp+N] is only valid BEFORE the switch (reading args) and AFTER the
;   restore (writing results).  In between, the res_* DGROUP globals are
;   used to ferry return values across the boundary.
;
; REENTRANCY GUARD:
;   in_handler is set to 1 on entry and cleared before returning.
;   If INT 2Fh fires again while we are inside (e.g. from the packet
;   driver callback), the nested call returns immediately with error.
;
; Subfunctions handled (AL value):
;   01h  RMDIR
;   02h  MKDIR
;   03h  SETCURDIR
;   05h  CHDIR
;   06h  CLOSE
;   08h  READ       ES:DI=SFT, CX=bytes → CX=bytes read
;   0Ch  DISKSPACE  AX=spc, BX=avail, CX=bps, DX=total
;   0Fh  GETATTR    → CX=attr
;   0Eh  SET FILE ATTR  CX=new attr
;   13h  DELETE
;   16h  OPEN       ES:DI=SFT
;   25h  post-EXEC notify → AX=0 for N: paths, chain otherwise
;   2Eh  SPOPNFIL   ES:DI=SFT → CX=1
;   1Bh  FINDFIRST  → CF cleared/set
;   1Ch  FINDNEXT   ES:DI=DTA → CF cleared/set
;   other — log + chain to old vector
;============================================================================

        .model  small

extrn   _old_int2f          : dword
extrn   _log_2f_call        : near   ; void __cdecl (ax,bx,cx,dx,ds,si,es,di)
extrn   _do_setcurdir       : near   ; unsigned __cdecl (void)
extrn   _do_rmdir           : near   ; unsigned __cdecl (void)
extrn   _do_mkdir           : near   ; unsigned __cdecl (void)
extrn   _do_chdir           : near   ; unsigned __cdecl (void)
extrn   _do_qualify         : near   ; unsigned __cdecl (unsigned es, unsigned di)
extrn   _s_qualify_buf      : byte  ; QUALIFY canonical path buffer (in DGROUP)
extrn   _do_findfirst       : near   ; unsigned __cdecl (void)
extrn   _do_findnext        : near   ; unsigned __cdecl (unsigned es, unsigned di)
extrn   _do_getattr         : near   ; unsigned __cdecl (void)
extrn   _do_open            : near   ; unsigned __cdecl (unsigned es, unsigned di)
extrn   _do_spopen          : near   ; unsigned __cdecl (unsigned es, unsigned di)
extrn   _do_read            : near   ; unsigned __cdecl (unsigned es, unsigned di, unsigned cx)
extrn   _do_close           : near   ; void __cdecl (unsigned es, unsigned di)
extrn   _do_diskspace       : near   ; void __cdecl (void)
extrn   _do_rename          : near   ; unsigned __cdecl (void)
extrn   _do_setattr         : near   ; unsigned __cdecl (unsigned cx)
extrn   _do_delete          : near   ; unsigned __cdecl (void)
extrn   _do_exec_notify     : near   ; unsigned __cdecl (void)
extrn   _do_write           : near   ; unsigned __cdecl (unsigned es, unsigned di, unsigned cx)
extrn   _do_create          : near   ; unsigned __cdecl (unsigned es, unsigned di)
extrn   _do_seekend         : near   ; unsigned long __cdecl (unsigned es, unsigned di, unsigned cx, unsigned bx)

;============================================================================
; DGROUP data — all accessible via DS (=DGROUP) from within the handler
;============================================================================
_DATA   segment word public 'DATA'

; Caller SS:SP saved before stack switch (sp = bp = top of push frame)
IFNDEF NOSTACK
saved_ss    dw  0
saved_sp    dw  0
ENDIF

; Caller register values copied before the stack switch
arg_ax      dw  0
arg_bx      dw  0
arg_cx      dw  0
arg_dx      dw  0
arg_di      dw  0
arg_es      dw  0
arg_si      dw  0
arg_ds_val  dw  0

; Result values to write back to caller frame after stack restore
res_ax      dw  0
res_bx      dw  0
res_cx      dw  0
res_dx      dw  0

; QUALIFY (AL=23h) ES:DI result — set to s_qualify_buf address
res_qualify_di  dw  0
res_qualify_es  dw  0

; Reentrancy guard: 0=idle, 1=inside handler
; Exported as _in_handler so C debug code can read it.
public _in_handler
_in_handler db  0
            db  0               ; padding for word alignment

IFNDEF NOSTACK
; Stack canary BELOW the buffer (overflow direction).
; If the stack uses all 8192 bytes it will corrupt this word.
public  _tsr_stack_lo_canary
_tsr_stack_lo_canary    dw  0A55Ah

; Private TSR stack — 8192 bytes, grows downward from tsr_tos.
; Filled with 0xCC so depth can be measured: count intact bytes from [0].
public  _tsr_stack
_tsr_stack              db  8192 dup(0CCh)
tsr_tos                 label word

; Canary ABOVE the top of stack (should never be touched).
public  _tsr_stack_hi_canary
_tsr_stack_hi_canary    dw  05AA5h
ENDIF

_DATA   ends

;============================================================================
_TEXT   segment byte public 'CODE'
        assume  cs:_TEXT, ds:DGROUP, ss:DGROUP

old2f_off   dw  0           ; filled by init_int2f_ptr__
old2f_seg   dw  0

;============================================================================
; INT 2Fh handler
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
        mov     bp, sp                  ; BP = top of saved frame on caller SS

        mov     ax, DGROUP
        mov     ds, ax                  ; DS = DGROUP; SS still = caller's

        mov     ax, [bp+16]             ; saved AX
        cmp     ah, 11h
        jne     do_2f_chain             ; not our subfunction — chain immediately

        ;--------------------------------------------------------------------
        ; AH=11h — network redirector subfunction for our drive.
        ;
        ; 1. Reentrancy check  (guard against recursive invocation)
        ; 2. Copy register args from [bp+N] to DGROUP globals
        ; 3. Save caller SS:SP, switch to private TSR stack
        ; 4. Dispatch on AL, call C helpers
        ; 5. Restore caller SS:SP, write results to [bp+N], return
        ;--------------------------------------------------------------------

        ; 1. Reentrancy check
        cmp     byte ptr [_in_handler], 0
        jne     do_reentry_err

        ; 2. Copy args to DGROUP globals (while still on caller's SS)
        mov     [arg_ax], ax            ; AX already loaded above
        mov     ax, [bp+14]
        mov     [arg_bx], ax
        mov     ax, [bp+12]
        mov     [arg_cx], ax
        mov     ax, [bp+10]
        mov     [arg_dx], ax
        mov     ax, [bp+6]
        mov     [arg_di], ax
        mov     ax, [bp+2]
        mov     [arg_es], ax
        mov     ax, [bp+8]
        mov     [arg_si], ax
        mov     ax, [bp+4]
        mov     [arg_ds_val], ax

        ; 3. Switch to private TSR stack (skipped with NOSTACK — C runs on caller SS:SP)
IFNDEF NOSTACK
        cli
        mov     [saved_ss], ss
        mov     [saved_sp], bp
        mov     ax, DGROUP
        mov     ss, ax
        mov     sp, offset tsr_tos
        sti
ENDIF

        mov     byte ptr [_in_handler], 1

        ; Initialise result regs: default = preserve caller's values
        mov     ax, [arg_bx]
        mov     [res_bx], ax
        mov     ax, [arg_cx]
        mov     [res_cx], ax
        mov     ax, [arg_dx]
        mov     [res_dx], ax

        ; 4. Dispatch on AL
        mov     al, byte ptr [arg_ax]   ; lo byte of saved AX

        cmp     al, 01h
        je      handle_1101
        cmp     al, 02h
        je      handle_1102
        cmp     al, 03h
        je      handle_1103
        cmp     al, 05h
        je      handle_1105
        cmp     al, 06h
        je      handle_1106
        cmp     al, 08h
        je      handle_1108
        cmp     al, 09h
        je      handle_1109
        cmp     al, 11h
        je      handle_1111
        cmp     al, 0Ch
        je      handle_110C
        cmp     al, 0Eh
        je      handle_110E
        cmp     al, 0Fh
        je      handle_110F
        cmp     al, 13h
        je      handle_1113
        cmp     al, 16h
        je      handle_1116
        cmp     al, 17h
        je      handle_1117
        cmp     al, 21h
        je      handle_1121
        cmp     al, 2Eh
        je      handle_112E
        cmp     al, 25h
        je      handle_1125
        cmp     al, 1Bh
        je      handle_111B
        cmp     al, 1Ch
        je      handle_111C
        jmp     do_2f_log

        ;--- AL=01h: RMDIR ---------------------------------------------------
handle_1101:
        call    _do_rmdir
        test    ax, ax
        jnz     do_rmdir_fail
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_rmdir_fail:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=02h: MKDIR ---------------------------------------------------
handle_1102:
        call    _do_mkdir
        test    ax, ax
        jnz     do_mkdir_fail
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_mkdir_fail:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=03h: SETCURDIR -----------------------------------------------
handle_1103:
        call    _do_setcurdir
        cmp     ax, 0FFFFh
        je      do_tsr_chain
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok

        ;--- AL=05h: CHDIR ---------------------------------------------------
handle_1105:
        call    _do_chdir
        test    ax, ax
        jnz     do_chdir_fail
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_chdir_fail:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=06h: CLOSE ---------------------------------------------------
handle_1106:
        push    word ptr [arg_di]
        push    word ptr [arg_es]
        call    _do_close
        add     sp, 4
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok

        ;--- AL=08h: READ  (ES:DI=SFT, CX=count) ----------------------------
handle_1108:
        push    word ptr [arg_cx]       ; cx_val  (3rd arg)
        push    word ptr [arg_di]       ; di_val  (2nd arg)
        push    word ptr [arg_es]       ; es_val  (1st arg)
        call    _do_read
        add     sp, 6
        mov     [res_cx], ax            ; CX = bytes actually read
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok

        ;--- AL=09h: WRITE (ES:DI=SFT, CX=count) ----------------------------
handle_1109:
        push    word ptr [arg_cx]       ; cx_val  (3rd arg)
        push    word ptr [arg_di]       ; di_val  (2nd arg)
        push    word ptr [arg_es]       ; es_val  (1st arg)
        call    _do_write
        add     sp, 6
        mov     [res_cx], ax            ; CX = bytes actually written
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok

        ;--- AL=11h: RENAME --------------------------------------------------
handle_1111:
        call    _do_rename
        test    ax, ax
        jnz     do_rename_fail
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_rename_fail:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=0Ch: DISKSPACE -----------------------------------------------
handle_110C:
        call    _do_diskspace
        mov     [res_ax], 8
        mov     [res_bx], 4096
        mov     [res_cx], 512
        mov     [res_dx], 4096
        jmp     do_tsr_ret_ok

        ;--- AL=0Eh: SET FILE ATTRIBUTES (DOS 6.x; CX=new attr) -------------
handle_110E:
        push    word ptr [arg_cx]       ; cx_val (new attributes)
        call    _do_setattr
        add     sp, 2
        test    ax, ax
        jnz     do_setattr_fail
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_setattr_fail:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=0Fh: GETATTR -------------------------------------------------
handle_110F:
        call    _do_getattr
        cmp     ax, 0FFFFh
        je      do_110F_nf
        mov     [res_cx], ax            ; CX = attribute byte
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_110F_nf:
        mov     [res_ax], 2             ; file not found
        jmp     do_tsr_ret_err

        ;--- AL=13h: DELETE --------------------------------------------------
handle_1113:
        call    _do_delete
        test    ax, ax
        jnz     do_delete_fail
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_delete_fail:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=16h: OPEN  (ES:DI=SFT) ---------------------------------------
handle_1116:
        push    word ptr [arg_di]
        push    word ptr [arg_es]
        call    _do_open
        add     sp, 4
        test    ax, ax
        jnz     do_open_err
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_open_err:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=17h: CREATE/OPEN (ES:DI=SFT) ---------------------------------
handle_1117:
        push    word ptr [arg_di]
        push    word ptr [arg_es]
        call    _do_create
        add     sp, 4
        test    ax, ax
        jnz     do_create_err
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_create_err:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=21h: SEEKEND (BX:CX=signed offset, ES:DI=SFT) ---------------
handle_1121:
        push    word ptr [arg_bx]       ; bx_val (4th arg, high word of offset)
        push    word ptr [arg_cx]       ; cx_val (3rd arg, low word of offset)
        push    word ptr [arg_di]       ; di_val (2nd arg)
        push    word ptr [arg_es]       ; es_val (1st arg)
        call    _do_seekend
        add     sp, 8
        mov     [res_ax], ax            ; DX:AX = new file position
        mov     [res_dx], dx
        jmp     do_tsr_ret_ok

        ;--- AL=2Eh: SPOPNFIL  (ES:DI=SFT) ----------------------------------
handle_112E:
        push    word ptr [arg_di]
        push    word ptr [arg_es]
        call    _do_spopen
        add     sp, 4
        test    ax, ax
        jnz     do_spopen_err
        mov     [res_cx], 1             ; CX = 1 (file opened)
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok
do_spopen_err:
        mov     [res_ax], ax
        jmp     do_tsr_ret_err

        ;--- AL=25h: post-EXEC notify (no output buffer) ---------------------
handle_1125:
        call    _do_exec_notify
        cmp     ax, 0FFFFh
        je      do_tsr_chain
        mov     [res_ax], 0
        jmp     do_tsr_ret_ok

        ;--- AL=1Bh: FINDFIRST -----------------------------------------------
handle_111B:
        call    _do_findfirst
        mov     [res_ax], ax
        test    ax, ax
        jnz     do_tsr_ret_err          ; error: CF=1
        jmp     do_tsr_ret_ok           ; success: CF=0

        ;--- AL=1Ch: FINDNEXT  (ES:DI=DTA from FINDFIRST) -------------------
handle_111C:
        push    word ptr [arg_di]
        push    word ptr [arg_es]
        call    _do_findnext
        add     sp, 4
        mov     [res_ax], ax
        test    ax, ax
        jnz     do_tsr_ret_err          ; EOF/error: CF=1
        jmp     do_tsr_ret_ok           ; found: CF=0

        ;--- AL=23h: QUALIFY FILENAME ----------------------------------------
        ; On exit: ES:DI must point to driver's OWN canonical path buffer.
        ; DOS reads ES:DI after CF=0 to update fn1; returning caller's ES:DI
        ; unchanged would leave fn1 = CWD (truncated path).
handle_1123:
        push    word ptr [arg_di]
        push    word ptr [arg_es]
        call    _do_qualify
        add     sp, 4
        test    ax, ax
        jnz     do_1123_chain
        mov     word ptr [res_qualify_di], offset _s_qualify_buf
        mov     ax, DGROUP
        mov     word ptr [res_qualify_es], ax
        mov     [res_ax], 0
        jmp     do_tsr_ret_qualify
do_1123_chain:
        jmp     do_tsr_chain

        ;--- unhandled AH=11h: log then chain --------------------------------
do_2f_log:
        push    word ptr [arg_di]       ; di_val  (8th arg, rightmost)
        push    word ptr [arg_es]       ; es_val  (7th)
        push    word ptr [arg_si]       ; si_val  (6th)
        push    word ptr [arg_ds_val]   ; ds_val  (5th)
        push    word ptr [arg_dx]       ; dx_val  (4th)
        push    word ptr [arg_cx]       ; cx_val  (3rd)
        push    word ptr [arg_bx]       ; bx_val  (2nd)
        push    word ptr [arg_ax]       ; ax_val  (1st, leftmost)
        call    _log_2f_call
        add     sp, 16
        jmp     do_tsr_chain

        ;====================================================================
        ; TSR stack restore + return paths
        ; All paths arrive here with results in res_ax / res_bx / res_cx / res_dx.
        ; Restore caller SS:SP, write results to the saved frame, then IRET.
        ;====================================================================

        ; CF=0 (success) — write AX/BX/CX/DX + ES:DI from res_qualify_es/di
        ; Used by AL=23h QUALIFY so DOS gets correct canonical path in ES:DI.
do_tsr_ret_qualify:
        mov     byte ptr [_in_handler], 0
IFNDEF NOSTACK
        cli
        mov     ax, [saved_ss]
        mov     ss, ax
        mov     sp, [saved_sp]          ; sp = bp = top of saved frame
        sti
ENDIF
        mov     ax, [res_ax]
        mov     word ptr [bp+16], ax
        mov     ax, [res_bx]
        mov     word ptr [bp+14], ax
        mov     ax, [res_cx]
        mov     word ptr [bp+12], ax
        mov     ax, [res_dx]
        mov     word ptr [bp+10], ax
        mov     ax, [res_qualify_di]
        mov     word ptr [bp+6], ax     ; DI = offset of s_qualify_buf
        mov     ax, [res_qualify_es]
        mov     word ptr [bp+2], ax     ; ES = DGROUP segment
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

        ; CF=0 (success) — write all result registers, clear carry
do_tsr_ret_ok:
        mov     byte ptr [_in_handler], 0
IFNDEF NOSTACK
        cli
        mov     ax, [saved_ss]
        mov     ss, ax
        mov     sp, [saved_sp]          ; sp = bp = top of saved frame
        sti
ENDIF
        ; BP is still the correct frame pointer (never modified)
        mov     ax, [res_ax]
        mov     word ptr [bp+16], ax
        mov     ax, [res_bx]
        mov     word ptr [bp+14], ax
        mov     ax, [res_cx]
        mov     word ptr [bp+12], ax
        mov     ax, [res_dx]
        mov     word ptr [bp+10], ax
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

        ; CF=1 (error) — write AX (error code), set carry
do_tsr_ret_err:
        mov     byte ptr [_in_handler], 0
IFNDEF NOSTACK
        cli
        mov     ax, [saved_ss]
        mov     ss, ax
        mov     sp, [saved_sp]
        sti
ENDIF
        mov     ax, [res_ax]
        mov     word ptr [bp+16], ax
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

        ; Restore and chain to old handler (log path falls through here)
do_tsr_chain:
        mov     byte ptr [_in_handler], 0
IFNDEF NOSTACK
        cli
        mov     ax, [saved_ss]
        mov     ss, ax
        mov     sp, [saved_sp]
        sti
ENDIF
        ; fall through to do_2f_chain

        ;====================================================================
        ; Chain to previous INT 2Fh vector (no stack restore needed for
        ; direct chain; stack already restored above for tsr path)
        ;====================================================================
do_2f_chain:
        pop     bp
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        db      02Eh, 0FFh, 02Eh        ; jmp cs:[old2f_off]  (far indirect)
        dw      offset old2f_off

        ;====================================================================
        ; Reentrancy detected — return AX=2 (path not found), CF=1
        ; We are still on the caller's stack (no TSR switch done).
        ;====================================================================
do_reentry_err:
        mov     word ptr [bp+16], 2
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
