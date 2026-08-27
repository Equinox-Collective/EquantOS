// equantmemtest.c - Production-grade Memory, PMM, VMM & Heap Stress Test for EquantOS
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_BRK         12
#define SYS_YIELD       24      // Либо твой номер SYS_YIELD
#define SYS_SYSINFO     99

#define PAGE_SIZE       4096

typedef struct {
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t used_ram;
    uint64_t pmm_total_pages;
    uint64_t pmm_used_pages;
    uint64_t kernel_heap_used;
} equant_sysinfo_t;

/* ========================================================================= */
/*                   x86_64 Syscall Gateway (Standard ABI)                   */
/* ========================================================================= */

static inline int64_t syscall0(uint64_t n) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(n)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall1(uint64_t n, uint64_t a1) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall3(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* ========================================================================= */
/*                          Minimal Standalone I/O                           */
/* ========================================================================= */

static void sys_putc(char c) {
    syscall3(SYS_WRITE, 1, (uint64_t)&c, 1);
}

static void sys_puts(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (uint64_t)s, len);
}

static void print_hex(uint64_t val, int width) {
    const char hex_chars[] = "0123456789ABCDEF";
    char buf[17];
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex_chars[val & 0xF];
        val >>= 4;
    }
    int start = 16 - width;
    if (start < 0) start = 0;
    sys_puts(&buf[start]);
}

static void print_dec(uint64_t val) {
    if (val == 0) {
        sys_putc('0');
        return;
    }
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    while (val > 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    sys_puts(&buf[i + 1]);
}

// Легковесный форматированный вывод (без libc)
static void kprintf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            sys_putc(fmt[i]);
            continue;
        }
        i++;
        switch (fmt[i]) {
            case 's': {
                const char *s = __builtin_va_arg(args, const char *);
                sys_puts(s ? s : "(null)");
                break;
            }
            case 'd': {
                int64_t d = __builtin_va_arg(args, int64_t);
                if (d < 0) {
                    sys_putc('-');
                    d = -d;
                }
                print_dec((uint64_t)d);
                break;
            }
            case 'u': {
                print_dec(__builtin_va_arg(args, uint64_t));
                break;
            }
            case 'x': {
                print_hex(__builtin_va_arg(args, uint32_t), 8);
                break;
            }
            case 'p': {
                sys_puts("0x");
                print_hex(__builtin_va_arg(args, uint64_t), 16);
                break;
            }
            case '%': {
                sys_putc('%');
                break;
            }
            default:
                sys_putc('%');
                sys_putc(fmt[i]);
                break;
        }
    }
    __builtin_va_end(args);
}

/* ========================================================================= */
/*                            Memory Operations                              */
/* ========================================================================= */

static void *sys_sbrk(intptr_t increment) {
    uint64_t current_brk = syscall1(SYS_BRK, 0);
    if (increment == 0) return (void *)current_brk;
    
    uint64_t target_brk = current_brk + increment;
    uint64_t new_brk = syscall1(SYS_BRK, target_brk);
    
    if (new_brk != target_brk) {
        return (void *)-1; // Allocation failed
    }
    return (void *)current_brk;
}

// Генератор псевдослучайных чисел (Xorshift64) для стресс-теста бит-паттернов
static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *state = x;
}

/* ========================================================================= */
/*                              Test Suite                                   */
/* ========================================================================= */

static void test_sysinfo(void) {
    kprintf("[TEST 1] Querying Kernel Sysinfo & Memory Map...\n");
    equant_sysinfo_t info;
    
    if (syscall1(SYS_SYSINFO, (uint64_t)&info) == 0) {
        kprintf("  -> Total RAM:       %u MB\n", info.total_ram / (1024 * 1024));
        kprintf("  -> Free RAM:        %u MB\n", info.free_ram / (1024 * 1024));
        kprintf("  -> Used RAM:        %u MB\n", info.used_ram / (1024 * 1024));
        kprintf("  -> PMM Pages:       %u / %u used\n", info.pmm_used_pages, info.pmm_total_pages);
        kprintf("  -> Kernel Heap:     %u KB\n", info.kernel_heap_used / 1024);
        kprintf("  [PASS] System info retrieved successfully.\n\n");
    } else {
        kprintf("  [FAIL] SYS_SYSINFO syscall returned error!\n\n");
    }
}

static void test_heap_expansion_and_patterns(void) {
    kprintf("[TEST 2] Testing Heap Expansion (sbrk) & Pattern Integrity...\n");

    const size_t num_pages = 64; // 256 KB
    const size_t total_size = num_pages * PAGE_SIZE;

    void *heap_start = sys_sbrk(total_size);
    if (heap_start == (void *)-1) {
        kprintf("  [FAIL] Unable to allocate %u bytes via sbrk!\n\n", total_size);
        return;
    }
    kprintf("  -> Allocated %u KB at virtual range: %p - %p\n", 
            total_size / 1024, heap_start, (void *)((uintptr_t)heap_start + total_size));

    // Phase A: Write pseudo-random entropy
    kprintf("  -> Writing entropy patterns across all page boundaries...\n");
    uint64_t seed = 0xDEADBEEFCAFEBABEULL;
    uint64_t *ptr = (uint64_t *)heap_start;
    size_t words = total_size / sizeof(uint64_t);

    for (size_t i = 0; i < words; i++) {
        ptr[i] = xorshift64(&seed);
    }

    // Phase B: Verify data integrity
    kprintf("  -> Verifying pattern integrity (Checking VMM mapping consistency)...\n");
    seed = 0xDEADBEEFCAFEBABEULL;
    bool corrupted = false;

    for (size_t i = 0; i < words; i++) {
        uint64_t expected = xorshift64(&seed);
        if (ptr[i] != expected) {
            kprintf("  [CORRUPTION] Mismatch at index %u (%p): expected %x, got %x\n",
                    i, &ptr[i], expected, ptr[i]);
            corrupted = true;
            break;
        }
    }

    if (!corrupted) {
        kprintf("  [PASS] 256 KB Verified with 0 bitflips / page corruption.\n\n");
    } else {
        kprintf("  [FAIL] Data corruption detected!\n\n");
    }
}

static void test_scheduler_stress(void) {
    kprintf("[TEST 3] Entering Scheduler & Multitasking Stress Loop...\n");
    volatile uint64_t iterations = 0;

    for (;;) {
        iterations++;

        if (iterations % 1000 == 0) {
            kprintf("[equantmemtest] Alive. Iterations: %u | brk: %p\n", 
                    iterations, (void *)syscall1(SYS_BRK, 0));
        }

        for (volatile int delay = 0; delay < 100000; delay++) {
            __asm__ volatile("pause");
        }

        syscall0(SYS_YIELD);
    }
}

void _start(void) {
    kprintf("\n==================================================\n");
    kprintf("       EQUANT-OS MEMORY & VMM VALIDATION SUITE     \n");
    kprintf("==================================================\n\n");

    test_sysinfo();
    test_heap_expansion_and_patterns();
    test_scheduler_stress();
}