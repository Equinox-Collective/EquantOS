#ifndef GDT_H
#define GDT_H

#include <stdint.h>

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;      // Стек ядра для Ring 0
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

// Для x86_64 TSS дескриптор занимает 16 байт
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  flags1;
    uint8_t  flags2;
    uint8_t  base_high;
    uint32_t base_upper32;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_entry_t;

typedef struct {
    gdt_entry_t entries[5]; // Null, KCode, KData, UData, UCode
    gdt_tss_entry_t tss;
} __attribute__((packed)) gdt_table_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

void init_gdt();
void gdt_set_tss_stack(uint64_t stack);

#endif