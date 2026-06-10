#ifndef FS_H
#define FS_H

#include <i86.h>
#include <stdint.h>
#include "config.h"

/* ------------------------------------------------------------------ */
/*  Runtime drive configuration                                         */
/* ------------------------------------------------------------------ */

/* Drive index used at runtime — set by fs_set_drive() before TSR install. */
extern unsigned char g_drive_idx;

/* Update the virtual drive letter; call once before going resident. */
void fs_set_drive(char letter);

/* Configure the directory cache; call once after config_load(). */
void fs_set_cache_config(uint8_t enabled, uint16_t ttl_secs, uint8_t dirs);

/* Invalidate the directory cache (e.g. after write/create/rename).
 * reason is a short string logged to the ring buffer. */
void cache_invalidate(const char *reason);

/* ------------------------------------------------------------------ */
/*  Opaque FS interface types                                           */
/* ------------------------------------------------------------------ */

typedef struct { int dir_ctx; int idx; } FsNode;
typedef struct { int dir_ctx; int idx; uint8_t tnfs_fd; } FsHandle;
typedef struct {
    int  dir_ctx;
    int  next_idx;
    char tmpl[11];   /* FCB pattern, near copy */
} FsDirEnum;

/* ------------------------------------------------------------------ */
/*  Path tests                                                          */
/* ------------------------------------------------------------------ */

/* Returns 1 if path is the root "X:\" or "X:" for the configured drive. */
int fs_is_root(const char far *path);

/* ------------------------------------------------------------------ */
/*  Node operations                                                     */
/* ------------------------------------------------------------------ */

/* Resolve "X:\[subdir\]name" → FsNode.  Returns 1 on success. */
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

/* dos_mode: 0=read-only, 1=write-only, 2=read-write (INT 21h AH=3Dh AL bits 0-2) */
void         fs_open (const FsNode *node, FsHandle *handle, unsigned char dos_mode);
unsigned int fs_read (const FsHandle *handle, unsigned long pos,
                      char far *buf, unsigned int n);
unsigned int fs_write(const FsHandle *handle, unsigned long pos,
                      const char far *buf, unsigned int n);
void         fs_close(const FsHandle *handle);

/* Directory create/delete — return 0 on success, TNFS error code otherwise. */
int fs_mkdir(const char far *fn1);
int fs_rmdir(const char far *fn1);

/* File delete — returns 0 on success, negative TNFS error otherwise. */
int fs_delete(const char far *fn1);

/* Rename fn1 → fn2 (both must be on the virtual drive). */
int fs_rename(const char far *fn1, const char far *fn2);

/* Attribute get/set via tnfs_stat/tnfs_chmod. */
int fs_getattr_stat(const char far *fn1, unsigned char *attr_out);
int fs_setattr     (const char far *fn1, unsigned char dos_attr);

/* Fill a DOS SFT for a network file described by an FsHandle. */
void sft_fill_handle(const FsHandle *handle, const FsNode *node, char far *sft);

/* Create (or truncate) fn1 and fill sft.  Returns 0 ok, DOS error code otherwise. */
int fs_create_and_open(const char far *fn1, char far *sft);

/* ------------------------------------------------------------------ */
/*  Directory enumeration                                               */
/* ------------------------------------------------------------------ */

/* Begin enumeration in path with FCB template tmpl.
 * Returns  1: directory found.
 * Returns  0: path not found / not on the virtual drive.
 * Returns -1: path component is a file (DOS error 3). */
int fs_enum_begin(const char far *path, const char far *tmpl, FsDirEnum *de);

/* Advance; fills *node if a match is found.  Returns 1 on match, 0 at EOF. */
int fs_enum_next(FsDirEnum *de, FsNode *node);


#endif
