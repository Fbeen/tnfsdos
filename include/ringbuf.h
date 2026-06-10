#ifndef RINGBUF_H
#define RINGBUF_H

#include <i86.h>

#ifdef TNFSDRV_DEBUG_RINGBUF

#define RING_MAGIC  0xBEEFu
#define RING_SIZE   32768

typedef struct {
    unsigned int  magic;
    unsigned int  head;
    unsigned int  tail;
    unsigned int  size;
    unsigned int  enabled;
    char          data[RING_SIZE];
} RingBuf;

extern RingBuf rbuf;

void rb_putc(char c);
void rb_write(const char *s);
void rb_hex8(unsigned char v);
void rb_hex16(unsigned int v);
void rb_dec(unsigned int v);
void rb_far_str(unsigned int seg, unsigned int off, int maxlen);

#else /* release build — no ring buffer data[] */

typedef struct {
    unsigned int  enabled;  /* always 0; rbuf.enabled checks still compile */
} RingBuf;

extern RingBuf rbuf;

#define rb_putc(c)          ((void)(c))
#define rb_write(s)         ((void)(s))
#define rb_hex8(v)          ((void)(v))
#define rb_hex16(v)         ((void)(v))
#define rb_dec(v)           ((void)(v))
#define rb_far_str(s,o,m)   ((void)(s))

#endif /* TNFSDRV_DEBUG_RINGBUF */

/* log_2f_call is always a real function: handler.asm calls it by address. */
void __cdecl log_2f_call(unsigned int ax_val, unsigned int bx_val,
                          unsigned int cx_val, unsigned int dx_val,
                          unsigned int ds_val, unsigned int si_val,
                          unsigned int es_val, unsigned int di_val);

#endif /* RINGBUF_H */
