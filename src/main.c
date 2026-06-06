/*
 * MAIN.C  —  TNFSDRV: TSR install + CDS initialisation.
 *
 * Usage:
 *   TNFSDRV              — uses [default] profile from TNFSDRV.CFG
 *   TNFSDRV <profile>    — uses [<profile>] section from TNFSDRV.CFG
 *
 * Responsibilities:
 *   - Load TNFSDRV.CFG and configure drive letter / server parameters
 *   - Find the DOS List of Lists and set up CDS entry for the virtual drive
 *   - Hook INT 2Fh to our handler
 *   - Stay resident (INT 21h AH=31h)
 */

#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <string.h>
#include "ringbuf.h"
#include "fs.h"
#include "redirector.h"
#include "netinit.h"

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
/* CDS_ENTRY_SIZE = 88: fixed for DOS 4–6 (DOSSTRUC.H cdsstruct)       */
/*   +0x00  current_path[67]                                            */
/*   +0x43  flags (word)  NET|PHY = 0xC000                             */
/*   +0x45  dpb_ptr (4 bytes) NULL for network drives                  */
/*   +0x49  net_union (6 bytes)                                         */
/*   +0x4F  backslash_offset = 2 for "X:\"                             */
/*   +0x51  DOS 4+ reserved[7]                                          */
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
    int i, j;
    int drive_idx = (int)((unsigned char)(cfg->driveletter - 'A'));

    rb_write("INIT_CDS START\r\n");

    r.h.ah = 0x30; r.h.al = 0x00;
    int86(0x21, &r, &r);
    rb_write("DOS="); rb_dec(r.h.al); rb_putc('.'); rb_dec(r.h.ah); rb_write("\r\n");

    r.w.ax = 0x5D06;
    int86x(0x21, &r, &r, &sr);
    glob_sdaptr = (char far *)MK_FP(sr.ds, r.w.si);
    rb_write("SDA="); rb_hex16(sr.ds); rb_putc(':'); rb_hex16(r.w.si); rb_write("\r\n");

    lol = (char far *)get_lol();
    rb_write("LoL="); rb_hex16(FP_SEG(lol)); rb_putc(':'); rb_hex16(FP_OFF(lol)); rb_write("\r\n");

    rb_write("LOL_DUMP\r\n");
    for (i = 0; i < 32; i += 16) {
        rb_hex16((unsigned)i); rb_putc(':');
        for (j = 0; j < 16; j++) { rb_putc(' '); rb_hex8((unsigned char)lol[i+j]); }
        rb_putc('\r'); rb_putc('\n');
    }

    {
        unsigned int cds_off, cds_seg, flags, bsoff;
        char far *cds_base;
        char far *entry;

        cds_off = (unsigned int)(unsigned char)lol[0x16]
                | ((unsigned int)(unsigned char)lol[0x17] << 8);
        cds_seg = (unsigned int)(unsigned char)lol[0x18]
                | ((unsigned int)(unsigned char)lol[0x19] << 8);
        cds_base = (char far *)MK_FP(cds_seg, cds_off);
        rb_write("CDS="); rb_hex16(cds_seg); rb_putc(':'); rb_hex16(cds_off); rb_write("\r\n");

        for (i = 0; i <= drive_idx; i++) {
            entry = cds_base + (unsigned)(i * CDS_ENTRY_SIZE);
            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            rb_putc('C'); rb_putc('D'); rb_putc('S'); rb_putc('[');
            rb_dec((unsigned)i); rb_write("]=\"");
            rb_far_str(FP_SEG(entry), FP_OFF(entry), 16);
            rb_write("\" FL="); rb_hex16(flags); rb_write("\r\n");
        }

        r.w.ax = 0x1100;
        int86(0x2F, &r, &r);
        rb_write("REDIR AX="); rb_hex16(r.w.ax); rb_write("\r\n");

        if (r.w.ax == 0x0001) {
            rb_write("REDIR NOT AVAILABLE\r\n");
        } else {
            entry = cds_base + (unsigned)(drive_idx * CDS_ENTRY_SIZE);
            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            bsoff = (unsigned int)(unsigned char)entry[0x4F]
                  | ((unsigned int)(unsigned char)entry[0x50] << 8);
            rb_write("CDS_BEFORE path=\"");
            rb_far_str(FP_SEG(entry), FP_OFF(entry), 16);
            rb_write("\" FL="); rb_hex16(flags);
            rb_write(" BS="); rb_hex16(bsoff); rb_write("\r\n");

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

            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            bsoff = (unsigned int)(unsigned char)entry[0x4F]
                  | ((unsigned int)(unsigned char)entry[0x50] << 8);
            rb_write("CDS_AFTER path=\"");
            rb_far_str(FP_SEG(entry), FP_OFF(entry), 16);
            rb_write("\" FL="); rb_hex16(flags);
            rb_write(" BS="); rb_hex16(bsoff); rb_write("\r\n");
        }
    }

    rb_write("INIT_CDS DONE\r\n");
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

    profile = (argc > 1) ? argv[1] : "default";

    if (!config_load("TNFSDRV.CFG", profile, &cfg))
        rb_write("TNFSDRV.CFG not found, using defaults\r\n");

    /* Validate protocol */
    if (strcmp(cfg.protocol, "UDP") != 0) {
        printf("TNFSDRV: protocol '%s' not supported (only UDP)\r\n", cfg.protocol);
        return 1;
    }

    /* Validate drive letter: C-Z only (A/B are floppy; anything outside A-Z is garbage) */
    if (cfg.driveletter < 'C' || cfg.driveletter > 'Z') {
        printf("TNFSDRV: invalid drive '%c:' — use C-Z\r\n", cfg.driveletter);
        return 1;
    }

    /* Show config before installing */
    printf("Profile:  %s\r\n",  cfg.profile);
    printf("Server:   %s\r\n",  cfg.servername[0] ? cfg.servername : "(none)");
    printf("Root:     %s\r\n",  cfg.serverroot);
    printf("Protocol: %s\r\n",  cfg.protocol);
    printf("Port:     %u\r\n",  cfg.port);
    printf("Drive:    %c:\r\n", cfg.driveletter);

    /* Network: connect to TNFS server and verify reachability */
    if (tnfsdrv_connect(&cfg) != 0) {
        return 1;
    }
    tnfsdrv_disconnect();   /* unhook mTCP timer before going TSR */

    /* Apply runtime drive letter to FS layer */
    fs_set_drive(cfg.driveletter);

    rb_write("TNFSDRV loaded OK\r\n");

    init_cds(&cfg);

    old_int2f = _dos_getvect(0x2F);
    init_int2f_ptr_();
    _dos_setvect(0x2F, (void (__interrupt __far *)())new_int2f_);

    paras = calc_resident_paras();
    printf("Virtual drive %c: installed\r\n", cfg.driveletter);
#ifdef TNFSDRV_DEBUG_RINGBUF
    seg = get_ds();
    off = (unsigned int)&rbuf;
    printf("Ring buffer at %04X:%04X\r\n", seg, off);
    printf("Run: DUMPBUF %04X:%04X\r\n", seg, off);
#endif
    printf("Staying resident (%u paragraphs).\r\n", paras);

    {
        static union REGS r;
        r.h.ah = 0x31;
        r.h.al = 0;
        r.w.dx = paras;
        int86(0x21, &r, &r);
    }
    return 0;
}
