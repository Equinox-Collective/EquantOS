#include "gdt.h"
#include <string.h>

gdt_table_t gdt;
gdt_ptr_t gdt_ptr;
tss_t tss;

extern void gdt_flush(uint64_t);

// Helper function to populate a GDT descriptor entry with proper limits and flags
static void set_gdt_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt.entries[index].base_low    = (base & 0xFFFF);
    gdt.entries[index].base_middle = (base >> 16) & 0xFF;
    gdt.entries[index].base_high   = (base >> 24) & 0xFF;
    gdt.entries[index].limit_low   = (limit & 0xFFFF);
    gdt.entries[index].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt.entries[index].access      = access;
}

void init_gdt(void) {
    memset(&gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    // 0x00: Null descriptor
    set_gdt_entry(0, 0, 0, 0, 0);

    // 0x08: Kernel Code Segment (Ring 0)
    // Access: 0x9A (Present, Ring 0, Code, Exec/Read)
    // Granularity: 0xA0 (G=1 [4KB blocks], L=1 [64-bit mode active], Limit high = 0xF)
    set_gdt_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);

    // 0x10: Kernel Data Segment (Ring 0)
    // Access: 0x92 (Present, Ring 0, Data, Read/Write)
    // Granularity: 0xC0 (G=1, DB=1 [32/64-bit data], Limit high = 0xF)
    set_gdt_entry(2, 0, 0xFFFFF, 0x92, 0xC0);

    // 0x18: User Data Segment (Ring 3)
    // Access: 0xF2 (Present, Ring 3, Data, Read/Write)
    // Granularity: 0xC0 (G=1, DB=1, Limit high = 0xF)
    set_gdt_entry(3, 0, 0xFFFFF, 0xF2, 0xC0);

    // 0x20: User Code Segment (Ring 3)
    // Access: 0xFA (Present, Ring 3, Code, Exec/Read)
    // Granularity: 0xA0 (G=1, L=1, Limit high = 0xF)
    set_gdt_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);

    // Configure Task State Segment (TSS) descriptor (spans 16 bytes: entries 5 and 6)
    uint64_t tss_base = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss_t) - 1;

    gdt.tss.limit_low = tss_limit & 0xFFFF;
    gdt.tss.base_low = tss_base & 0xFFFF;
    gdt.tss.base_mid = (tss_base >> 16) & 0xFF;
    gdt.tss.flags1 = 0x89; // Present, 64-bit TSS available type
    gdt.tss.flags2 = ((tss_limit >> 16) & 0x0F);
    gdt.tss.base_high = (tss_base >> 24) & 0xFF;
    gdt.tss.base_upper32 = (uint32_t)(tss_base >> 32);
    gdt.tss.reserved = 0;

    gdt_ptr.limit = sizeof(gdt_table_t) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    // Flush GDT and reload segment selectors
    gdt_flush((uint64_t)&gdt_ptr);
    
    // Load Task State Segment (selector 0x28)
    __asm__ volatile("mov $0x28, %ax; ltr %ax");
}

void gdt_set_tss_stack(uint64_t stack) {
    tss.rsp0 = stack;
}