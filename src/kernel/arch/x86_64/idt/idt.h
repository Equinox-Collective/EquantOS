#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// 64-bit IDT gate descriptor structure
typedef struct {
    uint16_t low_offset;    // Lower 16 bits of handler address
    uint16_t sel;           // Segment selector
    uint8_t  ist;           // Interrupt Stack Table
    uint8_t  flags;         // Flags (Present, DPL, Storage, Type)
    uint16_t mid_offset;    // Middle 16 bits of handler address
    uint32_t high_offset;   // Upper 32 bits of handler address
    uint32_t reserved;      // Reserved
} __attribute__((packed)) idt_gate_t;

// IDT register structure for LIDT instruction
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_register_t;

void set_idt_gate(int n, uint64_t handler, uint16_t sel);
void init_idt(void);
void idt_set_syscall_trap_gate(int on);

#endif // IDT_H