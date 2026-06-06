/*
 * FS_FAKE.C  —  Fake (static in-memory) filesystem backend for TNFSDRV.
 *
 * Implements the fs.h interface against a compile-time table of FsEntry
 * structs.  To replace with a TNFS backend: implement the same fs.h
 * functions in fs_tnfs.c and link that instead of this file.
 */

#include <i86.h>
#include <stddef.h>
#include "fs.h"
#include "ringbuf.h"

/* Drive letter arrays — written once by fs_set_drive() before TSR install. */
unsigned char g_drive_idx            = (unsigned char)DRIVE_IDX;
static char   s_drv_prefix[4]        = { (char)TNFSDRV_DRIVE_LETTER, ':', '\\', '\0' };
static char   s_drv_root[3]          = { (char)TNFSDRV_DRIVE_LETTER, ':', '\0' };

void fs_set_drive(char letter)
{
    g_drive_idx     = (unsigned char)(letter - 'A');
    s_drv_prefix[0] = letter;
    s_drv_root[0]   = letter;
}

/* ------------------------------------------------------------------ */
/*  Content strings and sizes                                           */
/* ------------------------------------------------------------------ */

static const char readme_content[]       = "Hello from TNFSDRV!\r\n";
static const char info_content[]         = "Written by Frank Beentjes + Claude\r\n";
static const char games_readme_content[] = "Hello from T:\\GAMES!\r\n";

#define README_SIZE       21
#define INFO_SIZE         35
#define GAMES_README_SIZE 22

/* ------------------------------------------------------------------ */
/*  FsEntry — backend-private type                                      */
/* ------------------------------------------------------------------ */

typedef struct FsEntry_s {
    const char             *name;
    unsigned char           attr;
    unsigned long           size;
    const char             *content;
    const struct FsEntry_s *children;
    int                     child_count;
} FsEntry;

static FsEntry games_entries[] = {
    { "PACMAN.EXE", 0x20, 0,                NULL,                 NULL, 0 },
    { "README.TXT", 0x20, GAMES_README_SIZE, games_readme_content, NULL, 0 }
};
#define GAMES_ENTRY_COUNT 2

static FsEntry root_entries[] = {
    { "GAMES",      0x10, 0,           NULL,           games_entries, GAMES_ENTRY_COUNT },
    { "README.TXT", 0x20, README_SIZE, readme_content, NULL,          0                 },
    { "INFO.TXT",   0x20, INFO_SIZE,   info_content,   NULL,          0                 }
};

/* ------------------------------------------------------------------ */
/*  Path helpers (internal)                                             */
/* ------------------------------------------------------------------ */

static int fn1_has_prefix(const char far *fn1, const char *prefix)
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

static int tmpl_matches(char far *tmpl, const char *pat)
{
    int i;
    for (i = 0; i < 11; i++) if (tmpl[i] != pat[i]) return 0;
    return 1;
}

static int tmpl_matches_near(const char *tmpl, const char *fcb)
{
    int i;
    for (i = 0; i < 11; i++) if (tmpl[i] != '?' && tmpl[i] != fcb[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Backend helpers (internal)                                          */
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

static int find_in(const FsEntry *entries, int count, const char *name)
{
    int i;
    for (i = 0; i < count; i++)
        if (str_eq_ci(entries[i].name, name)) return i;
    return -1;
}

static int g_dir_count;
static const FsEntry *dir_ctx_entries(int dir_ctx)
{
    if (dir_ctx == 0) { g_dir_count = fs_get_root_count(); return root_entries; }
    g_dir_count = root_entries[dir_ctx - 1].child_count;
    return root_entries[dir_ctx - 1].children;
}

static int g_fn1_dir_ctx;
static int g_fn1_idx;
static int fn1_find(const char far *fn1)
{
    static char c1[13], c2[13];
    const char far *p;
    int i, n;

    if (!fn1_has_prefix(fn1, s_drv_prefix)) return 0;
    p = fn1 + 3;

    for (i = 0; i < 12 && p[i] && p[i] != '\\'; i++) c1[i] = (char)p[i];
    c1[i] = '\0';
    if (!c1[0]) return 0;

    if (!p[i]) {
        n = find_in(root_entries, fs_get_root_count(), c1);
        if (n < 0) return 0;
        g_fn1_dir_ctx = 0; g_fn1_idx = n;
        return 1;
    }

    p = p + i + 1;
    for (i = 0; i < 12 && p[i] && p[i] != '\\'; i++) c2[i] = (char)p[i];
    c2[i] = '\0';
    if (!c2[0]) return 0;

    n = find_in(root_entries, fs_get_root_count(), c1);
    if (n < 0 || !(root_entries[n].attr & 0x10) || !root_entries[n].children) return 0;

    i = find_in(root_entries[n].children, root_entries[n].child_count, c2);
    if (i < 0) return 0;

    g_fn1_dir_ctx = n + 1; g_fn1_idx = i;
    return 1;
}

static int fn1_subdir_idx(const char far *fn1)
{
    static char name[13];
    const char far *p;
    int i;
    if (!fn1_has_prefix(fn1, s_drv_prefix)) return -1;
    p = fn1 + 3;
    for (i = 0; i < 12 && p[i] && p[i] != '\\'; i++) name[i] = (char)p[i];
    if (p[i] != '\\') return -1;
    name[i] = '\0';
    return find_in(root_entries, fs_get_root_count(), name);
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

static void fill_found_from_entry(FsEntry *e, char far *found)
{
    static char fcb[11];
    int k;
    entry_to_fcb(e->name, fcb);
    for (k = 0; k < 11; k++) found[k] = fcb[k];
    found[11] = e->attr;
    found[28] = (char)(e->size         & 0xFFUL);
    found[29] = (char)((e->size >>  8) & 0xFFUL);
    found[30] = (char)((e->size >> 16) & 0xFFUL);
    found[31] = (char)((e->size >> 24) & 0xFFUL);
}

static void fill_sft_entry(int dir_ctx, int idx, char far *sft)
{
    const FsEntry *entries = dir_ctx_entries(dir_ctx);
    const FsEntry *e = &entries[idx];
    static char fcb[11];
    int k;
    entry_to_fcb(e->name, fcb);
    sft[0x04] = e->attr;
    sft[0x05] = (char)g_drive_idx;
    sft[0x06] = 0x80;
    sft[0x07] = sft[0x08] = sft[0x09] = sft[0x0A] = 0;
    sft[0x0B] = (char)dir_ctx;
    sft[0x0C] = (char)idx;
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

static int fs_find_by_tmpl_in(char far *tmpl, const FsEntry *entries, int count)
{
    static char fcb[11];
    int i;
    for (i = 0; i < count; i++) {
        entry_to_fcb(entries[i].name, fcb);
        if (tmpl_matches(tmpl, fcb)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Public FS interface implementation                                  */
/* ------------------------------------------------------------------ */

int fs_is_root(const char far *path)
{
    return fn1_eq(path, s_drv_prefix) || fn1_eq(path, s_drv_root);
}

int fs_resolve(const char far *path, FsNode *node)
{
    if (!fn1_find(path)) return 0;
    node->dir_ctx = g_fn1_dir_ctx;
    node->idx     = g_fn1_idx;
    return 1;
}

int           fs_is_dir  (const FsNode *node) { return (dir_ctx_entries(node->dir_ctx)[node->idx].attr & 0x10) != 0; }
unsigned char fs_get_attr(const FsNode *node) { return dir_ctx_entries(node->dir_ctx)[node->idx].attr; }
unsigned long fs_get_size(const FsNode *node) { return dir_ctx_entries(node->dir_ctx)[node->idx].size; }
const char   *fs_get_name(const FsNode *node) { return dir_ctx_entries(node->dir_ctx)[node->idx].name; }

void fs_fill_found(const FsNode *node, char far *found)
{
    fill_found_from_entry((FsEntry *)&dir_ctx_entries(node->dir_ctx)[node->idx], found);
}

void fs_open(const FsNode *node, FsHandle *handle)
{
    handle->dir_ctx = node->dir_ctx;
    handle->idx     = node->idx;
}

unsigned int fs_read(const FsHandle *handle, unsigned long pos,
                      char far *buf, unsigned int n)
{
    const FsEntry *entries;
    const FsEntry *e;
    unsigned int avail, count, i;
    entries = dir_ctx_entries(handle->dir_ctx);
    if (handle->idx >= g_dir_count) return 0;
    e = &entries[handle->idx];
    if (!e->content || pos >= e->size) return 0;
    avail = (unsigned int)(e->size - pos);
    count = (n < avail) ? n : avail;
    for (i = 0; i < count; i++) buf[i] = e->content[(unsigned int)pos + i];
    return count;
}

void sft_fill_handle(const FsHandle *handle, char far *sft)
{
    fill_sft_entry(handle->dir_ctx, handle->idx, sft);
}

int fs_enum_begin(const char far *path, const char far *tmpl, FsDirEnum *de)
{
    int sub_idx, k;
    for (k = 0; k < 11; k++) de->tmpl[k] = (char)tmpl[k];
    de->next_idx = 0;
    sub_idx = fn1_subdir_idx(path);
    if (sub_idx >= 0) {
        if (!(root_entries[sub_idx].attr & 0x10)) return -1;
        de->dir_ctx = sub_idx + 1;
        return 1;
    }
    if (!fn1_has_prefix(path, s_drv_prefix)) return 0;
    de->dir_ctx = 0;
    return 1;
}

int fs_enum_next(FsDirEnum *de, FsNode *node)
{
    const FsEntry *entries;
    static char fcb[11];
    int i;
    entries = dir_ctx_entries(de->dir_ctx);
    while (de->next_idx < g_dir_count) {
        i = de->next_idx++;
        entry_to_fcb(entries[i].name, fcb);
        if (tmpl_matches_near(de->tmpl, fcb)) {
            node->dir_ctx = de->dir_ctx;
            node->idx     = i;
            return 1;
        }
    }
    return 0;
}
