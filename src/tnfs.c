#include "tnfs.h"
// #define DEBUG 1

/* 
 * TNFS_MAX_RESULTS must be in contrast with the total tnfs_buffer size and max_path length !!!
 * for example is TNFS_MAX_PATH_LEN is 256 and TNFS_MAX_RESULTS = 50 then TNFS_BUFFERSIZE must be: 10 bytes + (13 bytes + 256) * 50 = 13460 bytes 
 */
const uint8_t TNFS_MAX_RESULTS = 14;
const char    TNFS_PROTOCOL_VERSION[] = {0x02, 0x01};

/* tnfs global variables */
char 	 tnfs_buffer[TNFS_BUFFERSIZE];	// send and receive buffer
uint16_t tnfs_session_id = 0;		// stores current session id received from the server
uint8_t  tnfs_request_id = 0;		// request id increases each new request

/* sends the allready buffered data to the server and waits for a response */
int tnfs_sendReceive(int length)
{
    int retry   = 0;
    int rlength = 0;
#ifdef DEBUG
    int i = 0;
#endif


    do {
        /* send request */
        netw_send((const uint8_t*)tnfs_buffer, length);

#ifdef DEBUG
        printf("sent: ");
        for (i = 0; i < length; i++) {
            printf("%02X ", (uint8_t)tnfs_buffer[i]);
        }
        printf("\n");
#endif

        /* wait for response */
        rlength = netw_recv((uint8_t*)tnfs_buffer, TNFS_BUFFERSIZE);

        retry++;

    } while (rlength <= 0 && retry < TNFS_SEND_RETRIES);

    /* no response after retries */
    if (rlength <= 0) {
#ifdef DEBUG
        printf("Server did not respond, transfer aborted!\n\n");
#endif
        tnfs_buffer[4] = TNFS_EPROTO;   // mirror server behaviour
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
}

//* Establish a new session */
int tnfs_mount(const char* dir, const char* username, const char* password)
{
    size_t length = 6;
    uint16_t retry_time = 0;

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

#ifdef DEBUG
        printf("session id: %u\n", tnfs_session_id);
        printf("server minimal retry time: %u\n\n", retry_time);
#endif
    }

    return -tnfs_buffer[4];
}

/* Ends the session */
int tnfs_umount()
{
    tnfs_prepareCommand(0x01);
    tnfs_sendReceive(4);

    return tnfs_buffer[4] * -1; // return code
}

/* Open a directory */
int tnfs_opendir(const char* path)
{
    int length = 4;

    tnfs_prepareCommand(0x10);
    strcpy(&tnfs_buffer[length], path);
    length += strlen(path)+1;
    
    length = tnfs_sendReceive(length);
    if(length == 6 && tnfs_buffer[4] == 0x00)
    	return tnfs_buffer[5]; // tnfs file handle
    
    return tnfs_buffer[4] * -1; // return code
}

/* reads one entry from the open directory */
int tnfs_readdir(char handle, char* dest)
{
    int length = 5;

    tnfs_prepareCommand(0x11);
    tnfs_buffer[4] = handle;
    
    length = tnfs_sendReceive(length);
    if(tnfs_buffer[4] == 0x00) {
    	strcpy(dest, &tnfs_buffer[5]);
    }
    
    return tnfs_buffer[4] * -1; // return code
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

    tnfs_prepareCommand(0x12);
    tnfs_buffer[4] = handle;
    
    tnfs_sendReceive(length);
    
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
