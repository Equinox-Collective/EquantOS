// src/kernel/core/gen/idt.c
#include "idt.h"
#include "pic.h"
#include "../mem/vmm.h"

idt_gate_t idt[256];
idt_register_t idt_reg;

extern uint64_t isr_stub_table[];
extern void keyboard_handler(void);
extern void irq0_handler_asm(void);
extern void irq_ignore_handler(void);
extern void syscall_interrupt_asm(void);
extern void page_fault_asm(void);
extern void linux_syscall_interrupt_asm(void);
extern void sched_yield_asm(void);

void set_idt_gate(int n, uint64_t handler, uint16_t sel) {
    idt[n].low_offset = (uint16_t)(handler & 0xFFFF);
    idt[n].sel = sel;
    idt[n].ist = 0;
    idt[n].flags = 0x8E; // Present, DPL 0, Interrupt Gate
    idt[n].mid_offset = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].high_offset = (uint32_t)(handler >> 32);
    idt[n].reserved = 0;
}

void init_idt(void) {
    pic_remap();

    idt_reg.limit = (uint16_t)(sizeof(idt_gate_t) * 256 - 1);
    idt_reg.base = (uint64_t)&idt;

    uint16_t sel = 0x08;
  
    // 1. Исключения CPU 0..31
    for (int i = 0; i < 32; i++) { 
        set_idt_gate(i, isr_stub_table[i], sel); 
    }

    // 2. Page Fault (#PF Vector 14)
    set_idt_gate(14, (uint64_t)page_fault_asm, sel);

    // 3. Аппаратные IRQ 32..255
    for (int i = 32; i < 256; i++) {
        set_idt_gate(i, (uint64_t)irq_ignore_handler, sel);
    }

    set_idt_gate(32, (uint64_t)irq0_handler_asm, sel); 
    idt[32].flags = 0x8E;

    set_idt_gate(33, (uint64_t)keyboard_handler, sel);
    idt[8].ist = 1;

    // Родной сисколл int 0x80
    set_idt_gate(0x80, (uint64_t)syscall_interrupt_asm, sel);
    idt[0x80].flags = 0xEF; // DPL 3

    // === ШЛЮЗ ДЛЯ BASH И MUSL: INT 0x81 ===
    set_idt_gate(0x81, (uint64_t)linux_syscall_interrupt_asm, sel);
    idt[0x81].flags = 0xEF; // DPL 3 (РАЗРЕШАЕМ ВЫЗОВ ИЗ USER MODE!)

    set_idt_gate(0x82, (uint64_t)sched_yield_asm, sel);
    idt[0x82].flags = 0xEF; // DPL 3 (Разрешен вызов из Ring 0 и Ring 3)

    __asm__ __volatile__("lidt (%0)" : : "r"(&idt_reg));
}