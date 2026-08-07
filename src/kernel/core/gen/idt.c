// idt.c
#include "idt.h"
#include "pic.h"
extern void syscall_interrupt_asm(void);

idt_gate_t idt[256];
idt_register_t idt_reg;

extern uint64_t isr_stub_table[];
extern void keyboard_handler(void);
extern void irq0_handler_asm(void);

// Temporary stubs for syscalls until we implement userland ring 3
static void dummy_syscall_handler(void) {
    for(;;);
}

void set_idt_gate(int n, uint64_t handler, uint16_t sel) {
    idt[n].low_offset = (uint16_t)(handler & 0xFFFF);
    idt[n].sel = sel;
    idt[n].ist = 0;
    idt[n].flags = 0x8E; // Default: Present, DPL 0, Interrupt Gate
    idt[n].mid_offset = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].high_offset = (uint32_t)(handler >> 32);
    idt[n].reserved = 0;
}

void init_idt(void) {
    // 1. Remap 8259 PIC controllers
    pic_remap();

    idt_reg.limit = (uint16_t)(sizeof(idt_gate_t) * 256 - 1);
    idt_reg.base = (uint64_t)&idt;

    uint16_t sel = 0x08; // Kernel Code Segment Selector
  
    // Populate all 256 entries safely
    for (int i = 0; i < 256; i++) { 
        set_idt_gate(i, isr_stub_table[i < 32 ? i : 0], sel); 
    }

    // Hardware IRQs & Yield Vector
    // IRQ0 -> Context Switcher & PIT Timer (DPL 3 allowed for software int $32 yield)
    set_idt_gate(32, (uint64_t)irq0_handler_asm, sel); 
    idt[32].flags = 0xEE; // Present, DPL 3, 64-bit Interrupt Gate (allows Ring 3 int $32)

    set_idt_gate(33, (uint64_t)keyboard_handler, sel);

    // Syscall stub (0x80) for Linux/POSIX ABI & EquantOS API
    set_idt_gate(0x80, (uint64_t)syscall_interrupt_asm, sel);
    idt[0x80].flags = 0xEF;

    set_idt_gate(0x81, (uint64_t)dummy_syscall_handler, sel);
    idt[0x81].flags = 0xEF; // Trap gate, DPL 3

    // SAFE LIDT: Pass pointer via general-purpose register and dereference in AT&T syntax
    __asm__ __volatile__("lidt (%0)" : : "r"(&idt_reg));
}

void idt_set_syscall_trap_gate(int on) {
    idt[0x80].flags = on ? 0xEF : 0xEE;
    idt[0x81].flags = on ? 0xEF : 0xEE;
}