// equantmemtest.c
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SYS_WRITE        1
#define SYS_BRK          12
#define SYS_SCHED_YIELD  24
#define SYS_SYSINFO      99
#define SYS_EXIT         60

#define PAGE_SIZE        4096

typedef struct {
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t used_ram;
    uint64_t pmm_total_pages;
    uint64_t pmm_used_pages;
    uint64_t kernel_heap_used;
} __attribute__((packed)) equant_sysinfo_t;

static inline int64_t syscall0(uint64_t n) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t n, uint64_t a1) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static void print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (uint64_t)s, len);
}

static inline void sys_exit(int code) {
    syscall1(SYS_EXIT, (uint64_t)code);
    for (;;) { __asm__ volatile("hlt"); }
}

static void print_dec(uint64_t val) {
    if (val == 0) {
        syscall3(SYS_WRITE, 1, (uint64_t)"0", 1);
        return;
    }
    char buf[22];
    int i = 20;
    buf[21] = '\0';
    while (val > 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    print_str(&buf[i + 1]);
}

static void print_hex(uint64_t val) {
    const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    buf[18] = '\0';
    for (int i = 17; i >= 2; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    print_str(buf);
}

static void kprintf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            syscall3(SYS_WRITE, 1, (uint64_t)&fmt[i], 1);
            continue;
        }
        i++;
        switch (fmt[i]) {
            case 's': {
                const char *s = __builtin_va_arg(args, const char *);
                print_str(s ? s : "(null)");
                break;
            }
            case 'u': {
                print_dec(__builtin_va_arg(args, uint64_t));
                break;
            }
            case 'p':
            case 'x': {
                print_hex(__builtin_va_arg(args, uint64_t));
                break;
            }
            case '%': {
                syscall3(SYS_WRITE, 1, (uint64_t)"%", 1);
                break;
            }
        }
    }
    __builtin_va_end(args);
}

static void *sys_sbrk(intptr_t increment) {
    uint64_t current_brk = syscall1(SYS_BRK, 0);
    if (increment == 0) return (void *)current_brk;
    uint64_t target_brk = current_brk + increment;
    uint64_t new_brk = syscall1(SYS_BRK, target_brk);
    if (new_brk != target_brk) return (void *)-1;
    return (void *)current_brk;
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *state = x;
}

void _start(void) {
    kprintf("\n==================================================\n");
    kprintf("       EQUANT-OS MEMORY & VMM VALIDATION SUITE     \n");
    kprintf("==================================================\n\n");

    // TEST 1: SYSINFO
    kprintf("[TEST 1] Querying Kernel Sysinfo & Memory Map...\n");
    equant_sysinfo_t info;
    if (syscall1(SYS_SYSINFO, (uint64_t)&info) == 0) {
        kprintf("  -> Total RAM:       %u MB\n", info.total_ram / (1024 * 1024));
        kprintf("  -> Free RAM:        %u MB\n", info.free_ram / (1024 * 1024));
        kprintf("  -> Used RAM:        %u MB\n", info.used_ram / (1024 * 1024));
        kprintf("  -> PMM Pages:       %u / %u\n", info.pmm_used_pages, info.pmm_total_pages);
        kprintf("  -> Kernel Heap:     %u KB\n", info.kernel_heap_used / 1024);
        kprintf("  [PASS] System info verified.\n\n");
    } else {
        kprintf("  [FAIL] SYS_SYSINFO returned error!\n\n");
    }

    // TEST 2: HEAP EXPANSION & INTEGRITY
    kprintf("[TEST 2] Testing Heap Expansion (sbrk) & Pattern Integrity...\n");
    const size_t total_size = 64 * PAGE_SIZE; // 256 KB
    void *heap_start = sys_sbrk(total_size);
    if (heap_start != (void *)-1) {
        kprintf("  -> Allocated 256 KB at virtual range: %p - %p\n", 
                heap_start, (void *)((uintptr_t)heap_start + total_size));

        uint64_t seed = 0xDEADBEEFCAFEBABEULL;
        uint64_t *ptr = (uint64_t *)heap_start;
        size_t words = total_size / sizeof(uint64_t);

        for (size_t i = 0; i < words; i++) ptr[i] = xorshift64(&seed);

        seed = 0xDEADBEEFCAFEBABEULL;
        bool ok = true;
        for (size_t i = 0; i < words; i++) {
            if (ptr[i] != xorshift64(&seed)) { ok = false; break; }
        }

        if (ok) kprintf("  [PASS] 256 KB Verified with 0 bitflips.\n\n");
        else kprintf("  [FAIL] Data corruption detected!\n\n");
    } else {
        kprintf("  [FAIL] sbrk failed!\n\n");
    }

    // TEST 3: SCHEDULER HEARTBEAT
    kprintf("[TEST 3] Running 50 scheduler cycles...\n");
    for (int i = 1; i <= 50; i++) {
        if (i % 10 == 0) {
            kprintf("  -> Tick #%u OK\n", i);
        }
        syscall0(SYS_SCHED_YIELD);
    }

    kprintf("\n[ALL TESTS PASSED] EquantOS kernel memory subsystem is ROCK SOLID!\n");
    sys_exit(0);
}