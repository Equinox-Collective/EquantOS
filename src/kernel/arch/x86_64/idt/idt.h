#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT gate descriptor for 64-bit mode
typedef struct {
    uint16_t low_offset;    // Lower 16 bits of handler address
    uint16_t sel;           // Kernel segment selector (0x08)
    uint8_t  ist;           // Interrupt Stack Table offset
    uint8_t  flags;         // Type and attributes (Present, DPL, Gate Type)
    uint16_t mid_offset;    // Middle 16 bits of handler address
    uint32_t high_offset;   // Upper 32 bits of handler address
    uint32_t reserved;      // Reserved, must be zero
} __attribute__((packed)) idt_gate_t;

// IDT register structure for LIDT instruction
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_register_t;

void set_idt_gate(int n, uint64_t handler, uint16_t sel);
void init_idt(void);

#endif // IDT_H