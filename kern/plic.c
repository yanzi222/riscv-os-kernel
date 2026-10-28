// plic.c - RISC-V PLIC
//

#include "plic.h"
#include "console.h"

#include <stdint.h>

// COMPILE-TIME CONFIGURATION
//

// *** Note to student: you MUST use PLIC_IOBASE for all address calculations,
// as this will be used for testing!

#ifndef PLIC_IOBASE
#define PLIC_IOBASE 0x0C000000
#endif

#define PLIC_SRCCNT 0x400
#define PLIC_CTXCNT 1

#define PENDING_OFFSET 0x001000
#define ENBALE_OFFSET 0x002000
#define THRESHOLD_OFFSET 0x200000
#define CLAIM_COMPLETE_OFFSET 0x200004
#define REG_SIZE 32 // register size is 32 bits
#define PEND_EN_SIZE 0x0080 // pending and enable bits 128 bytes for each context
#define ALIGNMENT 0x001000 // 4k alignment for threshold, claim, and complete
#define REG_BYTES 4 //each register occupies 4 bytes

// INTERNAL FUNCTION DECLARATIONS
//

// *** Note to student: the following MUST be declared extern. Do not change these
// function delcarations!

extern void plic_set_source_priority(uint32_t srcno, uint32_t level);
extern int plic_source_pending(uint32_t srcno);
extern void plic_enable_source_for_context(uint32_t ctxno, uint32_t srcno);
extern void plic_disable_source_for_context(uint32_t ctxno, uint32_t srcno);
extern void plic_set_context_threshold(uint32_t ctxno, uint32_t level);
extern uint32_t plic_claim_context_interrupt(uint32_t ctxno);
extern void plic_complete_context_interrupt(uint32_t ctxno, uint32_t srcno);

// Currently supports only single-hart operation. The low-level PLIC functions
// already understand contexts, so we only need to modify the high-level
// functions (plic_init, plic_claim, plic_complete).

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
    int i;

    // Disable all sources by setting priority to 0, enable all sources for
    // context 0 (M mode on hart 0).

    for (i = 0; i < PLIC_SRCCNT; i++) {
        plic_set_source_priority(i, 0);
        plic_enable_source_for_context(1, i);
    }
}

extern void plic_enable_irq(int irqno, int prio) {
    trace("%s(irqno=%d,prio=%d)", __func__, irqno, prio);
    plic_set_source_priority(irqno, prio);
}

extern void plic_disable_irq(int irqno) {
    if (0 < irqno)
        plic_set_source_priority(irqno, 0);
    else
        debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_irq(void) {
    // Hardwired context 0 (M mode on hart 0)
    trace("%s()", __func__);
    return plic_claim_context_interrupt(1);
}

extern void plic_close_irq(int irqno) {
    // Hardwired context 0 (M mode on hart 0)
    trace("%s(irqno=%d)", __func__, irqno);
    plic_complete_context_interrupt(1, irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

// Function name: plic_set_source_priority
//
// Description:
// Sets the priority level for a specific interrupt source.
// This function modifies the priority array, where each entry corresponds to a specific interrupt source.
//
// Input: source number(uint32), priority_level(uint32)
// 
// Return: None
void plic_set_source_priority(uint32_t srcno, uint32_t level) {
    // FIXME your code goes here
    uint32_t* src_prio_ptr = (uint32_t *)PLIC_IOBASE + srcno; //calculate the address
    *src_prio_ptr = level;//set priority

}

// Function name: plic_source_pending
//
// Description:
// Checks if an interrupt source is pending by inspecting the pending array.
// It returns 1 if the interrupt is pending, 0 otherwise. The pending bit is determined by 
// checking the bit corresponding to srcno in the pending array.
//
// Input: source number(uint32)
// 
// Return: -1 if input is invalid, 1 if the interrupt is pending, 0 otherwise. (int)
int plic_source_pending(uint32_t srcno) {
    // FIXME your code goes here
    uint32_t* pending_reg = (uint32_t *)(PLIC_IOBASE + PENDING_OFFSET) + (srcno/REG_SIZE); //calculate the address of pending register
    uint32_t pending_bit = 1 << (srcno%REG_SIZE); //pending bit
    return ( (pending_bit&(*pending_reg)) == 0 ? 0 : 1 );
}

// Function name: plic_enable_source_for_context
//
// Description:
// Enables a specific interrupt source for a given context.
// This function sets the appropriate bit in the enable array. It calculates the index based on the source number
// and context, and sets the corresponding bit for the source.
//
// Input: context number(uint32), source number(uint32)
// 
// Return: None
void plic_enable_source_for_context(uint32_t ctxno, uint32_t srcno) {
    // FIXME your code goes here
    uint32_t* enable_reg = (uint32_t *)(PLIC_IOBASE + ENBALE_OFFSET) + PEND_EN_SIZE *ctxno/REG_BYTES + (srcno/REG_SIZE); //calculate the address of enable register
    uint32_t enable_bit = 1 << (srcno%REG_SIZE); //enable bit
    *enable_reg |= enable_bit; //enable 
}
// Function name: plic_disable_source_for_context
//
// Description:
// Disables a specific interrupt source for a given context.
// This function clears the appropriate bit in the enable array. Similar to plic enable source for context,
// it calculates the correct bit to clear for the given context and source.
//
// Input: context number(uint32), source number(uint32)
// 
// Return: None
void plic_disable_source_for_context(uint32_t ctxno, uint32_t srcno) {
    // FIXME your code goes here
    uint32_t* enable_reg = (uint32_t *)(PLIC_IOBASE + ENBALE_OFFSET) + PEND_EN_SIZE *ctxno/REG_BYTES + (srcno/REG_SIZE); //calculate the address of enable register
    uint32_t enable_bit = 1 << (srcno%REG_SIZE); //enable bit
    *enable_reg &= ~enable_bit; //disable
}

// Function name: plic_set_context_threshold
//
// Description:
// Sets the interrupt priority threshold for a specific context.
// Interrupts with a priority lower than the threshold will not be handled by the context.
//
// Input: context number(uint32), priority_level(uint32)
// 
// Return: None
void plic_set_context_threshold(uint32_t ctxno, uint32_t level) {
    // FIXME your code goes here
    uint32_t* ctx_thre_ptr = (uint32_t *)(PLIC_IOBASE + THRESHOLD_OFFSET) + ALIGNMENT*ctxno/REG_BYTES; //calculate the address
    *ctx_thre_ptr = level;//set threshold
}

// Function name: plic_claim_context_interrupt
//
// Description:
// Claims an interrupt for a given context.
// This function reads from the claim register and returns the interrupt ID of 
// the highest-priority pending interrupt. It returns 0 if no interrupts are pending.
// 
// Input: contexet number(uint32)
// 
// Return: Return -1 if input is invalid. Returns the interrupt ID of 
// the highest-priority pending interrupt. It returns 0 if no interrupts are pending. (uint32)
uint32_t plic_claim_context_interrupt(uint32_t ctxno) {
    // FIXME your code goes here
    uint32_t* ctx_claim_ptr = (uint32_t *)(PLIC_IOBASE + CLAIM_COMPLETE_OFFSET) + ALIGNMENT*ctxno/REG_BYTES; //calculate the address
    return *ctx_claim_ptr;
}
// Function name: plic_complete_context_interrupt
//
// Description:
// Completes the handling of an interrupt for a given context.
// This function writes the interrupt source number back to the claim register, notifying the PLIC that the interrupt
// has been serviced.
//
// Input: context number(uint32), source number(uint32)
// 
// Return: None
void plic_complete_context_interrupt(uint32_t ctxno, uint32_t srcno) {
    // FIXME your code goes here
    uint32_t* ctx_complete_ptr = (uint32_t *)(PLIC_IOBASE + CLAIM_COMPLETE_OFFSET) + ALIGNMENT*ctxno/REG_BYTES; //calculate the address
    *ctx_complete_ptr = srcno;
}
