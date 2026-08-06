#ifndef CPU_H
#define CPU_H

#include <stdint.h>

static inline void stac() {
    // __asm__ volatile("stac" ::: "cc");
}

static inline void clac() {
    // __asm__ volatile("clac" ::: "cc");
}

static inline uint64_t read_cr0() {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(val));
}

static inline uint64_t read_cr4() {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(val));
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void write_msr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}
#endif
