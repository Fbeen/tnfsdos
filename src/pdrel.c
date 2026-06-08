/*
 * PDREL.EXE — Release a packet driver handle.
 *
 * Usage:  PDREL <int_hex> <handle>
 * Example: PDREL 60 1
 *
 * Find the handle number in log.txt after TNFSDRV loads:
 *   AT: INT=60h ET=0800 ... h=1
 *
 * Packet driver function 3 (release_type): AH=3, BX=handle.
 * CF=0 on success, CF=1 + DX=error on failure.
 */

#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    unsigned int pd_int, handle;
    union REGS r;
    struct SREGS sr;

    if (argc < 3) {
        puts("Usage: PDREL <int_hex> <handle>");
        puts("  int_hex : packet driver interrupt (hex), e.g. 60");
        puts("  handle  : handle number from log.txt, e.g. 1");
        return 1;
    }

    pd_int = parse_hex(argv[1]);
    handle = (unsigned int)atoi(argv[2]);

    printf("INT %02Xh  release_type handle=%u\r\n", pd_int, handle);

    memset(&r,  0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.h.ah = 3;
    r.x.bx = handle;
    int86x((int)pd_int, &r, &r, &sr);

    if (r.x.cflag) {
        printf("FAIL: CF=1 AX=%04X DX=%04X\r\n", r.x.ax, r.x.dx);
        return 1;
    }

    printf("OK: handle %u released\r\n", handle);
    return 0;
}
