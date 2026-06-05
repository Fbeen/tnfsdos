#ifndef FS_H
#define FS_H

#include <i86.h>

/* Drive index for T: (A=0 … T=19).  CONFIG.SYS must have LASTDRIVE>=T. */
#define DRIVE_T_IDX  19

/* ------------------------------------------------------------------ */
/*  Opaque FS interface types                                           */
/* ------------------------------------------------------------------ */

typedef struct { int dir_ctx; int idx; } FsNode;
typedef struct { int dir_ctx; int idx; } FsHandle;
typedef struct {
    int  dir_ctx;
    int  next_idx;
    char tmpl[11];   /* FCB pattern, near copy */
} FsDirEnum;

/* ------------------------------------------------------------------ */
/*  Path tests                                                          */
/* ------------------------------------------------------------------ */

/* Returns 1 if path is the root "T:\" or "T:" */
int fs_is_root(const char far *path);

/* ------------------------------------------------------------------ */
/*  Node operations                                                     */
/* ------------------------------------------------------------------ */

/* Resolve "T:\[subdir\]name" → FsNode.  Returns 1 on success. */
int           fs_resolve  (const char far *path, FsNode *node);
int           fs_is_dir   (const FsNode *node);
unsigned char fs_get_attr (const FsNode *node);
unsigned long fs_get_size (const FsNode *node);
const char   *fs_get_name (const FsNode *node);

/* Fill 32-byte found_file struct (for FINDFIRST/FINDNEXT results). */
void fs_fill_found(const FsNode *node, char far *found);

/* ------------------------------------------------------------------ */
/*  File I/O                                                            */
/* ------------------------------------------------------------------ */

void         fs_open(const FsNode *node, FsHandle *handle);
unsigned int fs_read(const FsHandle *handle, unsigned long pos,
                     char far *buf, unsigned int n);

/* Fill a DOS SFT for a network file described by an FsHandle. */
void sft_fill_handle(const FsHandle *handle, char far *sft);

/* ------------------------------------------------------------------ */
/*  Directory enumeration                                               */
/* ------------------------------------------------------------------ */

/* Begin enumeration in path with FCB template tmpl.
 * Returns  1: directory found.
 * Returns  0: path not found / not on T:.
 * Returns -1: path component is a file (DOS error 3). */
int fs_enum_begin(const char far *path, const char far *tmpl, FsDirEnum *de);

/* Advance; fills *node if a match is found.  Returns 1 on match, 0 at EOF. */
int fs_enum_next(FsDirEnum *de, FsNode *node);

#endif
