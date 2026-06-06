#include <stdio.h>
#include "config.h"
#include "netinit.h"
#include "tnfs.h"
#include "netw.h"

extern uint16_t tnfs_session_id;

int tnfsdrv_connect(const TnfsDrvConfig *cfg)
{
    static char ip_str[20];
    int rc;

    printf("Connecting to %s:%u...\r\n", cfg->servername, cfg->port);

    rc = netw_connect((char *)cfg->servername, (int)cfg->port, 0);
    if (rc != 0) {
        printf("ERROR: mTCP init or DNS resolution failed\r\n");
        return -1;
    }

    netw_get_server_ip(ip_str, (int)sizeof(ip_str));
    printf("Server:   %s\r\n", ip_str);
    printf("mTCP initialized\r\n");

    rc = tnfs_mount(cfg->serverroot, "", "");
    if (rc != 0) {
        printf("ERROR: TNFS mount failed (%s)\r\n", tnfs_error_string(rc));
        netw_disconnect();
        return -1;
    }

    printf("TNFS session: 0x%04X\r\n", (unsigned int)tnfs_session_id);
    printf("TNFS server reachable.\r\n");
    return 0;
}

void tnfsdrv_disconnect(void)
{
    netw_disconnect();
}
