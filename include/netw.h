#ifndef __netw_h__
#define __netw_h__

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef __cplusplus
    #ifdef __WATCOMC__
        #ifndef bool
            typedef unsigned char bool;
        #endif
        #ifndef true
            #define true 1
        #endif
        #ifndef false
            #define false 0
        #endif
    #else
        #include <stdbool.h>
    #endif
#endif

#define NETW_ERR_TIMEOUT -2

#ifdef __cplusplus
extern "C" {
#endif

void setTimeoutTime(int t);

void netw_send(const uint8_t *buffer, int length);
int  netw_recv(uint8_t *buffer, int buffer_size);

bool netw_isValidIpAddress(char *ipAddress);
bool netw_getIpAddress(char *ip, char *hostname);

/* Returns 0 on success, -1 on failure (mTCP init, DNS, or ARP failed). */
int  netw_connect(char *host, int port, bool useTCP);
void netw_disconnect(void);

/* Fill buf with the resolved server IP as dotted-decimal string. */
void netw_get_server_ip(char *buf, int size);

#ifdef __cplusplus
}
#endif

#endif /* __netw_h__ */
