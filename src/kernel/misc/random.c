// src/kernel/misc/random.c - Hardware RDRAND and Entropy-Mixing CSPRNG
#include "random.h"
#include "rtc.h"
#include "../core/initcall.h"
#include "../drivers/serial/serial.h"
#include "timer.h"

static bool has_rdrand = false;
static uint64_t prng_state = 0x853c49e6748fea9bULL;

static inline bool cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (ecx & (1 << 30)) != 0; // ECX bit 30 = RDRAND support
}

void random_init(void) {
    has_rdrand = cpu_has_rdrand();
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    prng_state ^= tsc ^ rtc_get_unix_timestamp();

    if (has_rdrand) {
        serial_puts(COM1, "[RANDOM] Hardware RDRAND instruction detected and enabled.\n");
    } else {
        serial_puts(COM1, "[RANDOM] Hardware RDRAND not present. Using high-entropy fallback PRNG.\n");
    }
}

uint64_t random_get_u64(void) {
    if (has_rdrand) {
        uint64_t val = 0;
        unsigned char ok;
        for (int i = 0; i < 10; i++) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
            if (ok) return val;
        }
    }

    // SplitMix64 / XorShift64 Fallback with jitter mixing
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    prng_state += 0x9E3779B97F4A7C15ULL + tsc + tick;
    uint64_t z = prng_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint32_t random_get_u32(void) {
    return (uint32_t)(random_get_u64() & 0xFFFFFFFF);
}

void random_fill(void *buf, size_t len) {
    if (!buf || len == 0) return;
    uint8_t *ptr = (uint8_t *)buf;

    while (len >= 8) {
        uint64_t r = random_get_u64();
        *(uint64_t *)ptr = r;
        ptr += 8;
        len -= 8;
    }

    if (len > 0) {
        uint64_t r = random_get_u64();
        for (size_t i = 0; i < len; i++) {
            ptr[i] = (uint8_t)(r >> (i * 8));
        }
    }
}

static int __init random_initcall(void) {
    random_init();
    return 0;
}
core_initcall(random_initcall);