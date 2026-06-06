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

#define NETW_LOCAL_PORT     16384
#define NETW_TIMEOUT_MS     3000
#define NETW_PAYLOAD_OFFSET 42

static IpAddr_t  server_ip;
static uint16_t  remote_port = 16384;
static uint16_t  local_port  = NETW_LOCAL_PORT;

static volatile uint8_t packet_received = 0;
static int recv_length = 0;

#define NETW_MAX_PACKET 1500

static uint8_t send_buffer[NETW_MAX_PACKET];
static uint8_t recv_buffer[NETW_MAX_PACKET];

static void netw_udp_callback(const unsigned char *packet, const UdpHeader *udp)
{
    int len = ntohs(udp->len) - sizeof(UdpHeader);
    if (len < 0) len = 0;
    if (len > NETW_MAX_PACKET) len = NETW_MAX_PACKET;
    memcpy(recv_buffer, packet + NETW_PAYLOAD_OFFSET, len);
    recv_length    = len;
    packet_received = 1;
}

static void netw_poll(void)
{
    PACKET_PROCESS_SINGLE;
    Arp::driveArp();
}

static void netw_wait_for_dns(void)
{
    while (Dns::isQueryPending()) {
        netw_poll();
        Dns::drivePendingQuery();
    }
}

void setTimeoutTime(int t)  { (void)t; }

bool netw_isValidIpAddress(char *ipAddress)
{
    (void)ipAddress;
    return false;
}

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

void netw_get_server_ip(char *buf, int size)
{
    if (size >= 16)
        sprintf(buf, "%d.%d.%d.%d",
            server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    else if (size > 0)
        buf[0] = '\0';
}

int netw_connect(char *host, int port, bool useTCP)
{
    static char ip[32];
    (void)useTCP;

    remote_port = (uint16_t)port;
    local_port  = NETW_LOCAL_PORT;

    if (Utils::parseEnv() != 0)         return -1;
    if (Utils::initStack(0, 0, NULL, NULL)) return -1;

    Udp::registerCallback(local_port, netw_udp_callback);

    if (!netw_getIpAddress(ip, host)) {
        Utils::endStack();
        return -1;
    }

    return 0;
}

void netw_disconnect(void)
{
    Utils::endStack();
}

void netw_send(const uint8_t *buffer, int length)
{
    clockTicks_t start;
    int rc;

    if (length + (int)sizeof(UdpPacket_t) > NETW_MAX_PACKET) return;

    memset(send_buffer, 0, sizeof(send_buffer));
    memcpy(send_buffer + sizeof(UdpPacket_t), buffer, length);

    rc = Udp::sendUdp(server_ip, local_port, remote_port, length, send_buffer, 1);

    start = TIMER_GET_CURRENT();
    while (rc == 1) {
        netw_poll();
        if (Timer_diff(start, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(NETW_TIMEOUT_MS))
            return;
        rc = Udp::sendUdp(server_ip, local_port, remote_port, length, send_buffer, 1);
    }
}

int netw_recv(uint8_t *buffer, int buffer_size)
{
    clockTicks_t start = TIMER_GET_CURRENT();

    while (!packet_received) {
        netw_poll();
        if (Timer_diff(start, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(NETW_TIMEOUT_MS))
            return NETW_ERR_TIMEOUT;
    }

    {
        int len = recv_length;
        if (len > buffer_size) len = buffer_size;
        memcpy(buffer, recv_buffer, len);
        packet_received = 0;
        recv_length     = 0;
        return len;
    }
}
