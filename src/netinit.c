#include <stdio.h>
#include <string.h>
#include "config.h"
#include "netinit.h"
#include "tnfs.h"
#include "netw.h"
#include "netw_pd.h"

extern uint16_t tnfs_session_id;

int tnfsdrv_connect(const TnfsDrvConfig *cfg)
{
    static char ip_str[20];
    int rc;

    if (!cfg->localip[0]) {
        printf("ERROR: 'localip' not set in TNFSDRV.CFG\r\n");
        return -1;
    }

    printf("Packet driver: INT %02Xh\r\n", cfg->packetint);
    printf("Local IP:      %s\r\n", cfg->localip);
    printf("Server:        %s:%u\r\n", cfg->servername, cfg->port);

    netw_pd_set_params(cfg->packetint, cfg->localip);

    rc = netw_connect((char *)cfg->servername, (int)cfg->port, 0);
    if (rc != 0) {
        printf("ERROR: network init failed\r\n");
        return -1;
    }

    netw_get_server_ip(ip_str, (int)sizeof(ip_str));
    printf("Server IP:     %s\r\n", ip_str);

    rc = tnfs_mount(cfg->serverroot, "", "");
    if (rc != 0) {
        printf("ERROR: TNFS mount failed (%s)\r\n", tnfs_error_string(rc));
        netw_disconnect();
        return -1;
    }

    printf("TNFS session:  0x%04X  root: %s\r\n",
           (unsigned int)tnfs_session_id, cfg->serverroot);
    return 0;
}

void tnfsdrv_disconnect(void)
{
    netw_disconnect();
}
