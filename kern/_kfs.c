#include "kfs.h"
#include "heap.h"
#include "lock.h"

file_t file_table[32];  //max file loaded
boot_block_t boot;
static inode_t** inodes=NULL;     //max inodes 63
int file_num;
struct io_intf* block_io;
static struct lock fs_lock;    //filesystem lock
#define BLOCKSIZE 4096
#define TABLESIZE 32
static const struct io_ops file_ops={
    .ctl=fs_ioctl,
    .read=fs_read,
    .write=fs_write,
    .close=fs_close
};

/*
input: null
output: null
discription: initial the file system
*/
void fs_init(void) {
    memset(file_table, 0, sizeof(file_table)); //set the file table
}
/*
input: const char* name,char file_name[]
output: 1 if equal, 0 if not equal
discription: judge whether the two string equal
*/
int chars_equal(const char* name,char file_name[]){
    size_t size_1=strlen(name);
    size_t size_2=strlen(file_name);
    if(size_1!=size_2) return 0;        //if size not match, they cannot be equal
    for(size_t i=0;i<size_1;i++){
        if(name[i]!=file_name[i]) return 0;
    }
    return 1;
}
/*
input struct io_intf* io
output: 0 if success
discription: Takes an io intf* to the filesystem provider and sets up the filesystem for future fs open operations.
*/
int fs_mount(struct io_intf *io) {
    lock_init(&fs_lock, "filesystem");
    block_io = io;
    uint64_t pos=0;
    io->ops->ctl(io,IOCTL_SETPOS,&pos);
    io->ops->read(io,&boot,sizeof(boot_block_t));  //load boot block to global variable for convinence
    uint32_t inum=boot.num_inodes;
    inodes=(inode_t**)kmalloc(sizeof(inode_t*)*inum);
    pos=4096;
    io->ops->ctl(io,IOCTL_SETPOS,&pos);
    for(uint64_t i=0;i<inum;i++){
        inode_t* inode=(inode_t*)kmalloc(sizeof(inode_t));
        io->ops->read(io,inode,sizeof(inode_t));
        inodes[i]=inode;
    }
    return 0;
}
/*
input: const char* name, struct io_intf** io
output: -1 if fail to open, index in the file table if success
discription: Takes the name of the file to be opened and modifies
the given pointer to contain the io intf of the file
*/
int fs_open(const char* name, struct io_intf** io){
    uint32_t i=0;
    for(i=0;i<boot.num_dentry;i++){
        if(strcmp(boot.dir_entries[i].file_name,name)!=0) continue;  //get corresponding inode
        else{
            uint32_t inode_num=boot.dir_entries[i].inode;
            int index=0;
            for(int i=0;i<TABLESIZE;i++){                           //search the file table and find proper place
                if(file_table[i].flags==0){
                    index=i;
                    break;
                }
                else{
                    if(i==TABLESIZE-1) return -1;
                    else continue;
                }
            }
            file_table[index].file_position=0;
            file_table[index].file_size=inodes[inode_num]->byte_len;
            file_table[index].flags=1;
            file_table[index].inode=inode_num;
            file_table[index].io.ops=&file_ops;
            *io=&file_table[index].io;
            file_table[index].io.refcnt = 1;
            file_num++;
            return index;
        }
    }
    return -1;
}
/*
input: struct io_intf* io
output: null
discription: Marks the file struct associated with io as unused
*/
void fs_close(struct io_intf* io){
    if(io==NULL) return;
    file_t* file;
    file=(file_t*) io;
    file->flags=0;                  //set 0 as unused
    file_num--;
}
/*
input: struct io_intf* io, const void* buf, unsigned long n
output: -1 if fail read, 0 if EOF, or successfully written byte
discription: Writes n bytes from buf into the file associated with io
*/
long fs_write(struct io_intf* io, const void* buf, unsigned long n){
    lock_acquire(&fs_lock);
    file_t* file;
    file=(file_t*) io;
    const char* buffer=buf;
    if(file->flags==0) return -1;
    if(file->file_position==file->file_size){
        lock_release(&fs_lock);
        return 0;                  //EOF
    }
    else{
        long write_byte=0;
        for(unsigned long i=0;i<n;i++){
            if(file->file_size==file->file_position) return write_byte;
            else{
                uint64_t pos=file->file_position;
                uint64_t a=pos/BLOCKSIZE;
                uint64_t b=pos%BLOCKSIZE;
                uint64_t block_num=inodes[file->inode]->data_block_num[a];
                uint64_t cur=BLOCKSIZE+BLOCKSIZE*boot.num_inodes+BLOCKSIZE*block_num+b;
                block_io->ops->ctl(block_io,IOCTL_SETPOS,&cur);
                block_io->ops->write(block_io,buffer++,1);
                write_byte++;
                file->file_position++;
            }
        }
        lock_release(&fs_lock);
        return write_byte;
    }
}
/*
input: struct io_intf* io, void* buf, unsigned long n
output: -1 if fail to read, 0 if EOF, else reading byte
discription: Reads n bytes from the file associated with io into buf
*/
long fs_read(struct io_intf* io, void* buf, unsigned long n){
    lock_acquire(&fs_lock);
    file_t* file;
    file=(file_t*) io;
    char* buffer=buf;
    if(file->flags==0) return -1;
    if(file->file_size==file->file_position){
        lock_release(&fs_lock);
        return 0;
    }
    else{
        long read_byte=0;
        for(unsigned long i=0;i<n;i++){
            if(file->file_size==file->file_position) return read_byte;  //EOF
            else{
                uint64_t pos=file->file_position;
                uint64_t a=pos/BLOCKSIZE;
                uint64_t b=pos%BLOCKSIZE;
                uint64_t block_num=inodes[file->inode]->data_block_num[a];
                uint64_t cur=BLOCKSIZE+BLOCKSIZE*boot.num_inodes+BLOCKSIZE*block_num+b;
                block_io->ops->ctl(block_io,IOCTL_SETPOS,&cur);
                block_io->ops->read(block_io,buffer++,1);
                read_byte++;
                file->file_position++;
            }
        }
        lock_release(&fs_lock);
        return read_byte;
    }
}
/*
input：struct io intf*, int cmd, void* arg
output: -1 if error command, else return corresponding command code
discription: Performs a device-specific function based on cmd.
*/
int fs_ioctl(struct io_intf* io, int cmd, void* arg){
    file_t* fd=(file_t*) io;
    switch (cmd){                       //4 commands required
        case IOCTL_GETBLKSZ:
        return fs_getblksz(fd,arg);
        break;
        case IOCTL_GETLEN:
        return fs_getlen(fd,arg);
        break;
        case IOCTL_GETPOS:
        return fs_getpos(fd,arg);
        break;
        case IOCTL_SETPOS:
        return fs_setpos(fd,arg);
        break;
        default:
        return -1;
    }
}
/*
input: file_t* fd, void* arg
output: file size
discription: Returns the length of the file
*/
int fs_getlen(file_t* fd, void* arg){
    return fd->file_size;
}
/*
input: file_t* fd, void* arg
output: file current position
discription: Returns the current position in the file
*/
int fs_getpos(file_t* fd, void* arg){
    return fd->file_position;
}
/*
input: file_t* fd, void* arg
output: file position after set or -1 if fail
discription: Sets the current position in the file
*/
int fs_setpos(file_t* fd, void* arg){
    uint64_t* a=(uint64_t*)arg;
    uint64_t b=*a;
    uint64_t c=fd->file_size;
    if(b>c) return -1;    //-1 if fail
    fd->file_position=b;
    return fd->file_position;
}
/*
input: file_t* fd, void* arg
output: blocksize
discription: Returns the block size of the filesystem
*/
int fs_getblksz(file_t* fd, void* arg){
    return BLOCKSIZE;
}
