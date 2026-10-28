// memory.c - Memory management
//

#ifndef TRACE
#ifdef MEMORY_TRACE
#define TRACE
#endif
#endif

#ifndef DEBUG
#ifdef MEMORY_DEBUG
#define DEBUG
#endif
#endif

#include "config.h"

#include "memory.h"
#include "console.h"
#include "halt.h"
#include "heap.h"
#include "csr.h"
#include "string.h"
#include "error.h"
#include "thread.h"
#include "process.h"

#include <stdint.h>

// EXPORTED VARIABLE DEFINITIONS
//
#define VPN_MAX 1<<9

char memory_initialized = 0;
uintptr_t main_mtag;

// IMPORTED VARIABLE DECLARATIONS
//

// The following are provided by the linker (kernel.ld)

extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// INTERNAL TYPE DEFINITIONS
//

union linked_page {
    union linked_page * next;
    char padding[PAGE_SIZE];
};

struct pte {
    uint64_t flags:8;
    uint64_t rsw:2;
    uint64_t ppn:44;
    uint64_t reserved:7;
    uint64_t pbmt:2;
    uint64_t n:1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN2(vma) (((vma) >> (9+9+12)) & 0x1FF)
#define VPN1(vma) (((vma) >> (9+12)) & 0x1FF)
#define VPN0(vma) (((vma) >> 12) & 0x1FF)
#define MIN(a,b) (((a)<(b))?(a):(b))

// INTERNAL FUNCTION DECLARATIONS
//

static inline int wellformed_vma(uintptr_t vma);
static inline int wellformed_vptr(const void * vp);
static inline int aligned_addr(uintptr_t vma, size_t blksz);
static inline int aligned_ptr(const void * p, size_t blksz);
static inline int aligned_size(size_t size, size_t blksz);

static inline uintptr_t active_space_mtag(void);
static inline struct pte * mtag_to_root(uintptr_t mtag);
static inline struct pte * active_space_root(void);

static inline void * pagenum_to_pageptr(uintptr_t n);
static inline uintptr_t pageptr_to_pagenum(const void * p);

static inline void * round_up_ptr(void * p, size_t blksz);
static inline uintptr_t round_up_addr(uintptr_t addr, size_t blksz);
static inline size_t round_up_size(size_t n, size_t blksz);
static inline void * round_down_ptr(void * p, size_t blksz);
static inline size_t round_down_size(size_t n, size_t blksz);
static inline uintptr_t round_down_addr(uintptr_t addr, size_t blksz);

static inline struct pte leaf_pte (
    const void * pptr, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte (
    const struct pte * ptab, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

static inline void sfence_vma(void);

// INTERNAL GLOBAL VARIABLES
//

static union linked_page * free_list;

static struct pte main_pt2[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));
static struct pte         main_pt1_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));
static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

// EXPORTED VARIABLE DEFINITIONS
//

// EXPORTED FUNCTION DEFINITIONS
// 
/*
input: void
output: void
discription: Sets up page tables and performs virtual-to-physical 1:1 mapping of the kernel megapage. Enables
Sv39 paging. Initializes the heap memory manager. Puts free pages on the free pages list. Allows S
mode access of U mode memory.
*/
void memory_init(void) {
    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;
    void * heap_start;
    void * heap_end;
    size_t page_cnt;
    uintptr_t pma;
    const void * pp;

    trace("%s()", __func__);

    assert (RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
        RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)
    
    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic("Kernel too large");

    // Initialize main page table with the following direct mapping:
    // 
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB
    
    // Identity mapping of two gigabytes (as two gigapage mappings)
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level page table
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] =
        ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging. This part always makes me nervous.

    main_mtag =  // Sv39
        ((uintptr_t)RISCV_SATP_MODE_Sv39 << RISCV_SATP_MODE_shift) |
        pageptr_to_pagenum(main_pt2);
    
    csrw_satp(main_mtag);
    sfence_vma();

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = round_up_ptr(heap_start, PAGE_SIZE);
    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += round_up_size (
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("Not enough memory");
    
    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
        heap_start, heap_end, (heap_end - heap_start) / 1024);

    free_list = heap_end; // heap_end is page aligned
    page_cnt = (RAM_END - heap_end) / PAGE_SIZE;

    kprintf("Page allocator: [%p,%p): %lu pages free\n",
        free_list, RAM_END, page_cnt);

    // Put free pages on the free page list
    // TODO: FIXME implement this (must work with your implementation of
    // memory_alloc_page and memory_free_page).
    for(size_t i=0;i<page_cnt-1;i++){   //creat link list
        union linked_page* next;        
        next=free_list+1;
        next->next=free_list;   
        free_list=next;
    }
    // Allow supervisor to access user memory. We could be more precise by only
    // enabling it when we are accessing user memory, and disable it at other
    // times to catch bugs.

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

// INTERNAL FUNCTION DEFINITIONS
//

static inline int wellformed_vma(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits+1));
}

static inline int wellformed_vptr(const void * vp) {
    return wellformed_vma((uintptr_t)vp);
}

static inline int aligned_addr(uintptr_t vma, size_t blksz){
    return ((vma % blksz) == 0);
}

static inline int aligned_ptr(const void * p, size_t blksz) {
    return (aligned_addr((uintptr_t)p, blksz));
}

static inline int aligned_size(size_t size, size_t blksz) {
    return ((size % blksz) == 0);
}

static inline uintptr_t active_space_mtag(void) {
    return csrr_satp();
}

static inline struct pte * mtag_to_root(uintptr_t mtag) {
    return (struct pte *)((mtag << 20) >> 8);
}


static inline struct pte * active_space_root(void) {
    return mtag_to_root(active_space_mtag());
}

static inline void * pagenum_to_pageptr(uintptr_t n) {
    return (void*)(n << PAGE_ORDER);
}

static inline uintptr_t pageptr_to_pagenum(const void * p) {
    return (uintptr_t)p >> PAGE_ORDER;
}

static inline void * round_up_ptr(void * p, size_t blksz) {
    return (void*)((uintptr_t)(p + blksz-1) / blksz * blksz);
}

static inline uintptr_t round_up_addr(uintptr_t addr, size_t blksz) {
    return ((addr + blksz-1) / blksz * blksz);
}

static inline size_t round_up_size(size_t n, size_t blksz) {
    return (n + blksz-1) / blksz * blksz;
}

static inline void * round_down_ptr(void * p, size_t blksz) {
    return (void*)((uintptr_t)p / blksz * blksz);
}

static inline size_t round_down_size(size_t n, size_t blksz) {
    return n / blksz * blksz;
}

static inline uintptr_t round_down_addr(uintptr_t addr, size_t blksz) {
    return (addr / blksz * blksz);
}

static inline struct pte leaf_pte (
    const void * pptr, uint_fast8_t rwxug_flags)
{
    return (struct pte) {
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pageptr_to_pagenum(pptr)
    };
}

static inline struct pte ptab_pte (
    const struct pte * ptab, uint_fast8_t g_flag)
{
    return (struct pte) {
        .flags = g_flag | PTE_V,
        .ppn = pageptr_to_pagenum(ptab)
    };
}

static inline struct pte null_pte(void) {
    return (struct pte) { };
}

static inline void sfence_vma(void) {
    asm inline ("sfence.vma" ::: "memory");
}
/*
helper function
input: struct pte* root, uintptr_t vma, int create
output: struct pte* val
discription: It walks down the page table structure using the VPN fields of vma
return coressonding pte. if not exit, create a new one if create=1; else return null 
*/
struct pte* walk_pt(struct pte* root, uintptr_t vma, int create){
    if(create==1){
        struct pte level2_pte=root[VPN2(vma)];
        if(!(level2_pte.flags&PTE_V)){
            uintptr_t level1_page=(uintptr_t)memory_alloc_page();
            memset((void*)level1_page,0,PAGE_SIZE);
            root[VPN2(vma)].ppn=level1_page>>12;
            root[VPN2(vma)].flags=PTE_V;
        }
        else{
            if(level2_pte.flags&(PTE_R|PTE_W|PTE_X)){
                panic("should not a leaf\n");
                return NULL;
            }
        }
        struct pte* level1_page=(struct pte*)(uintptr_t)(root[VPN2(vma)].ppn<<12);
        struct pte level1_pte=level1_page[VPN1(vma)];
        if((level1_pte.flags&PTE_V)!=PTE_V){
            uintptr_t level0_page=(uintptr_t)memory_alloc_page();   //alloc for all level if not exist
            memset((void*)level0_page,0,PAGE_SIZE);
            level1_page[VPN1(vma)].ppn=level0_page>>12;
            level1_page[VPN1(vma)].flags=PTE_V;
        }
        else{
            if(level1_pte.flags&(PTE_R|PTE_W|PTE_X)){
                panic("should not a leaf\n");
                return NULL;
            }
        }
        struct pte* level0_page=(struct pte*)(uintptr_t)(level1_page[VPN1(vma)].ppn<<12);
        struct pte level0_pte=level0_page[VPN0(vma)];
        if((level0_pte.flags&PTE_V)!=PTE_V){
            uintptr_t data_pt=(uintptr_t)memory_alloc_page();
            memset((void*)data_pt,0,PAGE_SIZE);
            level0_page[VPN0(vma)].ppn=data_pt>>12;
            level0_page[VPN0(vma)].flags=PTE_V;
        }
        struct pte* val=&(level0_page[VPN0(vma)]);
        sfence_vma();
        return val;
    }
    else{
        struct pte level2_pte=root[VPN2(vma)];
        if((level2_pte.flags&PTE_V)!=PTE_V) return NULL;
        else{
            if(level2_pte.flags&(PTE_R|PTE_W|PTE_X)){
                panic("should not a leaf\n");
                return NULL;
            }
        }
        struct pte* level1_page=(struct pte*)(uintptr_t)(root[VPN2(vma)].ppn<<12);
        struct pte level1_pte=level1_page[VPN1(vma)];
        if((level1_pte.flags&PTE_V)!=PTE_V) return NULL;
        else{
            if(level1_pte.flags&(PTE_R|PTE_W|PTE_X)){
                panic("should not a leaf\n");
                return NULL;
            }
        }
        struct pte* zero_level=(struct pte*)(uintptr_t)(level1_page[VPN1(vma)].ppn<<12);
        struct pte data=zero_level[VPN0(vma)];
        if((data.flags&PTE_V)!=PTE_V) return NULL;
        struct pte* val=&(zero_level[VPN0(vma)]);
        return val;
    }
}
/*
input: void
output: void
discription: Switch the active memory space to the main memory space and reclaims the memory space that was
active on entry
*/
void memory_space_reclaim(void){
    uintptr_t mtag=active_space_mtag();
    if(mtag==main_mtag) return;
    struct pte* root=active_space_root();
    memory_unmap_and_free_user();
    memory_free_page(root);
    memory_space_switch(main_mtag);
}

/*
input: void
output: void
discription: Allocate a physical page of memory using the free pages list
*/
void * memory_alloc_page(void){
    if(!free_list){                         //empty link list
        panic("no free pages avaiable\n");
    }
    else{
        union linked_page* val=free_list;
        free_list=free_list->next;
        memset(val,0,PAGE_SIZE);
        return val;
    }
}

/*
input: void* pp
output: void
discription: free the page
*/
void memory_free_page(void * pp){
    union linked_page* val=pp;
    val->next=free_list;
    free_list=val;      //add to the link list
}
/*
input: uintptr_t vma, uint_fast8_t rwxug_flags
output: (void*)vma
discription: Allocate a single physical page and maps a virtual address to it with provided flags. 
*/
void * memory_alloc_and_map_page(uintptr_t vma, uint_fast8_t rwxug_flags){
    uintptr_t pp=(uintptr_t)memory_alloc_page();
    struct pte* root=active_space_root();
    struct pte* leaf=walk_pt(root,vma,1);   //get pte
    *leaf=leaf_pte((void*)pp,rwxug_flags);  //set flags
    sfence_vma();
    return (void*)vma;
}
/*
input: uintptr_t vma, size_t size, uint_fast8_t rwxug_flags
output: (void*)vma
discription: Allocates the range of memory and maps a virtual address with the provided flags
*/
void * memory_alloc_and_map_range (uintptr_t vma, size_t size, uint_fast8_t rwxug_flags){
    size=round_up_size(size,PAGE_SIZE);
    for(size_t i=0;i<size;i=i+PAGE_SIZE){
        memory_alloc_and_map_page(vma+i,rwxug_flags);   //use memory_alloc_and_map_page to achieve range alloc
    }
    return (void*)vma;
}
/*
input: const void *vp, uint8_t rwxug_flags
output: void
discription: Sets the flags of the PTE associated with vp
*/
void memory_set_page_flags(const void *vp, uint8_t rwxug_flags){
    struct pte* root=active_space_root();
    uintptr_t vma=(uintptr_t) vp;
    struct pte* leaf=walk_pt(root,vma,1);   //find pte
    leaf->flags=rwxug_flags;                //modify flags
}
/*
input: const void * vp, size_t size, uint_fast8_t rwxug_flags
output: void
discription: Modify flags of all PTE within the specified virtual memory range
*/
void memory_set_range_flags(const void * vp, size_t size, uint_fast8_t rwxug_flags){
    size=round_up_size(size,PAGE_SIZE);
    uintptr_t vma=(uintptr_t)vp;
    for(size_t i=0;i<size;i=i+PAGE_SIZE){
        void* vpp=(void*)(vma+i);
        memory_set_page_flags(vpp,rwxug_flags);
    }
}
/*
input: void
output: void
discription: Unmaps and frees ALL user space pages 
(that is, all pages with the U flag asserted) in the currentmemory space
*/
void memory_unmap_and_free_user(void){
    struct pte* root=active_space_root();
    for(uintptr_t vma=USER_START_VMA;vma<USER_END_VMA;vma+=PAGE_SIZE){
        struct pte* level0_pte=walk_pt(root,vma,0);
        if(level0_pte==NULL) continue;
        else{
            memory_free_page(pagenum_to_pageptr(level0_pte->ppn));
            *level0_pte=null_pte();
        }
   }
    sfence_vma();
}
/*
input: const void * vp, size_t len, uint_fast8_t rwxug_flags
output: 1 if valid, 0 else
discription: Ensure that the virtual pointer provided (vp) points to a mapped region of size len and
has at least the specified flags
*/
int memory_validate_vptr_len(const void * vp, size_t len, uint_fast8_t rwxug_flags){
    struct pte* root=mtag_to_root(active_space_mtag());
    void* vma=(void*)vp;
    const void* vp_low=round_down_ptr(vma,PAGE_SIZE);       
    const void* vp_up=round_down_ptr(vma+len,PAGE_SIZE);
    for(const void* i=vp_low;i<vp_up;i+=PAGE_SIZE){             //iterate all pages in the range
        struct pte* leaf=walk_pt(root,(uintptr_t)i,0);
        if(leaf){
            if((leaf->flags&rwxug_flags)!=rwxug_flags) return -1;
        }
        else return -1;
    }
    return 1;
}
/*
input: const char * vs, uint_fast8_t ug_flags
output: 1 if valid, 0 else
discription: Ensure that the virtual pointer provided (vs) contains a NUL-terminated string
*/
int memory_validate_vstr(const char * vs, uint_fast8_t ug_flags){
    const char* vs_end=vs;
    size_t i=0;
    while((*vs_end)!=0){
        vs_end++;
        i++;
    }
    return memory_validate_vptr_len(vs,i,ug_flags);         //after get char length, direct use the above function
}
/*
input: const void * vptr
output: void
discription: Handle a page fault at a virtual address
*/
void memory_handle_page_fault(const void * vptr){
    uint_fast8_t flag=PTE_R|PTE_W|PTE_U|PTE_X;
    if(vptr>=(const void*)USER_END_VMA||vptr<(const void*)USER_START_VMA){      //if not in user region
        panic("not in user region\n");
    }
    else{
        kprintf("page fault handled by alloc new page\n");                      //success handle the page fault
        memory_alloc_and_map_page((uintptr_t)vptr,flag);
    }
}
uintptr_t memory_space_create(uint_fast16_t asid){
    // Identity mapping of two gigabytes (as two gigapage mappings)
    struct pte* new_root=(struct pte*)memory_alloc_page();
    for (uintptr_t pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        new_root[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level page table
    new_root[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);
    uintptr_t new_mtag = ((uintptr_t)RISCV_SATP_MODE_Sv39 << RISCV_SATP_MODE_shift) | ((uintptr_t)asid << RISCV_SATP_ASID_shift) | pageptr_to_pagenum(new_root);
    return new_mtag;
}

uintptr_t memory_space_clone(uint_fast16_t asid){
    // Identity mapping of two gigabytes (as two gigapage mappings)
    struct pte* new_root=(struct pte*)memory_alloc_page();
    for (uintptr_t pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        new_root[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level page table
    new_root[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);
    struct pte* root=active_space_root();
    for(uintptr_t vma=USER_START_VMA;vma<USER_END_VMA;vma+=PAGE_SIZE){
        struct pte* leaf=walk_pt(root,vma,0);
        if(leaf==NULL) continue;
        else{
            struct pte* new_leaf=walk_pt(new_root,vma,1);
            new_leaf->flags=leaf->flags;
            void* data=pagenum_to_pageptr(leaf->ppn);
            void* new_data=pagenum_to_pageptr(new_leaf->ppn);
            memcpy(new_data,data,PAGE_SIZE);
        }
    }
    uintptr_t new_mtag = ((uintptr_t)RISCV_SATP_MODE_Sv39 << RISCV_SATP_MODE_shift) | ((uintptr_t)asid << RISCV_SATP_ASID_shift) | pageptr_to_pagenum(new_root);
    return new_mtag;
}
