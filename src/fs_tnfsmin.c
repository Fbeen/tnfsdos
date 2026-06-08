/*
 * FS_TNFSMIN.C — Minimal live TNFS backend for TNFSDRV.
 *
 * Uses tnfs_opendirx / tnfs_nextdirx / tnfs_closedir (cmd 0x17/0x18).
 * Supports root directory enumeration only — no subdirs, no file I/O.
 */

#include <i86.h>
#include <stddef.h>
#include <string.h>
#include "fs.h"
#include "ringbuf.h"
#include "tnfs.h"

/* Drive letter — initialised from compile-time default, overridden by fs_set_drive(). */
unsigned char g_drive_idx     = (unsigned char)DRIVE_IDX;
static char   s_drv_prefix[4] = { (char)TNFSDRV_DRIVE_LETTER, ':', '\\', '\0' };
static char   s_drv_root[3]   = { (char)TNFSDRV_DRIVE_LETTER, ':', '\0' };

void fs_set_drive(char letter)
{
    g_drive_idx     = (unsigned char)(letter - 'A');
    s_drv_prefix[0] = letter;
    s_drv_root[0]   = letter;
}

/* ------------------------------------------------------------------ */
/*  Stack canary diagnostics (symbols exported from handler.asm)       */
/* ------------------------------------------------------------------ */

#ifdef TNFSDRV_DEBUG_RINGBUF
extern unsigned int  tsr_stack_lo_canary;
extern unsigned int  tsr_stack_hi_canary;
extern unsigned char tsr_stack[];
#define TSR_STACK_SIZE 8192

static void log_stack_state(void)
{
    unsigned int i, used;
    for (i = 0; i < TSR_STACK_SIZE; i++) {
        if (tsr_stack[i] != (unsigned char)0xCC) break;
    }
    used = TSR_STACK_SIZE - i;
    rb_write("STK lo="); rb_hex16(tsr_stack_lo_canary);
    rb_write(" hi="); rb_hex16(tsr_stack_hi_canary);
    rb_write(" used="); rb_dec(used);
    rb_write("/8192\r\n");
    if (tsr_stack_lo_canary != 0xA55Au) rb_write("*** STACK OVERFLOW ***\r\n");
    if (tsr_stack_hi_canary != 0x5AA5u) rb_write("*** HI CANARY CORRUPT ***\r\n");
}
#else
#define log_stack_state() ((void)0)
#endif

/* ------------------------------------------------------------------ */
/*  Directory cache — loaded once at first FINDFIRST, then reused      */
/* ------------------------------------------------------------------ */

#define DIR_CACHE_MAX  32

typedef struct {
    char          name[13];  /* 8.3 + NUL, uppercased */
    unsigned char attr;
} DirEntry;

static DirEntry s_dir[DIR_CACHE_MAX];
static int      s_dir_count;
static int      s_cache_valid;

/* ------------------------------------------------------------------ */
/*  Path helpers                                                        */
/* ------------------------------------------------------------------ */

static int fn1_eq(const char far *fn1, const char *s)
{
    int i;
    for (i = 0; s[i]; i++) {
        char fc = fn1[i], pc = s[i];
        if (!fc) return 0;
        if (fc >= 'a' && fc <= 'z') fc -= 32;
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (fc != pc) return 0;
    }
    return (fn1[i] == '\0' || (fn1[i] == '\\' && fn1[i+1] == '\0'));
}

static int fn1_has_prefix(const char far *fn1)
{
    int i;
    for (i = 0; s_drv_prefix[i]; i++) {
        char fc = fn1[i], pc = s_drv_prefix[i];
        if (!fc) return 0;
        if (fc >= 'a' && fc <= 'z') fc -= 32;
        if (pc >= 'a' && pc <= 'z') pc -= 32;
        if (fc != pc) return 0;
    }
    return 1;
}

/* Returns 1 if fn1 is root-level (no backslash after the "X:\" prefix).
 * "N:\*.TXT" and "N:\????????.???" are root-level.
 * "N:\SUBDIR\" and "N:\SUBDIR\FILE.TXT" are not.
 * Prevents unnecessary tnfs_opendir calls for subdirectory paths we don't support. */
static int fn1_is_root_level(const char far *fn1)
{
    int i;
    for (i = 0; s_drv_prefix[i]; i++) ; /* skip "N:\" prefix (3 chars) */
    for (; fn1[i]; i++) {
        if (fn1[i] == '\\') return 0;
    }
    return 1;
}

int fs_is_root(const char far *path)
{
    return fn1_eq(path, s_drv_prefix) || fn1_eq(path, s_drv_root);
}

/* ------------------------------------------------------------------ */
/*  Name helpers                                                        */
/* ------------------------------------------------------------------ */

static int is_dot_entry(const char *name)
{
    return (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')));
}

static int is_8dot3(const char *name)
{
    int base = 0, ext = 0, dot = 0;
    for (; *name; name++) {
        if (*name == '.') {
            if (dot) return 0;
            dot = 1;
        } else if (dot) {
            if (++ext > 3) return 0;
        } else {
            if (++base > 8) return 0;
        }
    }
    return (base > 0);
}

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

static int tmpl_matches(const char *tmpl, const char *fcb)
{
    int i;
    for (i = 0; i < 11; i++)
        if (tmpl[i] != '?' && tmpl[i] != fcb[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Root directory loader                                               */
/* ------------------------------------------------------------------ */

static int load_root_min(void)
{
    static struct dirx_data data;
    static struct dirx_item xitem;
    int rc, i;
    char c;

    s_dir_count = 0;

    rc = tnfs_opendirx("/", "*.*", 0, 0, &data);
    if (rc < 0) {
        if (rbuf.enabled) {
            rb_write("MIN FAIL ");
            rb_write(rc == -(int)TNFS_EPROTO ? "TIMEOUT" : "ERR");
            rb_write("\r\n");
        }
        return 0;
    }

    for (;;) {
        if (s_dir_count >= DIR_CACHE_MAX) break;

        rc = tnfs_nextdirx(&data, &xitem);
        if (rc != 0) break;   /* TNFS_EOF or network error */

        if (is_dot_entry(xitem.name)) continue;

        if (!is_8dot3(xitem.name)) {
            if (rbuf.enabled) {
                rb_write("MIN skip \""); rb_write(xitem.name); rb_write("\"\r\n");
            }
            continue;
        }

        /* Uppercase the name into the cache slot */
        for (i = 0; xitem.name[i] && i < 12; i++) {
            c = xitem.name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            s_dir[s_dir_count].name[i] = c;
        }
        s_dir[s_dir_count].name[i] = '\0';
        s_dir[s_dir_count].attr = (xitem.flags & TNFS_DIRENTRY_DIR) ? 0x10 : 0x20;

        if (rbuf.enabled) {
            rb_write("MIN+ \""); rb_write(s_dir[s_dir_count].name); rb_write("\"\r\n");
        }
        s_dir_count++;
    }

    if (rbuf.enabled && rc != 0 && rc != TNFS_EOF) {
        rb_write("MIN ERR rc="); rb_hex8((unsigned char)rc); rb_write("\r\n");
    }

    tnfs_closedir(data.handle);

    if (rbuf.enabled) {
        rb_write("MIN n="); rb_dec((unsigned)s_dir_count); rb_write("\r\n");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Public fs.h interface                                               */
/* ------------------------------------------------------------------ */

int fs_resolve(const char far *path, FsNode *node)
{
    /* Not implemented: TNFSMIN root-only, no GETATTR/OPEN for individual files */
    (void)path; (void)node;
    return 0;
}

int           fs_is_dir  (const FsNode *node) { (void)node; return 0; }
unsigned char fs_get_attr(const FsNode *node) { (void)node; return 0x20; }
unsigned long fs_get_size(const FsNode *node) { (void)node; return 0; }

const char *fs_get_name(const FsNode *node)
{
    if (node->idx >= 0 && node->idx < s_dir_count)
        return s_dir[node->idx].name;
    return "";
}

void fs_fill_found(const FsNode *node, char far *found)
{
    static char fcb[11];
    const DirEntry *e;
    int k;
    if (node->idx < 0 || node->idx >= s_dir_count) return;
    e = &s_dir[node->idx];
    entry_to_fcb(e->name, fcb);
    for (k = 0;  k < 11; k++) found[k] = fcb[k];
    found[11] = e->attr;
    for (k = 12; k < 32; k++) found[k] = 0;
}

void fs_open(const FsNode *node, FsHandle *handle)
{
    handle->dir_ctx = node->dir_ctx;
    handle->idx     = node->idx;
}

unsigned int fs_read(const FsHandle *handle, unsigned long pos,
                     char far *buf, unsigned int n)
{
    /* No file I/O in TNFSMIN */
    (void)handle; (void)pos; (void)buf; (void)n;
    return 0;
}

void sft_fill_handle(const FsHandle *handle, char far *sft)
{
    static char fcb[11];
    const DirEntry *e;
    int k;
    if (handle->idx < 0 || handle->idx >= s_dir_count) return;
    e = &s_dir[handle->idx];
    entry_to_fcb(e->name, fcb);
    sft[0x04] = e->attr;
    sft[0x05] = (char)g_drive_idx;
    sft[0x06] = 0x80;
    for (k = 0x07; k <= 0x2A; k++) sft[k] = 0;
    for (k = 0;    k < 11;   k++) sft[0x20+k] = fcb[k];
}

int fs_enum_begin(const char far *path, const char far *tmpl, FsDirEnum *de)
{
    int k;
    for (k = 0; k < 11; k++) de->tmpl[k] = (char)tmpl[k];
    de->next_idx = 0;
    de->dir_ctx  = 0;

    log_stack_state();

    if (!fn1_has_prefix(path)) return 0;

    if (!fn1_is_root_level(path)) {
        if (rbuf.enabled) rb_write("EB: subdir\r\n");
        return 0;
    }

    if (!s_cache_valid) {
        if (!load_root_min()) return 0;
        s_cache_valid = 1;
    }
    return 1;
}

int fs_enum_next(FsDirEnum *de, FsNode *node)
{
    static char fcb[11];
    while (de->next_idx < s_dir_count) {
        int i = de->next_idx++;
        entry_to_fcb(s_dir[i].name, fcb);
        if (!tmpl_matches(de->tmpl, fcb)) continue;
        node->dir_ctx = 0;
        node->idx     = i;
        return 1;
    }
    return 0;
}
