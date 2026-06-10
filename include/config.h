#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Compile-time fallback drive letter — used to initialise g_drive_idx.
 * The runtime value is set by fs_set_drive() after config_load(). */
#define TNFSDRV_DRIVE_LETTER  'N'
#define DRIVE_IDX             ((int)((unsigned char)(TNFSDRV_DRIVE_LETTER) - 'A'))

/* ------------------------------------------------------------------ */
/*  Runtime configuration                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char         profile[16];
    char         servername[64];
    char         serverroot[128];
    char         protocol[8];   /* always stored uppercase */
    unsigned int port;
    char         driveletter;   /* always uppercase A-Z */
    /* TNFSPD-specific fields (ignored by mTCP builds) */
    char         localip[16];   /* local (486) IP as dotted-decimal    */
    char         netmask[16];   /* netmask — parsed but currently unused */
    char         gateway[16];   /* gateway — parsed but currently unused */
    unsigned int packetint;     /* packet driver interrupt, default 0x60 */
    /* Directory cache settings */
    uint8_t      cache_enabled;          /* 0=off, 1=on */
    uint16_t     cache_timeout_seconds;  /* TTL in seconds, default 300 */
    uint8_t      cache_dirs;             /* number of cached dirs (currently max 1) */
} TnfsDrvConfig;

/* Fill *cfg with built-in defaults (no file I/O). */
void config_set_defaults(TnfsDrvConfig *cfg);

/* Load filename, apply [default] then [profile] on top.
 * Returns 1 if file was opened, 0 if not found (defaults still applied). */
int config_load(const char *filename, const char *profile, TnfsDrvConfig *cfg);

#endif
