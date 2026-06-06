/*
 * REDIRECTOR.C  —  INT 2Fh AH=11h handler functions for TNFSDRV.
 *
 * Each function is called from handler.asm after it detects AH=11h and
 * dispatches on AL.  All filesystem access goes through the fs.h interface;
 * no FsEntry details leak into this file.
 *
 * SDA offsets used:
 *   +0x00C  curr_dta (far ptr, 4 bytes)
 *   +0x09E  fn1 (canonical path, ASCIIZ)
 *   +0x1B3  found_file (32 bytes)
 *   +0x22B  fcb_fn1[11]  (FCB template)
 *   +0x24D  srch_attr (byte)
 */

#include <i86.h>
#include "ringbuf.h"
#include "fs.h"
#include "redirector.h"

char far *glob_sdaptr;  /* written by init_cds; read by all handlers */

/* ------------------------------------------------------------------ */
/*  AL=05h  CHDIR                                                       */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_chdir(void)
{
    static FsNode node;
    char far *fn1;
    if (!glob_sdaptr) return 3;
    fn1 = glob_sdaptr + 0x9E;

    if (fs_is_root(fn1)) {
        if (rbuf.enabled) rb_write("2F 1105 CHDIR OK ROOT\r\n");
        return 0;
    }
    if (fs_resolve(fn1, &node) && fs_is_dir(&node)) {
        if (rbuf.enabled) { rb_write("2F 1105 CHDIR OK "); rb_write(fs_get_name(&node)); rb_write("\r\n"); }
        return 0;
    }
    if (rbuf.enabled) rb_write("2F 1105 CHDIR FAIL\r\n");
    return 3;
}

/* ------------------------------------------------------------------ */
/*  AL=1Bh  FINDFIRST                                                   */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_findfirst(void)
{
    char far *sda = glob_sdaptr;
    char far *fn1;
    char far *tmpl;
    char far *dta;
    char far *found;
    unsigned char srch_attr;
    unsigned int dta_off, dta_seg;
    static FsDirEnum de;
    static FsNode node;
    int rc, k;

    if (!sda) { if (rbuf.enabled) rb_write("2F 111B NO SDA\r\n"); return 0x12; }

    fn1       = sda + 0x9E;
    tmpl      = sda + 0x22B;
    srch_attr = (unsigned char)sda[0x24D];

    if (rbuf.enabled) {
        rb_write("2F 111B fn1=\""); rb_far_str(FP_SEG(fn1), FP_OFF(fn1), 32);
        rb_write("\" t=");
        for (k = 0; k < 11; k++) { char c = tmpl[k]; rb_putc(c >= 0x20 ? c : '.'); }
        rb_write("\r\n");
    }

    found = sda + 0x1B3;
    for (k = 0; k < 32; k++) found[k] = 0;

    if (srch_attr & 0x08u) {
        const char *lbl = VOLUME_LABEL;
        if (rbuf.enabled) rb_write("2F 111B VOLABEL OK\r\n");
        for (k = 0; k < 11; k++) found[k] = lbl[k];
        found[11] = 0x08;
        de.dir_ctx  = 0;
        de.next_idx = 0xFF;
    } else {
        rc = fs_enum_begin(fn1, tmpl, &de);
        if (rc == 0) {
            if (rbuf.enabled) rb_write("2F 111B UNKNOWN PATH\r\n");
            return 0x12;
        }
        if (rc < 0) {
            if (rbuf.enabled) rb_write("2F 111B INVALID\r\n");
            return 3;
        }
        if (!fs_enum_next(&de, &node)) {
            if (rbuf.enabled) rb_write("2F 111B NOMATCH\r\n");
            return 0x12;
        }
        if (rbuf.enabled) { rb_write("2F 111B FF "); rb_write(fs_get_name(&node)); rb_write("\r\n"); }
        fs_fill_found(&node, found);
    }

    dta_off = (unsigned int)(unsigned char)sda[0x0C]
            | ((unsigned int)(unsigned char)sda[0x0D] << 8);
    dta_seg = (unsigned int)(unsigned char)sda[0x0E]
            | ((unsigned int)(unsigned char)sda[0x0F] << 8);
    dta = (char far *)MK_FP(dta_seg, dta_off);

    dta[0] = (char)(g_drive_idx | 0x80);
    for (k = 0; k < 11; k++) dta[1+k] = tmpl[k];
    dta[12] = (char)srch_attr;
    dta[13] = (char)de.next_idx;
    dta[14] = (char)de.dir_ctx;
    dta[15] = 0; dta[16] = 0;
    dta[17] = 0; dta[18] = 0; dta[19] = 0; dta[20] = 0;
    for (k = 0; k < 32; k++) dta[0x15+k] = found[k];

    return 0;
}

/* ------------------------------------------------------------------ */
/*  AL=1Ch  FINDNEXT                                                    */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_findnext(unsigned int es_val, unsigned int di_val)
{
    char far *dta   = (char far *)MK_FP(es_val, di_val);
    char far *found = glob_sdaptr + 0x1B3;
    static FsDirEnum de;
    static FsNode node;
    int k;

    de.dir_ctx  = (int)(unsigned char)dta[14];
    de.next_idx = (int)(unsigned char)dta[13];
    for (k = 0; k < 11; k++) de.tmpl[k] = dta[1+k];

    if (!fs_enum_next(&de, &node)) {
        if (rbuf.enabled) rb_write("2F 111C EOF\r\n");
        return 0x12;
    }

    if (rbuf.enabled) { rb_write("2F 111C FINDNEXT "); rb_write(fs_get_name(&node)); rb_write("\r\n"); }

    for (k = 0; k < 32; k++) found[k] = 0;
    fs_fill_found(&node, found);

    dta[13] = (char)de.next_idx;

    for (k = 0; k < 32; k++) dta[0x15+k] = found[k];

    return 0;
}

/* ------------------------------------------------------------------ */
/*  AL=0Fh  GETATTR                                                     */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_getattr(void)
{
    static FsNode node;
    if (!glob_sdaptr) return 0xFFFF;
    if (rbuf.enabled) {
        rb_write("2F 110F \"");
        rb_far_str(FP_SEG(glob_sdaptr + 0x9E), FP_OFF(glob_sdaptr + 0x9E), 32);
        rb_write("\"\r\n");
    }
    if (!fs_resolve(glob_sdaptr + 0x9E, &node)) return 0xFFFF;
    return fs_get_attr(&node);
}

/* ------------------------------------------------------------------ */
/*  AL=16h  OPEN                                                        */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_open(unsigned int es_val, unsigned int di_val)
{
    static FsNode node;
    static FsHandle handle;
    if (!glob_sdaptr || !fs_resolve(glob_sdaptr + 0x9E, &node)) {
        if (rbuf.enabled) rb_write("2F 1116 OPEN NOTFOUND\r\n");
        return 2;
    }
    if (fs_is_dir(&node)) {
        if (rbuf.enabled) rb_write("2F 1116 OPEN DENIED\r\n");
        return 5;
    }
    if (rbuf.enabled) rb_write("2F 1116 OPEN OK\r\n");
    fs_open(&node, &handle);
    sft_fill_handle(&handle, (char far *)MK_FP(es_val, di_val));
    return 0;
}

/* ------------------------------------------------------------------ */
/*  AL=2Eh  SPOPNFIL (Special Open — used by TYPE/COPY in DOS 5+/6.x)  */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_spopen(unsigned int es_val, unsigned int di_val)
{
    static FsNode node;
    static FsHandle handle;
    if (!glob_sdaptr || !fs_resolve(glob_sdaptr + 0x9E, &node)) {
        if (rbuf.enabled) rb_write("2F 112E SPOP NOTFOUND\r\n");
        return 2;
    }
    if (fs_is_dir(&node)) {
        if (rbuf.enabled) rb_write("2F 112E SPOP DENIED\r\n");
        return 5;
    }
    if (rbuf.enabled) rb_write("2F 112E SPOP OK\r\n");
    fs_open(&node, &handle);
    sft_fill_handle(&handle, (char far *)MK_FP(es_val, di_val));
    return 0;
}

/* ------------------------------------------------------------------ */
/*  AL=08h  READ                                                        */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_read(unsigned int es_val, unsigned int di_val,
                              unsigned int cx_val)
{
    char far *sft = (char far *)MK_FP(es_val, di_val);
    char far *sda = glob_sdaptr;
    char far *buf;
    unsigned int dta_off, dta_seg;
    unsigned long pos;
    unsigned int nbytes;
    static FsHandle handle;

    handle.dir_ctx = (int)(unsigned char)sft[0x0B];
    handle.idx     = (int)(unsigned char)sft[0x0C];

    dta_off = (unsigned int)(unsigned char)sda[0x0C]
            | ((unsigned int)(unsigned char)sda[0x0D] << 8);
    dta_seg = (unsigned int)(unsigned char)sda[0x0E]
            | ((unsigned int)(unsigned char)sda[0x0F] << 8);
    buf = (char far *)MK_FP(dta_seg, dta_off);

    pos = (unsigned long)(unsigned char)sft[0x15]
        | ((unsigned long)(unsigned char)sft[0x16] << 8)
        | ((unsigned long)(unsigned char)sft[0x17] << 16)
        | ((unsigned long)(unsigned char)sft[0x18] << 24);

    nbytes = fs_read(&handle, pos, buf, cx_val);
    if (nbytes == 0) {
        if (rbuf.enabled) rb_write("2F 1108 EOF\r\n");
        return 0;
    }

    pos += nbytes;
    sft[0x15] = (char)pos;
    sft[0x16] = (char)(pos >> 8);
    sft[0x17] = (char)(pos >> 16);
    sft[0x18] = (char)(pos >> 24);

    if (rbuf.enabled) {
        rb_write("2F 1108 READ cnt="); rb_hex16(nbytes);
        rb_write(" DTA="); rb_hex16(dta_seg); rb_putc(':'); rb_hex16(dta_off);
        rb_write("\r\n");
    }
    return nbytes;
}

/* ------------------------------------------------------------------ */
/*  AL=06h  CLOSE  /  AL=0Ch  DISKSPACE                                 */
/* ------------------------------------------------------------------ */

void __cdecl do_close(void)
{
    if (rbuf.enabled) rb_write("2F 1106 CLOSE\r\n");
}

void __cdecl do_diskspace(void)
{
    if (rbuf.enabled) rb_write("2F 110C DISKSPACE 16MB\r\n");
}
