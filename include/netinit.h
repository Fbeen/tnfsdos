#ifndef NETINIT_H
#define NETINIT_H
#include "config.h"

/* Initialise mTCP, resolve server hostname, send TNFS mount.
 * Prints status to stdout.  Returns 0 on success, -1 on any failure. */
int  tnfsdrv_connect(const TnfsDrvConfig *cfg);

/* Tear down mTCP stack (unhooks timer interrupt). Call after connect succeeds. */
void tnfsdrv_disconnect(void);

#endif
