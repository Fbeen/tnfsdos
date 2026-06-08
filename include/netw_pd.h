#ifndef NETW_PD_H
#define NETW_PD_H

/*
 * netw_pd.h — packet-driver specific setup, called before netw_connect().
 *
 * The netw_*() API (connect/send/recv/clear_rx/disconnect/warmup/get_server_ip)
 * is declared in netw.h.  This header adds only the PD-specific setup call,
 * the receiver callback extern, and the shared state accessed by netinit.c.
 */

#include <stdint.h>

/* Supply packet driver interrupt and local IP before netw_connect(). */
void netw_pd_set_params(unsigned int pd_int, const char *local_ip_str);

/* Packet driver far receiver — defined in netw_pd_rcv.asm.
 * FAR convention: Watcom maps C __far "pd_receiver" to ASM "pd_receiver_". */
extern void __far pd_receiver(void);

/* Direct send via pushf/cli/call far — defined in netw_pd_rcv.asm.
 * Watcom register convention: AX=buf_off (DS-relative), DX=len_val.
 * Returns 0 on success, 1 on error (CF=1 from driver).
 * C name "pd_send_asm" → Watcom register symbol "pd_send_asm_" in ASM. */
extern int pd_send_asm(unsigned int buf_off, unsigned int len_val);

/* Shared state (netw_pd.c <-> netw_pd_rcv.asm) */
extern uint8_t            s_rx_buf[];
extern volatile uint16_t  s_rx_len;
extern volatile uint16_t  s_dbg_ax0_ok;
extern volatile uint16_t  s_dbg_ax0_busy;
extern volatile uint16_t  s_dbg_ax1;
extern volatile uint16_t  s_dbg_ax1_cx;

/* Far pointer to packet driver handler — read from IVT in netw_pd_set_params.
 * Used by pd_send_asm_ in netw_pd_rcv.asm via extrn _s_pd_vector:dword. */
extern uint32_t s_pd_vector;

#endif /* NETW_PD_H */
