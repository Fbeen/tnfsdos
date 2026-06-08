/*
 * netw.cpp — network layer for TNFSDRV.
 *
 * Identical to tnfstest/src/netw_mtcp.cpp with two additions:
 *   - netw_connect returns int (0=ok, -1=fail) so netinit.c can check it
 *   - numeric dotted-decimal servername resolves without DNS
 *   - netw_clear_rx / netw_warmup / netw_get_server_ip kept for tnfs.c
 */

#include <stdio.h>
#include <string.h>

#include "types.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "udp.h"
#include "dns.h"
#include "timer.h"

#include "netw.h"

/* Forward-declare ring buffer primitives without pulling in ringbuf.h
 * (which has #include <i86.h> that conflicts inside extern "C" in C++). */
#ifdef TNFSDRV_DEBUG_RINGBUF
extern "C" {
    struct _RingBuf { unsigned int magic, head, tail, size, enabled; char data[4096]; };
    extern _RingBuf rbuf;
    void rb_write(const char *s);
    void rb_hex8(unsigned char v);
}
#define NLOG(s)   do { if (rbuf.enabled) rb_write(s); } while(0)
#define NLOGH(v)  do { if (rbuf.enabled) rb_hex8(v); } while(0)
#else
#define NLOG(s)   ((void)0)
#define NLOGH(v)  ((void)0)
#endif

#define NETW_LOCAL_PORT     16384
#define NETW_TIMEOUT_MS     3000
#define NETW_PAYLOAD_OFFSET 42

static IpAddr_t  server_ip;
static uint16_t  remote_port = 16384;
static uint16_t  local_port  = NETW_LOCAL_PORT;

static volatile int      rx_ready  = 0;
static          int      rx_len    = 0;

#define NETW_MAX_PACKET 1500
#define RX_BUF_SIZE     1500

static uint8_t send_buffer[NETW_MAX_PACKET];
static uint8_t rx_buf[RX_BUF_SIZE];   /* own copy — never points into mTCP buffer pool */

static void netw_udp_callback(const unsigned char *packet, const UdpHeader *udp)
{
    int len = ntohs(udp->len) - sizeof(UdpHeader);
    if (len < 0) len = 0;
    if (len > RX_BUF_SIZE) len = RX_BUF_SIZE;

    if (rx_ready) {
        /* Previous packet not yet consumed — drop incoming, just free buffer */
        NLOG("RX DROP BUSY\r\n");
        Buffer_free(packet);
        return;
    }

    /* Copy payload into our own buffer before releasing mTCP's buffer */
    memcpy(rx_buf, packet + NETW_PAYLOAD_OFFSET, len);
    rx_len   = len;
    rx_ready = 1;

    /* Return buffer to mTCP pool — must happen after copy */
    Buffer_free(packet);
}

static void netw_poll(void)
{
    /* PACKET_PROCESS_SINGLE calls SLEEP() → int 28h when the ring buffer is
     * empty.  int 28h is a DOS call, and int 2Fh function 1680h re-enters
     * INT 2Fh itself.  Both are deadly from within the INT 2Fh TSR handler.
     * Disable the sleep for the duration of the poll, then restore. */
    uint8_t saved_sleep = mTCP_sleepCallEnabled;
    mTCP_sleepCallEnabled = 0;
    PACKET_PROCESS_SINGLE;
    mTCP_sleepCallEnabled = saved_sleep;
    Arp::driveArp();
}

static void netw_wait_for_dns(void)
{
    while (Dns::isQueryPending()) {
        netw_poll();
        Dns::drivePendingQuery();
    }
}

void setTimeoutTime(int t) { (void)t; }

bool netw_isValidIpAddress(char *ipAddress) { (void)ipAddress; return false; }

bool netw_getIpAddress(char *ip, char *hostname)
{
    int rc;
    rc = Dns::resolve(hostname, server_ip, 1);
    netw_wait_for_dns();
    rc = Dns::resolve(hostname, server_ip, 0);
    if (rc != 0) return false;
    sprintf(ip, "%d.%d.%d.%d",
        server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    return true;
}

/* Parse "a.b.c.d" directly into server_ip without DNS. */
static bool netw_parse_ip_direct(const char *s)
{
    unsigned int v[4];
    int i, n;
    for (i = 0; i < 4; i++) {
        v[i] = 0; n = 0;
        while (*s >= '0' && *s <= '9') { v[i] = v[i]*10u + (unsigned)(*s-'0'); s++; n++; }
        if (!n || v[i] > 255u) return false;
        if (i < 3) { if (*s++ != '.') return false; }
    }
    if (*s != '\0') return false;
    server_ip[0] = (uint8_t)v[0]; server_ip[1] = (uint8_t)v[1];
    server_ip[2] = (uint8_t)v[2]; server_ip[3] = (uint8_t)v[3];
    return true;
}

void netw_get_server_ip(char *buf, int size)
{
    if (size >= 16)
        sprintf(buf, "%d.%d.%d.%d",
            server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    else if (size > 0)
        buf[0] = '\0';
}

void netw_warmup(int iters)
{
    int i;
    for (i = 0; i < iters; i++) netw_poll();
}

int netw_connect(char *host, int port, bool useTCP)
{
    static char ip[32];
    (void)useTCP;

    remote_port = (uint16_t)port;
    local_port  = NETW_LOCAL_PORT;

    if (Utils::parseEnv() != 0) {
        printf("ERROR: could not parse mTCP config (MTCPCFG not set?)\r\n");
        return -1;
    }
    if (Utils::initStack(0, 0, NULL, NULL)) {
        printf("ERROR: could not init TCP/IP stack\r\n");
        return -1;
    }

    Udp::registerCallback(local_port, netw_udp_callback);

    /* Numeric IP (e.g. "192.168.178.10") resolves directly; use DNS otherwise */
    if (!netw_parse_ip_direct(host)) {
        printf("Resolving %s...\r\n", host);
        if (!netw_getIpAddress(ip, host)) {
            printf("ERROR: DNS failed for '%s'\r\n", host);
            Utils::endStack();
            return -1;
        }
        printf("Server IP: %s\r\n", ip);
    }

    return 0;
}

void netw_disconnect(void)
{
    Utils::endStack();
}

void netw_clear_rx(void)
{
    rx_ready = 0;
    rx_len   = 0;
}

int netw_send(const uint8_t *buffer, int length)
{
    clockTicks_t start;
    int rc;

    if (length + (int)sizeof(UdpPacket_t) > NETW_MAX_PACKET) return NETW_ERR_TIMEOUT;

    memset(send_buffer, 0, sizeof(send_buffer));
    memcpy(send_buffer + sizeof(UdpPacket_t), buffer, length);

    rc = Udp::sendUdp(server_ip, local_port, remote_port, length, send_buffer, 1);

    NLOG("SND "); NLOGH((uint8_t)rc); NLOG("\r\n");

    start = TIMER_GET_CURRENT();
    while (rc == 1) {
        netw_poll();
        if (Timer_diff(start, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(NETW_TIMEOUT_MS)) {
            NLOG("SND to\r\n");
            return NETW_ERR_TIMEOUT;
        }
        rc = Udp::sendUdp(server_ip, local_port, remote_port, length, send_buffer, 1);
    }
    NLOG("SND ok\r\n");
    return 0;
}

int netw_recv(uint8_t *buffer, int buffer_size)
{
    clockTicks_t start;
    int len;

    NLOG("RCV\r\n");

    start = TIMER_GET_CURRENT();
    while (!rx_ready) {
        netw_poll();
        if (Timer_diff(start, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(NETW_TIMEOUT_MS)) {
            NLOG("RCV to\r\n");
            return NETW_ERR_TIMEOUT;
        }
    }
    NLOG("RCV pkt\r\n");

    len = rx_len;
    if (len > buffer_size) len = buffer_size;
    memcpy(buffer, rx_buf, len);
    rx_ready = 0;
    rx_len   = 0;
    return len;
}
