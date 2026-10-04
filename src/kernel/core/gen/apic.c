// src/kernel/core/gen/apic.c - Local APIC & LAPIC Timer Implementation
#include "apic.h"
#include "cpu.h"
#include "io.h"
#include "../mem/vmm.h"
#include "../../drivers/serial/serial.h"

static volatile uint8_t *lapic_base = NULL;
static uint32_t lapic_ticks_per_ms = 0;

uint32_t apic_read(uint32_t reg) {
    if (!lapic_base) return 0;
    return *(volatile uint32_t *)(lapic_base + reg);
}

void apic_write(uint32_t reg, uint32_t value) {
    if (!lapic_base) return;
    *(volatile uint32_t *)(lapic_base + reg) = value;
}

void apic_eoi(void) {
    apic_write(LAPIC_EOI, 0);
}

static void disable_legacy_pic(void) {
    // Mask all interrupts on Master and Slave 8259 PIC
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    serial_puts(COM1, "[APIC] Legacy 8259 PIC hardware masked and disabled.\n");
}

static void apic_calibrate_timer(void) {
    // Set divider to 16
    apic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);

    // Prepare PIT channel 2 for calibration (10 milliseconds delay)
    outb(0x61, (inb(0x61) & 0xFD) | 1);
    outb(0x43, 0xB2);
    // 1193182 / 100 = 11931 ticks for 10ms
    outb(0x42, 0x9B);
    outb(0x42, 0x2E);

    // Reset PIT counter flag
    uint8_t reset_val = inb(0x61) & 0xFE;
    outb(0x61, reset_val);
    outb(0x61, reset_val | 1);

    // Set initial count to maximum
    apic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);

    // Wait until PIT timer completes 10ms
    while (!(inb(0x61) & 0x20));

    // Stop APIC timer
    apic_write(LAPIC_LVT_TIMER, 1U << 16); // Mask timer

    uint32_t ticks_passed = 0xFFFFFFFF - apic_read(LAPIC_TIMER_CURRCNT);
    lapic_ticks_per_ms = ticks_passed / 10;

    // Reset gate
    outb(0x61, reset_val);
}

void apic_timer_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 250;

    apic_calibrate_timer();

    uint32_t interval_ticks = (lapic_ticks_per_ms * 1000) / frequency_hz;

    // Set divider to 16
    apic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);

    // Vector 32 (IRQ0), Periodic Mode, Unmasked
    apic_write(LAPIC_LVT_TIMER, 32 | LAPIC_TIMER_MODE_PERIODIC);

    // Load initial tick count
    apic_write(LAPIC_TIMER_INITCNT, interval_ticks);

    serial_puts(COM1, "[APIC] LAPIC Timer calibrated and running in Periodic Mode.\n");
}

void apic_init(void) {
    disable_legacy_pic();

    // 1. Read physical base from IA32_APIC_BASE_MSR
    uint64_t apic_msr = read_msr(IA32_APIC_BASE_MSR);
    uint64_t lapic_phys = apic_msr & 0xFFFFF000ULL;

    // Enable APIC globally via MSR bit 11
    apic_msr |= IA32_APIC_BASE_MSR_ENABLE;
    write_msr(IA32_APIC_BASE_MSR, apic_msr);

    // 2. Map APIC MMIO space as strictly uncacheable
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_val & ~0xFFFULL);

    uint64_t lapic_virt = 0xFFFFC30000000000ULL;
    vmm_map(pml4, lapic_virt, lapic_phys, PTE_PRESENT | PTE_WRITABLE | PTE_MMIO_UC);
    lapic_base = (volatile uint8_t *)lapic_virt;

    // 3. Clear task priority to accept all interrupts
    apic_write(LAPIC_TPR, 0);

    // 4. Set Flat Model in Destination Format Register
    apic_write(LAPIC_DFR, 0xFFFFFFFF);
    apic_write(LAPIC_LDR, (apic_read(LAPIC_LDR) & 0x00FFFFFF) | 1);

    // 5. Enable APIC in Spurious Vector Register (Vector 255)
    apic_write(LAPIC_SVR, 0xFF | LAPIC_SVR_ENABLE);

    // 6. Start LAPIC Timer at 250 Hz
    apic_timer_init(250);

    serial_puts(COM1, "[APIC] Local APIC initialized successfully.\n");
}