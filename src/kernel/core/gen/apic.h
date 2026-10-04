// src/kernel/core/gen/apic.h - Advanced Programmable Interrupt Controller Interface
#ifndef APIC_H
#define APIC_H

#include <stdint.h>
#include <stdbool.h>

#define IA32_APIC_BASE_MSR          0x1B
#define IA32_APIC_BASE_MSR_ENABLE   0x800

// Local APIC Register Offsets
#define LAPIC_ID                    0x020
#define LAPIC_VERSION               0x030
#define LAPIC_TPR                   0x080
#define LAPIC_EOI                   0x0B0
#define LAPIC_LDR                   0x0D0
#define LAPIC_DFR                   0x0E0
#define LAPIC_SVR                   0x0F0
#define LAPIC_ESR                   0x280
#define LAPIC_ICR_LOW               0x300
#define LAPIC_ICR_HIGH              0x310
#define LAPIC_LVT_TIMER             0x320
#define LAPIC_LVT_LINT0             0x350
#define LAPIC_LVT_LINT1             0x360
#define LAPIC_LVT_ERROR             0x370
#define LAPIC_TIMER_INITCNT         0x380
#define LAPIC_TIMER_CURRCNT         0x390
#define LAPIC_TIMER_DIV             0x3E0

#define LAPIC_TIMER_MODE_PERIODIC   (1U << 17)
#define LAPIC_TIMER_DIV_16          0x03
#define LAPIC_SVR_ENABLE            (1U << 8)

void apic_init(void);
void apic_eoi(void);
void apic_timer_init(uint32_t frequency_hz);
uint32_t apic_read(uint32_t reg);
void apic_write(uint32_t reg, uint32_t value);

#endif // APIC_H