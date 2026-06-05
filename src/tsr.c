/*
 * TSR.C  -  TNFSDRV: virtual drive T: via MS-DOS INT 2Fh network redirector
 *
 * Presents drive T: to DOS by:
 *   1. Marking CDS entry 19 as a network+physical drive (flags = 0xC000).
 *   2. Handling INT 2Fh AH=11h redirector callbacks:
 *        AL=05h  CHDIR      — validate path; accept T:\ and T:\GAMES
 *        AL=06h  CLOSE      — always succeed
 *        AL=08h  READ       — serve fake README.TXT content
 *        AL=0Ch  DISKSPACE  — return fake 16 MB volume info
 *        AL=0Fh  GETATTR    — return attributes from SDA->fn1
 *        AL=16h  OPEN       — fill SFT for README.TXT
 *        AL=2Eh  SPOPNFIL   — same as OPEN (used by TYPE/COPY in DOS 5+/6.x)
 *        AL=1Bh  FINDFIRST  — enumerates root_entries[] via FsEntry layer
 *        AL=1Ch  FINDNEXT   — continues enumeration by dir_entry index
 *
 * Filesystem abstraction:
 *   FsEntry / root_entries[]  —  static table describing T:\ contents.
 *   Handlers never hardcode file names; all dispatch goes through
 *   fs_find_root(), fs_get_root_entry(), fs_find_by_tmpl() etc.
 *   Replace root_entries[] with a TNFS readdir result to add network FS.
 *
 * Compile: wcc -bt=dos -ms -3 -d2 -s -zu tsr.c
 * See README.md for full build and test instructions.
 */

#include <dos.h>
#include <i86.h>
#include <stdio.h>

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
/*  ASM handler interface                                               */
/* ------------------------------------------------------------------ */
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
#define VOLUME_LABEL    "TNFSDOS    "  /* 11 bytes, FCB-padded; change as needed */

/* ------------------------------------------------------------------ */
/*  Filesystem table  (replace with TNFS readdir later)                */
/* ------------------------------------------------------------------ */

static const char readme_content[] = "Hello from TNFSDRV!\r\n";

typedef struct {
    const char    *name;
    unsigned char  attr;
    unsigned long  size;
    const char    *content;   /* NULL for directories */
} FsEntry;

static FsEntry root_entries[] = {
    { "GAMES",      0x10, 0,           NULL           },
    { "README.TXT", 0x20, README_SIZE, readme_content }
};

#define CDS_FLAG_REMOTE 0x8000u

/* SDA (Swappable Data Area) pointer obtained at init via INT 21h AX=5D06h.
 * Used by do_findfirst/do_findnext to read search attrs and write found_file. */
static char far *glob_sdaptr;

/* tmpl_matches: compare 11-byte FCB template at far ptr against near literal. */
static int tmpl_matches(char far *tmpl, const char *pat)
{
    int i;
    for (i = 0; i < 11; i++) if (tmpl[i] != pat[i]) return 0;
    return 1;
}

/* is_wildcard_template: return 1 if all 11 bytes of FCB template are '?'. */
static int is_wildcard_template(char far *tmpl)
{
    int i;
    for (i = 0; i < 11; i++) if (tmpl[i] != '?') return 0;
    return 1;
}

/* fn1_has_prefix: case-insensitive check whether fn1 starts with prefix. */
static int fn1_has_prefix(char far *fn1, const char *prefix)
{
    int i;
    for (i = 0; prefix[i]; i++) {
        char fc = fn1[i], pc = prefix[i];
        if (!fc) return 0;
        if (fc >= 'a' && fc <= 'z') fc -= 32;
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (fc != pc) return 0;
    }
    return 1;
}

/* fn1_eq: case-insensitive exact match; trailing backslash in fn1 is optional. */
static int fn1_eq(char far *fn1, const char *s)
{
    int i;
    for (i = 0; s[i]; i++) {
        char fc = fn1[i], pc = s[i];
        if (!fc) return 0;
        if (fc >= 'a' && fc <= 'z') fc -= 32;
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (fc != pc) return 0;
    }
    /* allow fn1 to end here or with a single trailing backslash */
    return (fn1[i] == '\0' || (fn1[i] == '\\' && fn1[i+1] == '\0'));
}

static int last_component_is(const char *name); /* forward decl — defined below */

/* ------------------------------------------------------------------ */
/*  Filesystem helpers                                                  */
/* ------------------------------------------------------------------ */

static int str_eq_ci(const char *a, const char *b)
{
    int i;
    for (i = 0; ; i++) {
        char ac = a[i], bc = b[i];
        if (ac >= 'a' && ac <= 'z') ac -= 32;
        if (bc >= 'a' && bc <= 'z') bc -= 32;
        if (ac != bc) return 0;
        if (!ac) return 1;
    }
}

static int fs_get_root_count(void)
{
    return (int)(sizeof(root_entries) / sizeof(root_entries[0]));
}

static FsEntry *fs_get_root_entry(int idx) { return &root_entries[idx]; }

static FsEntry *fs_find_root(const char *name)
{
    int i;
    for (i = 0; i < fs_get_root_count(); i++)
        if (str_eq_ci(root_entries[i].name, name)) return &root_entries[i];
    return NULL;
}

/* Convert "README.TXT" → "README  TXT" (11-byte FCB format, near buffers). */
static void entry_to_fcb(const char *name, char fcb[11])
{
    int i, dot = -1;
    for (i = 0; i < 11; i++) fcb[i] = ' ';
    for (i = 0; name[i]; i++) { if (name[i] == '.') { dot = i; break; } }
    if (dot < 0) {
        for (i = 0; name[i] && i < 8; i++) fcb[i] = name[i];
    } else {
        for (i = 0; i < dot && i < 8; i++) fcb[i] = name[i];
        for (i = 0; name[dot+1+i] && i < 3; i++) fcb[8+i] = name[dot+1+i];
    }
}

/* Fill 32-byte found_file (SDA+0x1B3) from an FsEntry. */
static void fill_found_from_entry(FsEntry *e, char far *found)
{
    static char fcb[11];   /* static: DS-relative, avoids -zu SS≠DS warning */
    int k;
    entry_to_fcb(e->name, fcb);
    for (k = 0; k < 11; k++) found[k] = fcb[k];
    found[11] = e->attr;
    found[28] = (char)(e->size         & 0xFFUL);
    found[29] = (char)((e->size >>  8) & 0xFFUL);
    found[30] = (char)((e->size >> 16) & 0xFFUL);
    found[31] = (char)((e->size >> 24) & 0xFFUL);
}

/* Fill an SFT from root_entries[idx].
 * Stores idx in sft[0x0B] (start_sector) as private tag for do_read. */
static void fill_sft_entry(int idx, char far *sft)
{
    FsEntry *e = &root_entries[idx];
    static char fcb[11];
    int k;
    entry_to_fcb(e->name, fcb);
    sft[0x04] = e->attr;
    sft[0x05] = (char)DRIVE_T_IDX;
    sft[0x06] = 0x80;
    sft[0x07] = sft[0x08] = sft[0x09] = sft[0x0A] = 0;
    sft[0x0B] = (char)idx;   /* private: entry index, read back by do_read */
    sft[0x0C] = 0;
    sft[0x0D] = sft[0x0E] = sft[0x0F] = sft[0x10] = 0;
    sft[0x11] = (char)(e->size         & 0xFFUL);
    sft[0x12] = (char)((e->size >>  8) & 0xFFUL);
    sft[0x13] = (char)((e->size >> 16) & 0xFFUL);
    sft[0x14] = (char)((e->size >> 24) & 0xFFUL);
    sft[0x15] = sft[0x16] = sft[0x17] = sft[0x18] = 0;
    sft[0x19] = sft[0x1A] = sft[0x1B] = sft[0x1C] = 0;
    sft[0x1D] = sft[0x1E] = sft[0x1F] = 0;
    for (k = 0; k < 11; k++) sft[0x20+k] = fcb[k];
}

/* Return index of root entry whose FCB name matches tmpl, -1 if none. */
static int fs_find_by_tmpl(char far *tmpl)
{
    int i;
    static char fcb[11];
    for (i = 0; i < fs_get_root_count(); i++) {
        entry_to_fcb(root_entries[i].name, fcb);
        if (tmpl_matches(tmpl, fcb)) return i;
    }
    return -1;
}

/* If fn1 = "T:\<name>\..." and <name> is a known root entry, return that entry.
 * Returns NULL for root-level paths or unknown component. */
static FsEntry *fn1_subdir_entry(char far *fn1)
{
    static char name[13];
    char far *p;
    int i;
    if (!fn1_has_prefix(fn1, "T:\\")) return NULL;
    p = fn1 + 3;
    for (i = 0; i < 12 && p[i] && p[i] != '\\'; i++) name[i] = (char)p[i];
    if (p[i] != '\\') return NULL;
    name[i] = '\0';
    return fs_find_root(name);
}

/*
 * do_chdir: INT 2Fh AX=1105h CHDIR.
 * Validates the target path from SDA->fn1.  DOS updates the CDS itself on
 * success; we only decide whether the directory exists.
 * Returns 0 (success) or 3 (path not found, CF=1).
 */
unsigned int __cdecl do_chdir(void)
{
    char far *fn1;
    int i;
    if (!glob_sdaptr) return 3;
    fn1 = glob_sdaptr + 0x9E;

    if (fn1_eq(fn1, "T:\\") || fn1_eq(fn1, "T:")) {
        if (rbuf.enabled) rb_write("2F 1105 CHDIR OK ROOT\r\n");
        return 0;
    }
    if (fn1_has_prefix(fn1, "T:\\")) {
        for (i = 0; i < fs_get_root_count(); i++) {
            FsEntry *e = fs_get_root_entry(i);
            if ((e->attr & 0x10) && last_component_is(e->name)) {
                if (rbuf.enabled) { rb_write("2F 1105 CHDIR OK "); rb_write(e->name); rb_write("\r\n"); }
                return 0;
            }
        }
    }
    if (rbuf.enabled) rb_write("2F 1105 CHDIR FAIL\r\n");
    return 3;
}

/*
 * do_findfirst: handle INT 2Fh AX=111Bh (FINDFIRST).
 *
 * Follows EtherDFS approach: use glob_sdaptr (not ES:DI) for FINDFIRST.
 *
 * Classification uses BOTH fn1 (SDA+0x9E, full canonical path) AND the FCB
 * template (SDA+0x22B).  A wildcard template alone is insufficient because
 * "T:\*.*" and "T:\GAMES\*.*" both produce "???????????".
 *
 * Decision table:
 *   fn1 starts with "T:\README.TXT\" → invalid (AX=3, CF=1)
 *   fn1 starts with "T:\GAMES\"      → GAMES dir is empty (AX=12h, CF=1)
 *   fn1 starts with "T:\"            → root context; dispatch on template:
 *       ???????????  → return GAMES, dir_entry=1 (FINDNEXT → README)
 *       README  TXT  → return README.TXT, dir_entry=2 (FINDNEXT → EOF)
 *       GAMES        → return GAMES, dir_entry=2 (FINDNEXT → EOF)
 *       other        → AX=12h, CF=1
 *   anything else   → AX=12h, CF=1
 *
 * Returns 0 on success (AX=0 CF=0), 0x12 or 3 on error (CF=1).
 */
unsigned int __cdecl do_findfirst(void)
{
    char far *sda = glob_sdaptr;
    char far *fn1;
    char far *tmpl;
    char far *dta;
    char far *found;
    unsigned char srch_attr;
    unsigned int dta_off, dta_seg;
    unsigned char dir_entry_next;
    FsEntry *sub;   /* result of fn1_subdir_entry — not passed by address */
    int idx, k;

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
        dir_entry_next = (unsigned char)fs_get_root_count();
    } else if ((sub = fn1_subdir_entry(fn1)) != NULL) {
        if (sub->attr & 0x10) {
            if (rbuf.enabled) rb_write("2F 111B SUBDIR EMPTY\r\n");
            return 0x12;
        }
        if (rbuf.enabled) rb_write("2F 111B INVALID\r\n");
        return 3;
    } else if (!fn1_has_prefix(fn1, "T:\\")) {
        if (rbuf.enabled) rb_write("2F 111B UNKNOWN PATH\r\n");
        return 0x12;
    } else if (is_wildcard_template(tmpl)) {
        if (rbuf.enabled) rb_write("2F 111B FF ROOT FIRST\r\n");
        fill_found_from_entry(fs_get_root_entry(0), found);
        dir_entry_next = 1;
    } else {
        idx = fs_find_by_tmpl(tmpl);
        if (idx < 0) {
            if (rbuf.enabled) rb_write("2F 111B NOMATCH\r\n");
            return 0x12;
        }
        if (rbuf.enabled) { rb_write("2F 111B FF "); rb_write(root_entries[idx].name); rb_write("\r\n"); }
        fill_found_from_entry(fs_get_root_entry(idx), found);
        dir_entry_next = (unsigned char)fs_get_root_count();
    }

    dta_off = (unsigned int)(unsigned char)sda[0x0C]
            | ((unsigned int)(unsigned char)sda[0x0D] << 8);
    dta_seg = (unsigned int)(unsigned char)sda[0x0E]
            | ((unsigned int)(unsigned char)sda[0x0F] << 8);
    dta = (char far *)MK_FP(dta_seg, dta_off);

    dta[0] = (char)(DRIVE_T_IDX | 0x80);
    for (k = 0; k < 11; k++) dta[1+k] = tmpl[k];
    dta[12] = (char)srch_attr;
    dta[13] = dir_entry_next; dta[14] = 0;
    dta[15] = 0; dta[16] = 0;
    dta[17] = 0; dta[18] = 0; dta[19] = 0; dta[20] = 0;
    for (k = 0; k < 32; k++) dta[0x15+k] = found[k];

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
    int idx = (unsigned char)dta[13];
    int k;

    if (idx >= fs_get_root_count()) {
        if (rbuf.enabled) rb_write("2F 111C EOF\r\n");
        return 0x12;
    }

    if (rbuf.enabled) { rb_write("2F 111C FINDNEXT "); rb_write(root_entries[idx].name); rb_write("\r\n"); }

    for (k = 0; k < 32; k++) found[k] = 0;
    fill_found_from_entry(&root_entries[idx], found);

    dta[13] = (char)(idx + 1); dta[14] = 0;

    for (k = 0; k < 32; k++) dta[0x15+k] = found[k];

    return 0;
}

/* ------------------------------------------------------------------ */
/*  INT 2Fh file I/O handlers: GETATTR / OPEN / READ / CLOSE           */
/* ------------------------------------------------------------------ */

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
    int i;
    if (!glob_sdaptr) return 0xFFFF;
    if (rbuf.enabled) {
        rb_write("2F 110F \"");
        rb_far_str(FP_SEG(glob_sdaptr + 0x9E), FP_OFF(glob_sdaptr + 0x9E), 32);
        rb_write("\"\r\n");
    }
    for (i = 0; i < fs_get_root_count(); i++)
        if (last_component_is(root_entries[i].name)) return root_entries[i].attr;
    return 0xFFFF;
}

/*
 * do_open: INT 2Fh AX=1116h OPEN.
 * Returns 0 on success, 5 (access denied) for directories, 2 (not found) otherwise.
 * SFT layout is filled by fill_sft_entry(); see its comment for field details.
 */
unsigned int __cdecl do_open(unsigned int es_val, unsigned int di_val)
{
    int i;
    for (i = 0; i < fs_get_root_count(); i++) {
        if (!last_component_is(root_entries[i].name)) continue;
        if (root_entries[i].attr & 0x10) {
            if (rbuf.enabled) rb_write("2F 1116 OPEN DENIED\r\n");
            return 5;
        }
        if (rbuf.enabled) rb_write("2F 1116 OPEN OK\r\n");
        fill_sft_entry(i, (char far *)MK_FP(es_val, di_val));
        return 0;
    }
    if (rbuf.enabled) rb_write("2F 1116 OPEN NOTFOUND\r\n");
    return 2;
}

/*
 * do_spopen: INT 2Fh AX=112Eh SPOPNFIL (Special Open).
 * Used by TYPE and COPY in MS-DOS 5.0/6.x instead of regular OPEN.
 * Same SFT fill; caller (ASM) sets CX=1 (action: file opened) on success.
 */
unsigned int __cdecl do_spopen(unsigned int es_val, unsigned int di_val)
{
    int i;
    for (i = 0; i < fs_get_root_count(); i++) {
        if (!last_component_is(root_entries[i].name)) continue;
        if (root_entries[i].attr & 0x10) {
            if (rbuf.enabled) rb_write("2F 112E SPOP DENIED\r\n");
            return 5;
        }
        if (rbuf.enabled) rb_write("2F 112E SPOP OK\r\n");
        fill_sft_entry(i, (char far *)MK_FP(es_val, di_val));
        return 0;
    }
    if (rbuf.enabled) rb_write("2F 112E SPOP NOTFOUND\r\n");
    return 2;
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
    FsEntry *e;
    int idx;

    idx = (unsigned char)sft[0x0B];
    if (idx >= fs_get_root_count()) return 0;
    e = fs_get_root_entry(idx);
    if (!e->content) return 0;

    dta_off = (unsigned int)(unsigned char)sda[0x0C]
            | ((unsigned int)(unsigned char)sda[0x0D] << 8);
    dta_seg = (unsigned int)(unsigned char)sda[0x0E]
            | ((unsigned int)(unsigned char)sda[0x0F] << 8);
    buf = (char far *)MK_FP(dta_seg, dta_off);

    pos = (unsigned long)(unsigned char)sft[0x15]
        | ((unsigned long)(unsigned char)sft[0x16] << 8)
        | ((unsigned long)(unsigned char)sft[0x17] << 16)
        | ((unsigned long)(unsigned char)sft[0x18] << 24);

    if (pos >= e->size) {
        if (rbuf.enabled) rb_write("2F 1108 EOF\r\n");
        return 0;
    }
    avail = (unsigned int)(e->size - pos);
    count = (cx_val < avail) ? cx_val : avail;

    for (i = 0; i < count; i++) buf[i] = e->content[(unsigned int)pos + i];

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

/* do_diskspace: INT 2Fh AX=110Ch DISKSPACE — log only; registers set in ASM. */
void __cdecl do_diskspace(void)
{
    if (rbuf.enabled) rb_write("2F 110C DISKSPACE 16MB\r\n");
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

    init_cds();

    old_int2f = _dos_getvect(0x2F);
    init_int2f_ptr_();
    _dos_setvect(0x2F, (void (__interrupt __far *)())new_int2f_);

    paras = calc_resident_paras();
    seg   = get_ds();
    off   = (unsigned int)&rbuf;
    printf("TNFSDRV: virtual drive T:\r\n");
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
