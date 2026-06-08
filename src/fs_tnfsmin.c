/*
 * FS_TNFSMIN.C — Atomic-load TNFS backend for TNFSDRV.
 *
 * Directory loading is atomic: OPENDIRX → READDIRX* → CLOSEDIR, all before
 * returning from load_dir_cache().  No TNFS handle is ever left open after
 * load_dir_cache() returns.  FindFirst/FindNext work entirely from cache.
 *
 * FsNode.dir_ctx / FsDirEnum.dir_ctx: always 0 (single cache).
 * FsNode.idx: index into s_active_entries[].
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
/*  Single active directory cache                                       */
/* ------------------------------------------------------------------ */

#define DIR_CACHE_MAX  32

typedef struct {
    char          name[13];  /* 8.3 + NUL, uppercased */
    unsigned char attr;
    unsigned long size;
} DirEntry;

static char     s_active_path[128];           /* TNFS path, e.g. "/" or "/APAC" */
static uint8_t  s_active_valid;
static DirEntry s_active_entries[DIR_CACHE_MAX];
static int      s_active_count;

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

/* Returns 1 if fn1 has no backslash after the "X:\" prefix. */
static int fn1_is_root_level(const char far *fn1)
{
    int i;
    for (i = 0; s_drv_prefix[i]; i++) ;
    for (; fn1[i]; i++) {
        if (fn1[i] == '\\') return 0;
    }
    return 1;
}

/* Parse a DOS FindFirst path into a clean TNFS dir path and template string.
 * The last component becomes the template if it contains '?' or '*';
 * otherwise it is part of the directory path (no-wildcard CHDIR case).
 *   "N:\????????.???"       → dir="/",        tmpl="????????.???"
 *   "N:\APAC\????????"      → dir="/APAC",     tmpl="????????"
 *   "N:\APAC\SRC\*.TXT"     → dir="/APAC/SRC", tmpl="*.TXT"
 *   "N:\APAC"               → dir="/APAC",     tmpl=""
 * out_dir: 128 bytes, out_tmpl: 14 bytes. */
static void dos_find_path_to_tnfs_dir_and_template(
    const char far *raw,
    char *out_dir,
    char *out_tmpl)
{
    int i, j, last_bs, has_wc, start;
    char c;

    for (i = 0; s_drv_prefix[i]; i++) ; /* skip "N:\" prefix */

    last_bs = -1;
    for (j = i; raw[j]; j++)
        if (raw[j] == '\\') last_bs = j;

    start = (last_bs >= 0) ? last_bs + 1 : i;
    has_wc = 0;
    for (j = start; raw[j]; j++)
        if (raw[j] == '?' || raw[j] == '*') { has_wc = 1; break; }

    if (has_wc) {
        /* template = last component, dir = path up to last_bs */
        for (j = 0; raw[start + j] && j < 13; j++)
            out_tmpl[j] = raw[start + j];
        out_tmpl[j] = '\0';

        if (last_bs < 0) {
            out_dir[0] = '/'; out_dir[1] = '\0';
        } else {
            out_dir[0] = '/';
            for (j = 0; (i + j) < last_bs && j < 126; j++) {
                c = raw[i + j];
                if (c == '\\') c = '/';
                if (c >= 'a' && c <= 'z') c -= 32;
                out_dir[1 + j] = c;
            }
            out_dir[1 + j] = '\0';
        }
    } else {
        /* no wildcard: split on last backslash; SDA FCB tmpl handles matching */
        out_tmpl[0] = '\0';
        if (last_bs < 0) {
            out_dir[0] = '/'; out_dir[1] = '\0';
        } else {
            out_dir[0] = '/';
            for (j = 0; (i + j) < last_bs && j < 126; j++) {
                c = raw[i + j];
                if (c == '\\') c = '/';
                if (c >= 'a' && c <= 'z') c -= 32;
                out_dir[1 + j] = c;
            }
            out_dir[1 + j] = '\0';
        }
    }
}

/* Split fn1 into TNFS dir path + entry name (both into out buffers).
 *   "N:\APAC"          → dir="/",     entry="APAC"
 *   "N:\APAC\FILE.TXT" → dir="/APAC", entry="FILE.TXT"
 * dir_out: 128 bytes, entry_out: 13 bytes. Returns 1 on success. */
static int fn1_split(const char far *fn1, char *dir_out, char *entry_out)
{
    int i, j, last_bs;
    char c;

    if (!fn1_has_prefix(fn1)) return 0;
    for (i = 0; s_drv_prefix[i]; i++) ;

    last_bs = -1;
    for (j = i; fn1[j]; j++) {
        if (fn1[j] == '\\') last_bs = j;
    }

    if (last_bs < 0) {
        /* "N:\APAC" — entry is directly under root */
        dir_out[0] = '/'; dir_out[1] = '\0';
        for (j = 0; fn1[i + j] && j < 12; j++) {
            c = fn1[i + j];
            if (c >= 'a' && c <= 'z') c -= 32;
            entry_out[j] = c;
        }
        entry_out[j] = '\0';
    } else {
        /* "N:\APAC\FILE.TXT" */
        dir_out[0] = '/';
        for (j = 0; (i + j) < last_bs && j < 126; j++) {
            c = fn1[i + j];
            if (c == '\\') c = '/';
            if (c >= 'a' && c <= 'z') c -= 32;
            dir_out[1 + j] = c;
        }
        dir_out[1 + j] = '\0';
        for (j = 0; fn1[last_bs + 1 + j] && j < 12; j++) {
            c = fn1[last_bs + 1 + j];
            if (c >= 'a' && c <= 'z') c -= 32;
            entry_out[j] = c;
        }
        entry_out[j] = '\0';
    }

    return (entry_out[0] != '\0') ? 1 : 0;
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
/*  Single-active-directory cache loader                                */
/* ------------------------------------------------------------------ */

static int load_dir_cache(const char *path)
{
    static struct dirx_data data;
    static struct dirx_item xitem;
    uint8_t handle;
    int rc, i;
    char c;

    if (rbuf.enabled) { rb_write("DC_REQ path=\""); rb_write(path); rb_write("\"\r\n"); }

    /* Cache hit — same directory already loaded, no TNFS needed */
    if (s_active_valid && strcmp(s_active_path, path) == 0) {
        if (rbuf.enabled) { rb_write("DC_HIT path=\""); rb_write(path); rb_write("\"\r\n"); }
        return 1;
    }

    /* Invalidate before loading; will only become valid after CLOSEDIR OK */
    s_active_valid = 0;
    s_active_count = 0;

    if (rbuf.enabled) { rb_write("DC_LOAD path=\""); rb_write(path); rb_write("\"\r\n"); }

    rc = tnfs_opendirx((char *)path, "*.*", 0, 1, &data);
    if (rc < 0) {
        if (rbuf.enabled) { rb_write("DC_FAIL path=\""); rb_write(path); rb_write("\"\r\n"); }
        return 0;
    }
    handle = data.handle;
    if (rbuf.enabled) { rb_write("XDIR open h="); rb_hex8(handle); rb_write("\r\n"); }

    rc = 0;
    for (;;) {
        if (s_active_count >= DIR_CACHE_MAX) break;
        rc = tnfs_nextdirx(&data, &xitem);
        if (rc != 0) break;

        if (is_dot_entry(xitem.name)) continue;
        if (!is_8dot3(xitem.name)) continue;

        for (i = 0; xitem.name[i] && i < 12; i++) {
            c = xitem.name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            s_active_entries[s_active_count].name[i] = c;
        }
        s_active_entries[s_active_count].name[i] = '\0';
        s_active_entries[s_active_count].attr = (xitem.flags & TNFS_DIRENTRY_DIR) ? 0x10 : 0x20;
        s_active_entries[s_active_count].size = (unsigned long)xitem.size;
        if (rbuf.enabled) {
            rb_write("XDIR entry \""); rb_write(s_active_entries[s_active_count].name);
            rb_write("\"\r\n");
        }
        s_active_count++;
    }

    if (rc != 0 && rc != (int)TNFS_EOF) {
        if (rbuf.enabled) { rb_write("DC_FAIL path=\""); rb_write(path); rb_write("\" reason=READ\r\n"); }
        tnfs_closedir(handle);   /* best-effort close; result ignored */
        return 0;
    }

    if (rbuf.enabled) { rb_write("XDIR EOF\r\n"); }

    /* Atomic close — must succeed before cache becomes valid */
    if (rbuf.enabled) { rb_write("XDIR close h="); rb_hex8(handle); rb_write("\r\n"); }
    rc = tnfs_closedir(handle);
    if (rc != 0) {
        if (rbuf.enabled) { rb_write("XDIR close TIMEOUT after EOF\r\n"); }
        return 0;
    }
    if (rbuf.enabled) { rb_write("XDIR close OK\r\n"); }

    strcpy(s_active_path, path);
    s_active_valid = 1;
    if (rbuf.enabled) { rb_write("DC_READY path=\""); rb_write(path); rb_write("\" n="); rb_dec((unsigned)s_active_count); rb_write("\r\n"); }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Public fs.h interface                                               */
/* ------------------------------------------------------------------ */

int fs_resolve(const char far *path, FsNode *node)
{
    static char dir_path[128]; /* static = DS-relative, safe under -zu */
    static char entry[13];
    int i;

    if (!fn1_split(path, dir_path, entry)) return 0;

    if (rbuf.enabled) { rb_write("RES \""); rb_write(entry); rb_write("\" in \""); rb_write(dir_path); rb_write("\"\r\n"); }

    if (!load_dir_cache(dir_path)) return 0;

    for (i = 0; i < s_active_count; i++) {
        if (strcmp(entry, s_active_entries[i].name) == 0) {
            node->dir_ctx = 0;
            node->idx     = i;
            return 1;
        }
    }

    if (rbuf.enabled) rb_write("RES miss\r\n");
    return 0;
}

int fs_is_dir(const FsNode *node)
{
    if (node->idx < 0 || node->idx >= s_active_count) return 0;
    return (s_active_entries[node->idx].attr & 0x10) ? 1 : 0;
}

unsigned char fs_get_attr(const FsNode *node)
{
    if (node->idx < 0 || node->idx >= s_active_count) return 0x20;
    return s_active_entries[node->idx].attr;
}

unsigned long fs_get_size(const FsNode *node)
{
    if (node->idx < 0 || node->idx >= s_active_count) return 0;
    return s_active_entries[node->idx].size;
}

const char *fs_get_name(const FsNode *node)
{
    if (node->idx >= 0 && node->idx < s_active_count)
        return s_active_entries[node->idx].name;
    return "";
}

void fs_fill_found(const FsNode *node, char far *found)
{
    static char fcb[11];
    unsigned long sz;
    int k;
    if (node->idx < 0 || node->idx >= s_active_count) return;
    entry_to_fcb(s_active_entries[node->idx].name, fcb);
    for (k = 0;  k < 11; k++) found[k] = fcb[k];
    found[11] = s_active_entries[node->idx].attr;
    for (k = 12; k < 28; k++) found[k] = 0;
    sz = s_active_entries[node->idx].size;
    found[28] = (char)(sz);
    found[29] = (char)(sz >> 8);
    found[30] = (char)(sz >> 16);
    found[31] = (char)(sz >> 24);
}

static char s_read_buf[512]; /* intermediate buffer: tnfs_read (near) → far DTA copy */

void fs_open(const FsNode *node, FsHandle *handle)
{
    static char tnfs_path[128];
    const char *name;
    int i, rc;

    handle->dir_ctx = node->dir_ctx;
    handle->idx     = node->idx;
    handle->tnfs_fd = 0xFF;

    name = s_active_entries[node->idx].name;

    /* Build TNFS path: s_active_path + "/" + name, avoiding double slash at root */
    i = 0;
    if (s_active_path[0] == '/' && s_active_path[1] == '\0') {
        tnfs_path[i++] = '/';
    } else {
        int j = 0;
        while (s_active_path[j] && i < 120) tnfs_path[i++] = s_active_path[j++];
        tnfs_path[i++] = '/';
    }
    while (*name && i < 126) tnfs_path[i++] = *name++;
    tnfs_path[i] = '\0';

    if (rbuf.enabled) { rb_write("FS_OPEN "); rb_write(tnfs_path); rb_write("\r\n"); }

    rc = tnfs_open(tnfs_path, 0x0001, 0); /* O_RDONLY */
    if (rc < 0) {
        if (rbuf.enabled) { rb_write("FS_OPEN FAIL\r\n"); }
        return;
    }
    handle->tnfs_fd = (uint8_t)rc;
    if (rbuf.enabled) { rb_write("FS_OPEN fd="); rb_hex8(handle->tnfs_fd); rb_write("\r\n"); }
}

unsigned int fs_read(const FsHandle *handle, unsigned long pos,
                     char far *buf, unsigned int n)
{
    unsigned int total = 0, chunk, got, i;

    if (handle->tnfs_fd == 0xFF) return 0;
    tnfs_lseek(handle->tnfs_fd, 0, (uint32_t)pos);

    while (total < n) {
        chunk = n - total;
        if (chunk > sizeof(s_read_buf)) chunk = (unsigned int)sizeof(s_read_buf);
        got = (unsigned int)tnfs_read(s_read_buf, handle->tnfs_fd, (uint16_t)chunk);
        if (got == 0) break;
        for (i = 0; i < got; i++) buf[total + i] = s_read_buf[i];
        total += got;
        if (got < chunk) break;
    }
    return total;
}

void fs_close(const FsHandle *handle)
{
    if (handle->tnfs_fd == 0xFF) return;
    if (rbuf.enabled) { rb_write("FS_CLOSE fd="); rb_hex8(handle->tnfs_fd); rb_write("\r\n"); }
    tnfs_close(handle->tnfs_fd);
}

void sft_fill_handle(const FsHandle *handle, const FsNode *node, char far *sft)
{
    static char fcb[11];
    unsigned long sz;
    int k;
    if (node->idx < 0 || node->idx >= s_active_count) return;
    entry_to_fcb(s_active_entries[node->idx].name, fcb);
    sft[0x04] = s_active_entries[node->idx].attr;
    sft[0x05] = (char)g_drive_idx;
    sft[0x06] = 0x80;
    sft[0x07] = (char)handle->tnfs_fd;  /* TNFS file handle for read/close */
    for (k = 0x08; k <= 0x10; k++) sft[k] = 0;
    sz = s_active_entries[node->idx].size;
    sft[0x11] = (char)(sz);
    sft[0x12] = (char)(sz >> 8);
    sft[0x13] = (char)(sz >> 16);
    sft[0x14] = (char)(sz >> 24);
    for (k = 0x15; k <= 0x1F; k++) sft[k] = 0;
    for (k = 0;    k < 11;   k++) sft[0x20+k] = fcb[k];
    for (k = 0x2B; k <= 0x35; k++) sft[k] = 0;
}

int fs_enum_begin(const char far *path, const char far *tmpl, FsDirEnum *de)
{
    static char enum_path[128]; /* static = DS-relative, safe under -zu */
    static char enum_tmpl[14];  /* static = DS-relative, safe under -zu */
    int k;

    for (k = 0; k < 11; k++) de->tmpl[k] = (char)tmpl[k];
    de->next_idx = 0;
    de->dir_ctx  = 0;

    log_stack_state();

    if (!fn1_has_prefix(path)) return 0;

    dos_find_path_to_tnfs_dir_and_template(path, enum_path, enum_tmpl);

    if (rbuf.enabled) {
        int k;
        rb_write("FF raw=\"");
        for (k = 0; path[k] && k < 40; k++) rb_putc(path[k]);
        rb_write("\"\r\n");
        rb_write("FF dir=\""); rb_write(enum_path); rb_write("\"\r\n");
        rb_write("FF tmpl=\""); rb_write(enum_tmpl); rb_write("\"\r\n");
    }

    if (!load_dir_cache(enum_path)) return 0;

    return 1;
}

int fs_enum_next(FsDirEnum *de, FsNode *node)
{
    static char fcb[11];

    while (de->next_idx < s_active_count) {
        int i = de->next_idx++;
        entry_to_fcb(s_active_entries[i].name, fcb);
        if (!tmpl_matches(de->tmpl, fcb)) continue;
        node->dir_ctx = 0;
        node->idx     = i;
        return 1;
    }
    return 0;
}
