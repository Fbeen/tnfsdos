/*
 * NETW_PD.C — Minimal Ethernet/ARP/IPv4/UDP transport via CLARKSON packet driver.
 *
 * Replaces netw.cpp (mTCP).  No mTCP, no C++, no DHCP, no DNS, no timer hooks.
 *
 * Call sequence at startup (from netinit.c):
 *   1. netw_pd_set_params(cfg->packetint, cfg->localip)
 *   2. netw_connect(cfg->servername, cfg->port, 0)  — does ARP, keeps handle open
 *   3. tnfs_mount(...)
 *   Then go TSR.  The IP handle stays open for the session lifetime.
 *
 * During TSR operation (from tnfs.c, within INT 2Fh handler):
 *   netw_clear_rx() / netw_send() / netw_recv()
 *
 * Compiled with -zu (DS != SS in TSR context).
 * All static data is DS-relative (DGROUP); local vars on stack (SS-relative).
 * Startup calls (access_type, get_address, release) use int86x() to preserve DS.
 * Resident send uses pd_send_asm() (assembly stub); int86x() is too heavy in TSR context.
 */

#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "ringbuf.h"
#include "netw.h"      /* NETW_ERR_TIMEOUT, netw_*() prototypes */
#include "netw_pd.h"   /* pd_receiver, shared state externs    */

/* ------------------------------------------------------------------ */
/* DS helper                                                             */
/* ------------------------------------------------------------------ */

static unsigned short get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

/* ------------------------------------------------------------------ */
/* BIOS tick timer — safe to call from TSR context                      */
/* BIOS timer at 0040:006C, 18.2 ticks/sec.  Low 16 bits wrap every   */
/* ~3600 s; unsigned subtraction handles wrap-around for short spans.  */
/* ------------------------------------------------------------------ */

static uint16_t bios_ticks(void)
{
    return *(uint16_t far *)MK_FP(0x0040, 0x006C);
}

/* 3000 ms * 18.2 / 1000 ≈ 55 ticks — generous margin */
#define RECV_TIMEOUT_TICKS  55u
/* 1500 ms per ARP attempt */
#define ARP_TIMEOUT_TICKS   27u

/* ------------------------------------------------------------------ */
/* Frame layout constants (offsets from Ethernet frame byte 0)          */
/* ------------------------------------------------------------------ */

#define ETH_HDR    14
#define IP_BASE    ETH_HDR
#define IP_HDR     20
#define UDP_BASE   (ETH_HDR + IP_HDR)
#define UDP_HDR    8
#define DATA_OFF   (UDP_BASE + UDP_HDR)   /* 42 */

#define OFF_ETH_DST   0
#define OFF_ETH_SRC   6
#define OFF_ETH_TYPE  12

#define OFF_IP_VERHDR (IP_BASE +  0)
#define OFF_IP_DSCP   (IP_BASE +  1)
#define OFF_IP_LEN    (IP_BASE +  2)
#define OFF_IP_ID     (IP_BASE +  4)
#define OFF_IP_FLAGS  (IP_BASE +  6)
#define OFF_IP_TTL    (IP_BASE +  8)
#define OFF_IP_PROTO  (IP_BASE +  9)
#define OFF_IP_CSUM   (IP_BASE + 10)
#define OFF_IP_SRC    (IP_BASE + 12)
#define OFF_IP_DST    (IP_BASE + 16)

#define OFF_UDP_SPORT (UDP_BASE + 0)
#define OFF_UDP_DPORT (UDP_BASE + 2)
#define OFF_UDP_LEN   (UDP_BASE + 4)
#define OFF_UDP_CSUM  (UDP_BASE + 6)

/* ARP offsets (ARP payload follows the 14-byte Ethernet header) */
#define OFF_ARP_HTYPE (ETH_HDR +  0)
#define OFF_ARP_PTYPE (ETH_HDR +  2)
#define OFF_ARP_HLEN  (ETH_HDR +  4)
#define OFF_ARP_PLEN  (ETH_HDR +  5)
#define OFF_ARP_OPER  (ETH_HDR +  6)
#define OFF_ARP_SHA   (ETH_HDR +  8)
#define OFF_ARP_SPA   (ETH_HDR + 14)
#define OFF_ARP_THA   (ETH_HDR + 18)
#define OFF_ARP_TPA   (ETH_HDR + 24)
#define ARP_FRAME_LEN (ETH_HDR + 28)

/* Max frame: ETH + IP + UDP + 1024-byte TNFS payload */
#define FRAME_MAX  (DATA_OFF + 1024)

/* ------------------------------------------------------------------ */
/* Static state — all in DGROUP                                         */
/* ------------------------------------------------------------------ */

static unsigned int  s_pd_int;
static int           s_arp_handle;
static int           s_ip_handle;

static uint8_t  s_local_mac[6];
static uint8_t  s_server_mac[6];
static uint8_t  s_local_ip[4];
static uint8_t  s_server_ip[4];
static uint16_t s_server_port;
static uint16_t s_local_port;
static uint16_t s_ip_id;

static uint8_t  s_tx_buf[FRAME_MAX];

/* Receive buffer — also referenced by netw_pd_rcv.asm via EXTRN.
 * s_rx_len state machine (see netw_pd_rcv.asm):
 *   0        : free — callback can hand buffer to driver on next AX=0
 *   0xFFFF   : awaiting — buffer handed to driver, AX=1 not yet received
 *   1..1514  : frame received and ready for filtering/consume
 */
uint8_t           s_rx_buf[FRAME_MAX];
volatile uint16_t s_rx_len;

/* Far pointer to packet driver handler: [off16, seg16] (little-endian dword).
 * Read from IVT in netw_pd_set_params; used by pd_send_asm_ via call dword ptr. */
uint32_t s_pd_vector;

/* Callback counters — reset before each netw_recv, written by netw_pd_rcv.asm.
 *   ax0_ok    AX=0 calls where we handed the buffer (s_rx_len was 0)
 *   ax0_busy  AX=0 calls where we refused (s_rx_len was non-zero)
 *   ax1       AX=1 calls (frame delivered)
 *   ax1_cx    CX value of last AX=1 (frame length)
 * Extended per-frame info (populated in AX=1 path):
 *   cb_refuse_reason  s_rx_len value at last AX=0 refusal
 *   cb_last_et        raw word from s_rx_buf[12:13] (byte12 in low, byte13 in high)
 *   cb_last_mac[6]    src MAC from s_rx_buf[6..11]
 *   cb_last_ip[4]     src IP from s_rx_buf[26..29], valid only when cb_last_et==0x0008
 */
volatile uint16_t s_dbg_ax0_ok;
volatile uint16_t s_dbg_ax0_busy;
volatile uint16_t s_dbg_ax1;
volatile uint16_t s_dbg_ax1_cx;

volatile uint16_t s_cb_refuse_reason;  /* s_rx_len at last AX=0 refusal           */
volatile uint16_t s_cb_last_et;        /* EtherType word (little-endian) of last AX=1 frame */
uint8_t           s_cb_last_mac[6];    /* src MAC  of last AX=1 frame              */
uint8_t           s_cb_last_ip[4];     /* src IP   of last AX=1 frame (if IP)      */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void put16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint16_t ip_checksum(const uint8_t *hdr)
{
    uint32_t sum = 0;
    int i;
    for (i = 0; i < 20; i += 2)
        sum += (uint32_t)(((uint16_t)hdr[i] << 8) | hdr[i + 1]);
    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void parse_ip4(const char *s, uint8_t ip[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned int v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned)(*s - '0'); s++; }
        ip[i] = (uint8_t)v;
        if (*s == '.') s++;
    }
}

/* ------------------------------------------------------------------ */
/* Packet driver INT calls                                              */
/* Calling convention used here:                                        */
/*   AH = function code (2=access_type, 3=release_type, ...)           */
/*   AL = if_class      (1 = DIX Ethernet)                             */
/*   BX = if_type       (0xFFFF = receive all ethertype values)        */
/*   DL = if_number     (0 = first adapter)                            */
/*   DS:SI = type filter buffer (required by spec, ignored when BX=-1) */
/*   CX = typelen       (2 for Ethernet)                               */
/*   ES:DI = far pointer to receiver callback                           */
/* ------------------------------------------------------------------ */

/*
 * All pd_* functions use STATIC union REGS / struct SREGS so that the
 * address is DS-relative (DGROUP), not SS-relative.  With -zu (SS != DS),
 * local (stack) variables produce far pointers that int86x() cannot accept.
 */

static int pd_access_type(uint16_t ethertype, const uint8_t *type_buf)
{
    static union REGS  r;
    static struct SREGS sr;
    unsigned int cb_seg, cb_off, ds_val, si_val;
    int handle;

    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.h.ah  = 2;
    r.h.al  = 1;              /* class = Ethernet (DIX) */
    r.x.bx  = 0xFFFFu;        /* any interface type; ethertype filter is DS:SI/CX */
    r.h.dl  = 0;              /* first adapter          */
    ds_val  = get_ds();
    sr.ds   = ds_val;
    si_val  = (unsigned)type_buf;
    r.x.si  = si_val;
    r.x.cx  = 2;
    cb_seg  = FP_SEG((void __far *)pd_receiver);
    cb_off  = FP_OFF((void __far *)pd_receiver);
    sr.es   = cb_seg;
    r.x.di  = cb_off;
    int86x((int)s_pd_int, &r, &r, &sr);

    handle = r.x.cflag ? -1 : (int)(unsigned)r.x.ax;
    if (r.x.cflag)
        printf("AT: INT=%02Xh ET=%04X DS:SI=%04X:%04X ES:DI=%04X:%04X CF=1 AX=%04X DX=%04X h=-1\r\n",
               (unsigned)s_pd_int, (unsigned)ethertype,
               ds_val, si_val, cb_seg, cb_off,
               (unsigned)r.x.ax, (unsigned)r.x.dx);
    else
        printf("AT: INT=%02Xh ET=%04X DS:SI=%04X:%04X ES:DI=%04X:%04X CF=0 AX=%04X h=%d\r\n",
               (unsigned)s_pd_int, (unsigned)ethertype,
               ds_val, si_val, cb_seg, cb_off,
               (unsigned)r.x.ax, handle);
    return handle;
}

static void pd_release_type(int handle)
{
    static union REGS  r;
    static struct SREGS sr;
    if (handle < 0) return;
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.h.ah = 3;
    r.x.bx = (unsigned)handle;
    int86x((int)s_pd_int, &r, &r, &sr);
}

static int pd_send_pkt(const uint8_t *buf, unsigned int len)
{
    /* Direct far call via pd_send_asm (pushf/cli/call dword[s_pd_vector]).
     * buf is a near pointer (DS-relative in DGROUP) — cast gives its offset. */
    if (pd_send_asm((unsigned)buf, len) != 0) {
        rb_write("SND FAIL\r\n");
        return -1;
    }
    rb_write("SND ok\r\n");
    return 0;
}

static int pd_get_address(int handle)
{
    static union REGS  r;
    static struct SREGS sr;
    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.h.ah = 6;
    r.x.bx = (unsigned)handle;
    sr.es  = get_ds();
    r.x.di = (unsigned)s_local_mac;
    r.x.cx = 6;
    int86x((int)s_pd_int, &r, &r, &sr);
    return r.x.cflag ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* ARP resolution                                                        */
/* ------------------------------------------------------------------ */

static void build_arp_request(void)
{
    static uint8_t bcast[6]    = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static uint8_t zero_mac[6] = {0,0,0,0,0,0};

    memset(s_tx_buf, 0, ARP_FRAME_LEN);
    memcpy(s_tx_buf + OFF_ETH_DST,  bcast,       6);
    memcpy(s_tx_buf + OFF_ETH_SRC,  s_local_mac, 6);
    s_tx_buf[OFF_ETH_TYPE]     = 0x08;
    s_tx_buf[OFF_ETH_TYPE + 1] = 0x06;
    put16be(s_tx_buf + OFF_ARP_HTYPE, 0x0001u);
    put16be(s_tx_buf + OFF_ARP_PTYPE, 0x0800u);
    s_tx_buf[OFF_ARP_HLEN] = 6;
    s_tx_buf[OFF_ARP_PLEN] = 4;
    put16be(s_tx_buf + OFF_ARP_OPER,  0x0001u);
    memcpy(s_tx_buf + OFF_ARP_SHA, s_local_mac,  6);
    memcpy(s_tx_buf + OFF_ARP_SPA, s_local_ip,   4);
    memcpy(s_tx_buf + OFF_ARP_THA, zero_mac,      6);
    memcpy(s_tx_buf + OFF_ARP_TPA, s_server_ip,   4);
}

static int arp_resolve(void)
{
    static uint8_t et_arp[2] = {0x08, 0x06};
    uint16_t start;
    int retry;

    s_arp_handle = pd_access_type(0x0806u, et_arp);
    if (s_arp_handle < 0) {
        printf("PD: ARP access_type failed (INT %02Xh)\r\n", s_pd_int);
        rb_write("PD: ARP handle FAIL\r\n");
        return -1;
    }

    for (retry = 0; retry < 3; retry++) {
        s_rx_len = 0;
        s_dbg_ax0_ok = s_dbg_ax0_busy = s_dbg_ax1 = s_dbg_ax1_cx = 0;

        build_arp_request();
        printf("PD: ARP #%d -> %u.%u.%u.%u\r\n", retry,
               (unsigned)s_server_ip[0], (unsigned)s_server_ip[1],
               (unsigned)s_server_ip[2], (unsigned)s_server_ip[3]);
        rb_write("PD: ARP req #"); rb_dec((unsigned)retry); rb_write("\r\n");

        if (pd_send_pkt(s_tx_buf, ARP_FRAME_LEN) != 0) {
            printf("PD: ARP send failed\r\n");
            rb_write("PD: ARP send FAIL\r\n");
            break;
        }

        start = bios_ticks();
        while ((uint16_t)(bios_ticks() - start) < ARP_TIMEOUT_TICKS) {
            /* Skip: buffer free (0) or handed to driver but not yet delivered (0xFFFF). */
            if (s_rx_len == 0 || s_rx_len == 0xFFFFu) continue;

            /* Frame dump — before any filtering */
            {
                unsigned int i, n;
                printf("RX: len=%u ET=%02X%02X\r\n",
                       (unsigned)s_rx_len,
                       (unsigned)s_rx_buf[12], (unsigned)s_rx_buf[13]);
                n = ((unsigned)s_rx_len < 32u) ? (unsigned)s_rx_len : 32u;
                for (i = 0; i < n; i++)
                    printf("%02X%c", (unsigned)s_rx_buf[i],
                           ((i & 15u) == 15u) ? '\n' : ' ');
                printf("\r\n");
                if (s_rx_buf[12] == 0x08 && s_rx_buf[13] == 0x06) {
                    printf("ARP op=%04X"
                           " SHA=%02X:%02X:%02X:%02X:%02X:%02X SPA=%u.%u.%u.%u"
                           " THA=%02X:%02X:%02X:%02X:%02X:%02X TPA=%u.%u.%u.%u\r\n",
                           (unsigned)get16be(s_rx_buf + OFF_ARP_OPER),
                           (unsigned)s_rx_buf[OFF_ARP_SHA+0], (unsigned)s_rx_buf[OFF_ARP_SHA+1],
                           (unsigned)s_rx_buf[OFF_ARP_SHA+2], (unsigned)s_rx_buf[OFF_ARP_SHA+3],
                           (unsigned)s_rx_buf[OFF_ARP_SHA+4], (unsigned)s_rx_buf[OFF_ARP_SHA+5],
                           (unsigned)s_rx_buf[OFF_ARP_SPA+0], (unsigned)s_rx_buf[OFF_ARP_SPA+1],
                           (unsigned)s_rx_buf[OFF_ARP_SPA+2], (unsigned)s_rx_buf[OFF_ARP_SPA+3],
                           (unsigned)s_rx_buf[OFF_ARP_THA+0], (unsigned)s_rx_buf[OFF_ARP_THA+1],
                           (unsigned)s_rx_buf[OFF_ARP_THA+2], (unsigned)s_rx_buf[OFF_ARP_THA+3],
                           (unsigned)s_rx_buf[OFF_ARP_THA+4], (unsigned)s_rx_buf[OFF_ARP_THA+5],
                           (unsigned)s_rx_buf[OFF_ARP_TPA+0], (unsigned)s_rx_buf[OFF_ARP_TPA+1],
                           (unsigned)s_rx_buf[OFF_ARP_TPA+2], (unsigned)s_rx_buf[OFF_ARP_TPA+3]);
                }
                fflush(stdout);
            }

            /* Validate: ARP reply (opcode=2) from server IP */
            if (s_rx_len >= (uint16_t)ARP_FRAME_LEN &&
                get16be(s_rx_buf + OFF_ETH_TYPE) == 0x0806u &&
                get16be(s_rx_buf + OFF_ARP_OPER) == 0x0002u &&
                memcmp(s_rx_buf + OFF_ARP_SPA, s_server_ip, 4) == 0) {

                memcpy(s_server_mac, s_rx_buf + OFF_ARP_SHA, 6);
                s_rx_len = 0;
                pd_release_type(s_arp_handle);
                s_arp_handle = -1;

                printf("PD: ARP ok  %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                       (unsigned)s_server_mac[0], (unsigned)s_server_mac[1],
                       (unsigned)s_server_mac[2], (unsigned)s_server_mac[3],
                       (unsigned)s_server_mac[4], (unsigned)s_server_mac[5]);
                rb_write("PD: ARP ok\r\n");
                return 0;
            }
            s_rx_len = 0;   /* not our reply — discard */
        }

        printf("PD: ARP timeout #%d  ax0ok=%u ax0busy=%u ax1=%u cx=%u\r\n",
               retry,
               (unsigned)s_dbg_ax0_ok, (unsigned)s_dbg_ax0_busy,
               (unsigned)s_dbg_ax1,    (unsigned)s_dbg_ax1_cx);
        rb_write("PD: ARP to retry="); rb_dec((unsigned)retry); rb_write("\r\n");
    }

    pd_release_type(s_arp_handle);
    s_arp_handle = -1;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                            */
/* ------------------------------------------------------------------ */

void netw_pd_set_params(unsigned int pd_int, const char *local_ip_str)
{
    uint16_t ivt_off, ivt_seg;

    s_pd_int     = pd_int;
    s_arp_handle = -1;
    s_ip_handle  = -1;
    s_local_port = 0xC000u;   /* 49152 — arbitrary source port */
    s_ip_id      = 0x1234u;
    s_dbg_ax0_ok = s_dbg_ax0_busy = s_dbg_ax1 = s_dbg_ax1_cx = 0;
    parse_ip4(local_ip_str, s_local_ip);

    /* Read packet driver far pointer from IVT (segment 0, offset = int_num * 4).
     * Store as [off16:seg16] dword so pd_send_asm_ can call dword ptr [s_pd_vector]. */
    ivt_off = *(uint16_t far *)MK_FP(0, (uint16_t)pd_int * 4u);
    ivt_seg = *(uint16_t far *)MK_FP(0, (uint16_t)pd_int * 4u + 2u);
    s_pd_vector = ((uint32_t)ivt_seg << 16) | (uint32_t)ivt_off;
    printf("PD: INT %02Xh vector %04X:%04X\r\n",
           pd_int, (unsigned)ivt_seg, (unsigned)ivt_off);
}

int netw_connect(char *host, int port, bool useTCP)
{
    static uint8_t et_ip[2] = {0x08, 0x00};
    int temp;
    (void)useTCP;

    s_server_port = (uint16_t)(unsigned)port;
    parse_ip4(host, s_server_ip);

    /* Briefly open any handle to read the local MAC, then close it. */
    temp = pd_access_type(0x0800u, et_ip);
    if (temp < 0) {
        printf("PD: access_type failed at INT %02Xh\r\n", s_pd_int);
        rb_write("PD: access_type FAIL\r\n");
        return -1;
    }
    if (pd_get_address(temp) != 0) {
        pd_release_type(temp);
        printf("PD: get_address failed\r\n");
        rb_write("PD: get_addr FAIL\r\n");
        return -1;
    }
    pd_release_type(temp);

    printf("PD: MAC %02X:%02X:%02X:%02X:%02X:%02X  local %u.%u.%u.%u\r\n",
           (unsigned)s_local_mac[0], (unsigned)s_local_mac[1],
           (unsigned)s_local_mac[2], (unsigned)s_local_mac[3],
           (unsigned)s_local_mac[4], (unsigned)s_local_mac[5],
           (unsigned)s_local_ip[0],  (unsigned)s_local_ip[1],
           (unsigned)s_local_ip[2],  (unsigned)s_local_ip[3]);

    /* ARP: resolve server MAC. */
    if (arp_resolve() != 0) {
        printf("PD: ARP failed\r\n");
        return -1;
    }

    /* Register permanent IP receiver for the TNFS session. */
    s_ip_handle = pd_access_type(0x0800u, et_ip);
    if (s_ip_handle < 0) {
        printf("PD: IP access_type failed\r\n");
        rb_write("PD: IP handle FAIL\r\n");
        return -1;
    }

    s_rx_len = 0;
    s_dbg_ax0_ok = s_dbg_ax0_busy = s_dbg_ax1 = s_dbg_ax1_cx = 0;
    rb_write("PD: ready\r\n");
    return 0;
}

void netw_disconnect(void)
{
    pd_release_type(s_ip_handle);
    s_ip_handle = -1;
}

void netw_clear_rx(void)
{
    s_rx_len = 0;
}

void netw_warmup(int iters)
{
    /* No-op: receiver is interrupt-driven; no polling required. */
    (void)iters;
}

int netw_send(const uint8_t *payload, int len)
{
    unsigned int frame_len = (unsigned)(DATA_OFF + len);
    unsigned int send_len  = (frame_len < 60u) ? 60u : frame_len;
    uint16_t ip_total      = (uint16_t)(IP_HDR + UDP_HDR + (unsigned)len);
    uint16_t udp_total     = (uint16_t)(UDP_HDR + (unsigned)len);
    uint16_t ip_id;
    uint16_t csum;
    unsigned int i;

    if (frame_len > (unsigned)FRAME_MAX) return -1;

    /* Zero full send buffer (including Ethernet pad bytes) */
    memset(s_tx_buf, 0, send_len);

    /* Ethernet */
    memcpy(s_tx_buf + OFF_ETH_DST, s_server_mac, 6);
    memcpy(s_tx_buf + OFF_ETH_SRC, s_local_mac,  6);
    s_tx_buf[OFF_ETH_TYPE]     = 0x08;
    s_tx_buf[OFF_ETH_TYPE + 1] = 0x00;

    /* IPv4 */
    ip_id = s_ip_id++;
    s_tx_buf[OFF_IP_VERHDR] = 0x45;
    s_tx_buf[OFF_IP_DSCP]   = 0;
    put16be(s_tx_buf + OFF_IP_LEN,   ip_total);
    put16be(s_tx_buf + OFF_IP_ID,    ip_id);
    put16be(s_tx_buf + OFF_IP_FLAGS, 0x4000u);  /* DF */
    s_tx_buf[OFF_IP_TTL]   = 64;
    s_tx_buf[OFF_IP_PROTO] = 17;                /* UDP */
    put16be(s_tx_buf + OFF_IP_CSUM, 0);
    memcpy(s_tx_buf + OFF_IP_SRC, s_local_ip,  4);
    memcpy(s_tx_buf + OFF_IP_DST, s_server_ip, 4);
    csum = ip_checksum(s_tx_buf + IP_BASE);
    put16be(s_tx_buf + OFF_IP_CSUM, csum);

    /* UDP */
    put16be(s_tx_buf + OFF_UDP_SPORT, s_local_port);
    put16be(s_tx_buf + OFF_UDP_DPORT, s_server_port);
    put16be(s_tx_buf + OFF_UDP_LEN,   udp_total);
    put16be(s_tx_buf + OFF_UDP_CSUM,  0);

    /* Payload */
    memcpy(s_tx_buf + DATA_OFF, payload, (unsigned)len);

    /* Log TX header fields */
    rb_write("TX iid="); rb_hex16(ip_id);
    rb_write(" ilen="); rb_hex16(ip_total);
    rb_write(" cs="); rb_hex16(csum);
    rb_write(" sp="); rb_hex16(s_local_port);
    rb_write(" dp="); rb_hex16(s_server_port);
    rb_write(" ul="); rb_hex16(udp_total);
    rb_write(" fl="); rb_dec(send_len);
    rb_write("\r\n");

    /* Dump first 48 bytes of TX frame (ETH+IP+UDP+6 TNFS) */
    rb_write("TXH:");
    for (i = 0; i < 48u; i++) { rb_hex8(s_tx_buf[i]); rb_putc(' '); }
    rb_write("\r\n");

    return pd_send_pkt(s_tx_buf, send_len);
}

int netw_recv(uint8_t *buf, int buf_size)
{
    uint16_t start, elapsed;
    unsigned int payload_len;
    uint16_t udp_data_len;
    uint8_t pic1_start, pic2_start;

    /* Sample PIC mask registers right after send completes, before polling begins.
     * Logged on every call so working vs failing calls can be compared. */
    pic1_start = (uint8_t)inp(0x21);
    pic2_start = (uint8_t)inp(0xA1);
    rb_write("RCV p1="); rb_hex8(pic1_start);
    rb_write(" p2="); rb_hex8(pic2_start); rb_write("\r\n");
    /* Reset per-call diagnostics so the timeout line shows only this wait. */
    s_dbg_ax0_ok = s_dbg_ax0_busy = s_dbg_ax1 = s_dbg_ax1_cx = 0;
    s_cb_refuse_reason = 0;
    s_cb_last_et = 0;
    start = bios_ticks();

    for (;;) {
        elapsed = (uint16_t)(bios_ticks() - start);
        if (elapsed >= RECV_TIMEOUT_TICKS) {
            /* Line 1: counters */
            rb_write("RCV TO");
            rb_write(" rx=");   rb_hex16(s_rx_len);
            rb_write(" ax0=");  rb_hex8((uint8_t)(s_dbg_ax0_ok + s_dbg_ax0_busy));
            rb_write(" giv=");  rb_hex8((uint8_t)s_dbg_ax0_ok);
            rb_write(" ref=");  rb_hex8((uint8_t)s_dbg_ax0_busy);
            rb_write(" refr="); rb_hex16(s_cb_refuse_reason);
            rb_write(" ax1=");  rb_hex8((uint8_t)s_dbg_ax1);
            rb_write(" len=");  rb_hex16(s_dbg_ax1_cx);
            rb_write("\r\n");
            /* Line 2: last frame info from callback (valid if ax1 > 0) */
            rb_write("LAST et=");
            rb_hex8((uint8_t)(s_cb_last_et));           /* byte[12] */
            rb_hex8((uint8_t)(s_cb_last_et >> 8));      /* byte[13] */
            rb_write(" mac=");
            rb_hex8(s_cb_last_mac[0]); rb_putc(':');
            rb_hex8(s_cb_last_mac[1]); rb_putc(':');
            rb_hex8(s_cb_last_mac[2]); rb_putc(':');
            rb_hex8(s_cb_last_mac[3]); rb_putc(':');
            rb_hex8(s_cb_last_mac[4]); rb_putc(':');
            rb_hex8(s_cb_last_mac[5]);
            /* src IP — only meaningful when EtherType == 0x0800 */
            if ((uint8_t)(s_cb_last_et) == 0x08u && (uint8_t)(s_cb_last_et >> 8) == 0x00u) {
                rb_write(" ip=");
                rb_hex8(s_cb_last_ip[0]); rb_putc('.');
                rb_hex8(s_cb_last_ip[1]); rb_putc('.');
                rb_hex8(s_cb_last_ip[2]); rb_putc('.');
                rb_hex8(s_cb_last_ip[3]);
            }
            rb_write("\r\n");
            /* Line 3: PIC mask at start of recv vs at timeout — detects stuck IRQ mask */
            rb_write("PIC p1="); rb_hex8(pic1_start);
            rb_write(" p2="); rb_hex8(pic2_start);
            rb_write(" t1="); rb_hex8((uint8_t)inp(0x21));
            rb_write(" t2="); rb_hex8((uint8_t)inp(0xA1));
            rb_write("\r\n");
            return NETW_ERR_TIMEOUT;
        }

        /* Skip: buffer free (0) or handed to driver but not yet delivered (0xFFFF). */
        if (s_rx_len == 0 || s_rx_len == 0xFFFFu) continue;

        /* Log every received frame before filtering */
        rb_write("FR ln="); rb_hex8((uint8_t)s_rx_len);
        rb_write(" et="); rb_hex8(s_rx_buf[12]); rb_hex8(s_rx_buf[13]);
        rb_write(" src=");
        rb_hex8(s_rx_buf[6]);  rb_putc(':');
        rb_hex8(s_rx_buf[7]);  rb_putc(':');
        rb_hex8(s_rx_buf[8]);  rb_putc(':');
        rb_hex8(s_rx_buf[9]);  rb_putc(':');
        rb_hex8(s_rx_buf[10]); rb_putc(':');
        rb_hex8(s_rx_buf[11]);
        rb_write("\r\n");

        /* Minimum frame size */
        if (s_rx_len < (uint16_t)DATA_OFF) {
            rb_write("DROP short\r\n");
            s_rx_len = 0; continue;
        }

        /* EtherType must be IPv4 */
        if (get16be(s_rx_buf + OFF_ETH_TYPE) != 0x0800u) {
            rb_write("DROP et="); rb_hex8(s_rx_buf[12]); rb_hex8(s_rx_buf[13]); rb_write("\r\n");
            s_rx_len = 0; continue;
        }

        /* IP protocol must be UDP (17) */
        if (s_rx_buf[OFF_IP_PROTO] != 17u) {
            rb_write("DROP proto="); rb_hex8(s_rx_buf[OFF_IP_PROTO]); rb_write("\r\n");
            s_rx_len = 0; continue;
        }

        /* Source IP must be server */
        if (memcmp(s_rx_buf + OFF_IP_SRC, s_server_ip, 4) != 0) {
            rb_write("DROP srcip=");
            rb_hex8(s_rx_buf[OFF_IP_SRC]);   rb_putc('.');
            rb_hex8(s_rx_buf[OFF_IP_SRC+1]); rb_putc('.');
            rb_hex8(s_rx_buf[OFF_IP_SRC+2]); rb_putc('.');
            rb_hex8(s_rx_buf[OFF_IP_SRC+3]);
            rb_write(" exp=");
            rb_hex8(s_server_ip[0]); rb_putc('.');
            rb_hex8(s_server_ip[1]); rb_putc('.');
            rb_hex8(s_server_ip[2]); rb_putc('.');
            rb_hex8(s_server_ip[3]);
            rb_write("\r\n");
            s_rx_len = 0; continue;
        }

        /* Destination IP must be us */
        if (memcmp(s_rx_buf + OFF_IP_DST, s_local_ip, 4) != 0) {
            rb_write("DROP dstip=");
            rb_hex8(s_rx_buf[OFF_IP_DST]);   rb_putc('.');
            rb_hex8(s_rx_buf[OFF_IP_DST+1]); rb_putc('.');
            rb_hex8(s_rx_buf[OFF_IP_DST+2]); rb_putc('.');
            rb_hex8(s_rx_buf[OFF_IP_DST+3]);
            rb_write("\r\n");
            s_rx_len = 0; continue;
        }

        /* UDP src port must be server port */
        if (get16be(s_rx_buf + OFF_UDP_SPORT) != s_server_port) {
            rb_write("DROP sp="); rb_hex16(get16be(s_rx_buf + OFF_UDP_SPORT));
            rb_write(" exp=");    rb_hex16(s_server_port);
            rb_write("\r\n");
            s_rx_len = 0; continue;
        }

        /* UDP dst port must be our local port */
        if (get16be(s_rx_buf + OFF_UDP_DPORT) != s_local_port) {
            rb_write("DROP dp="); rb_hex16(get16be(s_rx_buf + OFF_UDP_DPORT));
            rb_write(" exp=");    rb_hex16(s_local_port);
            rb_write("\r\n");
            s_rx_len = 0; continue;
        }

        /* Extract payload */
        udp_data_len = get16be(s_rx_buf + OFF_UDP_LEN);
        if (udp_data_len < (uint16_t)UDP_HDR) {
            rb_write("DROP udplen="); rb_hex16(udp_data_len); rb_write("\r\n");
            s_rx_len = 0; continue;
        }
        payload_len = (unsigned)(udp_data_len - (uint16_t)UDP_HDR);
        if ((int)payload_len > buf_size) payload_len = (unsigned)buf_size;

        /* Log accepted frame + first 8 bytes of TNFS payload */
        rb_write("ACCEPT sp="); rb_hex16(s_server_port);
        rb_write(" dp="); rb_hex16(s_local_port);
        rb_write(" plen="); rb_hex8((uint8_t)payload_len);
        rb_write(" pay=");
        {
            unsigned int pi, pn;
            pn = (payload_len < 8u) ? payload_len : 8u;
            for (pi = 0; pi < pn; pi++) { rb_hex8(s_rx_buf[DATA_OFF + pi]); rb_putc(' '); }
        }
        rb_write("\r\n");

        memcpy(buf, s_rx_buf + DATA_OFF, payload_len);
        s_rx_len = 0;

        rb_write("RCV ok\r\n");
        return (int)payload_len;
    }
}

void netw_get_server_ip(char *buf, int size)
{
    if (size >= 16)
        sprintf(buf, "%u.%u.%u.%u",
                (unsigned)s_server_ip[0], (unsigned)s_server_ip[1],
                (unsigned)s_server_ip[2], (unsigned)s_server_ip[3]);
    else if (size > 0)
        buf[0] = '\0';
}
