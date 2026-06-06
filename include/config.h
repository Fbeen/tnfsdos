#ifndef CONFIG_H
#define CONFIG_H

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
} TnfsDrvConfig;

/* Fill *cfg with built-in defaults (no file I/O). */
void config_set_defaults(TnfsDrvConfig *cfg);

/* Load filename, apply [default] then [profile] on top.
 * Returns 1 if file was opened, 0 if not found (defaults still applied). */
int config_load(const char *filename, const char *profile, TnfsDrvConfig *cfg);

#endif
