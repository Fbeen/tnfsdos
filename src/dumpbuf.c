/*
 * DUMPBUF.C  -  Dump a TSR ring buffer to D:\TNFS\DEBUG.TXT
 * Usage:  DUMPBUF SSSS:OOOO
 * Compile: wcc -bt=dos -ms -3 -d2 -s dumpbuf.c
 *
 * DUMPBUF disables TSR logging permanently for this DOS session.
 * Reboot/reload TSR to enable logging again.
 *
 * Order of operations:
 *   1. rb->enabled = 0   (stop TSR logging immediately)
 *   2. Snapshot head/tail/size
 *   3. Open D:\TNFS\DEBUG.TXT
 *   4. Dump snapshot (loop never re-reads rb->head)
 *   5. fclose
 *   6. Print "Written DEBUG.TXT" to screen
 */

#include <dos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Must match main.c exactly */
#define RING_MAGIC  0xBEEFu
#define RING_SIZE   16384

typedef struct {
    unsigned int  magic;
    unsigned int  head;
    unsigned int  tail;
    unsigned int  size;
    unsigned int  enabled;        /* 0 = logging paused */
    char          data[RING_SIZE];
} RingBuf;

static unsigned int parse_hex(const char *s)
{
    unsigned int val = 0;
    char c;
    while ((c = *s++) != '\0') {
        val <<= 4;
        if      (c >= '0' && c <= '9') val |= (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') val |= (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') val |= (unsigned)(c - 'a' + 10);
    }
    return val;
}

int main(int argc, char *argv[])
{
    unsigned int  seg, off;
    RingBuf far  *rb;
    char         *colon;
    char          arg[20];
    unsigned int  snap_head, snap_tail, snap_size;
    unsigned int  i, count;
    FILE         *f;

    if (argc < 2) {
        puts("Usage: DUMPBUF SSSS:OOOO");
        return 1;
    }

    strncpy(arg, argv[1], sizeof(arg) - 1);
    arg[sizeof(arg) - 1] = '\0';
    colon = strchr(arg, ':');
    if (colon != NULL) {
        *colon = '\0';
        seg = parse_hex(arg);
        off = parse_hex(colon + 1);
    } else {
        seg = parse_hex(arg);
        off = 0;
    }

    rb = (RingBuf far *)MK_FP(seg, off);

    if (rb->magic != RING_MAGIC) {
        puts("Bad magic - not a valid ring buffer");
        return 1;
    }

    /* 1. Disable TSR logging permanently for this session */
    rb->enabled = 0;

    /* 2. Snapshot - no more INT 21h calls reach the logger from here */
    snap_head = rb->head;
    snap_tail = rb->tail;
    snap_size = rb->size;
    if (snap_size == 0 || snap_size > RING_SIZE)
        snap_size = RING_SIZE;

    /* 3. Open output file */
    f = fopen("D:\\TNFS\\DEBUG.TXT", "w");
    if (f == NULL) {
        puts("Cannot create D:\\TNFS\\DEBUG.TXT");
        return 1;
    }

    /* 4. Dump snapshot - loop bound is snap_head, never rb->head */
    fprintf(f, "Ring buffer  %04X:%04X\r\n", seg, off);
    fprintf(f, "head=%u  tail=%u  size=%u\r\n", snap_head, snap_tail, snap_size);
    fprintf(f, "--- contents ---\r\n");

    i     = snap_tail;
    count = 0;
    while (i != snap_head && count < snap_size) {
        fputc(rb->data[i], f);
        i++;
        if (i >= snap_size) i = 0;
        count++;
    }

    fprintf(f, "\r\n--- end (%u bytes) ---\r\n", count);

    /* 5. Close */
    fclose(f);

    /* 6. Short confirmation on screen */
    puts("Written DEBUG.TXT");
    return 0;
}
