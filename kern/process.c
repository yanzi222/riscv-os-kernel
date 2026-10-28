// process.c - user process
//

#include "process.h"
#include <stdint.h>
#include "elf.h"
#include "halt.h"
#include "trap.h"
#include "heap.h"
#include "memory.h"
#include "thread.h"

#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif
#define PROCESS_MAX 16

// COMPILE-TIME PARAMETERS
//

// INTERNAL FUNCTION DECLARATIONS
//

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_PID 0

// The main user process struct

static struct process main_proc;

// A table of pointers to all user processes in the system

struct process * proctab[16] = {
    [MAIN_PID] = &main_proc
};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @param void
 * @return void
 * @brief initialize the process manager
 */
void procmgr_init(void) {
    // set up the main process
    main_proc = (process) {
        .id = MAIN_PID,
        .tid = running_thread(),
        .mtag = active_memory_space()
    };
    
    // set main thread's process
    thread_set_process(main_proc.tid, &main_proc);
    
    // set all entries of main process's iotab to NULL
    for (int i = 0; i < PROCESS_IOMAX; ++i) {
        main_proc.iotab[i] = NULL;
    }
    
    // clear the rest of the process table
    for (int i = 0; i < PROCESS_MAX; i++) if (i != MAIN_PID) {
        proctab[i] = NULL;
    }
    
    procmgr_initialized = 1;
}

process * create_process() {
    int id = 0;
    
    for (; id < PROCESS_MAX && proctab[id] != NULL; ++id);
    if (id == PROCESS_MAX) panic("Too many processes");
    
    process *proc = kmalloc(sizeof(process));
    proc->id = id;
    proctab[id] = proc;
    return proc;
}

/**
 * @param exeio the IO interface of the execuatble
 * @return error code when something goes wrong, or 0 on success
 * @brief execute a process, and replace the current process
 */
int process_exec(struct io_intf* exeio) {
    int ret;
    
    process *proc = current_process();
    thread_set_process(proc->tid, proc);
    proc->mtag = active_memory_space();
    
    // free other user's memory mappings
    memory_unmap_and_free_user();
    
    // load the executable from the ELF file
    void (*entry) (struct io_intf*);
    if ((ret = elf_load(exeio, &entry)) < 0) return ret;
    
    // context swtich to U mode
    thread_jump_to_user(USER_STACK_VMA, (uintptr_t) entry);
    
    return 0;
}

/**
 * @param void
 * @return void
 * @brief exit the current process
 */
void process_exit(void) {
    // reclaim all non-global memory mappings
    memory_space_reclaim();
    
    // close all active IO interfaces
    struct process* proc = current_process();
    for (int i = 0; i < PROCESS_IOMAX; ++i) if (proc->iotab[i] != NULL) {
        ioclose(proc->iotab[i]);
    }

    // exit the current thread also
    thread_exit();
}

int process_fork(const struct trap_frame *tfr) {
    process *proc = current_process();
    process *child_proc = create_process();
    
    if (child_proc == NULL) return -1;
    
    if ((child_proc->mtag = memory_space_clone(0)) < 0) return -EINVAL;
    
    for (int i = 0; i < PROCESS_IOMAX; ++i) if (proc->iotab[i] != NULL) {
        struct io_intf *io = proc->iotab[i];
        child_proc->iotab[i] = io;
        ioref(io);
    }
    
    return thread_fork_to_user(child_proc, tfr);
}