/*
 * FS_TNFS.C  —  Live TNFS filesystem backend for TNFSDRV.
 *
 * Implements the fs.h interface with real TNFS network calls.
 * Called from redirector.c → handler.asm INT 2Fh dispatch.
 *
 * All TNFS calls happen on the private TSR stack (see handler.asm).
 *
 * -zu note: compiled with -zu (SS≠DS in handler context).
 * Every near pointer passed to non-zu TNFS functions must be static
 * (DGROUP-relative).  Never pass stack-local addresses across the boundary.
 */

#include <i86.h>
#include <string.h>
#include "fs.h"
#include "ringbuf.h"
#include "tnfs.h"

/* ------------------------------------------------------------------ */
/*  Drive state                                                         */
/* ------------------------------------------------------------------ */

unsigned char g_drive_idx      = (unsigned char)DRIVE_IDX;
static char   s_drv_prefix[4] = { (char)TNFSDRV_DRIVE_LETTER, ':', '\\', '\0' };
static char   s_drv_root[3]   = { (char)TNFSDRV_DRIVE_LETTER, ':', '\0' };

void fs_set_drive(char letter)
{
    g_drive_idx     = (unsigned char)(letter - 'A');
    s_drv_prefix[0] = letter;
    s_drv_root[0]   = letter;
}

/* ------------------------------------------------------------------ */
/*  Directory entry cache                                               */
/* ------------------------------------------------------------------ */

#define DIR_CACHE_MAX  64
#define FNAME_MAX      13    /* "XXXXXXXX.YYY\0" */

typedef struct {
    char          name[FNAME_MAX];      /* uppercase, for DOS / FCB matching */
    char          origname[FNAME_MAX];  /* server case, for TNFS path building */
    unsigned char attr;                 /* 0x10=dir 0x20=file */
    unsigned long size;
} DirEntry;

static DirEntry s_dir[DIR_CACHE_MAX];
static int      s_dir_count;

static DirEntry s_res;   /* single resolved-node slot */

/* ------------------------------------------------------------------ */
/*  TNFS work buffers  (static → DS-relative, safe across -zu boundary) */
/* ------------------------------------------------------------------ */

static struct dirx_data s_dd;
static struct dirx_item s_xi;
static char             s_path[TNFS_MAX_PATH_LEN];
static char             s_pattern[2] = "*";

/* Additional static buffers for component-wise path walking in fs_resolve */
static char s_rdir [TNFS_MAX_PATH_LEN]; /* current parent TNFS dir      */
static char s_comp [FNAME_MAX];          /* current DOS component (upper) */
static char s_corig[FNAME_MAX];          /* matched server-case name      */
static unsigned char s_mflags;           /* saved flags from matched entry */
static unsigned long s_msize;            /* saved size  from matched entry */

/* ------------------------------------------------------------------ */
/*  8.3 / dot helpers                                                   */
/* ------------------------------------------------------------------ */

static int is_dot_entry(const char *name)
{
    return (name[0] == '.' && (name[1] == '\0' ||
            (name[1] == '.' && name[2] == '\0')));
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
    return 1;
}

/* ------------------------------------------------------------------ */
/*  FCB helpers                                                         */
/* ------------------------------------------------------------------ */

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

int fs_is_root(const char far *path)
{
    return fn1_eq(path, s_drv_prefix) || fn1_eq(path, s_drv_root);
}

/*
 * Build TNFS full path from DOS fn1:  "N:\DIR\FILE.TXT" → "/DIR/FILE.TXT"
 * Backslashes become forward slashes; no case conversion (DOS gives uppercase).
 */
static void fn1_to_tnfs_path(const char far *fn1, char *out, int out_size)
{
    const char far *p = fn1 + 3;    /* skip "N:\" */
    int i = 0;
    out[i++] = '/';
    while (*p && i < out_size - 1) {
        char c = (char)*p++;
        out[i++] = (c == '\\') ? '/' : c;
    }
    if (i > 1 && out[i-1] == '/') i--;
    out[i] = '\0';
}

/*
 * Extract the TNFS directory path from fn1 (strip the last component).
 * "N:\GAMES\*.TXT" → "/GAMES"   "N:\*.TXT" → "/"
 */
static void fn1_to_tnfs_dir(const char far *fn1, char *out, int out_size)
{
    const char far *p = fn1 + 3;
    int i, len = 0, last_sep = -1;

    while (p[len]) {
        if (p[len] == '\\') last_sep = len;
        len++;
    }

    out[0] = '/';
    if (last_sep <= 0) { out[1] = '\0'; return; }

    for (i = 0; i < last_sep && i < out_size - 2; i++)
        out[i + 1] = (p[i] == '\\') ? '/' : (char)p[i];
    out[last_sep + 1] = '\0';
}

/* Concatenate a TNFS directory path and a filename. */
static void build_tnfs_path(const char *dir, const char *name,
                             char *out, int out_size)
{
    int i = 0, j;
    if (dir[0] == '/' && dir[1] == '\0') {
        out[i++] = '/';
    } else {
        for (j = 0; dir[j] && i < out_size - 2; j++) out[i++] = dir[j];
        out[i++] = '/';
    }
    for (j = 0; name[j] && i < out_size - 1; j++) out[i++] = name[j];
    out[i] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Directory cache loader  (called from fs_enum_begin)                */
/* ------------------------------------------------------------------ */

static int load_dir(char *tnfs_dir_path)
{
    int rc, i;
    char c;

    s_dir_count = 0;

    if (rbuf.enabled) {
        rb_write("FS opendirx \"");
        rb_write(tnfs_dir_path);
        rb_write("\"\r\n");
    }

    rc = tnfs_opendirx(tnfs_dir_path, s_pattern, 0, 0, &s_dd);

    if (rbuf.enabled) {
        rb_write("FS opendirx rc=");
        rb_dec((unsigned)rc);
        rb_write("\r\n");
    }

    if (rc != 0) return 0;

    while (s_dir_count < DIR_CACHE_MAX) {
        if (rbuf.enabled) rb_write("FS nxt\r\n");
        rc = tnfs_nextdirx(&s_dd, &s_xi);
        if (rbuf.enabled) { rb_write("FS nxt rc="); rb_dec((unsigned)(unsigned char)rc); rb_write("\r\n"); }
        if (rc != 0) {
            if (rbuf.enabled) rb_write("FS EOF\r\n");
            break;
        }

        if (is_dot_entry(s_xi.name)) continue;
        if (!is_8dot3(s_xi.name)) {
            if (rbuf.enabled) { rb_write("FS skip "); rb_write(s_xi.name); rb_write("\r\n"); }
            continue;
        }

        for (i = 0; s_xi.name[i] && i < FNAME_MAX - 1; i++)
            s_dir[s_dir_count].origname[i] = s_xi.name[i];
        s_dir[s_dir_count].origname[i] = '\0';

        for (i = 0; s_xi.name[i] && i < FNAME_MAX - 1; i++) {
            c = s_xi.name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            s_dir[s_dir_count].name[i] = c;
        }
        s_dir[s_dir_count].name[i] = '\0';

        s_dir[s_dir_count].attr = (s_xi.flags & TNFS_DIRENTRY_DIR) ? 0x10u : 0x20u;
        s_dir[s_dir_count].size = s_xi.size;

        if (rbuf.enabled) {
            rb_write("FS+ \"");
            rb_write(s_dir[s_dir_count].name);
            rb_write("\" a=");
            rb_hex8(s_dir[s_dir_count].attr);
            rb_write("\r\n");
        }
        s_dir_count++;
    }

    if (rbuf.enabled) rb_write("FS cdir\r\n");
    { int crc = tnfs_closedir(s_dd.handle);
      if (rbuf.enabled) { rb_write("FS cdir rc="); rb_dec((unsigned)(unsigned char)crc); rb_write("\r\n"); } }

    if (rbuf.enabled) {
        rb_write("FS dir n=");
        rb_dec((unsigned)s_dir_count);
        rb_write("\r\n");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Node accessor                                                       */
/* ------------------------------------------------------------------ */

static const DirEntry *node_entry(const FsNode *node)
{
    if (node->dir_ctx == 0 && node->idx >= 0 && node->idx < s_dir_count)
        return &s_dir[node->idx];
    return &s_res;
}

/* ------------------------------------------------------------------ */
/*  Public fs.h interface                                               */
/* ------------------------------------------------------------------ */

/*
 * fs_resolve: walk each DOS path component through TNFS parent directory
 * listings using case-insensitive matching.  Avoids tnfs_stat so a
 * case-sensitive server (Linux) still resolves uppercase DOS names.
 */
int fs_resolve(const char far *path, FsNode *node)
{
    const char far *p;
    int i, k, rc;
    char c;

    if (rbuf.enabled) {
        rb_write("FS resolve raw=\"");
        rb_far_str(FP_SEG(path), FP_OFF(path), 40);
        rb_write("\"\r\n");
    }

    if (fs_is_root(path)) return 0;

    /* Skip drive letter "X:" and optional leading '\\' */
    p = path;
    if (p[0] && p[1] == ':') p += 2;
    if (*p == '\\') p++;

    /* Start search at TNFS root */
    s_rdir[0] = '/'; s_rdir[1] = '\0';

    for (;;) {
        /* Extract one DOS component (up to next '\\' or end) */
        for (i = 0; i < FNAME_MAX - 1 && p[i] && p[i] != '\\'; i++) {
            c = (char)p[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            s_comp[i] = c;
        }
        s_comp[i] = '\0';
        p += i;
        if (*p == '\\') p++;

        if (s_comp[0] == '\0') return 0;

        if (rbuf.enabled) {
            rb_write("FS res \"");
            rb_write(s_comp);
            rb_write("\" in \"");
            rb_write(s_rdir);
            rb_write("\"\r\n");
        }

        /* Open parent dir to search for this component */
        rc = tnfs_opendirx(s_rdir, s_pattern, 0, 0, &s_dd);
        if (rbuf.enabled) { rb_write("FS res odx rc="); rb_dec((unsigned)(unsigned char)rc); rb_write("\r\n"); }
        if (rc != 0) return 0;

        /* Linear scan: case-insensitive match */
        s_corig[0] = '\0';
        s_mflags   = 0;
        s_msize    = 0;
        while (1) {
            rc = tnfs_nextdirx(&s_dd, &s_xi);
            if (rc != 0) break;
            {
                int match = 1;
                for (k = 0; ; k++) {
                    char fc = s_comp[k];       /* already uppercase */
                    char sc = s_xi.name[k];
                    if (sc >= 'a' && sc <= 'z') sc -= 32;
                    if (fc != sc) { match = 0; break; }
                    if (!fc) break;
                }
                if (match) {
                    /* copy name + save values BEFORE any further network call */
                    for (k = 0; s_xi.name[k] && k < FNAME_MAX - 1; k++)
                        s_corig[k] = s_xi.name[k];
                    s_corig[k] = '\0';
                    s_mflags = s_xi.flags;
                    s_msize  = s_xi.size;
                    break;
                }
            }
        }
        tnfs_closedir(s_dd.handle);

        if (s_corig[0] == '\0') {
            if (rbuf.enabled) rb_write("FS res not found\r\n");
            return 0;
        }
        if (rbuf.enabled) {
            rb_write("FS res match \"");
            rb_write(s_corig);
            rb_write("\" f="); rb_hex8(s_mflags);
            rb_write("\r\n");
        }

        if (*p == '\0') {
            /* Last component — fill result slot */
            for (i = 0; s_comp[i] && i < FNAME_MAX - 1; i++)
                s_res.name[i] = s_comp[i];   /* already uppercase */
            s_res.name[i] = '\0';
            for (i = 0; s_corig[i] && i < FNAME_MAX - 1; i++)
                s_res.origname[i] = s_corig[i];
            s_res.origname[i] = '\0';
            s_res.attr = (s_mflags & TNFS_DIRENTRY_DIR) ? 0x10u : 0x20u;
            s_res.size = s_msize;

            if (rbuf.enabled) { rb_write("FS res OK attr="); rb_hex8(s_res.attr); rb_write("\r\n"); }
            node->dir_ctx = 1;
            node->idx     = 0;
            return 1;
        }

        /* Intermediate component must be a directory */
        if (!(s_mflags & TNFS_DIRENTRY_DIR)) {
            if (rbuf.enabled) rb_write("FS res not_dir\r\n");
            return 0;
        }
        /* Descend: s_rdir = s_rdir + "/" + s_corig */
        build_tnfs_path(s_rdir, s_corig, s_path, sizeof(s_path));
        for (i = 0; s_path[i] && i < TNFS_MAX_PATH_LEN - 1; i++)
            s_rdir[i] = s_path[i];
        s_rdir[i] = '\0';
    }
}

int           fs_is_dir  (const FsNode *node) { return (node_entry(node)->attr & 0x10) != 0; }
unsigned char fs_get_attr(const FsNode *node) { return node_entry(node)->attr; }
unsigned long fs_get_size(const FsNode *node) { return node_entry(node)->size; }
const char   *fs_get_name(const FsNode *node) { return node_entry(node)->name; }

void fs_fill_found(const FsNode *node, char far *found)
{
    static char fcb[11];
    const DirEntry *e = node_entry(node);
    int k;
    entry_to_fcb(e->name, fcb);
    for (k = 0; k < 11; k++) found[k] = fcb[k];
    found[11] = e->attr;
    for (k = 12; k < 28; k++) found[k] = 0;
    found[28] = (char)( e->size        & 0xFFUL);
    found[29] = (char)((e->size >>  8) & 0xFFUL);
    found[30] = (char)((e->size >> 16) & 0xFFUL);
    found[31] = (char)((e->size >> 24) & 0xFFUL);
}

void fs_open(const FsNode *node, FsHandle *handle)
{
    handle->dir_ctx = node->dir_ctx;
    handle->idx     = node->idx;
}

unsigned int fs_read(const FsHandle *handle, unsigned long pos,
                     char far *buf, unsigned int n)
{
    (void)handle; (void)pos; (void)buf; (void)n;
    return 0;
}

void sft_fill_handle(const FsHandle *handle, char far *sft)
{
    static char fcb[11];
    const DirEntry *e = node_entry((const FsNode *)handle);
    int k;
    entry_to_fcb(e->name, fcb);
    sft[0x04] = e->attr;
    sft[0x05] = (char)g_drive_idx;
    sft[0x06] = 0x80;
    sft[0x07] = sft[0x08] = sft[0x09] = sft[0x0A] = 0;
    sft[0x0B] = (char)handle->dir_ctx;
    sft[0x0C] = (char)handle->idx;
    sft[0x0D] = sft[0x0E] = sft[0x0F] = sft[0x10] = 0;
    sft[0x11] = (char)( e->size        & 0xFFUL);
    sft[0x12] = (char)((e->size >>  8) & 0xFFUL);
    sft[0x13] = (char)((e->size >> 16) & 0xFFUL);
    sft[0x14] = (char)((e->size >> 24) & 0xFFUL);
    sft[0x15] = sft[0x16] = sft[0x17] = sft[0x18] = 0;
    sft[0x19] = sft[0x1A] = sft[0x1B] = sft[0x1C] = 0;
    sft[0x1D] = sft[0x1E] = sft[0x1F] = 0;
    for (k = 0; k < 11; k++) sft[0x20+k] = fcb[k];
}

/*
 * Resolve the DIRECTORY portion of a FINDFIRST fn1 path, case-insensitively.
 * All intermediate components (everything before the last backslash) are
 * walked through tnfs_opendirx to match server case.
 *
 * "N:\????????.???"         → "/"      (no subdirectory)
 * "N:\APAC\????????.???"    → "/apac"  (if server has "apac")
 * "N:\APAC\GAMES\*.TXT"     → "/apac/games"
 *
 * Returns 1 on success, 0 if a component was not found on the server.
 */
static int resolve_parent_dir(const char far *fn1, char *out, int out_size)
{
    const char far *p = fn1;
    int i, k, rc;
    char c;

    /* Skip drive "X:" and optional leading '\\' */
    if (p[0] && p[1] == ':') p += 2;
    if (*p == '\\') p++;

    /* If there is no '\\' in what remains, the last (only) component is the
     * wildcard/filename and the parent directory is root. */
    {
        const char far *q = p;
        while (*q && *q != '\\') q++;
        if (*q == '\0') {
            out[0] = '/'; out[1] = '\0';
            return 1;
        }
    }

    /* There are intermediate components — resolve them case-insensitively. */
    s_rdir[0] = '/'; s_rdir[1] = '\0';

    for (;;) {
        /* Extract current component (uppercase) */
        for (i = 0; i < FNAME_MAX - 1 && p[i] && p[i] != '\\'; i++) {
            c = (char)p[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            s_comp[i] = c;
        }
        s_comp[i] = '\0';
        p += i;

        /* No '\\' after this component → it's the last one (wildcard/file) */
        if (*p != '\\') {
            for (i = 0; s_rdir[i] && i < out_size - 1; i++) out[i] = s_rdir[i];
            out[i] = '\0';
            return 1;
        }
        p++;  /* skip '\\' */

        if (s_comp[0] == '\0') {
            out[0] = '/'; out[1] = '\0';
            return 1;
        }

        if (rbuf.enabled) {
            rb_write("FS rp \"");
            rb_write(s_comp);
            rb_write("\" in \"");
            rb_write(s_rdir);
            rb_write("\"\r\n");
        }

        rc = tnfs_opendirx(s_rdir, s_pattern, 0, 0, &s_dd);
        if (rbuf.enabled) { rb_write("FS rp odx rc="); rb_dec((unsigned)(unsigned char)rc); rb_write("\r\n"); }
        if (rc != 0) return 0;

        s_corig[0] = '\0';
        while (1) {
            rc = tnfs_nextdirx(&s_dd, &s_xi);
            if (rc != 0) break;
            {
                int match = 1;
                for (k = 0; ; k++) {
                    char fc = s_comp[k];
                    char sc = s_xi.name[k];
                    if (sc >= 'a' && sc <= 'z') sc -= 32;
                    if (fc != sc) { match = 0; break; }
                    if (!fc) break;
                }
                if (match) {
                    for (k = 0; s_xi.name[k] && k < FNAME_MAX - 1; k++)
                        s_corig[k] = s_xi.name[k];
                    s_corig[k] = '\0';
                    break;
                }
            }
        }
        tnfs_closedir(s_dd.handle);

        if (s_corig[0] == '\0') {
            if (rbuf.enabled) rb_write("FS rp not found\r\n");
            return 0;
        }
        if (rbuf.enabled) {
            rb_write("FS rp match \"");
            rb_write(s_corig);
            rb_write("\"\r\n");
        }

        build_tnfs_path(s_rdir, s_corig, s_path, sizeof(s_path));
        for (i = 0; s_path[i] && i < TNFS_MAX_PATH_LEN - 1; i++)
            s_rdir[i] = s_path[i];
        s_rdir[i] = '\0';
    }
}

int fs_enum_begin(const char far *path, const char far *tmpl, FsDirEnum *de)
{
    int k;
    for (k = 0; k < 11; k++) de->tmpl[k] = (char)tmpl[k];
    de->next_idx = 0;
    de->dir_ctx  = 0;

    if (rbuf.enabled) {
        rb_write("FS eb fn1=\"");
        rb_far_str(FP_SEG(path), FP_OFF(path), 60);
        rb_write("\"\r\nFS eb tmpl=");
        for (k = 0; k < 11; k++) {
            char c = (char)tmpl[k];
            rb_putc(c >= 0x20 ? c : '.');
        }
        rb_write("\r\n");
    }

    if (!fn1_has_prefix(path)) {
        if (rbuf.enabled) rb_write("FS eb no_prefix\r\n");
        return 0;
    }

    if (!resolve_parent_dir(path, s_path, sizeof(s_path))) {
        if (rbuf.enabled) rb_write("FS eb resolve fail\r\n");
        return 0;
    }

    if (rbuf.enabled) {
        rb_write("FS eb dir=\"");
        rb_write(s_path);
        rb_write("\"\r\n");
    }

    if (!load_dir(s_path)) return 0;

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
