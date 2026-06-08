#include "tnfs.h"
#include "ringbuf.h"
// #define DEBUG 1

const char TNFS_PROTOCOL_VERSION[] = {0x02, 0x01};

/* tnfs global variables */
char     tnfs_buffer[TNFS_BUFFERSIZE];  /* send and receive buffer */
uint16_t tnfs_session_id = 0;           /* session id from server */
uint8_t  tnfs_request_id = 0;           /* sequence counter, 8-bit wrapping */
uint16_t tnfs_min_retry_ms = 0;         /* server minimum retry interval (from MOUNT response) */
static int s_mounted = 0;               /* 1 after successful mount, 0 after umount */

static char s_tnfs_root[TNFS_MAX_PATH_LEN]; /* saved mount root for tnfs_remount */

/* Short error name for ring-buffer logging (fits on one token). */
static const char *tnfs_err_name(uint8_t err)
{
    switch (err) {
    case TNFS_OK:           return "OK";
    case TNFS_EPERM:        return "EPERM";
    case TNFS_ENOENT:       return "ENOENT";
    case TNFS_EIO:          return "EIO";
    case TNFS_ENXIO:        return "ENXIO";
    case TNFS_E2BIG:        return "E2BIG";
    case TNFS_EBADF:        return "EBADF";
    case TNFS_EAGAIN:       return "EAGAIN";
    case TNFS_ENOMEM:       return "ENOMEM";
    case TNFS_EACCES:       return "EACCES";
    case TNFS_EBUSY:        return "EBUSY";
    case TNFS_EEXIST:       return "EEXIST";
    case TNFS_ENOTDIR:      return "ENOTDIR";
    case TNFS_EISDIR:       return "EISDIR";
    case TNFS_EINVAL:       return "EINVAL";
    case TNFS_ENFILE:       return "ENFILE";
    case TNFS_EMFILE:       return "EMFILE";
    case TNFS_EFBIG:        return "EFBIG";
    case TNFS_ENOSPC:       return "ENOSPC";
    case TNFS_ESPIPE:       return "ESPIPE";
    case TNFS_EROFS:        return "EROFS";
    case TNFS_ENAMETOOLONG: return "ENAMETOOLONG";
    case TNFS_ENOSYS:       return "ENOSYS";
    case TNFS_ENOTEMPTY:    return "ENOTEMPTY";
    case TNFS_ELOOP:        return "ELOOP";
    case TNFS_ENODATA:      return "ENODATA";
    case TNFS_ENOSTR:       return "ENOSTR";
    case TNFS_EPROTO:       return "EPROTO";
    case TNFS_EBADFD:       return "EBADFD";
    case TNFS_EUSERS:       return "EUSERS";
    case TNFS_ENOBUFS:      return "ENOBUFS";
    case TNFS_EALREADY:     return "EALREADY";
    case TNFS_ESTALE:       return "ESTALE";
    case TNFS_EOF:          return "EOF";
    default:                return "?";
    }
}

/* sends the allready buffered data to the server and waits for a response */
int tnfs_sendReceive(int length)
{
    int retry   = 0;
    int rlength = 0;
    int src     = 0;
    /* Save request header before netw_recv() overwrites tnfs_buffer with the response. */
    uint8_t req_sid_lo = (uint8_t)tnfs_buffer[0];
    uint8_t req_sid_hi = (uint8_t)tnfs_buffer[1];
    uint8_t req_seq    = (uint8_t)tnfs_buffer[2];
    uint8_t req_cmd    = (uint8_t)tnfs_buffer[3];
#ifdef DEBUG
    int i = 0;
#endif

    do {
        /* clear receive state before sending so a fast response is not lost */
        netw_clear_rx();

        if (rbuf.enabled) {
            int pi, pn;
            rb_write("TX#"); rb_dec((unsigned)retry);
            rb_write(" sid=");
            rb_hex8(req_sid_hi);
            rb_hex8(req_sid_lo);
            rb_write(" seq="); rb_hex8(req_seq);
            rb_write(" cmd="); rb_hex8(req_cmd);
            rb_write(" len="); rb_dec((unsigned)length);
            rb_write(" pay=");
            pn = (length < 16) ? length : 16;
            for (pi = 0; pi < pn; pi++) {
                rb_hex8((unsigned char)tnfs_buffer[pi]);
                rb_putc(' ');
            }
            rb_write("\r\n");
        }

        src = netw_send((const uint8_t*)tnfs_buffer, length);

        if (src != 0) {
            rlength = src;  /* send failed — skip recv, count as retry */
        } else {
            rlength = netw_recv((uint8_t*)tnfs_buffer, TNFS_BUFFERSIZE);
        }

        if (rbuf.enabled) {
            if (rlength > 0) {
                int pi, pn;
                uint8_t rx_err = (uint8_t)tnfs_buffer[4];
                rb_write("RX sid=");
                rb_hex8((unsigned char)tnfs_buffer[1]);
                rb_hex8((unsigned char)tnfs_buffer[0]);
                rb_write(" seq="); rb_hex8((unsigned char)tnfs_buffer[2]);
                rb_write(" cmd="); rb_hex8((unsigned char)tnfs_buffer[3]);
                rb_write(" err="); rb_hex8(rx_err);
                rb_putc('('); rb_write(tnfs_err_name(rx_err)); rb_putc(')');
                rb_write(" len="); rb_dec((unsigned)rlength);
                rb_write(" pay=");
                pn = (rlength < 16) ? rlength : 16;
                for (pi = 0; pi < pn; pi++) {
                    rb_hex8((unsigned char)tnfs_buffer[pi]);
                    rb_putc(' ');
                }
                rb_write("\r\n");
            } else {
                rb_write("RX FAIL rlength="); rb_dec((unsigned)(rlength < 0 ? (unsigned)(-rlength) : 0));
                rb_write(" exp seq="); rb_hex8(req_seq);
                rb_write(" cmd="); rb_hex8(req_cmd);
                rb_write("\r\n");
            }
        }

        /* Detect stale/mismatched response: seq or cmd doesn't match request.
         * SID is only checked when a session exists (SID != 0) because MOUNT
         * responses carry a newly-assigned SID that differs from the request's 0x0000.
         * Cannot retry — buffer now contains the response, original request is lost. */
        if (rlength > 0) {
            int sid_ok = (req_sid_lo == 0 && req_sid_hi == 0) ||  /* MOUNT: skip SID check */
                         ((uint8_t)tnfs_buffer[0] == req_sid_lo &&
                          (uint8_t)tnfs_buffer[1] == req_sid_hi);
            if (!sid_ok ||
                (uint8_t)tnfs_buffer[2] != req_seq ||
                (uint8_t)tnfs_buffer[3] != req_cmd) {
                if (rbuf.enabled) {
                    rb_write("MISMATCH exp sid=");
                    rb_hex8(req_sid_hi); rb_hex8(req_sid_lo);
                    rb_write(" seq="); rb_hex8(req_seq);
                    rb_write(" cmd="); rb_hex8(req_cmd);
                    rb_write(" got sid=");
                    rb_hex8((uint8_t)tnfs_buffer[1]);
                    rb_hex8((uint8_t)tnfs_buffer[0]);
                    rb_write(" seq="); rb_hex8((uint8_t)tnfs_buffer[2]);
                    rb_write(" cmd="); rb_hex8((uint8_t)tnfs_buffer[3]);
                    rb_write(" err="); rb_hex8((uint8_t)tnfs_buffer[4]);
                    rb_write("\r\n");
                }
                tnfs_buffer[4] = TNFS_EPROTO;
                return -TNFS_EPROTO;
            }
        }

        retry++;

    } while (rlength <= 0 && retry < TNFS_SEND_RETRIES);

    /* no response after retries */
    if (rlength <= 0) {
        if (rbuf.enabled) {
            rb_write("TNFS TO exp sid=");
            rb_hex8(req_sid_hi); rb_hex8(req_sid_lo);
            rb_write(" seq="); rb_hex8(req_seq);
            rb_write(" cmd="); rb_hex8(req_cmd);
            rb_write(" tries="); rb_dec((unsigned)retry);
            rb_write("\r\n");
        }
#ifdef DEBUG
        printf("Server did not respond, transfer aborted!\n\n");
#endif
        tnfs_buffer[4] = TNFS_EPROTO;   /* mirror server behaviour */
        return -TNFS_EPROTO;
    }

#ifdef DEBUG
    printf("recv: ");
    for (i = 0; i < rlength; i++) {
        printf("%02X ", (uint8_t)tnfs_buffer[i]);
    }
    printf("\n\n");
#endif

    /* check server error code */
    if (tnfs_buffer[4] != 0x00 && tnfs_buffer[4] != 0x21) {
#ifdef DEBUG
        printf("Server returned error code: %02X\n\n",
               (uint8_t)tnfs_buffer[4]);
#endif
        return -tnfs_buffer[4];
    }

    return rlength;
}

/* buffers a new command header */
void tnfs_prepareCommand(uint8_t cmd)
{
    memset(tnfs_buffer, 0, sizeof(tnfs_buffer));
    memcpy(&tnfs_buffer[0], &tnfs_session_id, 2);
    tnfs_buffer[2] = tnfs_request_id++;
    tnfs_buffer[3] = cmd;
    if (rbuf.enabled) {
        rb_write("CMD ");
        rb_hex8(cmd);
        rb_write(" sid=");
        rb_hex8((uint8_t)(tnfs_session_id >> 8));
        rb_hex8((uint8_t)tnfs_session_id);
        rb_write(" seq=");
        rb_hex8(tnfs_buffer[2]);
        rb_write("\r\n");
    }
}

/* Establish a new session */
int tnfs_mount(const char* dir, const char* username, const char* password)
{
    size_t length = 6;
    uint16_t retry_time = 0;

    /* Save root path so tnfs_remount() can re-establish the session later */
    strncpy(s_tnfs_root, dir, TNFS_MAX_PATH_LEN - 1);
    s_tnfs_root[TNFS_MAX_PATH_LEN - 1] = '\0';

    tnfs_prepareCommand(0x00); /* TNFS_CMD_MOUNT */
    memcpy(&tnfs_buffer[4], TNFS_PROTOCOL_VERSION, 2);

    strcpy(&tnfs_buffer[length], dir);
    length += strlen(dir) + 1;

    strcpy(&tnfs_buffer[length], username);
    length += strlen(username) + 1;

    strcpy(&tnfs_buffer[length], password);
    length += strlen(password) + 1;

    length = tnfs_sendReceive((int)length);
    if (tnfs_buffer[4] == 0x00) {
        /* session id */
        memcpy(&tnfs_session_id, &tnfs_buffer[0], 2);

        /* retry time (uint16 little endian) */
        memcpy(&retry_time, &tnfs_buffer[7], 2);

        memcpy(&tnfs_min_retry_ms, &tnfs_buffer[7], 2);
        s_mounted = 1;
        if (rbuf.enabled) {
            rb_write("MOUNTED sid=");
            rb_hex8((uint8_t)(tnfs_session_id >> 8));
            rb_hex8((uint8_t)tnfs_session_id);
            rb_write(" retry_ms=");
            rb_hex8((uint8_t)(tnfs_min_retry_ms >> 8));
            rb_hex8((uint8_t)tnfs_min_retry_ms);
            rb_write("\r\n");
        }
#ifdef DEBUG
        printf("session id: %u\n", tnfs_session_id);
        printf("server minimal retry time: %u\n\n", retry_time);
#endif
    } else {
        if (rbuf.enabled) {
            rb_write("MOUNT FAIL err="); rb_hex8(tnfs_buffer[4]); rb_write("\r\n");
        }
    }

    return -tnfs_buffer[4];
}

/* Ends the session */
int tnfs_umount()
{
    if (rbuf.enabled) {
        rb_write("UMOUNT sid=");
        rb_hex8((uint8_t)(tnfs_session_id >> 8));
        rb_hex8((uint8_t)tnfs_session_id);
        rb_write("\r\n");
    }
    tnfs_prepareCommand(0x01);
    tnfs_sendReceive(4);
    /* Invalidate session regardless of server response. */
    tnfs_session_id = 0;
    s_mounted = 0;
    if (rbuf.enabled) rb_write("UMOUNT done sid=0000\r\n");
    return tnfs_buffer[4] * -1;
}

/*
 * Re-establish a fresh TNFS session using the root path from the last tnfs_mount().
 * Sets session_id=0 so the server allocates a new session.
 * Needed when the server drops the session (e.g. after CLOSEDIR in some implementations).
 */
int tnfs_remount(void)
{
    if (rbuf.enabled) {
        rb_write("REMOUNT root=\"");
        rb_write(s_tnfs_root);
        rb_write("\"\r\n");
    }
    tnfs_session_id = 0;
    return tnfs_mount(s_tnfs_root, "", "");
}

/* Open a directory.
 * TNFS spec (tnfs-protocol.md): OPENDIR requires a null-terminated ABSOLUTE path.
 * The path delimiter is always '/'.
 *
 * Canonicalisation rules applied before sending:
 *   NULL or ""  → "/"          (root)
 *   "foo"       → "/foo"       (relative → absolute)
 *   "/foo"      → "/foo"       (already absolute, unchanged)
 *
 * Wire format for root:   [hdr 4 bytes] 0x2F 0x00   (6 bytes total)
 * Old (broken) root send: [hdr 4 bytes] 0x00 0x00   (empty string + padding)
 */
int tnfs_opendir(const char* path)
{
    int length;

    if (!s_mounted || tnfs_session_id == 0) {
        if (rbuf.enabled) {
            rb_write("OPENDIR NO SESSION sid=");
            rb_hex8((uint8_t)(tnfs_session_id >> 8));
            rb_hex8((uint8_t)tnfs_session_id);
            rb_write(" mnt="); rb_hex8((uint8_t)s_mounted);
            rb_write("\r\n");
        }
        return -TNFS_EPROTO;
    }

    tnfs_prepareCommand(0x10);  /* clears tnfs_buffer[4..1023] to 0 */

    /* Build absolute path directly into tnfs_buffer[4..]. */
    if (path == NULL || path[0] == '\0') {
        /* Empty/NULL → root "/" */
        tnfs_buffer[4] = '/';
        tnfs_buffer[5] = '\0';
        length = 6;
    } else if (path[0] != '/') {
        /* Relative path → prepend "/" */
        tnfs_buffer[4] = '/';
        strcpy(&tnfs_buffer[5], path);
        length = 5 + (int)strlen(path) + 1;
    } else {
        /* Already absolute */
        strcpy(&tnfs_buffer[4], path);
        length = 4 + (int)strlen(path) + 1;
    }

    if (rbuf.enabled) {
        rb_write("OPENDIR path=\"");
        rb_write(&tnfs_buffer[4]);
        rb_write("\"\r\n");
    }

    length = tnfs_sendReceive(length);
    if (length == 6 && tnfs_buffer[4] == 0x00)
        return tnfs_buffer[5];  /* directory handle */

    if (rbuf.enabled) {
        if ((uint8_t)tnfs_buffer[4] == TNFS_EPROTO) {
            rb_write("OPENDIR TIMEOUT\r\n");
        } else {
            rb_write("OPENDIR ERR err=");
            rb_hex8((uint8_t)tnfs_buffer[4]);
            rb_putc('('); rb_write(tnfs_err_name((uint8_t)tnfs_buffer[4])); rb_putc(')');
            rb_write("\r\n");
        }
    }
    return tnfs_buffer[4] * -1;
}

/* reads one entry from the open directory */
int tnfs_readdir(char handle, char* dest)
{
    int length = 5;

    tnfs_prepareCommand(0x11);
    tnfs_buffer[4] = handle;
    
    length = tnfs_sendReceive(length);
    if (tnfs_buffer[4] == 0x00) {
        strcpy(dest, &tnfs_buffer[5]);
    } else if (rbuf.enabled) {
        uint8_t re = (uint8_t)tnfs_buffer[4];
        if (re == TNFS_EOF) {
            rb_write("READDIR EOF\r\n");
        } else if (re == TNFS_EPROTO) {
            rb_write("READDIR TIMEOUT\r\n");
        } else {
            rb_write("READDIR ERR=");
            rb_hex8(re);
            rb_putc('('); rb_write(tnfs_err_name(re)); rb_putc(')');
            rb_write("\r\n");
        }
    }

    return tnfs_buffer[4] * -1; /* return code */
}

/* Open a directory (with a lot of options) */
int tnfs_opendirx(char* path, char* pattern, uint8_t diropts, uint8_t sortopts, struct dirx_data* data)
{
    int length = 8;

    tnfs_prepareCommand(0x17);
    tnfs_buffer[4] = diropts;			// directory options
    tnfs_buffer[5] = sortopts;			// sort options
    // leave tnfs_buffer[6] and [7] to zero  because we want the total files found
    strcpy(&tnfs_buffer[length], pattern);		// search pattern
    length += strlen(pattern)+1;
    strcpy(&tnfs_buffer[length], path);		// directory path
    length += strlen(path)+1;
    
    length = tnfs_sendReceive(length);
    
    memset(data, 0, sizeof(struct dirx_data)); // set whole structure to zeros
    if(length == 8 && tnfs_buffer[4] == 0x00) {
    	data->handle = tnfs_buffer[5];
    	memcpy(&data->entries, &tnfs_buffer[6], 2); // copy the number of matching directory entries found in odirx.entries.
    }
    
    return tnfs_buffer[4] * -1; // Return code
}

/* Closes a directory */
int tnfs_closedir(char handle)
{
    int length = 5;
    int rc;

    tnfs_prepareCommand(0x12);
    tnfs_buffer[4] = handle;

    rc = tnfs_sendReceive(length);
    if (rbuf.enabled && rc <= 0) {
        rb_write("CLOSEDIR FAIL rc="); rb_hex8((uint8_t)(-rc)); rb_write("\r\n");
    }
    return tnfs_buffer[4] * -1;
}

/* fills the buffer with multiple entries from the open directory with extra stat information for each entry */
int tnfs_readdirx(struct dirx_data* data)
{
    int length = 6;

    tnfs_prepareCommand(0x18);
    tnfs_buffer[4] = data->handle;
    tnfs_buffer[5] = TNFS_MAX_RESULTS;
    
    length = tnfs_sendReceive(length);
    
    if(length > 8 && tnfs_buffer[4] == 0x00) {
    	data->count  = tnfs_buffer[5];
    	data->status = tnfs_buffer[6];
    	memcpy(&data->dirpos, &tnfs_buffer[7], 2); // copy the position of first entry as given by TELLDIR
    }
    
    return tnfs_buffer[4] * -1;
}

/* reads one entry from the open directory with extra stat information */
int tnfs_nextdirx(struct dirx_data* data, struct dirx_item* xitem)
{
    int code;
    
    if(data->entry >= data->count) {
    	if(data->status == TNFS_DIRSTATUS_EOF) {
    	    return TNFS_EOF;
    	}
	code = tnfs_readdirx(data);
	if(code != 0) {
	    return code * -1; // error
	}
    	data->needle = 9;
    	data->entry = 0;
    }
    /* fill dirx_item structure */
    xitem->flags = tnfs_buffer[data->needle];
    memcpy(&xitem->size, &tnfs_buffer[data->needle + 1], 4); 
    memcpy(&xitem->modified, &tnfs_buffer[data->needle + 5], 4); 
    memcpy(&xitem->created, &tnfs_buffer[data->needle + 9], 4); 
    xitem->name = &tnfs_buffer[data->needle + 13];
    
    /* increase counters */
    data->needle += strlen(xitem->name) + 14;
    data->entry++;
    
    return 0;
}

/* Returns the entry position within current directory results */
int tnfs_telldir(char handle, uint32_t* position) 
{
    int length = 5;

    tnfs_prepareCommand(0x15);
    tnfs_buffer[4] = handle;
    
    length = tnfs_sendReceive(length);
    
    if(length == 9 && tnfs_buffer[4] == 0x00) {
    	memcpy(position, &tnfs_buffer[5], 4);
    }

    return tnfs_buffer[4];
}

/* Moves current directory results position to new value */
int tnfs_seekdir(char handle, uint32_t position) 
{
    int length = 9;

    tnfs_prepareCommand(0x16);
    tnfs_buffer[4] = handle;
    memcpy(&tnfs_buffer[5], &position, 4);
    
    tnfs_sendReceive(length);
    
    return tnfs_buffer[4];
}

/* Make a new directory */
int tnfs_mkdir(char* dir)
{
    int length = 4;

    tnfs_prepareCommand(0x13);
    strcpy(&tnfs_buffer[length], dir);
    length += strlen(dir)+1;
    
    tnfs_sendReceive(length);

    return tnfs_buffer[4];
}

/* Deletes a empty directory */
int tnfs_rmdir(char* dir)
{
    int length = 4;

    tnfs_prepareCommand(0x14);
    strcpy(&tnfs_buffer[length], dir);
    length += strlen(dir)+1;
    
    tnfs_sendReceive(length);

    return tnfs_buffer[4];
}

/* Open a file */
int tnfs_open(char* filename, uint16_t flags, uint16_t mode)
{
    int length = 8;

    tnfs_prepareCommand(0x29);
    memcpy(&tnfs_buffer[4], &flags, 2);
    memcpy(&tnfs_buffer[6], &mode, 2);
    strcpy(&tnfs_buffer[length], filename);
    length += strlen(filename)+1;

    length = tnfs_sendReceive(length);
    if(tnfs_buffer[4] != 0x00)
    	return tnfs_buffer[4] * -1;
    
    return tnfs_buffer[5]; // filehandle
}

/* read data from a file */
int tnfs_read(char* data, uint8_t handle, uint16_t maxlen)
{
    int length = 7;

    tnfs_prepareCommand(0x21);
    tnfs_buffer[4] = handle;
    memcpy(&tnfs_buffer[5], &maxlen, 2);

    length = tnfs_sendReceive(length);
    
    if(tnfs_buffer[4] != 0x00) {
        return tnfs_buffer[4] * -1; // return code
    }
    
    memcpy(&maxlen, &tnfs_buffer[5], 2);
    memcpy(data, &tnfs_buffer[7], maxlen);
    
    return maxlen; // actual length of data
}

/* write data to a file */
int tnfs_write(char* data, uint8_t handle, uint16_t maxlen)
{
    int length = 7;

    tnfs_prepareCommand(0x22);
    tnfs_buffer[4] = handle;
    memcpy(&tnfs_buffer[5], &maxlen, 2);
    memcpy(&tnfs_buffer[7], data, maxlen);
    length += maxlen;

    tnfs_sendReceive(length);
    
    return tnfs_buffer[4] * -1; // return code
}

/* close a file */
int tnfs_close(uint8_t handle)
{
    int length = 5;

    tnfs_prepareCommand(0x23);
    tnfs_buffer[4] = handle;
    tnfs_sendReceive(length);
    
    return tnfs_buffer[4] * -1; // Return code
}

/* Get stat information from a file */
int tnfs_stat(char* filename, struct fstat* st)
{
    int length = 4;

    tnfs_prepareCommand(0x24);
    strcpy(&tnfs_buffer[length], filename);
    length += strlen(filename)+1;

    tnfs_sendReceive(length);
    if(tnfs_buffer[4] == 0x00) {
    	memcpy(&st->mode, &tnfs_buffer[5], 2);
    	memcpy(&st->uid, &tnfs_buffer[7], 2);
    	memcpy(&st->gid, &tnfs_buffer[9], 2);
    	memcpy(&st->size, &tnfs_buffer[11], 4);
    	memcpy(&st->atime, &tnfs_buffer[15], 4);
    	memcpy(&st->mtime, &tnfs_buffer[19], 4);
    	memcpy(&st->ctime, &tnfs_buffer[23], 4);
    	strcpy(st->uidstring, &tnfs_buffer[27]);
    	strcpy(st->gidstring, &tnfs_buffer[28+strlen(st->uidstring)]);
    }
    
    return tnfs_buffer[4] * -1; // Return code
}

/* Seeks to a new position in a file */
int tnfs_lseek(uint8_t handle, uint8_t seektype, uint32_t position)
{
    int length = 10;

    tnfs_prepareCommand(0x25);
    tnfs_buffer[4] = handle;
    tnfs_buffer[5] = seektype;
    memcpy(&tnfs_buffer[6], &position, 4);

    tnfs_sendReceive(length);
   
    return tnfs_buffer[4] * -1; // Return code
}

/* Delete a file */
int tnfs_unlink(char* filename)
{
    int length = 4;

    tnfs_prepareCommand(0x26);
    strcpy(&tnfs_buffer[length], filename);
    length += strlen(filename)+1;

    tnfs_sendReceive(length);
   
    return tnfs_buffer[4] * -1; // Return code
}

/* change permissions for a file */
int tnfs_chmod(uint16_t mode, char* filename)
{
    int length = 6;

    tnfs_prepareCommand(0x27);
    memcpy(&tnfs_buffer[4], &mode, 2);
    strcpy(&tnfs_buffer[length], filename);
    length += strlen(filename)+1;

    tnfs_sendReceive(length);
   
    return tnfs_buffer[4] * -1; // Return code
}

/* rename or moves a file within a filesystem */
int tnfs_rename(char* source, char* destination)
{
    int length = 4;

    tnfs_prepareCommand(0x28);
    strcpy(&tnfs_buffer[length], source);
    length += strlen(source)+1;
    strcpy(&tnfs_buffer[length], destination);
    length += strlen(destination)+1;

    tnfs_sendReceive(length);
   
    return tnfs_buffer[4] * -1; // Return code
}

/* Requests the size of the mounted filesystem */
int tnfs_size(uint32_t* kb)
{
    int length = 4;
    tnfs_prepareCommand(0x30);
    tnfs_sendReceive(length);
    
    memset(kb, 0, sizeof(uint32_t));
    if(tnfs_buffer[4] == 0) {
        memcpy(kb, &tnfs_buffer[5], 4);
    }
   
    return tnfs_buffer[4] * -1; // Return code
}

/* Requests the amount of free space on the filesystem */
int tnfs_free(uint32_t* kb)
{
    int length = 4;
    tnfs_prepareCommand(0x31);
    tnfs_sendReceive(length);
    
    memset(kb, 0, sizeof(uint32_t));
    if(tnfs_buffer[4] == 0) {
        memcpy(kb, &tnfs_buffer[5], 4);
    }
   
    return tnfs_buffer[4] * -1; // Return code
}

/* Returns human-readable TNFS error string */
const char* tnfs_error_string(int error)
{
    if (error < 0)
        error = -error;

    switch (error) {
        case TNFS_OK:           return "OK";
        case TNFS_EPERM:        return "Operation not permitted";
        case TNFS_ENOENT:       return "No such file or directory";
        case TNFS_EIO:          return "I/O error";
        case TNFS_ENXIO:        return "No such device or address";
        case TNFS_E2BIG:        return "Argument list too long";
        case TNFS_EBADF:        return "Bad file number";
        case TNFS_EAGAIN:       return "Try again";
        case TNFS_ENOMEM:       return "Out of memory";
        case TNFS_EACCES:       return "Permission denied";
        case TNFS_EBUSY:        return "Device or resource busy";
        case TNFS_EEXIST:       return "File exists";
        case TNFS_ENOTDIR:      return "Not a directory";
        case TNFS_EISDIR:       return "Is a directory";
        case TNFS_EINVAL:       return "Invalid argument";
        case TNFS_ENFILE:       return "File table overflow";
        case TNFS_EMFILE:       return "Too many open files";
        case TNFS_EFBIG:        return "File too large";
        case TNFS_ENOSPC:       return "No space left on device";
        case TNFS_ESPIPE:       return "Illegal seek";
        case TNFS_EROFS:        return "Read-only filesystem";
        case TNFS_ENAMETOOLONG: return "Filename too long";
        case TNFS_ENOSYS:       return "Function not implemented";
        case TNFS_ENOTEMPTY:    return "Directory not empty";
        case TNFS_ELOOP:        return "Too many symbolic links";
        case TNFS_ENODATA:      return "No data available";
        case TNFS_ENOSTR:       return "Out of streams resources";
        case TNFS_EPROTO:       return "Protocol error";
        case TNFS_EBADFD:       return "File descriptor in bad state";
        case TNFS_EUSERS:       return "Too many users";
        case TNFS_ENOBUFS:      return "No buffer space available";
        case TNFS_EALREADY:     return "Operation already in progress";
        case TNFS_ESTALE:       return "Stale TNFS handle";
        case TNFS_EOF:          return "End of file";
        default:                return "Unknown TNFS error";
    }
}
