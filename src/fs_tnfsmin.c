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
static uint16_t s_active_loaded_ticks;        /* BIOS tick count at last load */

/* Cache configuration (set by fs_set_cache_config before TSR install) */
static uint8_t  g_cache_enabled  = 1;
static uint16_t g_cache_ttl_secs = 300;
static uint8_t  g_cache_dirs     = 1;

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
/*  Cache helpers                                                       */
/* ------------------------------------------------------------------ */

static uint16_t get_bios_ticks(void)
{
    return *(uint16_t far *)MK_FP(0x0040, 0x006C);
}

void cache_invalidate(const char *reason)
{
    if (rbuf.enabled) { rb_write("DC_INVALIDATE reason="); rb_write(reason); rb_write("\r\n"); }
    s_active_valid = 0;
}

void fs_set_cache_config(uint8_t enabled, uint16_t ttl_secs, uint8_t dirs)
{
    g_cache_enabled  = enabled;
    g_cache_ttl_secs = ttl_secs;
    g_cache_dirs     = (dirs > 1) ? 1 : dirs;
    if (rbuf.enabled) {
        rb_write("DC_CFG enabled="); rb_dec(g_cache_enabled);
        rb_write(" ttl="); rb_dec(g_cache_ttl_secs);
        rb_write(" dirs="); rb_dec(g_cache_dirs); rb_write("\r\n");
    }
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

    if (!g_cache_enabled) {
        /* Cache disabled — always load fresh */
        if (rbuf.enabled) { rb_write("DC_OFF path=\""); rb_write(path); rb_write("\"\r\n"); }
    } else if (s_active_valid && strcmp(s_active_path, path) == 0) {
        /* Same directory — check TTL */
        uint16_t now   = get_bios_ticks();
        uint16_t age_t = (uint16_t)(now - s_active_loaded_ticks);
        uint16_t age_s = age_t / 18u;
        if (age_s < g_cache_ttl_secs) {
            if (rbuf.enabled) {
                rb_write("DC_HIT path=\""); rb_write(path);
                rb_write("\" age="); rb_dec(age_s); rb_write("\r\n");
            }
            return 1;
        }
        if (rbuf.enabled) {
            rb_write("DC_MISS path=\""); rb_write(path);
            rb_write("\" reason=expired age="); rb_dec(age_s); rb_write("\r\n");
        }
    } else if (s_active_valid) {
        if (rbuf.enabled) {
            rb_write("DC_MISS path=\""); rb_write(path); rb_write("\" reason=path\r\n");
        }
    } else {
        if (rbuf.enabled) {
            rb_write("DC_MISS path=\""); rb_write(path); rb_write("\" reason=none\r\n");
        }
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
    s_active_loaded_ticks = get_bios_ticks();
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

    /* Fallback: TNFS STAT the file directly.
     * Needed for newly-created files not yet returned by READDIRX, and for
     * files without extensions that Linux *-dot-* glob silently omits. */
    {
        static char stat_path[128];
        static struct fstat st;
        int fp = 0, k;
        if (dir_path[0] == '/' && dir_path[1] == '\0') {
            stat_path[fp++] = '/';
        } else {
            for (k = 0; dir_path[k] && fp < 120; k++) stat_path[fp++] = dir_path[k];
            stat_path[fp++] = '/';
        }
        for (k = 0; entry[k] && fp < 126; k++) stat_path[fp++] = entry[k];
        stat_path[fp] = '\0';
        if (rbuf.enabled) { rb_write("RES STAT "); rb_write(stat_path); rb_write("\r\n"); }
        if (tnfs_stat(stat_path, &st) == 0 && s_active_count < DIR_CACHE_MAX) {
            if (rbuf.enabled) rb_write("RES STAT OK\r\n");
            for (k = 0; entry[k] && k < 12; k++) s_active_entries[s_active_count].name[k] = entry[k];
            s_active_entries[s_active_count].name[k] = '\0';
            s_active_entries[s_active_count].attr = (st.mode & 0x4000u) ? 0x10 : 0x20;
            s_active_entries[s_active_count].size = (unsigned long)st.size;
            node->dir_ctx = 0;
            node->idx     = s_active_count;
            s_active_count++;
            return 1;
        }
    }
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

static char s_read_buf[512];  /* tnfs_read (near) → far DTA */
static char s_write_buf[512]; /* far DTA → tnfs_write (near) */

void fs_open(const FsNode *node, FsHandle *handle, unsigned char dos_mode)
{
    static char tnfs_path[128];
    const char *name;
    uint16_t tnfs_flags;
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

    if (dos_mode == 1)      tnfs_flags = TNFS_O_WRONLY;
    else if (dos_mode == 2) tnfs_flags = TNFS_O_RDWR;
    else                    tnfs_flags = TNFS_O_RDONLY;

    if (rbuf.enabled) { rb_write("FS_OPEN "); rb_write(tnfs_path);
                        rb_write(" mode="); rb_hex8(dos_mode); rb_write("\r\n"); }

    rc = tnfs_open(tnfs_path, tnfs_flags, 0);
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
        int rc;
        chunk = n - total;
        if (chunk > sizeof(s_read_buf)) chunk = (unsigned int)sizeof(s_read_buf);
        rc = tnfs_read(s_read_buf, handle->tnfs_fd, (uint16_t)chunk);
        if (rc <= 0) break;  /* 0 = EOF, negative = error; never cast negative to unsigned */
        got = (unsigned int)rc;
        for (i = 0; i < got; i++) buf[total + i] = s_read_buf[i];
        total += got;
        if (got < chunk) break;
    }
    return total;
}

unsigned int fs_write(const FsHandle *handle, unsigned long pos,
                      const char far *buf, unsigned int n)
{
    unsigned int total = 0, chunk, i;
    int rc;

    if (handle->tnfs_fd == 0xFF) return 0;
    tnfs_lseek(handle->tnfs_fd, 0, (uint32_t)pos);

    while (total < n) {
        chunk = n - total;
        if (chunk > sizeof(s_write_buf)) chunk = (unsigned int)sizeof(s_write_buf);
        for (i = 0; i < chunk; i++) s_write_buf[i] = buf[total + i];
        rc = tnfs_write(s_write_buf, handle->tnfs_fd, (uint16_t)chunk);
        if (rc != 0) break;
        total += chunk;
    }
    return total;
}

/* Convert fn1 DOS path to TNFS path (static buffer, 128 bytes). Returns 1 ok, 0 not our drive. */
static int fn1_to_tnfs_path(const char far *fn1, char *out, int maxlen)
{
    int i, j;
    char c;
    if (!fn1_has_prefix(fn1)) return 0;
    for (i = 0; s_drv_prefix[i]; i++) ;
    out[0] = '/';
    j = 1;
    while (fn1[i] && j < maxlen - 1) {
        c = fn1[i++];
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
    }
    out[j] = '\0';
    return 1;
}

int fs_mkdir(const char far *fn1)
{
    static char path[128];
    static struct fstat st;
    int rc;
    if (!fn1_to_tnfs_path(fn1, path, sizeof(path))) return -1;
    if (tnfs_stat(path, &st) == 0) return TNFS_EEXIST;
    rc = tnfs_mkdir(path);
    cache_invalidate("mkdir");
    return rc;
}

int fs_rmdir(const char far *fn1)
{
    static char path[128];
    int rc;
    if (!fn1_to_tnfs_path(fn1, path, sizeof(path))) return -1;
    if (load_dir_cache(path) && s_active_count > 0) {
        if (rbuf.enabled) rb_write("FS_RMDIR NOTEMPTY\r\n");
        return TNFS_ENOTEMPTY;
    }
    rc = tnfs_rmdir(path);
    cache_invalidate("rmdir");
    return rc;
}

int fs_delete(const char far *fn1)
{
    static char path[128];
    if (!fn1_to_tnfs_path(fn1, path, sizeof(path))) return -1;
    cache_invalidate("del");
    return tnfs_unlink(path);
}

int fs_rename(const char far *fn1, const char far *fn2)
{
    static char old_path[128];
    static char new_path[128];
    int rc;
    if (!fn1_to_tnfs_path(fn1, old_path, sizeof(old_path))) return -1;
    if (!fn1_to_tnfs_path(fn2, new_path, sizeof(new_path))) return -1;
    cache_invalidate("ren");
    if (rbuf.enabled) { rb_write("FS_REN "); rb_write(old_path); rb_write(" -> "); rb_write(new_path); rb_write("\r\n"); }
    rc = tnfs_rename(old_path, new_path);
    if (rbuf.enabled) { rb_write("FS_REN "); rb_write(rc == 0 ? "OK" : "FAIL"); rb_write("\r\n"); }
    return rc;
}

/* POSIX permission bit masks used by fs_getattr_stat / fs_setattr.
 * Values match the standard Linux/POSIX definitions (see chmod(2)).
 * S_IFMT (0xF000) is stripped before calling tnfs_chmod — the server
 * expects permission bits only, not the file-type field from stat. */
#define POSIX_S_IFDIR    0x4000u          /* directory type bit from stat */
#define POSIX_WRITE_MASK 0x0092u          /* S_IWUSR|S_IWGRP|S_IWOTH     */
#define POSIX_S_IWUSR    0x0080u          /* owner-write bit              */

int fs_getattr_stat(const char far *fn1, unsigned char *attr_out)
{
    static char path[128];
    static struct fstat st;
    static FsNode node;
    unsigned char attr;

    /* Prefer the directory cache — gives the stored attribute (possibly updated
     * by a previous setfileattr call) and avoids an extra TNFS network round-trip. */
    if (fs_resolve(fn1, &node)) {
        *attr_out = fs_get_attr(&node);
        if (rbuf.enabled) {
            rb_write("ATTR RET idx="); rb_hex8((unsigned char)node.idx);
            rb_write(" dos="); rb_hex8(*attr_out); rb_write("\r\n");
        }
        return 1;
    }

    /* Cache miss: fall back to tnfs_stat (e.g. for newly-created files) */
    if (!fn1_to_tnfs_path(fn1, path, sizeof(path))) return 0;
    if (rbuf.enabled) { rb_write("ATTR GET tnfs="); rb_write(path); rb_write("\r\n"); }
    if (tnfs_stat(path, &st) != 0) return 0;
    if (rbuf.enabled) { rb_write("ATTR STAT mode="); rb_hex16((unsigned int)st.mode); rb_write("\r\n"); }
    if (st.mode & POSIX_S_IFDIR) {
        attr = 0x10;
    } else {
        attr = 0x20;
        if ((st.mode & POSIX_WRITE_MASK) == 0)
            attr |= 0x01;
    }
    *attr_out = attr;
    if (rbuf.enabled) { rb_write("ATTR RET dos="); rb_hex8(attr); rb_write("\r\n"); }
    return 1;
}

int fs_setattr(const char far *fn1, unsigned char dos_attr)
{
    static FsNode node;

    /* Store the new attribute in the directory cache.  The server-side chmod
     * command (0x27) is not used here: the server on which this was tested does
     * not implement it (every request times out after 5 retries × ~3 s = ~15 s),
     * which would stall the test and risk corrupting the packet-driver/TNFS session
     * state.  Attribute changes are therefore memory-only and survive only for the
     * duration of the DOS session. */
    if (!fs_resolve(fn1, &node)) return -2;
    if (node.idx >= 0 && node.idx < s_active_count)
        s_active_entries[node.idx].attr = dos_attr;
    if (rbuf.enabled) {
        rb_write(dos_attr & 0x01 ? "ATTR SET +R" : "ATTR SET -R");
        rb_write(" idx="); rb_hex8((unsigned char)node.idx); rb_write("\r\n");
    }
    return 0;
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
    sft[0x06] = 0x80;  /* char device flag — keeps DOS I/O path via DTA (SDA+0x0C) */
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

int fs_create_and_open(const char far *fn1, char far *sft)
{
    static char path[128];
    static char name[13];
    static char fcb[11];
    int i, k, last_sep, len, rc;

    if (!fn1_to_tnfs_path(fn1, path, sizeof(path))) return 5;
    cache_invalidate("create");

    if (rbuf.enabled) { rb_write("FS_CREATE "); rb_write(path); rb_write("\r\n"); }

    rc = tnfs_open(path, TNFS_O_RDWR | TNFS_O_CREAT | TNFS_O_TRUNC, 0x01B6);
    if (rc < 0) {
        if (rbuf.enabled) { rb_write("FS_CREATE FAIL\r\n"); }
        return (rc == -2) ? 3 : 5;  /* -ENOENT=path not found(3), else access denied(5) */
    }

    /* Extract filename component from fn1 for FCB name in SFT */
    len = 0;
    while (fn1[len]) len++;
    last_sep = -1;
    for (i = len - 1; i >= 0; i--) {
        if (fn1[i] == '\\' || fn1[i] == ':') { last_sep = i; break; }
    }
    i = (last_sep >= 0) ? last_sep + 1 : 0;
    k = 0;
    while (fn1[i] && k < 12) name[k++] = (char)fn1[i++];
    name[k] = '\0';
    entry_to_fcb(name, fcb);

    sft[0x04] = 0x20;               /* archive attribute */
    sft[0x05] = (char)g_drive_idx;
    sft[0x06] = 0x80;  /* char device flag — keeps DOS I/O path via DTA (SDA+0x0C) */
    sft[0x07] = (char)(uint8_t)rc;  /* TNFS file handle */
    for (k = 0x08; k <= 0x14; k++) sft[k] = 0;  /* zero: dir entry cluster, size */
    for (k = 0x15; k <= 0x1F; k++) sft[k] = 0;  /* file position = 0 */
    for (k = 0;    k < 11;    k++) sft[0x20+k] = fcb[k];
    for (k = 0x2B; k <= 0x35; k++) sft[k] = 0;

    if (rbuf.enabled) { rb_write("FS_CREATE OK fd="); rb_hex8((uint8_t)rc); rb_write("\r\n"); }
    return 0;
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
