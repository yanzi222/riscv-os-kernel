#include "error.h"
#include "process.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "memory.h"
#include "thread.h"
#include "console.h"
#include "halt.h"
#include "trap.h"
#include "scnum.h"
#include "timer.h"
#include "heap.h"
#include "intr.h"

#define ECHILD  10
#define NPROC 16

static int sysexit(void);
static int sysexec(int fd);
static int sysmsgout(const char * msg);
static int sysdevopen(int fd, const char * name, int instno);
static int sysfsopen(int fd, const char * name);
static int sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int sysioctl(int fd, int cmd, void * arg);
static int sysfork(const struct trap_frame * tfr);
static int syswait(int tid);
static int sysusleep(unsigned long us);
void syscall_handler(struct trap_frame *tfr);


//input: none
//output: none
//function: Exits the currently running process.
int sysexit(void){
    process_exit();
}

//input: pointer msg
//output: 0 for success
//function: Prints msg to the console.
int sysmsgout(const char * msg){
    int result;
    trace("%s(msg=%p)", __func__, msg);
    result = memory_validate_vstr(msg, PTE_U);        //memory validation
    if(result != 1){
            return result;
    }
    kprintf("Thread <%s:%d> says: %s\n",            //print msg
        thread_name(running_thread()),
        running_thread(), msg);
    return 0;
}

//input: int fd, pointer name, int instno
//output: fd
//function: Opens a device at the specified file descriptor and returns error code on failure
int sysdevopen(int fd, const char * name, int instno){
    int result = memory_validate_vstr(name, PTE_U);
    if(result != 1){
        return result;
    }
    if(fd < 0){                                //find next fd
        for(fd = 0; fd < PROCESS_IOMAX; fd++){
            if(current_process()->iotab[fd] == NULL)
                break;
        }
    }
    if(current_process()->iotab[fd] != NULL){
        return -EBUSY;
    }
    struct io_intf *io;
    result = device_open(&io, name, instno);
    if(result < 0){
        return result;
    }
    current_process()->iotab[fd] = io;          //device at fd
    return fd;
}

//input: int fd, pointer name
//output: fd
//function: Opens a file at the specified file descriptor and returns error code on failure.
int sysfsopen(int fd, const char *name) {
    int result = memory_validate_vstr(name, PTE_U);
    if(result != 1){
        return result;
    }
    if(fd < 0){                                 //find next fd
        for(fd = 0; fd < PROCESS_IOMAX; fd++){
            if(current_process()->iotab[fd] == NULL)
                break;
        }
    }
    if(current_process()->iotab[fd] != NULL){
        return -EBUSY;
    }
    struct io_intf *io;
    result = fs_open(name, &io);
    if(result < 0){
        return result;
    }
    current_process()->iotab[fd] = io;   //file at fd
    return fd;
}

//input: int fd
//output: 0 for success
//function: Closes the device at the specified file descriptor.
int sysclose(int fd) {
    if(fd < 0 || fd >= PROCESS_IOMAX)
        return -EBADFD;
    struct io_intf *io = current_process()->iotab[fd];    //get device at fd
    if(!io)
        return -EINVAL;
    ioclose(io);               //close
    current_process()->iotab[fd] = NULL;
    return 0;
}

//input: int fd, pointer buf, size_t bufsz
//output: bytes have been read
//function: Reads from the opened file descriptor and writes bufsz bytes into buf.
 static long sysread(int fd, void *buf, size_t bufsz) {
    int result = memory_validate_vptr_len(buf, bufsz, PTE_R | PTE_U);  //memory validation
    if(result != 1){
        return result;
    }
    if(fd < 0 ||fd >= PROCESS_IOMAX){
        return -EBADFD;
    }
    struct io_intf *io = current_process()->iotab[fd];
    if(!io){
        return -EINVAL;
    }
    long read = ioread_full(io, buf, bufsz);   //write bytes into buf
    if(read < 0){
        return -EIO;
    }
    return read;
}

//input: int fd, pointer buf, size_t len
//output: bytes have been written
//fucntion: Reads bufsz bytes from buf and writes it to the opened file descriptor
static long syswrite(int fd, const void *buf, size_t len) {
    int result = memory_validate_vptr_len(buf, len, PTE_W | PTE_U);   //memry validation
    if(result != 1){
        return result;
    }
    if(fd < 0 || fd >= PROCESS_IOMAX){
        return -EBADFD;
    }
    struct io_intf *io = current_process()->iotab[fd];
    if(!io){
        return -EINVAL;
    }
    long written = iowrite(io, buf, len);     //write bytes to opened fd
    if(written < 0){
        return -EIO;
    }
    return written;
}
//input: int fd, int cmd, pointer arg
//output: 0 for success
//function: Performs desired ioctl based on cmd
static int sysioctl(int fd, int cmd, void *arg) {
    if(fd < 0 || fd >= PROCESS_IOMAX){
        return -EBADFD;
    }
    struct io_intf *io = current_process()->iotab[fd];  //specific fd
    if(!io){
        return -EINVAL;
    }
    if(arg != NULL){
        int result = memory_validate_vptr_len(arg, sizeof(void *), PTE_U | PTE_W);  //memory validation
        if(result != 1){
            return result;
        }
    }
    int result = ioctl(io, cmd, arg);    //specific command
    if(result < 0){
        return -EIO;
    }
    return 0;
}

//input: int fd
//output: process_exec
//function: Halts currently running user program and starts new program based on opened file at file descriptor
static int sysexec(int fd) {
    if(fd < 0 ||fd >= PROCESS_IOMAX){
        return -EBADFD;
    }
    struct io_intf *io = current_process()->iotab[fd];  //specific fd
    if (!io)
        return -EINVAL;
    current_process()->iotab[fd] = NULL;     //halt program
    return process_exec(io);
}

//input: pointer tfr
//output: none
//function: Called from the usermode exception handler to handle all syscalls. Jumps to a system call based on
//the specified system call number.
void syscall_handler(struct trap_frame *tfr) {
    tfr->sepc += 4;
    const uint64_t *const a = tfr->x + TFR_A0;
    switch (tfr->x[17]) { //different cases
        case SYSCALL_EXIT:
            tfr->x[TFR_A0] = sysexit();
            break;
            
        case SYSCALL_MSGOUT:
            tfr->x[TFR_A0] = sysmsgout((const char*)a[0]);
            break;
            
        case SYSCALL_DEVOPEN:
            tfr->x[TFR_A0] = sysdevopen(a[0], (const char*)a[1], a[2]);
            break;
            
        case SYSCALL_FSOPEN:
            tfr->x[TFR_A0] = sysfsopen(a[0], (const char*)a[1]);
            break;
            
        case SYSCALL_CLOSE:
            tfr->x[TFR_A0] = sysclose(a[0]);
            break;
            
        case SYSCALL_READ:
            tfr->x[TFR_A0] = sysread(a[0], (void*)a[1], a[2]);
            break;
            
        case SYSCALL_WRITE:
            tfr->x[TFR_A0] = syswrite(a[0], (const void*)a[1], a[2]);
            break;
            
        case SYSCALL_IOCTL:
            tfr->x[TFR_A0] = sysioctl(a[0], a[1], (void*)a[2]);
            break;
            
        case SYSCALL_EXEC:
            tfr->x[TFR_A0] = sysexec(a[0]);
            break;

        case SYSCALL_USLEEP:
            tfr->x[TFR_A0] = sysusleep(a[0]);
            break;

        case SYSCALL_WAIT:
            tfr->x[TFR_A0] = syswait(a[0]);
            break;

        case SYSCALL_FORK:
            tfr->x[TFR_A0] = sysfork(tfr);
            break;
    }
}


static int sysusleep(unsigned long us) {
    uint64_t ticks = (uint64_t)us * TIMER_FREQ / 1000000;
    struct alarm al;
    alarm_init(&al, "usleep");
    alarm_sleep(&al, ticks);
    return 0;
}

static int syswait(int tid) {
    trace("%s(%d)", __func__, tid);
    if(tid == 0){
        int result = thread_join_any();
        if(result < 0){
            return -ECHILD;       //no children
            }
        return result;
    }
    else{
        int result = thread_join(tid);
        if(result < 0){
            return -ECHILD;       //no children
        }
        return result;
    }
}

int sysfork(const struct trap_frame * tfr){

   return process_fork(tfr);
}
