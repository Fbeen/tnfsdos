#ifndef __netw_h__
#define __netw_h__

#include <stdint.h>

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

void netw_clear_rx(void);

int  netw_send(const uint8_t *buffer, int length);
int  netw_recv(uint8_t *buffer, int buffer_size);

int  netw_connect(char *host, int port, bool useTCP);
void netw_disconnect(void);

void netw_get_server_ip(char *buf, int size);
void netw_warmup(int iters);

#ifdef __cplusplus
}
#endif

#endif /* __netw_h__ */
