#include <i86.h>
#include <stddef.h>
#include "ringbuf.h"

#ifdef TNFSDRV_DEBUG_RINGBUF

extern char far *glob_sdaptr;  /* SDA pointer from redirector.c */

static const char *al_name(unsigned char al)
{
    switch (al) {
    case 0x00: return "INSTALL";
    case 0x01: return "RMDIR";
    case 0x02: return "MKDIR";
    case 0x03: return "SETCURDIR";
    case 0x05: return "CHDIR";
    case 0x06: return "CLOSE";
    case 0x07: return "COMMIT";
    case 0x08: return "READ";
    case 0x09: return "WRITE";
    case 0x0A: return "LOCK";
    case 0x0B: return "UNLOCK";
    case 0x0C: return "DISKSPACE";
    case 0x0D: return "SETATTR";
    case 0x0E: return "GETVOL";
    case 0x0F: return "GETATTR";
    case 0x10: return "DELETE";
    case 0x11: return "RENAME";
    case 0x12: return "FILEDATE";
    case 0x17: return "LSEEK";
    case 0x19: return "QUALIFY";
    case 0x1B: return "FINDFIRST";
    case 0x1C: return "FINDNEXT";
    case 0x1E: return "FILEIO";
    case 0x21: return "SEEKEND";
    case 0x22: return "GETDIR";
    case 0x2E: return "SPOPNFIL";
    default:   return NULL;
    }
}


RingBuf rbuf;

void rb_putc(char c)
{
    unsigned int next = (rbuf.head + 1) % RING_SIZE;
    if (next == rbuf.tail)
        rbuf.tail = (rbuf.tail + 1) % RING_SIZE;
    rbuf.data[rbuf.head] = c;
    rbuf.head = next;
}

void rb_write(const char *s)
{
    while (*s) rb_putc(*s++);
}

static const char hex_chars[] = "0123456789ABCDEF";

void rb_hex8(unsigned char v)
{
    rb_putc(hex_chars[(v >> 4) & 0xF]);
    rb_putc(hex_chars[v & 0xF]);
}

void rb_hex16(unsigned int v)
{
    rb_hex8((unsigned char)(v >> 8));
    rb_hex8((unsigned char)v);
}

void rb_dec(unsigned int v)
{
    char buf[6];
    int i = 0, j;
    if (v == 0) { rb_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = i - 1; j >= 0; j--) rb_putc(buf[j]);
}

void rb_far_str(unsigned int seg, unsigned int off, int maxlen)
{
    char far *ptr = (char far *)MK_FP(seg, off);
    int i;
    char c;
    for (i = 0; i < maxlen; i++) {
        c = ptr[i];
        if (c == '\0' || c == '\r') break;
        rb_putc((c >= 0x20 && c < 0x7F) ? c : '.');
    }
}

void __cdecl log_2f_call(unsigned int ax_val, unsigned int bx_val,
                          unsigned int cx_val, unsigned int dx_val,
                          unsigned int ds_val, unsigned int si_val,
                          unsigned int es_val, unsigned int di_val)
{
    unsigned char al;
    const char *name;
    (void)bx_val; (void)cx_val; (void)dx_val;
    (void)ds_val; (void)si_val; (void)es_val; (void)di_val;
    if (!rbuf.enabled) return;
    if ((ax_val & 0xFF00u) != 0x1100u) return;
    al = (unsigned char)ax_val;
    name = al_name(al);
    rb_write("2F?");
    if (name) rb_write(name);
    else { rb_write("AL="); rb_hex8(al); }
    if (glob_sdaptr) {
        char far *fn1 = glob_sdaptr + 0x9E;
        if (fn1[0]) { rb_write(" \""); rb_far_str(FP_SEG(fn1), FP_OFF(fn1), 64); rb_putc('"'); }
    }
    rb_write("\r\n");
}

#else /* release build */

RingBuf rbuf = { 0 };

void __cdecl log_2f_call(unsigned int ax_val, unsigned int bx_val,
                          unsigned int cx_val, unsigned int dx_val,
                          unsigned int ds_val, unsigned int si_val,
                          unsigned int es_val, unsigned int di_val)
{
    (void)ax_val; (void)bx_val; (void)cx_val; (void)dx_val;
    (void)ds_val; (void)si_val; (void)es_val; (void)di_val;
}

#endif /* TNFSDRV_DEBUG_RINGBUF */
