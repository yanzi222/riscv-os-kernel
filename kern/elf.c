#include "elf.h"
#include "error.h"
#include "io.h"
#include <stdlib.h>
#include "string.h"
#include "heap.h"
#include "memory.h"
#include "config.h"
#include "console.h"

/* 64-bit ELF base types. */
typedef uint64_t	Elf64_Addr;
typedef uint16_t	Elf64_Half;
typedef int16_t	    Elf64_SHalf;
typedef uint64_t	Elf64_Off;
typedef int32_t	    Elf64_Sword;
typedef uint32_t	Elf64_Word;
typedef uint64_t	Elf64_Xword;
typedef int64_t	    Elf64_Sxword;

//Magic numbers
#define EI_NIDENT 16 // number of identification

#define	EI_MAG0		0		/* e_ident[] indexes */
#define	EI_MAG1		1
#define	EI_MAG2		2
#define	EI_MAG3		3
#define	EI_CLASS	4
#define	EI_DATA		5
#define	EI_VERSION	6
#define	EI_OSABI	7
#define	EI_PAD		8

#define	ELFMAG0		0x7f	 /* EI_MAG */
#define	ELFMAG1		'E'
#define	ELFMAG2		'L'
#define	ELFMAG3		'F'

#define	ELFCLASS64	2       /* EI_CLASS */

#define ELFDATA2LSB	1       /* e_ident[EI_DATA] */

#define EV_NONE		0		/* e_version, EI_VERSION */
#define EV_CURRENT	1
#define EV_NUM		2

#define ET_EXEC   2         /*e_type*/

#define EM_RISCV	243	    /* e_machine RISC-V */

#define ELFOSABI_NONE	0
#define ELFOSABI_LINUX	3

#ifndef ELF_OSABI
#define ELF_OSABI ELFOSABI_NONE
#endif

#define PF_R		0x4
#define PF_W		0x2
#define PF_X		0x1

typedef struct {
    unsigned char e_ident[EI_NIDENT];// file information
    uint16_t      e_type; // Oject file type
    uint16_t      e_machine; // Architecture
    uint32_t      e_version; // Oject file version
    Elf64_Addr     e_entry; // Entry point
    Elf64_Off      e_phoff; // Program header offset
    Elf64_Off      e_shoff; // Section header offset
    uint32_t      e_flags; // Flags
    uint16_t      e_ehsize; // ELF header size
    uint16_t      e_phentsize; // Program header table entry size
    uint16_t      e_phnum; // Program header table entry number
    uint16_t      e_shentsize; // Section header table entry size
    uint16_t      e_shnum; // Section header table entry number
    uint16_t      e_shstrndx; // Section header string table index
} Elf64_Ehdr;


#define PT_LOAD    1    /*Constants for the segment types */

typedef struct {
    uint32_t   p_type; // Type of this segment
    uint32_t   p_flags; // Flags
    Elf64_Off  p_offset; // Offset in the file
    Elf64_Addr p_vaddr; // virtual address of the segment
    Elf64_Addr p_paddr; // Pysical address of the segment
    uint64_t   p_filesz; // Size of the segment in the file
    uint64_t   p_memsz; // Size of the segmeent in memory
    uint64_t   p_align; // Alignment
} Elf64_Phdr;

#define MEM_START 0x80100000
#define MEM_END 0x81000000


/*
Function Name: elf_load
Description:
This function ingests an io intf to read the executable and do some validation on it 
(to make sure it’s actually an executable). It is then the job of elf load to read the
ELF header and process the program headers. The loader only load program header entries of type
PT LOAD. 

Input: io_interface(struct io_intf), entry function pointer(pointer to a function pointer)
Return: 0 if load successfully, negative error code if fail.
*/

int elf_load(struct io_intf *io, void (**entryptr)(void)){
    Elf64_Ehdr header;
    if(ioread(io,&header,sizeof(header))!= sizeof(header)){
        return -EIO; // Fail to read the ELF header
    }

    if(header.e_ident[EI_MAG0] != ELFMAG0 || header.e_ident[EI_MAG1] != ELFMAG1
    || header.e_ident[EI_MAG2] != ELFMAG2 || header.e_ident[EI_MAG3] != ELFMAG3){
        return -EINVAL; // It is not an ELF
    }

    if(header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB
    ||header.e_machine != EM_RISCV || header.e_type != ET_EXEC ||header.e_version != EV_CURRENT){
        return -EINVAL; // It is not little-endian riscv64 executable
    }

    *entryptr = (void*)header.e_entry; // set the entry point of the ELF file

    uint16_t pnum = header.e_phnum;
    for(int i = 0; i< pnum; ++i){
        Elf64_Phdr phdr;
        if(ioseek(io,header.e_phoff+i*header.e_phentsize)<0){
            return -EIO;// Fail to seek
        }
        if(ioread(io,&phdr,header.e_phentsize)!=header.e_phentsize){
            return -EIO; // Fail to read program header
        }
        if(phdr.p_type != PT_LOAD || !phdr.p_memsz){
            continue; // Skip headers that are not PT_LOAD or have 0 size
        }
        if(phdr.p_vaddr< USER_START_VMA || phdr.p_vaddr+phdr.p_memsz > USER_END_VMA){
            continue; // Verifying all of the sections of the 
                      // program are loaded inside user memory.
        }
        if(phdr.p_filesz>phdr.p_memsz)
            continue; // Bad filesize        
        // write this segment into the specified memory
        void* pvaddr = (void*) phdr.p_vaddr;

        //allocate memery
        if(memory_initialized == 0) memory_init();

        uint_fast8_t flag = PTE_U | PTE_R | PTE_W | PTE_X | PTE_V | PTE_A | PTE_D; // set flags before loading 

        void* usermem = memory_alloc_and_map_range((uintptr_t)pvaddr,phdr.p_memsz,flag);

        memset(pvaddr,0,phdr.p_memsz); // initialize memory

        // Write the segment to the memory
        if(ioseek(io, phdr.p_offset)<0){
            return -EIO;// fail to seek
        }
        if (ioread(io, usermem, phdr.p_filesz) != phdr.p_filesz) {
            return -EIO; // Fail to load segment into memory
        }
        
        uint_fast8_t user_program_flag = PTE_U | PTE_V | PTE_A | PTE_D;//  vaild in user space
        if(phdr.p_flags&PF_R){
            user_program_flag |= PTE_R; // set read flag
        }
        if(phdr.p_flags&PF_W){
            user_program_flag |= PTE_W; // set write flag
        }
        if(phdr.p_flags&PF_X){
            user_program_flag |= PTE_X; // set execute flag
        }
        memory_set_range_flags(pvaddr,phdr.p_memsz,user_program_flag);
        
    }
    return 0; // return 0 if succeed
}