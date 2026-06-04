/*
 * TSR.C  -  TNFSDRV: virtual drive T: via MS-DOS INT 2Fh network redirector
 *
 * Presents drive T: to DOS by:
 *   1. Marking CDS entry 19 as a network+physical drive (flags = 0xC000).
 *   2. Handling INT 2Fh AH=11h redirector callbacks:
 *        AL=05h  CHDIR      — always succeed
 *        AL=1Bh  FINDFIRST  — return fake directory listing
 *        AL=1Ch  FINDNEXT   — return next fake entry or EOF
 *
 * Also contains a LEGACY INT 21h C:\TNFS path-hook (AH=4Eh/4Fh/43h)
 * that pre-dates the redirector approach.  It is still compiled and active
 * as a diagnostic reference; see the LEGACY section below.
 *
 * Compile: wcc -bt=dos -ms -3 -d2 -s -zu tsr.c
 * See README.md for full build and test instructions.
 */

#include <dos.h>
#include <i86.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Configuration  -  change TNFS_BASE to match the real directory     */
/*  that exists on the DOS machine.  Rebuild after changing.           */
/* ------------------------------------------------------------------ */
#define TNFS_BASE   "C:\\TNFS"

/* ------------------------------------------------------------------ */
/*  Ring buffer  (no DOS calls - safe from ISR context)                */
/* ------------------------------------------------------------------ */

#define RING_MAGIC  0xBEEFu
#define RING_SIZE   4096

typedef struct {
    unsigned int  magic;
    unsigned int  head;
    unsigned int  tail;
    unsigned int  size;
    unsigned int  enabled;        /* 0 = logging paused (SHOWBUF running) */
    char          data[RING_SIZE];
} RingBuf;

static RingBuf rbuf;

static void rb_putc(char c)
{
    unsigned int next = (rbuf.head + 1) % RING_SIZE;
    if (next == rbuf.tail)
        rbuf.tail = (rbuf.tail + 1) % RING_SIZE;   /* drop oldest */
    rbuf.data[rbuf.head] = c;
    rbuf.head = next;
}

static void rb_write(const char *s)
{
    while (*s) rb_putc(*s++);
}

static const char hex_chars[] = "0123456789ABCDEF";

static void rb_hex8(unsigned char v)
{
    rb_putc(hex_chars[(v >> 4) & 0xF]);
    rb_putc(hex_chars[v & 0xF]);
}

static void rb_hex16(unsigned int v)
{
    rb_hex8((unsigned char)(v >> 8));
    rb_hex8((unsigned char)v);
}

static void rb_dec(unsigned int v)
{
    char buf[6];
    int i = 0, j;
    if (v == 0) { rb_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    for (j = i - 1; j >= 0; j--) rb_putc(buf[j]);
}

static void rb_far_str(unsigned int seg, unsigned int off, int maxlen)
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

/* ------------------------------------------------------------------ */
/*  Virtual directory entries for TNFS_BASE root                       */
/* ------------------------------------------------------------------ */

/* CX low byte saved from last AH=4E call; written into DTA[0].       */
unsigned char last_search_attr = 0x10;

/*
 * fill_dta_new: fill the DTA at ES:BX for AH=4Eh/4Fh FindFirst/Next.
 *
 * DTA layout (43 bytes):
 *   +00      search attribute  (last_search_attr from caller's CX)
 *   +01..+0B search template   ('?' wildcards)
 *   +0C..+14 DOS internal state (left as 0)
 *   +15      attribute of found entry
 *   +16..+17 time
 *   +18..+19 date
 *   +1A..+1D file size (32-bit LE)
 *   +1E..+2A filename (NUL-terminated, up to 12 chars + NUL)
 *
 * entry=0: GAMES (directory)
 * entry=1: README.TXT (file, 123 bytes)
 */
void __cdecl fill_dta_new(unsigned int es_val, unsigned int bx_val,
                           unsigned int entry)
{
    char far *d = (char far *)MK_FP(es_val, bx_val);
    static const char n_games[]  = "GAMES";
    static const char n_readme[] = "README.TXT";
    const char *name;
    int i;

    for (i = 0; i < 43; i++) d[i] = 0;

    d[0]  = last_search_attr;
    for (i = 1; i <= 11; i++) d[i] = '?';   /* wildcard template */

    if (entry == 0) {
        d[21] = 0x10;                        /* attribute: directory */
        name = n_games;
    } else {
        d[21] = 0x20;                        /* attribute: archive   */
        d[26] = 123;                         /* size low byte        */
        name = n_readme;
    }
    for (i = 0; i < 12 && name[i]; i++) d[30 + i] = name[i];
}

/* ------------------------------------------------------------------ */
/*  Path matching                                                       */
/* ------------------------------------------------------------------ */

static const char tnfs_base[] = TNFS_BASE;
extern unsigned char in_tnfs_dir;   /* defined below; needed by classify_4e_content */

/*
 * path_match_base: return 1 if p begins with TNFS_BASE followed by
 * '\' or NUL.  Comparison is case-insensitive throughout.
 */
static int path_match_base(const char far *p)
{
    int i;
    char bc, pc;
    for (i = 0; tnfs_base[i]; i++) {
        bc = tnfs_base[i];
        pc = p[i];
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (bc >= 'a' && bc <= 'z') bc -= 32;
        if (bc != pc) return 0;
    }
    return (p[i] == '\0' || p[i] == '\\');
}

/* match_base: exported wrapper for handler.asm */
int __cdecl match_base(unsigned int ds_val, unsigned int dx_val)
{
    return path_match_base((char far *)MK_FP(ds_val, dx_val));
}

/*
 * match_tnfs_tail: return 1 if path is the bare last component of TNFS_BASE,
 * optionally preceded by ".\".
 * e.g. TNFS_BASE="C:\TNFS" → matches "TNFS", "tnfs", ".\TNFS", ".\\tnfs".
 */
int __cdecl match_tnfs_tail(unsigned int ds_val, unsigned int dx_val)
{
    const char far *p = (const char far *)MK_FP(ds_val, dx_val);
    const char *tail;
    int i;
    char pc, tc;

    /* find last component of TNFS_BASE (text after final '\') */
    tail = tnfs_base;
    for (i = 0; tnfs_base[i]; i++)
        if (tnfs_base[i] == '\\') tail = tnfs_base + i + 1;

    /* allow optional ".\" prefix in the path */
    if (p[0] == '.' && p[1] == '\\') p += 2;

    /* case-insensitive match against tail */
    for (i = 0; tail[i]; i++) {
        pc = p[i]; tc = tail[i];
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (tc >= 'a' && tc <= 'z') tc -= 32;
        if (pc != tc) return 0;
    }
    return (p[i] == '\0' || p[i] == '\\');
}

/*
 * classify_4e_content: decide what to enumerate for an AH=4Eh call.
 * Returns:
 *   0  chain to DOS (path is not inside TNFS_BASE)
 *   1  enumerate TNFS_BASE root  → GAMES + README.TXT
 *   2  enumerate TNFS_BASE\GAMES → empty for now
 */
int __cdecl classify_4e_content(unsigned int ds_val, unsigned int dx_val)
{
    const char far *p = (const char far *)MK_FP(ds_val, dx_val);
    int blen;

    if (!path_match_base(p)) {
        /* Relative path: no drive letter and no leading '\' */
        if (!(p[0] && p[1] == ':') && p[0] != '\\') {
            if (rbuf.enabled) {
                rb_write("AH=4E REL in_tnfs_dir=");
                rb_putc((char)('0' + (in_tnfs_dir ? 1 : 0)));
                rb_putc('\r'); rb_putc('\n');
            }
            if (in_tnfs_dir) return 1;
        }
        return 0;
    }

    for (blen = 0; tnfs_base[blen]; blen++);

    /* Path is exactly TNFS_BASE, or TNFS_BASE\ or TNFS_BASE\*.* */
    if (p[blen] == '\0') return 1;
    if (p[blen + 1] == '\0' || p[blen + 1] == '*') return 1;

    /* Look at what follows the backslash */
    p += blen + 1;

    if ((p[0]=='G'||p[0]=='g') && (p[1]=='A'||p[1]=='a') &&
        (p[2]=='M'||p[2]=='m') && (p[3]=='E'||p[3]=='e') &&
        (p[4]=='S'||p[4]=='s') &&
        (p[5]=='\\' || p[5]=='\0' || p[5]=='*')) return 2;

    return 1;   /* other path within TNFS_BASE: treat as root for now */
}

/*
 * handle_43: determine AH=43h response for a path in TNFS_BASE.
 * ax_val: caller's AX (AL=0 GET attributes, AL=1 SET attributes)
 * Returns:
 *   0      chain to DOS (path not in TNFS_BASE, or unknown sub-path)
 *   0x10   return CX=0x10 (directory), AX=0
 *   0x20   return CX=0x20 (archive file), AX=0
 *   0x100  return AX=0, CX unchanged (SET accepted silently)
 */
int __cdecl handle_43(unsigned int ax_val, unsigned int ds_val,
                      unsigned int dx_val)
{
    const char far *p = (const char far *)MK_FP(ds_val, dx_val);
    int blen;

    if (!path_match_base(p)) return 0;

    if ((unsigned char)ax_val == 1) return 0x100;   /* SET: accept */

    /* GET: determine attribute from path */
    for (blen = 0; tnfs_base[blen]; blen++);

    if (p[blen] == '\0') return 0x10;               /* TNFS_BASE itself */

    p += blen + 1;                                   /* skip '\' */

    if ((p[0]=='G'||p[0]=='g') && (p[1]=='A'||p[1]=='a') &&
        (p[2]=='M'||p[2]=='m') && (p[3]=='E'||p[3]=='e') &&
        (p[4]=='S'||p[4]=='s') && p[5]=='\0') return 0x10;

    if ((p[0]=='R'||p[0]=='r') && (p[1]=='E'||p[1]=='e') &&
        (p[2]=='A'||p[2]=='a') && (p[3]=='D'||p[3]=='d') &&
        (p[4]=='M'||p[4]=='m') && (p[5]=='E'||p[5]=='e') &&
        p[6]=='.'              &&
        (p[7]=='T'||p[7]=='t') && (p[8]=='X'||p[8]=='x') &&
        (p[9]=='T'||p[9]=='t') && p[10]=='\0') return 0x20;

    return 0;   /* unknown name inside TNFS: chain */
}

/* ------------------------------------------------------------------ */
/*  Logging                                                             */
/* ------------------------------------------------------------------ */

void __cdecl log_handled_4e(void)
{
    if (!rbuf.enabled) return;
    rb_write("HANDLED AH=4E\r\n");
}

/* entry: 0=GAMES, 1=README, 0xFF=EOF */
void __cdecl log_handled_4f(unsigned int entry)
{
    if (!rbuf.enabled) return;
    rb_write("HANDLED AH=4F");
    if (entry == 0xFF) {
        rb_write(" EOF");
    } else {
        rb_write(" ENTRY=");
        rb_putc((char)('0' + entry));
    }
    rb_putc('\r');
    rb_putc('\n');
}

/* log ALL AH=4E calls (path + search attrs) */
void __cdecl log_4e_pre(unsigned int ds_val, unsigned int dx_val,
                         unsigned int cx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=4E CX=");
    rb_hex8((unsigned char)(cx_val >> 8));
    rb_hex8((unsigned char)cx_val);
    rb_write(" \"");
    rb_far_str(ds_val, dx_val, 32);
    rb_write("\"\r\n");
}

void __cdecl log_dta_dump(unsigned int es_val, unsigned int bx_val)
{
    char far *d = (char far *)MK_FP(es_val, bx_val);
    int i;
    if (!rbuf.enabled) return;
    rb_write("DTA ");
    rb_hex8((unsigned char)(es_val >> 8)); rb_hex8((unsigned char)es_val);
    rb_putc(':');
    rb_hex8((unsigned char)(bx_val >> 8)); rb_hex8((unsigned char)bx_val);
    rb_write(" ATR=");
    rb_hex8((unsigned char)d[21]);
    rb_write(" SZ=");
    rb_hex8((unsigned char)d[29]); rb_hex8((unsigned char)d[28]);
    rb_hex8((unsigned char)d[27]); rb_hex8((unsigned char)d[26]);
    rb_write(" NAME=\"");
    for (i = 0; i < 13; i++) {
        unsigned char c = (unsigned char)d[30 + i];
        if (c == 0) break;
        rb_putc((c >= 0x20 && c < 0x7F) ? (char)c : '.');
    }
    rb_write("\"\r\n");
}

/* log find_state on every AH=4F entry */
void __cdecl log_4f_pre(unsigned int state)
{
    if (!rbuf.enabled) return;
    rb_write("AH=4F STATE=");
    rb_hex8((unsigned char)state);
    rb_write("\r\n");
}

/* AH=43h Get/Set File Attributes - log result of handle_43 */
void __cdecl log_43_result(unsigned int result)
{
    if (!rbuf.enabled) return;
    if (result == 0) {
        rb_write("AH=43 CHAIN\r\n");
    } else if (result == 0x100) {
        rb_write("AH=43 SET OK\r\n");
    } else {
        rb_write("AH=43 ATR=");
        rb_hex8((unsigned char)result);
        rb_write("\r\n");
    }
}

void __cdecl log_3d(unsigned int ds_val, unsigned int dx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=3D \"");
    rb_far_str(ds_val, dx_val, 32);
    rb_write("\"\r\n");
}

void __cdecl log_43(unsigned int ax_val, unsigned int ds_val,
                    unsigned int dx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=43 AL=");
    rb_hex8((unsigned char)ax_val);
    rb_write(" \"");
    rb_far_str(ds_val, dx_val, 32);
    rb_write("\"\r\n");
}

void __cdecl log_60(unsigned int ds_val, unsigned int si_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=60 \"");
    rb_far_str(ds_val, si_val, 32);
    rb_write("\"\r\n");
}

/* ------------------------------------------------------------------ */
/* ================================================================== */
/*  LEGACY: INT 21h path-hook experiment                               */
/*  -------------------------------------------------------------------*/
/*  The code below intercepts INT 21h to virtualise C:\TNFS.           */
/*  It works for DIR but FCB-based size fields proved unreliable and   */
/*  the approach cannot cleanly support a real drive letter via CDS.   */
/*  Kept for reference; see "CDS / Redirector" section near main()     */
/*  for the new direction.                                              */
/* ================================================================== */

/*  Non-static globals referenced by handler.asm                       */
/* ------------------------------------------------------------------ */

unsigned char in_handler = 0;
void (__interrupt __far *old_int21)(void);

/* AH=4E/4F enumeration state for active TNFS_BASE search.
 * 0 = no active search
 * 1 = GAMES returned by FindFirst; README.TXT is next
 * 2 = README.TXT returned; next FindNext returns EOF      */
unsigned char find_state = 0;

/* AH=11h/12h FCB enumeration state.
 * 0 = no active FCB search
 * 1 = GAMES returned; next AH=12h returns EOF                 */
unsigned char fcb_find_state = 0;

/* Set to 1 by our AH=4Eh handler when it fills GAMES for TNFS_BASE.
 * Cleared by AH=3Bh handler when the directory changes away from TNFS.
 * Prevents intercepting extended FCB searches outside our directory. */
unsigned char in_tnfs_dir = 0;
unsigned char last_tnfs_fcb_eof = 0;  /* consumed by AH=59h intercept */

/* ------------------------------------------------------------------ */
/*  FCB FindFirst/FindNext support (AH=11h/12h)                        */
/* ------------------------------------------------------------------ */

/*
 * fill_dta_fcb: fill the DTA at ES:BX in FCB search-result format.
 * FCB result layout (37 bytes):
 *   +00       drive (1-based; C:=3)
 *   +01..+08  filename (8 chars, space-padded)
 *   +09..+11  extension (3 chars, space-padded)
 *   +12..+15  reserved
 *   +16..+19  file size (32-bit LE)
 * entry=0: GAMES   entry=1: README.TXT (size 123)
 */
void __cdecl fill_dta_fcb(unsigned int es_val, unsigned int bx_val,
                           unsigned int entry)
{
    char far *d = (char far *)MK_FP(es_val, bx_val);
    int i;

    for (i = 0; i < 37; i++) d[i] = 0;
    d[0] = 3;   /* drive: C: (1-based) */

    if (entry == 0) {
        d[1]='G'; d[2]='A'; d[3]='M'; d[4]='E'; d[5]='S';
        d[6]=' '; d[7]=' '; d[8]=' ';
        d[9]=' '; d[10]=' '; d[11]=' ';
    } else {
        d[1]='R'; d[2]='E'; d[3]='A'; d[4]='D'; d[5]='M'; d[6]='E';
        d[7]=' '; d[8]=' ';
        d[9]='T'; d[10]='X'; d[11]='T';
        d[16] = 123;
    }
}

void __cdecl log_11_pre(void)
{
    if (!rbuf.enabled) return;
    rb_write("AH=11 FCB\r\n");
}

/* entry: 0=GAMES */
void __cdecl log_handled_11(unsigned int entry)
{
    if (!rbuf.enabled) return;
    rb_write("HANDLED AH=11 ENTRY=");
    rb_putc((char)('0' + entry));
    rb_putc('\r');
    rb_putc('\n');
}

/* entry: 1=README, 0xFF=EOF */
void __cdecl log_handled_12(unsigned int entry)
{
    if (!rbuf.enabled) return;
    rb_write("HANDLED AH=12");
    if (entry == 0xFF) {
        rb_write(" EOF");
    } else {
        rb_write(" ENTRY=");
        rb_putc((char)('0' + entry));
    }
    rb_putc('\r');
    rb_putc('\n');
}

/*
 * fill_dta_fcb_ext: fill DTA with an extended FCB search result.
 * Used when AH=11h was called with an extended FCB (byte[0] == 0xFF).
 *
 * Extended FCB result layout (37 bytes):
 *   +00     0xFF  (extended FCB marker)
 *   +01..+05  00  (reserved)
 *   +06     file attribute (0x10=directory)
 *   +07     drive code (1-based; C:=3)
 *   +08..+15  filename (8 chars, space-padded)
 *   +16..+18  extension (3 chars, space-padded)
 *   +19..+35  zero (block, size, date, time, etc.)
 *
 * Only entry=0 (GAMES) implemented; README.TXT deferred until file I/O works.
 */
void __cdecl fill_dta_fcb_ext(unsigned int es_val, unsigned int bx_val,
                               unsigned int entry)
{
    char far *d = (char far *)MK_FP(es_val, bx_val);
    int i;

    for (i = 0; i < 64; i++) d[i] = 0;
    d[0] = (char)0xFF;  /* extended FCB marker */
    d[7] = 3;           /* drive C: (1-based)  */

    if (entry == 0) {
        d[6]  = 0x10;   /* attribute: directory */
        d[8] ='G'; d[9] ='A'; d[10]='M'; d[11]='E'; d[12]='S';
        d[13]=' '; d[14]=' '; d[15]=' ';
        d[16]=' '; d[17]=' '; d[18]=' ';
    } else {
        d[6]  = 0x20;   /* attribute: archive */
        d[8] ='R'; d[9] ='E'; d[10]='A'; d[11]='D'; d[12]='M'; d[13]='E';
        d[14]=' '; d[15]=' ';
        d[16]='T'; d[17]='X'; d[18]='T';
        /* Hypothesis: offset 23 (0x17) is the attribute byte in this COMMAND.COM's
         * FCB result layout; the earlier probe had 0x22 there which set bit 0x10
         * (directory).  Set 0x20 (archive) to test: if README stops showing <DIR>
         * then offset 23 is indeed the attribute. */
        d[23]=0x20;
        /* Size probe at offset 26 (0x1A).  Try 0x7B = 123. */
        d[26]=0x7B; d[27]=0x00; d[28]=0x00; d[29]=0x00;
    }
}

/* ------------------------------------------------------------------ */
/*  Diagnostic logging (no behaviour change, chain-only calls)         */
/* ------------------------------------------------------------------ */

/* AH=29h Parse Filename - input string at DS:SI */
void __cdecl log_29(unsigned int ax_val, unsigned int ds_val,
                    unsigned int si_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=29 AL=");
    rb_hex8((unsigned char)ax_val);
    rb_write(" \"");
    rb_far_str(ds_val, si_val, 32);
    rb_write("\"\r\n");
}

/* AH=1Ah Set DTA - new DTA at DS:DX */
void __cdecl log_1a(unsigned int ds_val, unsigned int dx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=1A DTA=");
    rb_hex8((unsigned char)(ds_val >> 8)); rb_hex8((unsigned char)ds_val);
    rb_putc(':');
    rb_hex8((unsigned char)(dx_val >> 8)); rb_hex8((unsigned char)dx_val);
    rb_write("\r\n");
}

/* AH=2Fh Get DTA - return hook: ES:BX = current DTA */
void __cdecl log_2f_return(unsigned int es_val, unsigned int bx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=2F DTA=");
    rb_hex8((unsigned char)(es_val >> 8)); rb_hex8((unsigned char)es_val);
    rb_putc(':');
    rb_hex8((unsigned char)(bx_val >> 8)); rb_hex8((unsigned char)bx_val);
    rb_write("\r\n");
}

/* AH=3Bh Change Directory - path at DS:DX */
void __cdecl log_3b(unsigned int ds_val, unsigned int dx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=3B \"");
    rb_far_str(ds_val, dx_val, 32);
    rb_write("\"\r\n");
}

/* AH=3Bh decision log: flag=1 entering TNFS, flag=0 leaving */
void __cdecl log_3b_set(unsigned int flag)
{
    if (!rbuf.enabled) return;
    rb_write("AH=3B SET in_tnfs_dir=");
    rb_putc((char)('0' + (flag ? 1 : 0)));
    rb_putc('\r'); rb_putc('\n');
}

/* AH=47h Get Current Directory - DL = drive (1-based) */
void __cdecl log_47(unsigned int dx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=47 DL=");
    rb_hex8((unsigned char)dx_val);
    rb_write("\r\n");
}

/* AH=4Eh FindFirst - DTA address obtained from AH=2Fh, before fill */
void __cdecl log_4e_dta(unsigned int es_val, unsigned int bx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=4E DTA=");
    rb_hex8((unsigned char)(es_val >> 8)); rb_hex8((unsigned char)es_val);
    rb_putc(':');
    rb_hex8((unsigned char)(bx_val >> 8)); rb_hex8((unsigned char)bx_val);
    rb_write("\r\n");
}

/* AH=11h FCB FindFirst - raw hex dump of first 32 FCB bytes at DS:DX.
 * Does not try to parse: drv=FF means extended FCB with a different layout. */
void __cdecl log_11_fcb_raw(unsigned int ds_val, unsigned int dx_val)
{
    char far *fcb = (char far *)MK_FP(ds_val, dx_val);
    int i;
    if (!rbuf.enabled) return;
    rb_write("AH=11 FCB DS:DX=");
    rb_hex8((unsigned char)(ds_val >> 8)); rb_hex8((unsigned char)ds_val);
    rb_putc(':');
    rb_hex8((unsigned char)(dx_val >> 8)); rb_hex8((unsigned char)dx_val);
    rb_write(" [");
    for (i = 0; i < 32; i++) {
        if (i > 0) rb_putc(' ');
        rb_hex8((unsigned char)fcb[i]);
    }
    rb_write("]\r\n");
}

/*
 * fcb_set_state / fcb_get_state: manage TNFS private continuation state
 * stored in the caller's extended FCB reserved bytes [1..5]:
 *   [1..4] = 'T','N','F','S' marker
 *   [5]    = state byte (1=next README, 2=EOF)
 * state=0 means clear the marker (search exhausted or not ours).
 */
void __cdecl fcb_set_state(unsigned int ds_val, unsigned int dx_val,
                            unsigned int state)
{
    char far *fcb = (char far *)MK_FP(ds_val, dx_val);
    if (state == 0) {
        fcb[1] = fcb[2] = fcb[3] = fcb[4] = fcb[5] = 0;
    } else {
        fcb[1] = 'T'; fcb[2] = 'N'; fcb[3] = 'F'; fcb[4] = 'S';
        fcb[5] = (char)state;
    }
}

/* Write README.TXT size 123 (0x7B) into extended FCB at caller's DS:DX offset 0x17.
 * 0x17 is the documented extended FCB file-size DWORD field. */
void __cdecl fcb_set_readme_size(unsigned int ds_val, unsigned int dx_val)
{
    char far *fcb = (char far *)MK_FP(ds_val, dx_val);
    fcb[0x15] = 0x80; fcb[0x16] = 0x00;  /* logical record size = 128 */
    fcb[0x17] = 0x7B;
    fcb[0x18] = 0x00;
    fcb[0x19] = 0x00;
    fcb[0x1A] = 0x00;
}

/* Returns TNFS state byte (1 or 2), or 0 if marker absent. */
unsigned int __cdecl fcb_get_state(unsigned int ds_val, unsigned int dx_val)
{
    const char far *fcb = (const char far *)MK_FP(ds_val, dx_val);
    if (fcb[1]=='T' && fcb[2]=='N' && fcb[3]=='F' && fcb[4]=='S')
        return (unsigned char)fcb[5];
    return 0;
}

/*
 * log_fcb_bytes: dump first 20 bytes of caller's FCB with a phase tag.
 * tag 0 = "11PRE", 1 = "11POST", 2 = "12PRE", 3 = "12POST"
 */
void __cdecl log_fcb_bytes(unsigned int ds_val, unsigned int dx_val,
                            unsigned int tag)
{
    const char far *fcb = (const char far *)MK_FP(ds_val, dx_val);
    int i;
    if (!rbuf.enabled) return;
    if      (tag == 0) rb_write("11PRE");
    else if (tag == 1) rb_write("11POST");
    else if (tag == 2) rb_write("12PRE");
    else               rb_write("12POST");
    rb_write(" FCB [");
    for (i = 0; i < 32; i++) {
        if (i > 0) rb_putc(' ');
        rb_hex8((unsigned char)fcb[i]);
    }
    rb_write("]\r\n");
}

/* Raw hex dump of 40 bytes from ES:BX — used after fill_dta_fcb_ext to locate size field */
void __cdecl log_fcb_result_raw(unsigned int es_val, unsigned int bx_val)
{
    char far *d = (char far *)MK_FP(es_val, bx_val);
    int i;
    if (!rbuf.enabled) return;
    rb_write("FCB_DTA [");
    for (i = 0; i < 64; i++) {
        if (i > 0) rb_putc(' ');
        rb_hex8((unsigned char)d[i]);
    }
    rb_write("]\r\n");
}

/* AH=12h FCB FindNext - log call, chain to DOS */
void __cdecl log_12_pre(void)
{
    if (!rbuf.enabled) return;
    rb_write("AH=12 FCB\r\n");
}

/* AH=12h chained to DOS - log what DOS returned in AX (AL=00 found, AL=FF EOF) */
void __cdecl log_12_chain_return(unsigned int ax_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=12 DOS AL=");
    rb_hex8((unsigned char)ax_val);
    rb_write("\r\n");
}

/* AH=12h FCB FindNext - log immediately before return: what was in AX and what AL becomes */
void __cdecl log_12_return(unsigned int saved_ax, unsigned int final_al)
{
    if (!rbuf.enabled) return;
    rb_write("AH=12 RET saved_AX=");
    rb_hex8((unsigned char)(saved_ax >> 8)); rb_hex8((unsigned char)saved_ax);
    rb_write(" final_AL=");
    rb_hex8((unsigned char)final_al);
    rb_write("\r\n");
}

/* AH=59h Get Extended Error - return hook: AX=error BH=class BL=action CH=locus */
void __cdecl log_59_return(unsigned int ax_val, unsigned int bx_val,
                            unsigned int cx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=59 AX=");
    rb_hex8((unsigned char)(ax_val >> 8)); rb_hex8((unsigned char)ax_val);
    rb_write(" BH="); rb_hex8((unsigned char)(bx_val >> 8));  /* error class  */
    rb_write(" BL="); rb_hex8((unsigned char)bx_val);          /* action       */
    rb_write(" CH="); rb_hex8((unsigned char)(cx_val >> 8));   /* locus        */
    rb_write("\r\n");
}

/* AH=6Ch Extended Open/Create - path at DS:SI (not DS:DX!), BX=mode, DX=action */
void __cdecl log_6c(unsigned int ds_val, unsigned int si_val,
                    unsigned int bx_val, unsigned int dx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=6C BX=");
    rb_hex8((unsigned char)(bx_val >> 8)); rb_hex8((unsigned char)bx_val);
    rb_write(" DX=");
    rb_hex8((unsigned char)(dx_val >> 8)); rb_hex8((unsigned char)dx_val);
    rb_write(" \"");
    rb_far_str(ds_val, si_val, 64);
    rb_write("\"\r\n");
}

/* AH=42h Move File Pointer - AL=origin, BX=handle, CX:DX=offset */
void __cdecl log_42(unsigned int ax_val, unsigned int bx_val,
                    unsigned int cx_val, unsigned int dx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=42 BX=");
    rb_hex8((unsigned char)(bx_val >> 8)); rb_hex8((unsigned char)bx_val);
    rb_write(" AL=");
    rb_hex8((unsigned char)ax_val);
    rb_write(" CX:DX=");
    rb_hex8((unsigned char)(cx_val >> 8)); rb_hex8((unsigned char)cx_val);
    rb_putc(':');
    rb_hex8((unsigned char)(dx_val >> 8)); rb_hex8((unsigned char)dx_val);
    rb_write("\r\n");
}

/* AH=3Fh Read from File - BX=handle, CX=bytes requested */
void __cdecl log_3f(unsigned int bx_val, unsigned int cx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=3F BX=");
    rb_hex8((unsigned char)(bx_val >> 8)); rb_hex8((unsigned char)bx_val);
    rb_write(" CX=");
    rb_hex8((unsigned char)(cx_val >> 8)); rb_hex8((unsigned char)cx_val);
    rb_write("\r\n");
}

/* AH=3Eh Close File - BX=handle */
void __cdecl log_3e(unsigned int bx_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=3E BX=");
    rb_hex8((unsigned char)(bx_val >> 8)); rb_hex8((unsigned char)bx_val);
    rb_write("\r\n");
}

/* INT 2Fh AX=1105 CHDIR: always return success for now. */
void __cdecl log_2f_1105(void)
{
    if (!rbuf.enabled) return;
    rb_write("2F 1105 CHDIR OK\r\n");
}

/* INT 2Fh AH=11h redirector call.
 * Volume control: full regs only for AX=1123/1125; one line for everything else. */
void __cdecl log_2f_call(unsigned int ax_val, unsigned int bx_val, unsigned int cx_val,
                          unsigned int dx_val, unsigned int ds_val, unsigned int si_val,
                          unsigned int es_val, unsigned int di_val)
{
    if (!rbuf.enabled) return;
    if (ax_val != 0x1123 && ax_val != 0x1125) {
        rb_write("2F AX="); rb_hex16(ax_val); rb_write("\r\n");
        return;
    }
    /* AX=1123 or AX=1125: full register set */
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
        /* AX=1123 showed readable paths at DS:SI — log the string */
        rb_write("  SI\""); rb_far_str(ds_val, si_val, 48); rb_write("\"\r\n");
    }
    /* No memory dumps yet: first confirm register layout is correct */
}

/* ------------------------------------------------------------------ */
/*  Catch-all log                                                       */
/* ------------------------------------------------------------------ */

void __cdecl log_int21(unsigned int ax_val)
{
    if (!rbuf.enabled) return;
    rb_write("AH=");
    rb_hex8((unsigned char)(ax_val >> 8));
    rb_putc('\r');
    rb_putc('\n');
}

/* ------------------------------------------------------------------ */
/*  ASM handler interface                                               */
/* ------------------------------------------------------------------ */
extern void new_int21_(void);
extern void init_cs_ptr_(void);
extern void new_int2f_(void);
extern void init_int2f_ptr_(void);

void (__interrupt __far *old_int2f)(void);

unsigned short get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

extern unsigned int  _psp;       /* PSP segment — set by Watcom cstart */
extern unsigned short _STACKTOP; /* initial SP = top of DGROUP = end of program */

/* Compute the number of paragraphs from PSP to end of DGROUP (inclusive of
 * PSP, code, data, BSS, stack).  Replaces the old hardcoded constant. */
static unsigned int calc_resident_paras(void)
{
    unsigned long end = (unsigned long)(get_ds() - _psp) * 16UL + _STACKTOP;
    return (unsigned int)((end + 15) >> 4);
}

/* ================================================================== */
/*  CDS / Redirector approach  (new direction, June 2026)              */
/*  -------------------------------------------------------------------*/
/*  Step B: find List of Lists and CDS array; print addresses.         */
/*  Step C: initialise drive T: in the CDS as a remote drive so DOS   */
/*          stops reporting "Invalid drive".                           */
/*  Step D (later): install INT 2Fh redirector for actual file I/O.   */
/* ================================================================== */

/*
 * Critical constants — do not guess these; see README.md for derivation.
 *
 * DRIVE_T_IDX = 19: A=0, B=1, ... T=19. CONFIG.SYS must have LASTDRIVE>=T.
 *
 * CDS_ENTRY_SIZE = 88: fixed for DOS 4–6 (DOSSTRUC.H cdsstruct).
 *   CDS layout (offsets into each 88-byte entry):
 *     +0x00  current_path[67]   ASCIIZ root path, e.g. "T:\"
 *     +0x43  flags (word)       NET|PHY = 0xC000  (NET alone = 0x8000 is ignored by DOS 6!)
 *     +0x45  dpb_ptr (4 bytes)  NULL for network drives
 *     +0x49  net_union (6 bytes)
 *     +0x4F  backslash_offset   2 for "T:\"
 *     +0x51  DOS 4+ reserved[7]
 *
 * SDA offsets used by FINDFIRST/FINDNEXT (INT 21h AX=5D06h → DS:SI):
 *     +0x00C  curr_dta (far ptr)
 *     +0x1B3  found_file (32 bytes)
 *     +0x22B  fcb_fn1[11]
 *     +0x24D  srch_attr
 */
#define DRIVE_T_IDX     19      /* T: = A(0)+19, 0-based               */
#define CDS_ENTRY_SIZE  88      /* DOS 4–6: 88 bytes per CDS entry      */
#define README_SIZE     21      /* sizeof("Hello from TNFSDRV!\r\n") - 1 */

#define CDS_FLAG_REMOTE 0x8000u

/* SDA (Swappable Data Area) pointer obtained at init via INT 21h AX=5D06h.
 * Used by do_findfirst/do_findnext to read search attrs and write found_file. */
static char far *glob_sdaptr;

/*
 * do_findfirst: handle INT 2Fh AX=111Bh (FINDFIRST).
 *
 * Follows EtherDFS approach: use glob_sdaptr (not ES:DI) for FINDFIRST.
 *
 * SDA offsets (DOS 4+, from DOSSTRUC.H):
 *   +0x00C  curr_dta (far ptr: offset word at +C, segment word at +E)
 *   +0x1B3  found_file (32 bytes, foundfilestruct)
 *   +0x22B  fcb_fn1[11]  search template in FCB format
 *   +0x24D  srch_attr    search attribute byte
 *
 * DTA layout written by redirector (SDB 21 bytes + foundfilestruct 32 bytes):
 *   +0x00  drv_lett  (drive_idx | 0x80)
 *   +0x01  srch_tmpl (11 bytes, from SDA.fcb_fn1)
 *   +0x0C  srch_attr
 *   +0x0D  dir_entry (word — our sequence counter; 1 = README next)
 *   +0x0F  par_clstr (word, 0)
 *   +0x11  f1[4]     (0)
 *   +0x15  found_file (32 bytes, copy of SDA.found_file)
 *
 * Returns 0 on success (AX=0 CF=0), 0x12 if not found (AX=12h CF=1).
 */
unsigned int __cdecl do_findfirst(void)
{
    char far *sda = glob_sdaptr;
    char far *dta;
    char far *found;
    unsigned char srch_attr;
    unsigned int dta_off, dta_seg;
    int k;

    if (!sda) {
        if (rbuf.enabled) rb_write("2F 111B NO SDA\r\n");
        return 0x12;
    }

    /* Search attribute from SDA+0x24D */
    srch_attr = (unsigned char)sda[0x24D];
    if (rbuf.enabled) { rb_write("2F 111B srch="); rb_hex8(srch_attr); rb_write("\r\n"); }

    /* Volume-label search: not found */
    if (srch_attr & 0x08u) {
        if (rbuf.enabled) rb_write("2F 111B VOLABEL\r\n");
        return 0x12;
    }

    if (rbuf.enabled) rb_write("2F 111B FINDFIRST GAMES\r\n");

    /* Fill SDA found_file at SDA+0x1B3 */
    found = sda + 0x1B3;
    for (k = 0; k < 32; k++) found[k] = 0;
    found[0]='G'; found[1]='A'; found[2]='M'; found[3]='E'; found[4]='S';
    found[5]=' '; found[6]=' '; found[7]=' ';
    found[8]=' '; found[9]=' '; found[10]=' ';
    found[11] = 0x10;    /* fattr: directory; time/date/size stay 0 */

    /* Get curr_dta from SDA+0x0C (far ptr: offset word, segment word) */
    dta_off = (unsigned int)(unsigned char)sda[0x0C]
            | ((unsigned int)(unsigned char)sda[0x0D] << 8);
    dta_seg = (unsigned int)(unsigned char)sda[0x0E]
            | ((unsigned int)(unsigned char)sda[0x0F] << 8);
    dta = (char far *)MK_FP(dta_seg, dta_off);
    if (rbuf.enabled) {
        rb_write("DTA="); rb_hex16(dta_seg); rb_putc(':'); rb_hex16(dta_off); rb_write("\r\n");
    }

    /* Init SDB in DTA (bytes 0..20) */
    dta[0] = (char)(DRIVE_T_IDX | 0x80);          /* drv_lett */
    for (k = 0; k < 11; k++) dta[1+k] = sda[0x22B+k]; /* srch_tmpl from SDA.fcb_fn1 */
    dta[12] = (char)srch_attr;
    dta[13] = 1; dta[14] = 0;                     /* dir_entry = 1 (README next) */
    dta[15] = 0; dta[16] = 0;                     /* par_clstr = 0 */
    dta[17] = 0; dta[18] = 0; dta[19] = 0; dta[20] = 0;

    /* Copy SDA.found_file (32 bytes) to DTA+0x15 (after SDB) */
    for (k = 0; k < 32; k++) dta[0x15+k] = found[k];

    /* Log first 16 bytes of filled DTA+0x15 */
    if (rbuf.enabled) {
        rb_write("FFDT:");
        for (k = 0; k < 16; k++) { rb_putc(' '); rb_hex8((unsigned char)dta[0x15+k]); }
        rb_write("\r\n");
    }

    return 0;
}

/*
 * do_findnext: handle INT 2Fh AX=111Ch (FINDNEXT).
 *
 * For FINDNEXT, ES:DI IS the DTA (set up by our FINDFIRST).
 * Read dir_entry from DTA[13] to decide what to return next.
 *
 *   dir_entry == 1  → return README.TXT, set dir_entry = 2
 *   dir_entry >= 2  → EOF (AX=0x12, CF set)
 *
 * foundfilestruct offsets (from DOSSTRUC.H):
 *   +00  fname[11]
 *   +11  fattr
 *   +12  f1[10]
 *   +22  time_lstupd (word)
 *   +24  date_lstupd (word)
 *   +26  start_clstr (word)
 *   +28  fsize (dword)
 */
unsigned int __cdecl do_findnext(unsigned int es_val, unsigned int di_val)
{
    char far *dta   = (char far *)MK_FP(es_val, di_val);
    char far *found = glob_sdaptr + 0x1B3;
    int k;

    /* dir_entry from DTA/SDB+13 */
    if ((unsigned char)dta[13] >= 2) {
        if (rbuf.enabled) rb_write("2F 111C EOF\r\n");
        return 0x12;
    }

    if (rbuf.enabled) rb_write("2F 111C FINDNEXT README\r\n");

    /* Fill SDA found_file with README.TXT */
    for (k = 0; k < 32; k++) found[k] = 0;
    found[0]='R'; found[1]='E'; found[2]='A'; found[3]='D'; found[4]='M'; found[5]='E';
    found[6]=' '; found[7]=' ';
    found[8]='T'; found[9]='X'; found[10]='T';
    found[11] = 0x20;               /* archive */
    found[28] = (char)README_SIZE;  /* fsize low byte; 29..31 already 0 */

    /* Advance state: dir_entry = 2 (next FINDNEXT → EOF) */
    dta[13] = 2; dta[14] = 0;

    /* Copy found_file to DTA+0x15 */
    for (k = 0; k < 32; k++) dta[0x15+k] = found[k];

    return 0;
}

/* ------------------------------------------------------------------ */
/*  INT 2Fh file I/O handlers: GETATTR / OPEN / READ / CLOSE           */
/* ------------------------------------------------------------------ */

/* Content served by T:\README.TXT (README_SIZE defined near top of section) */
static const char readme_content[] = "Hello from TNFSDRV!\r\n";

/*
 * last_component_is: return 1 if the last backslash-separated component of
 * SDA->fn1 (at glob_sdaptr+0x9E) equals `name` (case-insensitive).
 */
static int last_component_is(const char *name)
{
    char far *fn1 = glob_sdaptr + 0x9E;
    char far *comp;
    int i;
    char pc, nc;

    comp = fn1;
    for (i = 0; fn1[i]; i++)
        if (fn1[i] == '\\') comp = fn1 + i + 1;

    for (i = 0; name[i]; i++) {
        pc = comp[i]; nc = name[i];
        if (!pc) return 0;
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (nc >= 'a' && nc <= 'z') nc -= 32;
        if (pc != nc) return 0;
    }
    return (comp[i] == '\0');
}

/*
 * do_getattr: INT 2Fh AX=110Fh GET FILE ATTRIBUTES.
 * Filename is in SDA->fn1 (glob_sdaptr+0x9E).
 * Returns attribute byte on success, 0xFFFF if not found.
 * Caller (ASM) sets CX=retval, AX=0, CF=0 on success;
 *              sets AX=2, CF=1 on 0xFFFF.
 */
unsigned int __cdecl do_getattr(void)
{
    if (!glob_sdaptr) return 0xFFFF;
    if (rbuf.enabled) {
        rb_write("2F 110F \"");
        rb_far_str(FP_SEG(glob_sdaptr + 0x9E), FP_OFF(glob_sdaptr + 0x9E), 32);
        rb_write("\"\r\n");
    }
    if (last_component_is("README.TXT")) return 0x20;  /* archive */
    if (last_component_is("GAMES"))     return 0x10;   /* directory */
    return 0xFFFF;
}

/*
 * fill_sft_readme: shared SFT fill for OPEN (AL=16h) and SPOPNFIL (AL=2Eh).
 *
 * SFT layout (DOSSTRUC.H sftstruct):
 *   +00  handle_count (word)   — set by DOS, leave
 *   +02  open_mode    (word)   — set by DOS, leave
 *   +04  file_attr    (byte)
 *   +05  dev_info_word(word)   — bit 15 = network
 *   +07  dev_drvr_ptr (4 bytes)— NULL for network
 *   +0B  start_sector (word)
 *   +0D  file_time    (dword)
 *   +11  file_size    (dword)
 *   +15  file_pos     (dword)
 *   +19..+1F other fields
 *   +20  file_name[11] (FCB format)
 */
static void fill_sft_readme(char far *sft)
{
    sft[0x04] = 0x20;               /* file_attr: archive */
    sft[0x05] = (char)DRIVE_T_IDX; /* dev_info_word low: drive number */
    sft[0x06] = 0x80;               /* dev_info_word high: bit 15 = network */
    sft[0x07] = sft[0x08] = sft[0x09] = sft[0x0A] = 0; /* dev_drvr_ptr: NULL */
    sft[0x0B] = sft[0x0C] = 0;     /* start_sector */
    sft[0x0D] = sft[0x0E] = sft[0x0F] = sft[0x10] = 0; /* file_time */
    sft[0x11] = README_SIZE;        /* file_size low byte */
    sft[0x12] = sft[0x13] = sft[0x14] = 0;
    sft[0x15] = sft[0x16] = sft[0x17] = sft[0x18] = 0; /* file_pos: 0 */
    sft[0x19] = sft[0x1A] = sft[0x1B] = sft[0x1C] = 0;
    sft[0x1D] = sft[0x1E] = sft[0x1F] = 0;
    sft[0x20]='R'; sft[0x21]='E'; sft[0x22]='A'; sft[0x23]='D';
    sft[0x24]='M'; sft[0x25]='E'; sft[0x26]=' '; sft[0x27]=' ';
    sft[0x28]='T'; sft[0x29]='X'; sft[0x2A]='T';
}

/* do_open: INT 2Fh AX=1116h OPEN */
void __cdecl do_open(unsigned int es_val, unsigned int di_val)
{
    if (rbuf.enabled) rb_write("2F 1116 OPEN\r\n");
    fill_sft_readme((char far *)MK_FP(es_val, di_val));
}

/*
 * do_spopen: INT 2Fh AX=112Eh SPOPNFIL (Special Open).
 * Used by TYPE and COPY in MS-DOS 5.0/6.x instead of regular OPEN.
 * Same SFT fill as OPEN; caller (ASM) sets CX=1 (action: file opened).
 */
void __cdecl do_spopen(unsigned int es_val, unsigned int di_val)
{
    if (rbuf.enabled) rb_write("2F 112E SPOP\r\n");
    fill_sft_readme((char far *)MK_FP(es_val, di_val));
}

/*
 * do_read: INT 2Fh AX=1108h READ.
 * ES:DI → SFT; CX = bytes requested.
 * Buffer is SDA->curr_dta (EtherDFS pattern — NOT DS:DX).
 * Reads from readme_content at current file_pos, updates SFT->file_pos.
 * Returns bytes actually read (placed in CX by caller; AX=0, CF=0).
 */
unsigned int __cdecl do_read(unsigned int es_val, unsigned int di_val,
                              unsigned int cx_val)
{
    char far *sft = (char far *)MK_FP(es_val, di_val);
    char far *sda = glob_sdaptr;
    char far *buf;
    unsigned int dta_off, dta_seg;
    unsigned long pos;
    unsigned int avail, count, i;

    /* Read buffer = SDA->curr_dta (far ptr at SDA+0x0C) */
    dta_off = (unsigned int)(unsigned char)sda[0x0C]
            | ((unsigned int)(unsigned char)sda[0x0D] << 8);
    dta_seg = (unsigned int)(unsigned char)sda[0x0E]
            | ((unsigned int)(unsigned char)sda[0x0F] << 8);
    buf = (char far *)MK_FP(dta_seg, dta_off);

    /* Read file_pos from SFT+0x15 (dword, little-endian) */
    pos = (unsigned long)(unsigned char)sft[0x15]
        | ((unsigned long)(unsigned char)sft[0x16] << 8)
        | ((unsigned long)(unsigned char)sft[0x17] << 16)
        | ((unsigned long)(unsigned char)sft[0x18] << 24);

    if (pos >= README_SIZE) {
        if (rbuf.enabled) rb_write("2F 1108 EOF\r\n");
        return 0;
    }
    avail = README_SIZE - (unsigned int)pos;
    count = (cx_val < avail) ? cx_val : avail;

    for (i = 0; i < count; i++) buf[i] = readme_content[(unsigned int)pos + i];

    pos += count;
    sft[0x15] = (char)pos;
    sft[0x16] = (char)(pos >> 8);
    sft[0x17] = (char)(pos >> 16);
    sft[0x18] = (char)(pos >> 24);

    if (rbuf.enabled) {
        rb_write("2F 1108 READ cnt="); rb_hex16(count);
        rb_write(" DTA="); rb_hex16(dta_seg); rb_putc(':'); rb_hex16(dta_off);
        rb_write("\r\n");
    }
    return count;
}

/* do_close: INT 2Fh AX=1106h CLOSE — always return success. */
void __cdecl do_close(void)
{
    if (rbuf.enabled) rb_write("2F 1106 CLOSE\r\n");
}

/* INT 21h AH=52h: return ES:BX as a far pointer. */
static void far *get_lol(void)
{
    /* static: with -zu (DS!=SS), int86x needs DS-relative pointers, not SS. */
    static union REGS r;
    static struct SREGS sr;
    union { unsigned w[2]; void far *p; } u;
    r.h.ah = 0x52;
    int86x(0x21, &r, &r, &sr);
    u.w[0] = r.w.bx;
    u.w[1] = sr.es;
    return u.p;
}

static void init_cds(void)
{
    static union REGS r;
    static struct SREGS sr;
    char far *lol;
    int i, j;

    rb_write("INIT_CDS START\r\n");

    /* DOS version: AH=30h → AL=major, AH=minor */
    r.h.ah = 0x30;
    r.h.al = 0x00;
    int86(0x21, &r, &r);
    rb_write("DOS=");
    rb_dec(r.h.al); rb_putc('.');
    rb_dec(r.h.ah);
    rb_write("\r\n");

    /* SDA pointer: INT 21h AX=5D06h → DS:SI (DOS 3.1+) */
    r.w.ax = 0x5D06;
    int86x(0x21, &r, &r, &sr);
    glob_sdaptr = (char far *)MK_FP(sr.ds, r.w.si);
    rb_write("SDA=");
    rb_hex16(sr.ds); rb_putc(':'); rb_hex16(r.w.si);
    rb_write("\r\n");

    /* List of Lists: INT 21h AH=52h → ES:BX */
    lol = (char far *)get_lol();
    rb_write("LoL=");
    rb_hex16(FP_SEG(lol)); rb_putc(':'); rb_hex16(FP_OFF(lol));
    rb_write("\r\n");

    /* Dump first 32 bytes from LoL to verify pointer offsets. */
    rb_write("LOL_DUMP\r\n");
    for (i = 0; i < 32; i += 16) {
        rb_hex16((unsigned)i); rb_putc(':');
        for (j = 0; j < 16; j++) {
            rb_putc(' ');
            rb_hex8((unsigned char)lol[i + j]);
        }
        rb_putc('\r'); rb_putc('\n');
    }

    /* CDS far pointer: offset word at LoL+0x16, segment word at LoL+0x18.
     * Read byte-by-byte to avoid far pointer cast ambiguity in Watcom. */
    {
        unsigned int cds_off, cds_seg, flags, bsoff;
        char far *cds_base;
        char far *entry;

        cds_off = (unsigned int)(unsigned char)lol[0x16]
                | ((unsigned int)(unsigned char)lol[0x17] << 8);
        cds_seg = (unsigned int)(unsigned char)lol[0x18]
                | ((unsigned int)(unsigned char)lol[0x19] << 8);
        cds_base = (char far *)MK_FP(cds_seg, cds_off);

        rb_write("CDS=");
        rb_hex16(cds_seg); rb_putc(':'); rb_hex16(cds_off);
        rb_write("\r\n");

        /* Dump CDS[0..T:] so we can verify stride and pointer. */
        for (i = 0; i <= DRIVE_T_IDX; i++) {
            entry = cds_base + (unsigned)(i * CDS_ENTRY_SIZE);
            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            rb_putc('C'); rb_putc('D'); rb_putc('S'); rb_putc('[');
            rb_dec((unsigned)i);
            rb_write("]=\"");
            rb_far_str(FP_SEG(entry), FP_OFF(entry), 16);
            rb_write("\" FL=");
            rb_hex16(flags);
            rb_write("\r\n");
        }

        /* Redirector availability check (EtherDFS pattern). */
        r.w.ax = 0x1100;
        int86(0x2F, &r, &r);
        rb_write("REDIR AX=");
        rb_hex16(r.w.ax);
        rb_write("\r\n");

        if (r.w.ax == 0x0001) {
            rb_write("REDIR NOT AVAILABLE\r\n");
        } else {
            /* Log CDS[T:] before any writes. */
            entry = cds_base + (unsigned)(DRIVE_T_IDX * CDS_ENTRY_SIZE);
            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            bsoff = (unsigned int)(unsigned char)entry[0x4F]
                  | ((unsigned int)(unsigned char)entry[0x50] << 8);
            rb_write("CDS19_BEFORE path=\"");
            rb_far_str(FP_SEG(entry), FP_OFF(entry), 16);
            rb_write("\" FL="); rb_hex16(flags);
            rb_write(" BS="); rb_hex16(bsoff);
            rb_write("\r\n");

            /* --- Write CDS[T:] as a network drive (EtherDFS layout) --- */

            /* current_path[0..66]: "T:\" NUL, rest zero */
            entry[0] = 'T'; entry[1] = ':'; entry[2] = '\\'; entry[3] = '\0';
            for (i = 4; i < 67; i++) entry[i] = 0;

            /* flags at +0x43: NET|PHY (0xC000).
             * EtherDFS note: MS-DOS 6.0 ignores the drive without CDSFLAG_PHY. */
            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            flags |= 0x8000u | 0x4000u;
            entry[0x43] = (char)(flags & 0xFF);
            entry[0x44] = (char)(flags >> 8);

            /* DPB pointer at +0x45: NULL (no physical drive) */
            entry[0x45] = 0; entry[0x46] = 0; entry[0x47] = 0; entry[0x48] = 0;

            /* network union at +0x49: zero for now */
            entry[0x49] = 0; entry[0x4A] = 0; entry[0x4B] = 0;
            entry[0x4C] = 0; entry[0x4D] = 0; entry[0x4E] = 0;

            /* backslash_offset at +0x4F: 2 (index of '\' in "T:\") */
            entry[0x4F] = 2; entry[0x50] = 0;

            /* DOS 4+ reserved bytes at +0x51: zero */
            entry[0x51] = 0; entry[0x52] = 0; entry[0x53] = 0; entry[0x54] = 0;
            entry[0x55] = 0; entry[0x56] = 0; entry[0x57] = 0;

            /* Log CDS[T:] after writes. */
            flags = (unsigned int)(unsigned char)entry[0x43]
                  | ((unsigned int)(unsigned char)entry[0x44] << 8);
            bsoff = (unsigned int)(unsigned char)entry[0x4F]
                  | ((unsigned int)(unsigned char)entry[0x50] << 8);
            rb_write("CDS19_AFTER path=\"");
            rb_far_str(FP_SEG(entry), FP_OFF(entry), 16);
            rb_write("\" FL="); rb_hex16(flags);
            rb_write(" BS="); rb_hex16(bsoff);
            rb_write("\r\n");
        }
    }

    rb_write("INIT_CDS DONE\r\n");
}

/* ------------------------------------------------------------------ */
/*  TSR install                                                         */
/* ------------------------------------------------------------------ */

int main(void)
{
    unsigned int seg, off, paras;

    rbuf.magic   = RING_MAGIC;
    rbuf.head    = 0;
    rbuf.tail    = 0;
    rbuf.size    = RING_SIZE;
    rbuf.enabled = 1;

    rb_write("TNFSDRV loaded OK\r\n");

    /* CDS setup runs before the INT 21h hook so it calls clean DOS directly. */
    init_cds();

    old_int21 = _dos_getvect(0x21);
    init_cs_ptr_();
    _dos_setvect(0x21, (void (__interrupt __far *)())new_int21_);

    old_int2f = _dos_getvect(0x2F);
    init_int2f_ptr_();
    _dos_setvect(0x2F, (void (__interrupt __far *)())new_int2f_);

    paras = calc_resident_paras();
    seg   = get_ds();
    off   = (unsigned int)&rbuf;
    printf("TNFSDRV: hooking %s\r\n", TNFS_BASE);
    printf("Ring buffer at %04X:%04X\r\n", seg, off);
    printf("Run: SHOWBUF %04X:%04X\r\n", seg, off);
    printf("Staying resident (%u paragraphs).\r\n", paras);

    {
        static union REGS r;
        r.h.ah = 0x31;
        r.h.al = 0;
        r.w.dx = paras;
        int86(0x21, &r, &r);
    }
    return 0;
}
