#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Структура записи в IDT для 64-битного режима
typedef struct {
    uint16_t low_offset;    // 0-15 бит адреса обработчика
    uint16_t sel;           // Сегментный селектор
    uint8_t  ist;           // Interrupt Stack Table
    uint8_t  flags;         // Флаги (P, DPL, S, Type)
    uint16_t mid_offset;    // 16-31 бит адреса обработчика
    uint32_t high_offset;   // 32-63 бит адреса обработчика
    uint32_t reserved;      // Зарезервировано
} __attribute__((packed)) idt_gate_t;

// Указатель на IDT
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_register_t;

// ВАЖНО: Добавлен третий аргумент uint16_t sel
void set_idt_gate(int n, uint64_t handler, uint16_t sel);
void init_idt();
// on=1 -> trap gate (IF не гасится в сисколле), on=0 -> штатный interrupt gate.
void idt_set_syscall_trap_gate(int on);

#endif