/*
 * FINDTEST.C - Direct AH=4Eh/4Fh test for C:\TNFS
 *
 * Calls INT 21h FindFirst/FindNext directly, bypassing COMMAND.COM.
 * Run AFTER loading the TSR.  Output goes to stdout (CON).
 *
 * Compile: wcc -bt=dos -ms -3 -s findtest.c
 */

#include <dos.h>
#include <stdio.h>

/*
 * Handle-based FindFirst/FindNext DTA (INT 21h AH=4Eh/4Fh).
 *
 * Offset  Size  Field
 *  +00     21   reserved (DOS internal search state)
 *  +15      1   attribute of found entry
 *  +16      2   time
 *  +18      2   date
 *  +1A      4   file size (32-bit LE)
 *  +1E     13   name (NUL-terminated, uppercase 8.3)
 */
typedef struct {
    char           reserved[21];
    unsigned char  attrib;
    unsigned short ftime;
    unsigned short fdate;
    unsigned long  fsize;
    char           name[13];
} FindDTA;

static FindDTA    dta;
static char       path[] = "C:\\TNFS\\*.*";

int main(void)
{
    union REGS r;
    int n = 0;

    puts("FINDTEST: direct INT 21h AH=4Eh/4Fh");
    printf("Path: %s\r\n", path);
    puts("-------------------------------");

    /* AH=1Ah: point DTA at our local struct */
    r.h.ah = 0x1A;
    r.w.dx = (unsigned)&dta;
    intdos(&r, &r);

    /* AH=4Eh: FindFirst, CX=0x10 includes subdirectories */
    r.h.ah = 0x4E;
    r.w.cx = 0x10;
    r.w.dx = (unsigned)path;
    intdos(&r, &r);

    if (r.w.cflag) {
        printf("FindFirst FAILED  AX=%04X\r\n", r.w.ax);
        return 1;
    }

    /* Print entries; AH=4Fh fetches each subsequent one */
    for (;;) {
        printf("[%d] attrib=%02X  size=%lu  name=%s\r\n",
               n++,
               (unsigned)dta.attrib,
               dta.fsize,
               dta.name);

        r.h.ah = 0x4F;
        intdos(&r, &r);
        if (r.w.cflag) break;
    }

    puts("-------------------------------");
    printf("Total entries: %d\r\n", n);
    return 0;
}
