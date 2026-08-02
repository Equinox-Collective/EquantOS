#include "idt.h"

idt_gate_t idt[256];
idt_register_t idt_reg;

extern uint64_t isr_stub_table[];
extern void keyboard_handler(void);
extern void timer_handler(void);

void set_idt_gate(int n, uint64_t handler, uint16_t sel) {
    idt[n].low_offset = (uint16_t)(handler & 0xFFFF);
    idt[n].sel = sel;
    idt[n].ist = 0;
    idt[n].flags = 0x8E; // Present, Ring 0, 64-bit Interrupt Gate
    idt[n].mid_offset = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].high_offset = (uint32_t)(handler >> 32);
    idt[n].reserved = 0;
}

void init_idt(void) {
    idt_reg.limit = (uint16_t)(sizeof(idt_gate_t) * 256 - 1);
    idt_reg.base = (uint64_t)&idt;

    uint16_t kcode_sel = 0x08;

    for (int i = 0; i < 32; i++) {
        set_idt_gate(i, isr_stub_table[i], kcode_sel); 
    }

    // Hardware IRQs
    set_idt_gate(32, (uint64_t)timer_handler, kcode_sel);    // IRQ0: PIT Timer
    set_idt_gate(33, (uint64_t)keyboard_handler, kcode_sel); // IRQ1: PS/2 Keyboard

    __asm__ __volatile__("lidt %0" : : "m"(idt_reg));
}