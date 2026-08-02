#include "gdt.h"
#include "../../syslibc/string.h"

gdt_table_t gdt;
gdt_ptr_t gdt_ptr;
tss_t tss;

extern void gdt_flush(uint64_t); // Напишем в ASM

void init_gdt() {
    memset(&gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    // 0x00: Null
    // 0x08: Kernel Code (Ring 0)
    gdt.entries[1].access = 0x9A; // Present, Ring 0, Code, Exec/Read
    gdt.entries[1].granularity = 0x20; // Long mode flag

    // 0x10: Kernel Data (Ring 0)
    gdt.entries[2].access = 0x92; // Present, Ring 0, Data, Read/Write
    
    // 0x18: User Data (Ring 3)
    gdt.entries[3].access = 0xF2; // Present, Ring 3, Data, Read/Write

    // 0x20: User Code (Ring 3)
    gdt.entries[4].access = 0xFA; // Present, Ring 3, Code, Exec/Read
    gdt.entries[4].granularity = 0x20;

    // Настройка TSS
    uint64_t tss_base = (uint64_t)&tss;
    gdt.tss.limit_low = sizeof(tss_t) - 1; 
    gdt.tss.base_low = tss_base & 0xFFFF;
    gdt.tss.base_mid = (tss_base >> 16) & 0xFF;
    gdt.tss.flags1 = 0x89; // Present, Executable, Accessed (TSS type)
    gdt.tss.flags2 = 0x00;
    gdt.tss.base_high = (tss_base >> 24) & 0xFF;
    gdt.tss.base_upper32 = (tss_base >> 32) & 0xFFFFFFFF;

    gdt_ptr.limit = sizeof(gdt_table_t) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    gdt_flush((uint64_t)&gdt_ptr);
    
    // Загружаем TSS
    __asm__ volatile("mov $0x28, %ax; ltr %ax");
}

void gdt_set_tss_stack(uint64_t stack) {
    tss.rsp0 = stack;
}