#ifndef NETINIT_H
#define NETINIT_H
#include "config.h"

/* Set up packet-driver backend, ARP-resolve server, TNFS mount.
 * Prints status to stdout.  Returns 0 on success, -1 on any failure. */
int  tnfsdrv_connect(const TnfsDrvConfig *cfg);

/* Release packet-driver handle (only call if not going TSR). */
void tnfsdrv_disconnect(void);

#endif
