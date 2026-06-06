#include <i86.h>
#include "ringbuf.h"

#ifdef TNFSDRV_DEBUG_RINGBUF

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
    if (!rbuf.enabled) return;
    if (ax_val != 0x1123 && ax_val != 0x1125) {
        rb_write("2F AX="); rb_hex16(ax_val); rb_write("\r\n");
        return;
    }
    rb_write("2F AX="); rb_hex16(ax_val);
    rb_write(" BX="); rb_hex16(bx_val);
    rb_write(" CX="); rb_hex16(cx_val);
    rb_write(" DX="); rb_hex16(dx_val);
    rb_write("\r\n");
    rb_write("   DS="); rb_hex16(ds_val);
    rb_write(" SI="); rb_hex16(si_val);
    rb_write(" ES="); rb_hex16(es_val);
    rb_write(" DI="); rb_hex16(di_val);
    rb_write("\r\n");
    if (ax_val == 0x1123) {
        rb_write("  SI\""); rb_far_str(ds_val, si_val, 48); rb_write("\"\r\n");
    }
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
