#include "io.h"
#include "error.h"
#include <string.h>
#include <stddef.h>
#include "heap.h"
#include "console.h"
#include "lock.h"

#define MAX_FILES 32
#define FS_NAMELEN 32
#define BLOCK_SIZE 4096
#define VIO_SIZE 512

typedef struct dentry_t
{
    char file_name[FS_NAMELEN];
    uint32_t inode;
    uint8_t reserved[28];
} __attribute((packed)) dentry_t;

typedef struct boot_block_t
{
    uint32_t num_dentry; // Number of directory entries
    uint32_t num_inodes; // Number of inodes
    uint32_t num_data;   // Number of data blocks
    uint8_t reserved[52];
    dentry_t dir_entries[63];
} __attribute((packed)) boot_block_t;

typedef struct inode_t
{
    uint32_t byte_len;             // Length of the file in bytes
    uint32_t data_block_num[1023]; // Data block numbers
} __attribute((packed)) inode_t;

typedef struct data_block_t
{
    uint8_t data[BLOCK_SIZE];
} __attribute((packed)) data_block_t;

typedef struct file_t
{
    struct io_intf io;           // I/O interface
    unsigned long file_position; // Current position in file
    unsigned long file_size;     // Size of the file
    unsigned long inode_number;  // Inode number of the file
    unsigned long flags;         // Flags to indicate the status of the file
} file_t;

int fs_mount(struct io_intf *io);
int fs_open(const char *name, struct io_intf **io);
void fs_close(struct io_intf *io);
long fs_read(struct io_intf *io, void *buf, unsigned long n);
long fs_write(struct io_intf *io, const void *buf, unsigned long n);
int fs_ioctl(struct io_intf *io, int cmd, void *arg);
int fs_getlen(file_t *fd, void *arg);
int fs_getpos(file_t *fd, void *arg);
int fs_setpos(file_t *fd, void *arg);
int fs_getblksz(file_t *fd, void *arg);

static boot_block_t bootblock;

static file_t file_table[MAX_FILES];
static struct io_intf *filesystem_io = NULL;

static int find_free_file_slot(void);
static inode_t** inodes=NULL;
struct lock fs_lock;
int filecount;

/**
 * fs_mount - Mount the filesystem
 * io: Input - A pointer to the IO interface representing the filesystem
 *
 * Output: Returns 0 on success, or an error code (e.g., -EINVAL, -EIO)
 * Functionality: Initializes the filesystem, including loading the boot block and resetting file tables.
 * Side Effects: Sets `filesystem_io` to point to the provided IO interface.
 */
int fs_mount(struct io_intf *io)
{
    filesystem_io = io;
    long ret = ioseek(io, 0); // the address of bootblock in device
    if (ret < 0)
    {
        return ret;
    }
    long out = ioread_full(io, &bootblock, BLOCK_SIZE);
    if (out != BLOCK_SIZE)
        return -EIO;
    if (out < 0)
        return -EIO;
    uint32_t inum=bootblock.num_inodes;
    inodes=(inode_t**)kmalloc(sizeof(inode_t*)*inum);
    ret = ioseek(io, BLOCK_SIZE); // the address of bootblock in device
    if (ret < 0)
    {
        return ret;
    }
    for(uint64_t i=0;i<inum;i++){
        inode_t* inode=(inode_t*)kmalloc(sizeof(inode_t));
        long out = ioread_full(io, inode,sizeof(inode_t));
        if (out !=sizeof(inode_t))
            return -EIO;
        if (out < 0)
            return -EIO;
        inodes[i]=inode;
    }
    lock_init(&fs_lock, "filesystem");
    return 0;
}

// Static function to find an empty slot in the file table
// Find an empty slot in the file table
// i Output - position for spare file
static int find_free_file_slot(void)
{
    for (int i = 0; i < MAX_FILES; ++i)
    {
        if (file_table[i].flags == 0)
        { // get the require file
            return i;
        }
    }
    return -ENOTSUP; // Too many open files
}

/**
 * fs_close - Close a file
 * io: Input - A pointer to the IO interface of the file to close
 *
 * Functionality: Marks the file in the file table as unused.
 * Side Effects: Updates the `flags` field of the file structure to indicate it is closed.
 */
// Close a file and mark its file struct as unused
void fs_close(struct io_intf *io)
{
    if(io==NULL) return;
    struct file_t* file = (void*)io - offsetof(struct file_t, io); //get the position of file 
    file->flags=0;                  //set 0 as unused
    filecount--;
}

/**
 * fs_read - Read data from a file
 * io: Input - A pointer to the IO interface of the file to read from
 * buf: Output - A buffer to store the read data
 * n: Input - The number of bytes to read
 *
 * Output: Returns the number of bytes read on success, or an error code (e.g., -EINVAL, -EIO)
 * Functionality: Reads data from the specified file into the provided buffer.
 * Side Effects: Updates the file's current position.
 */
// Read n bytes from a file associated with io into buf
long fs_read(struct io_intf *io, void *buf, unsigned long n)
{


    lock_acquire(&fs_lock);
    if (io == NULL || buf == NULL)
    {
        lock_release(&fs_lock);
        return -EINVAL;
    }
    struct file_t *const file = (void *)io - offsetof(struct file_t, io);

    // Validate file flags
    if (file->flags == 0)
    {
        lock_release(&fs_lock);
        return -ENOENT;
    }

    // Calculate remaining bytes and position
    unsigned long remaining_bytes = file->file_size - file->file_position; // get the bytes remaing to read
    if (n > remaining_bytes)
    {
        lock_release(&fs_lock);
        return -EBADFMT;
    }
    unsigned long bytes_to_read = n;
    // unsigned long bytes_to_read = (n > remaining_bytes) ? remaining_bytes : n;
    void *curbuf = buf;

    // Read inode
    int64_t nodenum = file->inode_number; // serarch for node
    int set = ioseek(filesystem_io, (nodenum + 1) * BLOCK_SIZE);
    if (set < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }

    // Calculate initial data block and offset within the block
    uint64_t jump_block_count = file->file_position / BLOCK_SIZE;
    uint64_t over_point = file->file_position % BLOCK_SIZE;
    unsigned long bytes_remaining = bytes_to_read;

    // Read first block (partial)
    uint32_t data_block_offset = (uint32_t)(bootblock.num_inodes + 1 + inodes[file->inode_number]->data_block_num[jump_block_count]) * BLOCK_SIZE;
    set = ioseek(filesystem_io, data_block_offset);
    if (set < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }

    data_block_t current_block;
    int ret = ioread_full(filesystem_io, &current_block, sizeof(data_block_t)); // read data block
    if (ret < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }
    if (ret != sizeof(data_block_t))
    {
        lock_release(&fs_lock);
        return -EIO;
    }

    unsigned long first_read_size = (BLOCK_SIZE - over_point < bytes_remaining) ? BLOCK_SIZE - over_point : bytes_remaining;
    memcpy(curbuf, current_block.data + over_point, first_read_size);
    curbuf += first_read_size;
    bytes_remaining -= first_read_size;
    jump_block_count++;

    // Read full blocks
    while (bytes_remaining >= BLOCK_SIZE)
    {
        data_block_offset = (uint32_t)(bootblock.num_inodes + 1 + inodes[file->inode_number]->data_block_num[jump_block_count]) * BLOCK_SIZE;
        set = ioseek(filesystem_io, data_block_offset);
        if (set < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        ret = ioread_full(filesystem_io, curbuf, BLOCK_SIZE);
        if (ret < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }
        if (ret != BLOCK_SIZE)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        curbuf += BLOCK_SIZE;
        bytes_remaining -= BLOCK_SIZE;
        jump_block_count++;
    }

    // Read remaining part of the last block
    if (bytes_remaining > 0)
    {
        data_block_offset = (uint32_t)(bootblock.num_inodes + 1 + inodes[file->inode_number]->data_block_num[jump_block_count]) * BLOCK_SIZE;
        set = ioseek(filesystem_io, data_block_offset);
        if (set < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        ret = ioread_full(filesystem_io, &current_block, sizeof(data_block_t));
        if (ret < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        memcpy(curbuf, current_block.data, bytes_remaining);
    }

    file->file_position += bytes_to_read;
    lock_release(&fs_lock);
    return bytes_to_read;

}
/**
 * fs_write - Write data to a file
 * io: Input - A pointer to the IO interface of the file to write to
 * buf: Input - The buffer containing the data to write
 * n: Input - The number of bytes to write
 *
 * Output: Returns the number of bytes written on success, or an error code (e.g., -EINVAL, -EIO)
 * Functionality: Writes data from the provided buffer into the specified file.
 * Side Effects: Updates the file's current position.
 */
// Write n bytes from buf into the file associated with io
long fs_write(struct io_intf *io, const void *buf, unsigned long n)
{
    lock_acquire(&fs_lock);
    trace("fs write run");
    
    if (io == NULL || buf == NULL)
    {
        lock_release(&fs_lock);
        return -EINVAL;
    }
    struct file_t *const file = (void *)io - offsetof(struct file_t, io);

    // Validate file flags
    if (file->flags == 0)
    {
        lock_release(&fs_lock);
        return -ENOENT;
    }

    // Calculate remaining bytes and position
    unsigned long remaining_bytes = file->file_size - file->file_position;
    if (n > remaining_bytes)
    {
        lock_release(&fs_lock);
        return -EBADFMT;
    }
    unsigned long bytes_to_write = n;
    // unsigned long bytes_to_write = (n > remaining_bytes) ? remaining_bytes : n;
    const void *curbuf = buf;

    // Read inode
    int64_t nodenum = file->inode_number;
    int set = ioseek(filesystem_io, (nodenum + 1) * BLOCK_SIZE); // search for inode
    if (set < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }


    // Calculate initial data block and offset within the block
    uint64_t jump_block_count = file->file_position / BLOCK_SIZE;
    uint64_t over_point = file->file_position % BLOCK_SIZE;
    unsigned long bytes_remaining = bytes_to_write;

    // Write first block (partial)
    uint32_t data_block_offset = (uint32_t)(bootblock.num_inodes + 1 + inodes[file->inode_number]->data_block_num[jump_block_count]) * BLOCK_SIZE;
    set = ioseek(filesystem_io, data_block_offset);
    if (set < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }

    data_block_t current_block;
    int ret = ioread_full(filesystem_io, &current_block, sizeof(data_block_t));
    if (ret < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }

    unsigned long first_write_size = (BLOCK_SIZE - over_point < bytes_remaining) ? BLOCK_SIZE - over_point : bytes_remaining;

    memcpy(current_block.data + over_point, curbuf, first_write_size);

    set = ioseek(filesystem_io, data_block_offset);
    if (set < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }

    ret = iowrite(filesystem_io, &current_block, sizeof(data_block_t));
    if (ret < 0)
    {
        lock_release(&fs_lock);
        return -EIO;
    }

    curbuf += first_write_size;
    bytes_remaining -= first_write_size;
    jump_block_count++;

    // Write full blocks
    while (bytes_remaining >= BLOCK_SIZE)
    {
        data_block_offset = (uint32_t)(bootblock.num_inodes + 1 + inodes[file->inode_number]->data_block_num[jump_block_count]) * BLOCK_SIZE;
        set = ioseek(filesystem_io, data_block_offset);
        if (set < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        ret = iowrite(filesystem_io, curbuf, BLOCK_SIZE);
        if (ret < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        curbuf += BLOCK_SIZE;
        bytes_remaining -= BLOCK_SIZE;
        jump_block_count++;
    }

    // Write remaining part of the last block
    if (bytes_remaining > 0)
    {
        data_block_offset = (uint32_t)(bootblock.num_inodes + 1 + inodes[file->inode_number]->data_block_num[jump_block_count]) * BLOCK_SIZE;
        set = ioseek(filesystem_io, data_block_offset);
        if (set < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        ret = ioread_full(filesystem_io, &current_block, sizeof(data_block_t));
        if (ret < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        memcpy(current_block.data, curbuf, bytes_remaining);

        set = ioseek(filesystem_io, data_block_offset);
        if (set < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }

        ret = iowrite(filesystem_io, &current_block, sizeof(data_block_t));
        if (ret < 0)
        {
            lock_release(&fs_lock);
            return -EIO;
        }
    }

    file->file_position += bytes_to_write;
    lock_release(&fs_lock);
    return bytes_to_write;

}

/**
 * fs_getlen - Get the length of a file
 * fd: Input - A pointer to the file structure
 * arg: Output - A pointer to store the length of the file
 *
 * Output: Returns the length of the file on success, or an error code (e.g., -EINVAL)
 * Functionality: Retrieves the size of the specified file.
 * Side Effects: Modifies the `arg` to store the file size.
 */
// Helper function to get file length
int fs_getlen(file_t *fd, void *arg)
{
    if (fd == NULL || arg == NULL)
        return -EINVAL;
    *(uint64_t *)arg = fd->file_size; // return size
    return fd->file_size;
}

/**
 * fs_getpos - Get the current position in a file
 * fd: Input - A pointer to the file structure
 * arg: Output - A pointer to store the current position
 *
 * Output: Returns the current position on success, or an error code (e.g., -EINVAL)
 * Functionality: Retrieves the current read/write position in the file.
 * Side Effects: Modifies the `arg` to store the current position.
 */
// Helper function to get file position
int fs_getpos(file_t *fd, void *arg)
{
    if (fd == NULL || arg == NULL)
        return -EINVAL;
    *(uint64_t *)arg = fd->file_position; // return position
    return fd->file_position;
}

/**
 * fs_setpos - Set the current position in a file
 * fd: Input - A pointer to the file structure
 * arg: Input - A pointer containing the new position
 *
 * Output: Returns 0 on success, or an error code (e.g., -EINVAL)
 * Functionality: Sets the current read/write position in the file.
 * Side Effects: Updates the file's `file_position` field.
 */
// Helper function to set file position
int fs_setpos(file_t *fd, void *arg)
{

    if (fd == NULL || arg == NULL){
        lock_release(&fs_lock);
        return -EINVAL;}
    unsigned long new_pos = *((unsigned long *)arg);
    if (new_pos > fd->file_size){
        lock_release(&fs_lock);
        return -EINVAL;}
    fd->file_position = new_pos; // set
    return 0;
}

/**
 * fs_getblksz - Get the block size of the filesystem
 * fd: Input - A pointer to the file structure
 * arg: Output - A pointer to store the block size
 *
 * Output: Returns the block size on success, or an error code (e.g., -EINVAL)
 * Functionality: Retrieves the block size of the filesystem.
 * Side Effects: Modifies the `arg` to store the block size.
 */
// Helper function to get block size
int fs_getblksz(file_t *fd, void *arg)
{
    if (fd == NULL || arg == NULL)
        return -EINVAL;
    *(uint64_t *)arg = BLOCK_SIZE; // return size
    return BLOCK_SIZE;
}

/**
 * fs_ioctl - Perform a control operation on a file
 * io: Input - A pointer to the IO interface of the file
 * cmd: Input - The control command to execute (e.g., IOCTL_GETLEN, IOCTL_GETPOS)
 * arg: Output/Input - A pointer to the argument for the command
 *
 * Output: Returns 0 on success, or an error code (e.g., -EINVAL)
 * Functionality: Executes a control operation based on the provided command.
 * Side Effects: May modify `arg` to return information about the file.
 */
int fs_ioctl(struct io_intf *io, int cmd, void *arg)
{
    struct file_t *const file = (void *)io - offsetof(struct file_t, io);
    if (file == NULL)
        return -EINVAL;
    switch (cmd)
    {
    case IOCTL_GETLEN:
        return fs_getlen(file, arg); // call helper function
    case IOCTL_GETPOS:
        return fs_getpos(file, arg);
    case IOCTL_SETPOS:
        return fs_setpos(file, arg);
    case IOCTL_GETBLKSZ:
        return fs_getblksz(file, arg);
    default:
        return -EINVAL;
    }
}

static struct io_ops fs_io_ops = {
    // io inter face
    .close = fs_close,
    .read = fs_read,
    .write = fs_write,
    .ctl = fs_ioctl,
};

/**
 * fs_open - Open a file
 * name: Input - The name of the file to open
 * io: Output - A pointer to the IO interface of the opened file
 *
 * Output: Returns 0 on success, or an error code (e.g., -ENOENT, -ENOTSUP)
 * Functionality: Finds the requested file in the boot block, initializes file metadata, and assigns an IO interface.
 * Side Effects: Updates the file table with the file's metadata.
 */
int fs_open(const char *name, struct io_intf **io) {
    if (!filesystem_io) return -ENODEV;
    if (name == NULL || io == NULL)  return -EINVAL;
    uint32_t loop = bootblock.num_dentry;
    for (uint32_t t = 0; t < loop; t++) {
        if (strcmp((bootblock.dir_entries[t].file_name), name) == 0) { 
            int file_idx = find_free_file_slot();
            if (file_idx <0) return -ENOENT;
            int32_t nodenum = bootblock.dir_entries[t].inode; //get node num
            file_table[file_idx].io.ops = &fs_io_ops;   //initialize file struct
            file_table[file_idx].inode_number = nodenum;
            file_table[file_idx].flags = 1;
            file_table[file_idx].file_position = 0;
            file_table[file_idx].file_size = inodes[nodenum]->byte_len;
            *io = &file_table[file_idx].io;
            file_table[file_idx].io.refcnt=1;
            filecount++;
            return 0;
        }
    }
    return -ENOENT; // No such file or directory
}
