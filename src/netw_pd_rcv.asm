;============================================================================
; NETW_PD_RCV.ASM — Packet driver receiver callback + direct send helper
;
; The CLARKSON packet driver spec calls pd_receiver_ twice per received packet:
;
;   AX=0  "buffer request"
;         Return ES:DI = address of receive buffer,
;         or 0000:0000 if busy (buffer in use or awaiting delivery).
;         All registers except ES and DI MUST be preserved (including FLAGS).
;
;   AX=1  "packet delivered"
;         CX = total frame length.
;         We record CX into s_rx_len so netw_recv() can pick it up.
;         All registers MUST be preserved (including FLAGS).
;
; Receive buffer state machine (matches EtherDFS approach):
;   s_rx_len = 0        : free — available for a new packet
;   s_rx_len = RX_AWAITING (0xFFFF) : buffer handed to driver, delivery pending
;   s_rx_len = 1..1514  : frame received and ready for netw_recv()
;
; The awaiting state prevents a second AX=0 call from re-issuing the same
; buffer before AX=1 fires (which would corrupt s_rx_buf if two packets
; arrived back-to-back in a single NIC IRQ burst).
;
; pd_send_asm_:
;   Direct packet driver send via pushf/cli/call far (AH=4, DS:SI=buf, CX=len).
;   Avoids int86x() overhead; interrupt handler does IRET which restores IF=1.
;   Caller ensures DS=DGROUP (true in both pre-TSR and TSR contexts).
;
;   C name: pd_send_asm — Watcom register calling convention appends "_":
;     public pd_send_asm_  ← matches C symbol "pd_send_asm"
;   Arguments (Watcom register convention):
;     AX = buf_off    (DS-relative offset of frame buffer in DGROUP)
;     DX = len_val    (frame length in bytes)
;   Returns AX = 0 on success, 1 on error (CF was set by driver).
;============================================================================

RX_AWAITING     EQU     0FFFFh  ; sentinel: buffer given to driver, not yet delivered

        .model  small

extrn   _s_rx_len           : word
extrn   _s_rx_buf           : byte
extrn   _s_dbg_ax0_ok       : word
extrn   _s_dbg_ax0_busy     : word
extrn   _s_dbg_ax1          : word
extrn   _s_dbg_ax1_cx       : word
extrn   _s_cb_refuse_reason : word  ; s_rx_len value at last AX=0 refusal
extrn   _s_cb_last_et       : word  ; EtherType word (byte12 low, byte13 high)
extrn   _s_cb_last_mac      : byte  ; src MAC bytes [0..5]
extrn   _s_cb_last_ip       : byte  ; src IP  bytes [0..3] (valid when last_et==0x0008)
extrn   _s_pd_vector        : dword ; [off:seg] far pointer to packet driver

; Stub _DATA declaration so WASM generates DGROUP-relative fixups for extrns.
_DATA   segment word public 'DATA'
_DATA   ends

_TEXT   segment byte public 'CODE'
        assume  cs:_TEXT, ds:DGROUP

;============================================================================
; pd_receiver_ — packet driver receive callback (FAR)
;
; Preserves: ALL registers including FLAGS (Clarkson spec requirement).
; Modifies:  ES:DI only (AX=0 path returns buffer address).
;============================================================================
public  pd_receiver_
pd_receiver_ proc far

        push    ds
        push    bx
        pushf                               ; preserve FLAGS — spec requires all regs

        mov     bx, DGROUP
        mov     ds, bx                      ; DS = DGROUP for all [_s_*] accesses

        test    ax, ax
        jnz     short rcv_ax1

        ;--- AX=0: supply receive buffer ---
        ; Busy if already awaiting delivery (RX_AWAITING) or frame ready (>0).
        cmp     word ptr [_s_rx_len], 0
        jne     short rcv_discard

        ; Buffer free — hand it to the driver and mark as awaiting.
        mov     word ptr [_s_rx_len], RX_AWAITING
        inc     word ptr [_s_dbg_ax0_ok]
        mov     es, bx                      ; ES = DGROUP
        mov     di, offset _s_rx_buf        ; DI = buffer offset in DGROUP
        popf
        pop     bx
        pop     ds
        ret                                 ; plain FAR RETURN (driver uses CALL FAR)

rcv_discard:
        inc     word ptr [_s_dbg_ax0_busy]
        ; Record s_rx_len so caller knows WHY we refused:
        ;   0xFFFF = already awaiting delivery
        ;   1..N   = previous frame still unconsumed in buffer
        mov     bx, word ptr [_s_rx_len]
        mov     word ptr [_s_cb_refuse_reason], bx
        xor     bx, bx
        mov     es, bx                      ; ES:DI = 0000:0000 — discard packet
        mov     di, bx
        popf
        pop     bx
        pop     ds
        ret

        ;--- AX=1: frame delivered, CX = length ---
rcv_ax1:
        inc     word ptr [_s_dbg_ax1]
        mov     word ptr [_s_dbg_ax1_cx], cx
        mov     word ptr [_s_rx_len], cx    ; replaces RX_AWAITING with actual length

        ; Capture frame metadata from s_rx_buf (frame is now fully in buffer).
        ; EtherType: bytes 12-13 of frame (big-endian in wire, stored as little-endian word).
        ;   byte[12] -> low byte of s_cb_last_et, byte[13] -> high byte.
        mov     bx, word ptr [_s_rx_buf + 12]
        mov     word ptr [_s_cb_last_et], bx

        ; Src MAC: bytes 6-11.
        mov     bx, word ptr [_s_rx_buf + 6]
        mov     word ptr [_s_cb_last_mac], bx
        mov     bx, word ptr [_s_rx_buf + 8]
        mov     word ptr [_s_cb_last_mac + 2], bx
        mov     bx, word ptr [_s_rx_buf + 10]
        mov     word ptr [_s_cb_last_mac + 4], bx

        ; Src IP: bytes 26-29 (IP_BASE=14, OFF_IP_SRC=14+12=26).
        ; Only meaningful if EtherType==0x0800 (stored as 0x0008 little-endian).
        cmp     word ptr [_s_cb_last_et], 0008h
        jne     short rcv_ax1_done
        mov     bx, word ptr [_s_rx_buf + 26]
        mov     word ptr [_s_cb_last_ip], bx
        mov     bx, word ptr [_s_rx_buf + 28]
        mov     word ptr [_s_cb_last_ip + 2], bx
rcv_ax1_done:
        popf
        pop     bx
        pop     ds
        ret

pd_receiver_ endp

;============================================================================
; pd_send_asm_ — direct packet driver send via pushf/cli/call far
;
; Watcom 16-bit REGISTER calling convention (not cdecl):
;   AX = buf_off   (DS-relative offset of frame buffer in DGROUP)
;   DX = len_val   (frame length in bytes)
;   Returns AX = 0 on success, AX = 1 on error (CF was set by driver).
;
; Must preserve: SI, DI, BP, DS  (Watcom callee-saved registers).
; May clobber:   AX, BX, CX, DX (Watcom caller-saved; AX = return value).
;
; The packet driver is invoked with:
;   AH  = 4          (send_pkt function code)
;   DS:SI = DGROUP:buf_off  (frame buffer)
;   CX  = len_val    (frame length)
; via pushf / cli / call dword ptr [_s_pd_vector].
;
; The driver returns with IRET (or RETF — both work with pushf+call setup):
;   IRET pops IP, CS, FLAGS — restoring caller FLAGS (IF=1).
;============================================================================
public  pd_send_asm_
pd_send_asm_ proc near

        push    si                          ; SI is callee-saved in Watcom conv

        mov     si, ax                      ; SI = buffer offset (DS:SI = DGROUP:buf)
        mov     cx, dx                      ; CX = frame length
        mov     ah, 4                       ; send_pkt function code

        pushf                               ; simulate INT: save FLAGS for IRET
        cli                                 ; inhibit interrupts before far call
        call    dword ptr [_s_pd_vector]    ; far call — driver ends with IRET
        ; IRET restores FLAGS including IF=1 → interrupts re-enabled on return

        pop     si                          ; restore SI

        mov     ax, 0
        jnc     psa_ok
        mov     ax, 1
psa_ok:
        ret

pd_send_asm_ endp

_TEXT   ends
        end
