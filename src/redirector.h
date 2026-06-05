#ifndef REDIRECTOR_H
#define REDIRECTOR_H

#include <i86.h>

#define VOLUME_LABEL  "TNFSDOS    "   /* 11-byte FCB-padded volume label */

/* SDA pointer — written by init_cds (dosutil), read by all handlers. */
extern char far *glob_sdaptr;

/* INT 2Fh AH=11h handler functions (called from handler.asm) */
unsigned int __cdecl do_chdir    (void);
unsigned int __cdecl do_findfirst(void);
unsigned int __cdecl do_findnext (unsigned int es_val, unsigned int di_val);
unsigned int __cdecl do_getattr  (void);
unsigned int __cdecl do_open     (unsigned int es_val, unsigned int di_val);
unsigned int __cdecl do_spopen   (unsigned int es_val, unsigned int di_val);
unsigned int __cdecl do_read     (unsigned int es_val, unsigned int di_val,
                                   unsigned int cx_val);
void         __cdecl do_close    (void);
void         __cdecl do_diskspace(void);

#endif
