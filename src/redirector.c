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
#include <stdint.h>
#include "ringbuf.h"
#include "fs.h"
#include "redirector.h"

char far *glob_sdaptr;      /* written by init_cds; read by all handlers */
char far *g_cds_entry_ptr;  /* written by init_cds; updated by do_chdir */

/* ------------------------------------------------------------------ */
/*  AL=01h  RMDIR  /  AL=02h  MKDIR  /  AL=03h  SETCURDIR              */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_setcurdir(void)
{
    static FsNode node;
    unsigned char c;
    char far *fn1;
    int rc;

    if (!glob_sdaptr) return 0xFFFFu;
    c = (unsigned char)(glob_sdaptr[0x9E]);
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c != (unsigned char)('A' + g_drive_idx)) return 0xFFFFu;

    fn1 = glob_sdaptr + 0x9E;

    if (fs_is_root(fn1)) {
        if (rbuf.enabled) rb_write("2F 1103 SETCURDIR ROOT\r\n");
        return 0;
    }
    if (fs_resolve(fn1, &node) && fs_is_dir(&node)) {
        /* Directory already exists — mkdir must fail (INT 21h AH=39h semantics) */
        if (rbuf.enabled) { rb_write("2F 1103 SETCURDIR EXIST "); rb_write(fs_get_name(&node)); rb_write("\r\n"); }
        return 5;
    }
    /* Path doesn't exist — try to create it (DOS uses SETCURDIR for mkdir) */
    rc = fs_mkdir(fn1);
    if (rc == 0) {
        if (rbuf.enabled) rb_write("2F 1103 SETCURDIR MKDIR OK\r\n");
        return 0;
    }
    if (rbuf.enabled) rb_write("2F 1103 SETCURDIR MKDIR FAIL\r\n");
    return 3;
}

unsigned int __cdecl do_rmdir(void)
{
    char far *fn1;
    int rc;
    if (!glob_sdaptr) return 3;
    fn1 = glob_sdaptr + 0x9E;
    rc = fs_rmdir(fn1);
    if (rbuf.enabled) { rb_write("2F 1101 RMDIR "); rb_write(rc == 0 ? "OK" : "FAIL"); rb_write("\r\n"); }
    if (rc == 0) return 0;
    return (rc == 0x17) ? 16 : 5;  /* ENOTEMPTY→16, else access denied */
}

unsigned int __cdecl do_mkdir(void)
{
    char far *fn1;
    int rc;
    if (!glob_sdaptr) return 3;
    fn1 = glob_sdaptr + 0x9E;
    rc = fs_mkdir(fn1);
    if (rbuf.enabled) { rb_write("2F 1102 MKDIR "); rb_write(rc == 0 ? "OK" : "FAIL"); rb_write("\r\n"); }
    if (rc == 0) return 0;
    return (rc == 0x02) ? 3 : 5;  /* ENOENT→path not found, else access denied */
}

/* ------------------------------------------------------------------ */
/*  AL=05h  CHDIR                                                       */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_chdir(void)
{
    static FsNode node;
    char far *fn1;
    int k;
    if (!glob_sdaptr) return 3;
    fn1 = glob_sdaptr + 0x9E;

    if (fs_is_root(fn1)) {
        if (g_cds_entry_ptr) {
            g_cds_entry_ptr[0] = fn1[0];
            g_cds_entry_ptr[1] = ':';
            g_cds_entry_ptr[2] = '\\';
            g_cds_entry_ptr[3] = '\0';
        }
        if (rbuf.enabled) rb_write("2F 1105 CHDIR OK ROOT\r\n");
        return 0;
    }
    if (fs_resolve(fn1, &node) && fs_is_dir(&node)) {
        if (g_cds_entry_ptr) {
            for (k = 0; k < 66 && fn1[k]; k++)
                g_cds_entry_ptr[k] = fn1[k];
            g_cds_entry_ptr[k] = '\0';
        }
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
/*  AL=23h  QUALIFY FILENAME                                            */
/*  Copy the canonical path (fn1) from SDA to ES:DI buffer.            */
/*  Returning CF=0 lets DOS propagate CX from AL=0Fh back to the app.  */
/* ------------------------------------------------------------------ */

char s_qualify_buf[128];  /* public: handler.asm returns ES:DI -> here */

unsigned int __cdecl do_qualify(unsigned int es_val, unsigned int di_val)
{
    char far *fn1 = glob_sdaptr + 0x9E;
    unsigned char c;
    int i;
    if (!glob_sdaptr) return 0xFFFF;
    c = (unsigned char)fn1[0];
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c != (unsigned char)('A' + g_drive_idx)) return 0xFFFF;
    /* Copy fn1 to our own buffer; caller's ES:DI on entry may be CWD only */
    for (i = 0; i < 127; i++) {
        s_qualify_buf[i] = fn1[i];
        if (fn1[i] == '\0') break;
    }
    s_qualify_buf[127] = '\0';
    if (rbuf.enabled) {
        rb_write("1123 edi=");
        rb_far_str(es_val, di_val, 40);
        rb_write(" fn1=");
        rb_far_str(FP_SEG(fn1), FP_OFF(fn1), 40);
        rb_write("\r\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  AL=0Eh  SET FILE ATTRIBUTES  (DOS 6.x; CX = new attr byte)         */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_setattr(unsigned int cx_val)
{
    char far *fn1;
    int rc;
    if (!glob_sdaptr) return 5;
    fn1 = glob_sdaptr + 0x9E;
    if (rbuf.enabled) {
        rb_write("ATTR SET dos="); rb_hex8((unsigned char)cx_val);
        rb_write(" "); rb_far_str(FP_SEG(fn1), FP_OFF(fn1), 40);
        rb_write("\r\n");
    }
    rc = fs_setattr(fn1, (unsigned char)(cx_val & 0xFF));
    if (rc == 0) return 0;
    return (rc == -2) ? 2 : 5;
}

/* ------------------------------------------------------------------ */
/*  AL=0Fh  GETATTR                                                     */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_getattr(void)
{
    static unsigned char attr;
    char far *fn1;
    if (!glob_sdaptr) return 0xFFFF;
    fn1 = glob_sdaptr + 0x9E;
    if (rbuf.enabled) { rb_write("ATTR GET raw="); rb_far_str(FP_SEG(fn1), FP_OFF(fn1), 40); rb_write("\r\n"); }
    if (!fs_getattr_stat(fn1, &attr)) return 0xFFFF;
    if (rbuf.enabled) { rb_write("ATTR RET dos="); rb_hex8(attr); rb_write("\r\n"); }
    return (unsigned int)attr;
}

/* ------------------------------------------------------------------ */
/*  AL=16h  OPEN                                                        */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_open(unsigned int es_val, unsigned int di_val)
{
    static FsNode node;
    static FsHandle handle;
    char far *sft = (char far *)MK_FP(es_val, di_val);
    unsigned char dos_mode;
    if (!glob_sdaptr || !fs_resolve(glob_sdaptr + 0x9E, &node)) {
        if (rbuf.enabled) rb_write("2F 1116 OPEN NOTFOUND\r\n");
        return 2;
    }
    if (fs_is_dir(&node)) {
        if (rbuf.enabled) rb_write("2F 1116 OPEN DENIED\r\n");
        return 5;
    }
    dos_mode = (unsigned char)sft[0x02] & 0x07;
    if (dos_mode != 0 && (fs_get_attr(&node) & 0x01)) {
        if (rbuf.enabled) rb_write("2F 1116 OPEN RDONLY\r\n");
        return 5;  /* access denied — file is read-only */
    }
    if (rbuf.enabled) { rb_write("2F 1116 OPEN OK mode="); rb_hex8(dos_mode); rb_write("\r\n"); }
    fs_open(&node, &handle, dos_mode);
    sft_fill_handle(&handle, &node, sft);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  AL=2Eh  SPOPNFIL (Special Open — open existing OR create if new)   */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_spopen(unsigned int es_val, unsigned int di_val)
{
    static FsNode node;
    static FsHandle handle;
    char far *sft = (char far *)MK_FP(es_val, di_val);
    char far *fn1;
    unsigned char dos_mode;
    int rc;

    if (!glob_sdaptr) return 5;
    fn1 = glob_sdaptr + 0x9E;

    if (fs_resolve(fn1, &node)) {
        if (fs_is_dir(&node)) {
            if (rbuf.enabled) rb_write("2F 112E SPOP DENIED\r\n");
            return 5;
        }
        dos_mode = (unsigned char)sft[0x02] & 0x07;
        if (rbuf.enabled) { rb_write("2F 112E SPOP EXIST mode="); rb_hex8(dos_mode); rb_write("\r\n"); }
        fs_open(&node, &handle, dos_mode);
        sft_fill_handle(&handle, &node, sft);
        return 0;
    }

    /* File doesn't exist — create it (SPOPNFIL semantics: open OR create) */
    if (rbuf.enabled) rb_write("2F 112E SPOP CREATE\r\n");
    rc = fs_create_and_open(fn1, sft);
    if (rc == 0) return 0;
    if (rbuf.enabled) rb_write("2F 112E SPOP FAIL\r\n");
    return (unsigned int)rc;
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

    handle.dir_ctx = 0;
    handle.idx     = 0;
    handle.tnfs_fd = (uint8_t)sft[0x07];

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
        rb_write("2F 1108 READ req="); rb_hex16(cx_val);
        rb_write(" got="); rb_hex16(nbytes);
        rb_write(" buf="); rb_hex16(dta_seg); rb_putc(':'); rb_hex16(dta_off);
        rb_write("\r\n");
    }
    return nbytes;
}

/* ------------------------------------------------------------------ */
/*  AL=06h  CLOSE  /  AL=0Ch  DISKSPACE                                 */
/* ------------------------------------------------------------------ */

void __cdecl do_close(unsigned int es_val, unsigned int di_val)
{
    char far *sft = (char far *)MK_FP(es_val, di_val);
    static FsHandle handle;
    handle.dir_ctx = 0;
    handle.idx     = 0;
    handle.tnfs_fd = (uint8_t)sft[0x07];
    fs_close(&handle);
}

void __cdecl do_diskspace(void) { }

/* ------------------------------------------------------------------ */
/*  AL=11h  RENAME  (fn1=SDA+0x9E old name, fn2=SDA+0x11D new name)   */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_rename(void)
{
    char far *fn1;
    char far *fn2;
    int rc;
    if (!glob_sdaptr) return 5;
    fn1 = glob_sdaptr + 0x9E;
    fn2 = glob_sdaptr + 0x11E;  /* fn1 buf = 128 bytes (0x9E+0x80), fn2 follows */
    if (rbuf.enabled) {
        rb_write("2F 1111 REN ");
        rb_far_str(FP_SEG(fn1), FP_OFF(fn1), 40);
        rb_write(" -> ");
        rb_far_str(FP_SEG(fn2), FP_OFF(fn2), 40);
        rb_write("\r\n");
    }
    rc = fs_rename(fn1, fn2);
    if (rc == 0) return 0;
    return (rc == -2) ? 2 : 5;
}

/* ------------------------------------------------------------------ */
/*  AL=10h  DELETE                                                      */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_delete(void)
{
    char far *fn1;
    int rc;
    if (!glob_sdaptr) return 2;
    fn1 = glob_sdaptr + 0x9E;
    rc = fs_delete(fn1);
    if (rbuf.enabled) { rb_write("2F 1113 DEL "); rb_write(rc == 0 ? "OK" : "FAIL"); rb_write("\r\n"); }
    if (rc == 0) return 0;
    return (rc == -2) ? 2 : 5;  /* -ENOENT→file not found, else access denied */
}

/* ------------------------------------------------------------------ */
/*  AL=25h  post-EXEC notification (no output buffer)                  */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_exec_notify(void)
{
    unsigned char c;
    if (!glob_sdaptr) return 0xFFFFu;
    c = (unsigned char)(glob_sdaptr[0x9E]);
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c != (unsigned char)('A' + g_drive_idx)) return 0xFFFFu;
    if (rbuf.enabled) rb_write("2F 1125 OK\r\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  AL=09h  WRITE                                                       */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_write(unsigned int es_val, unsigned int di_val,
                               unsigned int cx_val)
{
    char far *sft = (char far *)MK_FP(es_val, di_val);
    char far *sda = glob_sdaptr;
    char far *buf;
    unsigned int dta_off, dta_seg;
    unsigned long pos, new_pos, fsize;
    unsigned int nbytes;
    static FsHandle handle;

    handle.dir_ctx = 0;
    handle.idx     = 0;
    handle.tnfs_fd = (uint8_t)sft[0x07];

    dta_off = (unsigned int)(unsigned char)sda[0x0C]
            | ((unsigned int)(unsigned char)sda[0x0D] << 8);
    dta_seg = (unsigned int)(unsigned char)sda[0x0E]
            | ((unsigned int)(unsigned char)sda[0x0F] << 8);
    buf = (char far *)MK_FP(dta_seg, dta_off);

    pos = (unsigned long)(unsigned char)sft[0x15]
        | ((unsigned long)(unsigned char)sft[0x16] << 8)
        | ((unsigned long)(unsigned char)sft[0x17] << 16)
        | ((unsigned long)(unsigned char)sft[0x18] << 24);

    nbytes = fs_write(&handle, pos, buf, cx_val);

    new_pos = pos + nbytes;
    sft[0x15] = (char)new_pos;
    sft[0x16] = (char)(new_pos >> 8);
    sft[0x17] = (char)(new_pos >> 16);
    sft[0x18] = (char)(new_pos >> 24);

    fsize = (unsigned long)(unsigned char)sft[0x11]
          | ((unsigned long)(unsigned char)sft[0x12] << 8)
          | ((unsigned long)(unsigned char)sft[0x13] << 16)
          | ((unsigned long)(unsigned char)sft[0x14] << 24);
    if (new_pos > fsize) {
        sft[0x11] = (char)new_pos;
        sft[0x12] = (char)(new_pos >> 8);
        sft[0x13] = (char)(new_pos >> 16);
        sft[0x14] = (char)(new_pos >> 24);
    }

    if (rbuf.enabled) {
        rb_write("2F 1109 WRITE req="); rb_hex16(cx_val);
        rb_write(" got="); rb_hex16(nbytes);
        rb_write("\r\n");
    }
    return nbytes;
}

/* ------------------------------------------------------------------ */
/*  AL=17h  CREATE/OPEN (AH=3Ch create/truncate, AH=5Bh create-new)   */
/* ------------------------------------------------------------------ */

unsigned int __cdecl do_create(unsigned int es_val, unsigned int di_val)
{
    char far *fn1;
    int rc;
    if (!glob_sdaptr) return 5;
    fn1 = glob_sdaptr + 0x9E;
    if (rbuf.enabled) { rb_write("2F 1117 CREATE "); rb_far_str(FP_SEG(fn1), FP_OFF(fn1), 40); rb_write("\r\n"); }
    rc = fs_create_and_open(fn1, (char far *)MK_FP(es_val, di_val));
    if (rc == 0) return 0;
    if (rbuf.enabled) rb_write("2F 1117 FAIL\r\n");
    return (unsigned int)rc;
}

/* ------------------------------------------------------------------ */
/*  AL=21h  SEEKEND  (BX:CX = signed offset from end)                  */
/* ------------------------------------------------------------------ */

unsigned long __cdecl do_seekend(unsigned int es_val, unsigned int di_val,
                                  unsigned int cx_val, unsigned int bx_val)
{
    char far *sft;
    unsigned long fsize, offset, new_pos;
    unsigned char fd_byte, mode_byte;

    if (rbuf.enabled) {
        rb_write("2F 1121 SEEKEND SFT=");
        rb_hex16(es_val); rb_putc(':'); rb_hex16(di_val);
        rb_write(" BX="); rb_hex16(bx_val);
        rb_write(" CX="); rb_hex16(cx_val);
        rb_write("\r\n");
    }

    sft = (char far *)MK_FP(es_val, di_val);
    mode_byte = (unsigned char)sft[0x02];
    fd_byte   = (unsigned char)sft[0x07];

    fsize = (unsigned long)(unsigned char)sft[0x11]
          | ((unsigned long)(unsigned char)sft[0x12] << 8)
          | ((unsigned long)(unsigned char)sft[0x13] << 16)
          | ((unsigned long)(unsigned char)sft[0x14] << 24);

    offset  = (unsigned long)cx_val | ((unsigned long)bx_val << 16);
    new_pos = fsize + offset;

    sft[0x15] = (char)new_pos;
    sft[0x16] = (char)(new_pos >> 8);
    sft[0x17] = (char)(new_pos >> 16);
    sft[0x18] = (char)(new_pos >> 24);

    if (rbuf.enabled) {
        rb_write("2F 1121 SEEKEND mode="); rb_hex8(mode_byte);
        rb_write(" fd="); rb_hex8(fd_byte);
        rb_write(" sz=");
        rb_hex16((unsigned int)(fsize >> 16)); rb_hex16((unsigned int)fsize);
        rb_write(" new=");
        rb_hex16((unsigned int)(new_pos >> 16)); rb_hex16((unsigned int)new_pos);
        rb_write("\r\n");
    }
    return new_pos;
}
