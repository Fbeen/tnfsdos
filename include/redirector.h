#ifndef REDIRECTOR_H
#define REDIRECTOR_H

#include <i86.h>

#define VOLUME_LABEL  "TNFSDOS    "   /* 11-byte FCB-padded volume label */

/* SDA pointer — written by init_cds (main), read by all handlers. */
extern char far *glob_sdaptr;

/* CDS entry pointer for our drive — written by init_cds, updated by do_chdir. */
extern char far *g_cds_entry_ptr;

/* QUALIFY response buffer — handler.asm returns ES:DI pointing here */
extern char s_qualify_buf[];

/* INT 2Fh AH=11h handler functions (called from handler.asm) */
unsigned int __cdecl do_qualify  (unsigned int es_val, unsigned int di_val);
unsigned int __cdecl do_setcurdir(void);
unsigned int __cdecl do_rmdir    (void);
unsigned int __cdecl do_mkdir    (void);
unsigned int __cdecl do_chdir    (void);
unsigned int __cdecl do_findfirst(void);
unsigned int __cdecl do_findnext (unsigned int es_val, unsigned int di_val);
unsigned int __cdecl do_getattr  (void);
unsigned int __cdecl do_open     (unsigned int es_val, unsigned int di_val);
unsigned int __cdecl do_spopen   (unsigned int es_val, unsigned int di_val);
unsigned int __cdecl do_read     (unsigned int es_val, unsigned int di_val,
                                   unsigned int cx_val);
void         __cdecl do_close       (unsigned int es_val, unsigned int di_val);
void         __cdecl do_diskspace   (void);
unsigned int __cdecl do_rename       (void);
unsigned int __cdecl do_setattr     (unsigned int cx_val);
unsigned int __cdecl do_delete      (void);
unsigned int __cdecl do_exec_notify (void);
unsigned int __cdecl do_write       (unsigned int es_val, unsigned int di_val,
                                     unsigned int cx_val);
unsigned int __cdecl do_create      (unsigned int es_val, unsigned int di_val);
unsigned long __cdecl do_seekend    (unsigned int es_val, unsigned int di_val,
                                     unsigned int cx_val, unsigned int bx_val);

#endif
