#ifndef __tnfs_h__
#define __tnfs_h__

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef __cplusplus
    #ifdef __WATCOMC__
        #ifndef bool
            typedef unsigned char bool;
            #define true  1
            #define false 0
        #endif
    #else
        #include <stdbool.h>
    #endif
#endif

#include "netw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TNFS_PORT 16384		// port 16384 is the standard port for the tnfs protocol
#define TNFS_MAX_PATH_LEN 64	// maximum length of a complete file path
#define TNFS_BUFFERSIZE 1024	// send/receive buffer (must fit TNFS_DIRX_REQUIRED)
#define TNFS_MAX_RESULTS 4	// max directory entries per readdirx call
#define TNFS_SEND_RETRIES 5	// repeat sending commands up to x times before giving up
#define TNFS_NET_TIMEOUT_MS 2000// timeout in microseconds if the server doesn't respond.

/* Compile-time sanity check: buffer must fit the largest possible readdirx response */
#define TNFS_DIRX_ENTRY_MAX  (13 + TNFS_MAX_PATH_LEN)
#define TNFS_DIRX_REQUIRED   (10 + TNFS_DIRX_ENTRY_MAX * TNFS_MAX_RESULTS)
#if TNFS_DIRX_REQUIRED > TNFS_BUFFERSIZE
#error TNFS_BUFFERSIZE too small for TNFS_MAX_RESULTS and TNFS_MAX_PATH_LEN
#endif

#define TNFS_DIRENTRY_DIR       0x01
#define TNFS_DIRENTRY_HIDDEN    0x02
#define TNFS_DIRENTRY_SPECIAL   0x04
 
/* structure used by opendirx() and dirx_next() function */
struct dirx_data {
    uint8_t  handle;  	// the tnfs file handle
    uint16_t entries;	// amount of "directories and/or files" entries that the directory contains
    uint8_t  count;  	// number of entries returned by tnfs READDIRX command
    uint8_t  status; 	// TNFS_DIRSTATUS flags (only TNFS_DIRSTATUS_EOF at the end of all entries, otherwise zero)
    uint16_t dirpos; 	// Position of first entry as given by TELLDIR
    uint16_t needle;	// points somewhere in tnfs_buffer to remember the starting point of current directory entry that we are reading with dirx_next()
    uint16_t entry;	// current directory entry
};

/* structure that holds information about one file or directory */
struct dirx_item {
    uint8_t  flags;	// TNFS_DIRENTRY_DIR (Entry denotes a directory) , TNFS_DIRENTRY_HIDDEN (Entry is hidden) , TNFS_DIRENTRY_SPECIAL (Entry is special)
    uint32_t size;	// size of the file in bytes
    uint32_t modified;	// modification time in seconds since epoch
    uint32_t created;	// change time in seconds since epoch
    char* name; 	// pointer! to a string with the filename
};

/* structure that holds stat information about one file or directory */
struct fstat {
    uint16_t mode;      // 2 bytes: File permissions, little endian byte order
    uint16_t uid;       // 2 bytes: Numeric UID of owner
    uint16_t gid;       // 2 bytes: Numeric GID of owner
    uint32_t size;      // 4 bytes: Unsigned 32 bit little endian size of file in bytes
    uint32_t atime;     // 4 bytes: Access time in seconds since the epoch, little endian
    uint32_t mtime;     // 4 bytes: Modification time (as above)
    uint32_t ctime;     // 4 bytes: Time of last status change (as above)
    char uidstring[32]; // 0 or more bytes: Null terminated user id string
    char gidstring[32]; // 0 or more bytes: Null terminated group id string
};

/* response codes from the TNFS server */
#define TNFS_OK			0x00	// Operation successful
#define TNFS_EPERM		0x01	// Operation not permitted
#define TNFS_ENOENT		0x02	// No such file or directory
#define TNFS_EIO		0x03	// I/O error
#define TNFS_ENXIO		0x04	// No such device or address
#define TNFS_E2BIG		0x05	// Argument list too long
#define TNFS_EBADF		0x06	// Bad file number
#define TNFS_EAGAIN		0x07	// Try again
#define TNFS_ENOMEM		0x08	// Out of memory
#define TNFS_EACCES		0x09	// Permission denied
#define TNFS_EBUSY		0x0A	// Device or resource busy
#define TNFS_EEXIST		0x0B	// File exists
#define TNFS_ENOTDIR		0x0C	// Is not a directory
#define TNFS_EISDIR		0x0D	// Is a directory
#define TNFS_EINVAL		0x0E	// Invalid argument
#define TNFS_ENFILE		0x0F	// File table overflow
#define TNFS_EMFILE		0x10	// Too many open files
#define TNFS_EFBIG		0x11	// File too large
#define TNFS_ENOSPC		0x12	// No space left on device
#define TNFS_ESPIPE		0x13	// Attempt to seek on a FIFO or pipe
#define TNFS_EROFS		0x14	// Read only filesystem
#define TNFS_ENAMETOOLONG	0x15	// Filename too long
#define TNFS_ENOSYS		0x16	// Function not implemented
#define TNFS_ENOTEMPTY		0x17	// Directory not empty
#define TNFS_ELOOP		0x18	// Too many symbolic links encountered
#define TNFS_ENODATA		0x19	// No data available
#define TNFS_ENOSTR		0x1A	// Out of streams resources
#define TNFS_EPROTO		0x1B	// Protocol error
#define TNFS_EBADFD		0x1C	// File descriptor in bad state
#define TNFS_EUSERS		0x1D	// Too many users
#define TNFS_ENOBUFS		0x1E	// No buffer space available
#define TNFS_EALREADY		0x1F	// Operation already in progress
#define TNFS_ESTALE		0x20	// Stale TNFS handle
#define TNFS_EOF		0x21	// End of file

/* directory options for opendirx() */
#define TNFS_DIROPT_NO_FOLDERSFIRST 0x01 	// Disable sorting directories before regular files
#define TNFS_DIROPT_NO_SKIPHIDDEN 0x02   	// Disable ignoring hidden files
#define TNFS_DIROPT_NO_SKIPSPECIAL 0x04  	// Disable ignoring special flies
#define TNFS_DIROPT_DIR_PATTERN 0x08     	// Match pattern applies to directories also

/* sorting options for opendirx() */
#define TNFS_DIRSORT_NONE 0x01			// Disable all sorting
#define TNFS_DIRSORT_CASE 0x02			// Sorting is case-sensitive
#define TNFS_DIRSORT_DESCENDING 0x04		// Sorting is descendnig, not ascending
#define TNFS_DIRSORT_MODIFIED 0x08		// Sort by file's modified timestamp instead of name
#define TNFS_DIRSORT_SIZE 0x10			// Sort by the file's size instead of name

#define TNFS_DIRSTATUS_EOF 0x01			// flag indicating that we received all directory entries

/* options for open() */
#define TNFS_O_RDONLY	0x0001	// Open read only
#define TNFS_O_WRONLY	0x0002	// Open write only
#define TNFS_O_RDWR	0x0003	// Open read/write
#define TNFS_O_APPEND	0x0008	// Append to the file, if it exists (write only)
#define TNFS_O_CREAT	0x0100	// Create the file if it doesn't exist (write only)
#define TNFS_O_TRUNC	0x0200	// Truncate the file on open for writing
#define TNFS_O_EXCL	0x0400	// With O_CREAT, returns an error if the file exists

/* options for lseek() */
#define TNFS_SEEK_SET	0x00	// Go to an absolute position in the file
#define TNFS_SEEK_CUR	0x01	// Go to a relative offset from the current position
#define TNFS_SEEK_END	0x02	// Seek to EOF

/* private functions (do not use them) */
int  tnfs_sendReceive(int length);
void tnfs_prepareCommand(uint8_t cmd);
int  tnfs_readdirx(struct dirx_data* data);

/* public functions */
char* tnfs_get_buffer();
void tnfs_connect(char* host, bool useTCP);
void tnfs_disconnect();
int  tnfs_mount(const char* dir, const char* username, const char* password);
int  tnfs_remount(void);   /* re-mount with saved root; use when session dies after closedir */
int  tnfs_umount();
int  tnfs_opendir(const char* dir);
int  tnfs_readdir(char handle, char* dest);
int  tnfs_opendirx(char* dir, char* pattern, uint8_t diropts, uint8_t sortopts, struct dirx_data* data);
int  tnfs_closedir(char handle);
int  tnfs_nextdirx(struct dirx_data* data, struct dirx_item* xitem);
int  tnfs_telldir(char handle, uint32_t* position);
int  tnfs_seekdir(char handle, uint32_t position);
int  tnfs_mkdir(char* dir);
int  tnfs_rmdir(char* dir);
int  tnfs_open(char* filename, uint16_t flags, uint16_t mode);
int  tnfs_read(char* fb, uint8_t handle, uint16_t maxlen);
int  tnfs_write(char* fb, uint8_t handle, uint16_t maxlen);
int  tnfs_close(uint8_t handle);
int  tnfs_stat(char* filename, struct fstat* st);
int  tnfs_lseek(uint8_t handle, uint8_t seektype, uint32_t position);
int  tnfs_unlink(char* filename);
int  tnfs_chmod(uint16_t mode, char* filename);
int  tnfs_rename(char* source, char* destination);
int  tnfs_size(uint32_t* kb);
int  tnfs_free(uint32_t* kb);
const char* tnfs_error_string(int error);

#ifdef __cplusplus
}
#endif

#endif /* __tnfs_h__ */


