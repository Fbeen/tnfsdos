/*
 * MAIN.C  —  TNFSDRV: TSR install + CDS initialisation.
 *
 * Usage:
 *   TNFSDRV              — uses [default] profile from TNFSDRV.CFG
 *   TNFSDRV <profile>    — uses [<profile>] section from TNFSDRV.CFG
 */

#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <string.h>
#include "ringbuf.h"
#include "fs.h"
#include "redirector.h"
#include "netinit.h"
#include "tnfs.h"

/* ------------------------------------------------------------------ */
/*  ASM handler interface                                               */
/* ------------------------------------------------------------------ */

extern void new_int2f_(void);
extern void init_int2f_ptr_(void);

void (__interrupt __far *old_int2f)(void);

unsigned short get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

extern unsigned int   _psp;
extern unsigned short _STACKTOP;

static unsigned int calc_resident_paras(void)
{
    unsigned long end = (unsigned long)(get_ds() - _psp) * 16UL + _STACKTOP;
    return (unsigned int)((end + 15) >> 4);
}

/* ------------------------------------------------------------------ */
/*  CDS constants                                                       */
/*                                                                      */
/* CDS_ENTRY_SIZE = 88: fixed for DOS 4–6                              */
/*   +0x00  current_path[67]                                            */
/*   +0x43  flags (word)  NET|PHY = 0xC000                             */
/*   +0x45  dpb_ptr (4 bytes) NULL for network drives                  */
/*   +0x4F  backslash_offset = 2 for "X:\"                             */
/* ------------------------------------------------------------------ */

#define CDS_ENTRY_SIZE  88

/* ------------------------------------------------------------------ */
/*  DOS internals helpers                                               */
/* ------------------------------------------------------------------ */

static void far *get_lol(void)
{
    static union REGS r;
    static struct SREGS sr;
    union { unsigned w[2]; void far *p; } u;
    r.h.ah = 0x52;
    int86x(0x21, &r, &r, &sr);
    u.w[0] = r.w.bx;
    u.w[1] = sr.es;
    return u.p;
}

static void init_cds(const TnfsDrvConfig *cfg)
{
    static union REGS r;
    static struct SREGS sr;
    char far *lol;
    int i;
    int drive_idx = (int)((unsigned char)(cfg->driveletter - 'A'));

    lol = (char far *)get_lol();

    {
        unsigned int cds_off, cds_seg;
        char far *cds_base;
        char far *entry;
        unsigned int flags;

        cds_off = (unsigned int)(unsigned char)lol[0x16]
                | ((unsigned int)(unsigned char)lol[0x17] << 8);
        cds_seg = (unsigned int)(unsigned char)lol[0x18]
                | ((unsigned int)(unsigned char)lol[0x19] << 8);
        cds_base = (char far *)MK_FP(cds_seg, cds_off);

        r.w.ax = 0x5D06;
        int86x(0x21, &r, &r, &sr);
        glob_sdaptr = (char far *)MK_FP(sr.ds, r.w.si);

        r.w.ax = 0x1100;
        int86(0x2F, &r, &r);

        if (r.w.ax != 0x0001) {
            entry = cds_base + (unsigned)(drive_idx * CDS_ENTRY_SIZE);
            g_cds_entry_ptr = entry;

            entry[0] = cfg->driveletter;
            entry[1] = ':'; entry[2] = '\\'; entry[3] = '\0';
            for (i = 4; i < 67; i++) entry[i] = 0;

            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            flags |= 0x8000u | 0x4000u;
            entry[0x43] = (char)(flags & 0xFF);
            entry[0x44] = (char)(flags >> 8);

            entry[0x45] = 0; entry[0x46] = 0; entry[0x47] = 0; entry[0x48] = 0;
            entry[0x49] = 0; entry[0x4A] = 0; entry[0x4B] = 0;
            entry[0x4C] = 0; entry[0x4D] = 0; entry[0x4E] = 0;
            entry[0x4F] = 2; entry[0x50] = 0;
            entry[0x51] = 0; entry[0x52] = 0; entry[0x53] = 0; entry[0x54] = 0;
            entry[0x55] = 0; entry[0x56] = 0; entry[0x57] = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  TSR install                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static TnfsDrvConfig cfg;
    const char   *profile;
#ifdef TNFSDRV_DEBUG_RINGBUF
    unsigned int  seg, off;
#endif
    unsigned int  paras;

#ifdef TNFSDRV_DEBUG_RINGBUF
    rbuf.magic   = RING_MAGIC;
    rbuf.head    = 0;
    rbuf.tail    = 0;
    rbuf.size    = RING_SIZE;
    rbuf.enabled = 1;
#endif

    freopen("log.txt", "w", stdout);

    profile = (argc > 1) ? argv[1] : "default";

    if (!config_load("TNFSDRV.CFG", profile, &cfg))
        printf("TNFSDRV.CFG not found, using defaults\r\n");

    if (strcmp(cfg.protocol, "UDP") != 0) {
        freopen("CON", "w", stdout);
        printf("TNFSDRV: protocol '%s' not supported (only UDP)\r\n", cfg.protocol);
        return 1;
    }

    if (cfg.driveletter < 'C' || cfg.driveletter > 'Z') {
        freopen("CON", "w", stdout);
        printf("TNFSDRV: invalid drive '%c:' — use C-Z\r\n", cfg.driveletter);
        return 1;
    }

    printf("Profile:  %s\r\n",  cfg.profile);
    printf("Drive:    %c:\r\n", cfg.driveletter);
    printf("Server:   %s\r\n",  cfg.servername[0] ? cfg.servername : "(none)");
    printf("Root:     %s\r\n",  cfg.serverroot);
    printf("Protocol: %s  Port: %u\r\n", cfg.protocol, cfg.port);

    if (tnfsdrv_connect(&cfg) != 0) {
        freopen("CON", "w", stdout);
        printf("TNFSDRV: connect failed — see log.txt\r\n");
        return 1;
    }

    fs_set_drive(cfg.driveletter);
    fs_set_cache_config(cfg.cache_enabled, cfg.cache_timeout_seconds, cfg.cache_dirs);
    printf("TNFSDRV loaded OK\r\n");

    init_cds(&cfg);

    old_int2f = _dos_getvect(0x2F);
    init_int2f_ptr_();
    _dos_setvect(0x2F, (void (__interrupt __far *)())new_int2f_);

    paras = calc_resident_paras();
#ifdef TNFSDRV_DEBUG_RINGBUF
    seg = get_ds();
    off = (unsigned int)&rbuf;
    printf("BufAddr: %04X:%04X\r\n", seg, off);  /* to log.txt */
#endif
    freopen("CON", "w", stdout);
    printf("Drive %c: installed", cfg.driveletter);
#ifdef TNFSDRV_DEBUG_RINGBUF
    printf("  [buf %04X:%04X]", seg, off);        /* to screen */
#endif
    printf("\r\n");
    fclose(stdout);

    {
        static union REGS r;
        r.h.ah = 0x31;
        r.h.al = 0;
        r.w.dx = paras;
        int86(0x21, &r, &r);
    }
    return 0;
}
