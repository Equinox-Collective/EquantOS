// src/kernel/core/gen/gdt.c - Global Descriptor Table & Task State Segment Setup
#include "gdt.h"
#include "string.h"

gdt_table_t gdt;
gdt_ptr_t gdt_ptr;
tss_t tss;

// Dedicated 4KB stack for Double Fault (#DF) exception recovery via IST1
static uint8_t double_fault_stack[4096] __attribute__((aligned(16)));

extern void gdt_flush(uint64_t);

void init_gdt(void) {
    memset(&gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    // Entry 0: 0x00 Null Descriptor (Required by x86-64 architecture)

    // Entry 1: 0x08 Kernel Code (Ring 0, 64-bit Long Mode)
    gdt.entries[1].access = 0x9A;      // Present, Ring 0, Code, Exec/Read
    gdt.entries[1].granularity = 0x20; // L-bit = 1 (64-bit Long Mode)

    // Entry 2: 0x10 Kernel Data (Ring 0)
    gdt.entries[2].access = 0x92;      // Present, Ring 0, Data, Read/Write
    gdt.entries[2].granularity = 0x00;

    // Entry 3: 0x18 User Data (Ring 3)
    gdt.entries[3].access = 0xF2;      // Present, Ring 3, Data, Read/Write
    gdt.entries[3].granularity = 0x00;

    // Entry 4: 0x20 User Code (Ring 3, 64-bit Long Mode)
    gdt.entries[4].access = 0xFA;      // Present, Ring 3, Code, Exec/Read
    gdt.entries[4].granularity = 0x20; // L-bit = 1 (64-bit Long Mode)

    // Entry 5 & 6: 0x28 Task State Segment (Takes 16 bytes in Long Mode)
    uint64_t tss_base = (uint64_t)&tss;
    gdt.tss.limit_low    = sizeof(tss_t) - 1;
    gdt.tss.base_low     = tss_base & 0xFFFF;
    gdt.tss.base_mid     = (tss_base >> 16) & 0xFF;
    gdt.tss.flags1       = 0x89;       // Present, 64-bit TSS (Available)
    gdt.tss.flags2       = 0x00;
    gdt.tss.base_high    = (tss_base >> 24) & 0xFF;
    gdt.tss.base_upper32 = (tss_base >> 32) & 0xFFFFFFFF;
    gdt.tss.reserved     = 0;

    // Emergency Interrupt Stack Table 1 (IST1) for Double Fault (#DF)
    // Points to the top of the allocated stack buffer
    tss.ist[0] = (uint64_t)&double_fault_stack[sizeof(double_fault_stack)];
    tss.iopb_offset = sizeof(tss_t);   // Point beyond TSS to deny direct I/O port bypass

    gdt_ptr.limit = sizeof(gdt_table_t) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    gdt_flush((uint64_t)&gdt_ptr);

    // Load Task Register with 0x28 selector
    __asm__ volatile("mov $0x28, %ax; ltr %ax");
}

// Sets the Ring 0 kernel stack pointer for privilege transitions (Ring 3 -> Ring 0)
void gdt_set_tss_stack(uint64_t stack) {
    tss.rsp0 = stack;
}