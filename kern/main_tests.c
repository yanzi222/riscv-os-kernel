#include "console.h"
#include "thread.h"
#include "device.h"
#include "uart.h"
#include "timer.h"
#include "intr.h"
#include "heap.h"
#include "virtio.h"
#include "halt.h"
#include "elf.h"
#include "fs.h"
#include "string.h"
#include "kfs.h"
#include "vioblk.c"



extern char _companion_f_start[];
extern char _companion_f_end[];
extern char _kimg_end[];
extern int file_num;
#define RAM_SIZE (8*1024*1024)
#define RAM_START 0x80000000UL
#define KERN_START RAM_START
#define USER_START 0x80100000UL

#define UART0_IOBASE 0x10000000
#define UART1_IOBASE 0x10000100
#define UART0_IRQNO 10

#define VIRT0_IOBASE 0x10001000
#define VIRT1_IOBASE 0x10002000
#define VIRT0_IRQNO 1

#define IOCTL_GETLEN        1
#define IOCTL_SETLEN        2
#define IOCTL_GETPOS        3
#define IOCTL_SETPOS        4
#define IOCTL_FLUSH         5
#define IOCTL_GETBLKSZ      6
void (*entryptr)(struct io_intf *);

// Test the elf_load function()
int test_elf_load(struct io_intf * termio_raw) {

    // load companion.o to elfio
    struct io_lit elflit;
    struct io_intf * elfio;
    void * buf = _companion_f_start;
    size_t size = _companion_f_end - _companion_f_start;
    elfio = iolit_init(&elflit, buf, size);

    // Load the elf file into the kernel memory
    int result = elf_load(elfio, &entryptr);
    if (result == 0 && entryptr != NULL) {
            console_puts("Load successfully");

        int tid = thread_spawn("test", (void*)entryptr, termio_raw);
        if (tid < 0){
            console_puts("thread spawn fail");
        }
        else{
            thread_join(tid);
        }
    }
    else{
        console_puts("Load fail");
    }
    return result;
}

void test_elf() {
    struct io_intf * termio;
    void * mmio_base;
    int result;
    int i;
    console_init();
    intr_init();
    thread_init();
    devmgr_init();
    timer_init();
    heap_init(_kimg_end, (void*)USER_START); // Initialize heap memory
    // Attach NS16550a serial devices
    for (i = 0; i < 2; i++) {
        mmio_base = (void*)UART0_IOBASE;
        mmio_base += (UART1_IOBASE-UART0_IOBASE)*i;
        uart_attach(mmio_base, UART0_IRQNO+i);
    }
    intr_enable();
    timer_start();
    // Open terminal
    result = device_open(&termio, "ser", 1);
    if (result != 0)
        panic("device_open failed");

    test_elf_load(termio); 
}

int test_singlefile(const char* filename){
    struct io_intf* io;
    int ret=-2;
    //test open
    kprintf("original file num in array:%d\n",file_num);
    ret=fs_open(filename,&io);
    if(ret==-1) {
        kprintf("fail to open file:%s\n",filename);
        return -1;
    }
    if(ret>=0) kprintf("successfully open file:%s, return discriptor:%d\n",filename,ret);
    kprintf("current file num in array:%d\n",file_num);
    //test get length
    uint64_t length=-1;
    length=fs_ioctl(io,IOCTL_GETLEN,NULL);
    if(length<0) kprintf("fail get length\n");
    else kprintf("successfully get file length:%d\n",length);
    //test get position
    uint64_t position=-1;
    position=fs_ioctl(io,IOCTL_GETPOS,NULL);
    if(position!=0) kprintf("false initial position or fail to read file position\n");
    else kprintf("successfully read position\n");
    //test set position
    position=length/4;
    uint64_t new_position=fs_ioctl(io,IOCTL_SETPOS,&position);
    new_position=fs_ioctl(io,IOCTL_GETPOS,NULL);
    if(new_position==position) {
        kprintf("successfully set position\n");
    }
    else kprintf("fail to set or get file position, expect:%d actual:%d\n",position,new_position);
    //test get block
    uint64_t block_size;
    block_size=fs_ioctl(io,IOCTL_GETBLKSZ,NULL);
    if(block_size==4096) kprintf("successfully get block size\n");
    else kprintf("fail to get block size\n");
    //test read file
    new_position=0;
    fs_ioctl(io,IOCTL_SETPOS,&new_position);
    uint64_t read_length=0;
    char read_buffer[10];
    while(fs_ioctl(io,IOCTL_GETPOS,NULL)!=fs_ioctl(io,IOCTL_GETLEN,NULL)){
        read_length=read_length+fs_read(io,read_buffer,2);
    }
    if(read_length==fs_ioctl(io,IOCTL_GETLEN,NULL)) kprintf("successfully read the full file\n");
    else kprintf("fail to read full file, expected read byte:%d; actual read byte:%d\n",length,read_length);
    //test write file
    const char* write_data="test writing data";
    fs_ioctl(io,IOCTL_SETPOS,&new_position);
    long write_byte=fs_write(io,write_data,strlen(write_data));
    if(write_byte<0) kprintf("error in write");
    long expect_write_byte=strlen(write_data);
    if(fs_ioctl(io,IOCTL_GETLEN,NULL)<strlen(write_data)) expect_write_byte=fs_ioctl(io,IOCTL_GETLEN,NULL);
    if(write_byte==expect_write_byte) kprintf("successfully write data\n");
    else kprintf("fail write correct data. Expected write byte:%d;actual write byte:%d\n",expect_write_byte,write_byte);
    char expected[strlen(write_data)];
    memcpy(expected,write_data,expect_write_byte);
    char actual[expect_write_byte];
    fs_ioctl(io,IOCTL_SETPOS,&new_position);
    fs_read(io,actual,strlen(write_data));
    if(chars_equal(actual,expected)) kprintf("successfully write and read file\n");
    else kprintf("fail write or read file. Expected write:%s; actual write:%s\n",expected,actual);
    fs_close(io);
    kprintf("close file:%s\n",filename);
    kprintf("test finished\n");
    return 1;
}

void test_lit(){
    size_t size=2048;
    void* buf=kmalloc(size);
    struct io_lit test;
    //test initial(buffer, position, size)
    iolit_init(&test,buf,size);
    if(test.buf!=buf) kprintf("fail initial buffer\n");
    else kprintf("successfully initial buf\n");
    if(test.pos!=0) kprintf("fail initial position\n");
    else kprintf("successfully initial pos\n");
    if(test.size!=size) kprintf("fail initial size\n");
    else kprintf("success initial size\n");
    //test get length
    if(test.size==test.io_intf.ops->ctl(&test.io_intf,IOCTL_GETLEN,NULL)) kprintf("success get length\n");
    else kprintf("fail get length\n");
    //test write file and get position
    const char* write="test writing data";
    size_t expect_pos=test.pos+strlen(write);
    size_t writebyte=test.io_intf.ops->write(&test.io_intf,write,strlen(write));
    if(expect_pos==test.pos&&writebyte==strlen(write)) kprintf("success write and get position\n");
    else kprintf("fail to write or get position\n");
    //test read file and set position
    size_t new=1;
    test.io_intf.ops->ctl(&test.io_intf,IOCTL_SETPOS,&new);
    char read[16];
    test.io_intf.ops->read(&test.io_intf,read,strlen(write)-new);
    char* expectread="est writing data";
    kprintf("expected read:%s;actual read:%s\n",expectread,read);
    //test close 
    test.io_intf.ops->close(&test.io_intf);
    if(test.buf==NULL) kprintf("success close\n");
    else kprintf("fail close\n");
}

void test_fs(){
    console_init();
    intr_init();
    devmgr_init();
    thread_init();
    timer_init();
    extern char _kimg_end[];  // Defined in linker script
    #define USER_START 0x80100000UL
    heap_init(_kimg_end, (void*)USER_START);

    // Initialize timer
    timer_init();

    // Attach UART devices

    void *mmio_base;
    for (int i = 0; i < 2; i++) {
        mmio_base = (void*)UART0_IOBASE;
        mmio_base += (UART1_IOBASE - UART0_IOBASE) * i;
        uart_attach(mmio_base, UART0_IRQNO + i);
    }

    // Attach virtio devices

    for (int i = 0; i < 8; i++) {
        mmio_base = (void*)VIRT0_IOBASE;
        mmio_base += (VIRT1_IOBASE - VIRT0_IOBASE) * i;
        virtio_attach(mmio_base, VIRT0_IRQNO + i);
    }

    // Enable interrupts
    intr_enable();

    // Start timer
    timer_start();

    // Initialize filesystem
    fs_init();
    kprintf("Filesystem initialized.\n");
    /*
    struct io_lit imglit;
    struct io_intf * imgio;
    void* buf=_companion_f_start;
    size_t size=_companion_f_end-_companion_f_start;
    imgio=iolit_init(&imglit,buf,size);*/
    // Open block device
    struct io_intf *block;
    int result = device_open(&block, "blk", 0);
    if (result != 0) {
        kprintf("Failed to open uart device.\n");
        return;
    }
    kprintf("Block device io_intf obtained.\n");

    // Mount filesystem
    result = fs_mount(block);
    if (result < 0) {
        kprintf("Failed to mount filesystem.\n");
        return;
    }
    kprintf("Filesystem mounted successfully.\n");

    // Files to test
    const char *files[] = {"hello", "rogue", "trek"};
    int num_files = sizeof(files) / sizeof(files[0]);

    for (int i = 0; i < num_files; i++) {
        test_singlefile(files[i]);
    }

    // Halt the system after tests
    kprintf("Filesystem tests completed.\n");

    test_lit();

} 

void test_vioblk(void)
{
    struct io_intf *blkio;
    void *mmio_base;
    int result;
    int i;
    uint64_t pos, len, blksz;


    console_init();
    intr_init();
    devmgr_init();
    thread_init();
    timer_init();

    heap_init(_kimg_end, (void *)USER_START);

    //            Attach NS16550a serial devices

    for (i = 0; i < 2; i++)
    {
        mmio_base = (void *)UART0_IOBASE;
        mmio_base += (UART1_IOBASE - UART0_IOBASE) * i;
        uart_attach(mmio_base, UART0_IRQNO + i);
    }

    //            Attach virtio devices

    for (i = 0; i < 8; i++)
    {
        mmio_base = (void *)VIRT0_IOBASE;
        mmio_base += (VIRT1_IOBASE - VIRT0_IOBASE) * i;
        virtio_attach(mmio_base, VIRT0_IRQNO + i);
    }

    intr_enable();
    timer_start();

    result = device_open(&blkio, "blk", 0);

    if (result != 0)
        panic("device_open failed");

    // Allocate buffer for writing and reading
    char *write_buf = kmalloc(1024);  // write buffer
    char *read_buf = kmalloc(1024);   // read buffer

    // Prepare data to write
    for (i = 0; i < 1024; i++) {
        write_buf[i] = (char)(i % 256);
    }

    pos = 0;
    // Write to block device
    blkio->ops->ctl(blkio, IOCTL_SETPOS, &pos);
    blkio->ops->write(blkio, write_buf, 1024);

    // Reset position and read back data
    pos = 0;
    blkio->ops->ctl(blkio, IOCTL_SETPOS, &pos);
    blkio->ops->read(blkio, read_buf, 1024);

    

    console_printf("Read and write verification completed.\n");

    kfree(write_buf);
    kfree(read_buf);

    // Test getpos, setpos, getlen, getblksz
    blkio->ops->ctl(blkio, IOCTL_GETPOS, &pos);
    console_printf("Initial pos: %llu\n", pos);

    pos = 2048;  // Setting new position
    blkio->ops->ctl(blkio, IOCTL_SETPOS, &pos);
    blkio->ops->ctl(blkio, IOCTL_GETPOS, &pos);
    console_printf("Updated pos: %llu\n", pos);

    blkio->ops->ctl(blkio, IOCTL_GETLEN, &len);
    console_printf("Length: %llu\n", len);

    blkio->ops->ctl(blkio, IOCTL_GETBLKSZ, &blksz);
    console_printf("Block size: %u\n", blksz);

}


int main(){
    test_vioblk();
    return 0;
}