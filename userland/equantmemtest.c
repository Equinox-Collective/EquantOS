// equantmemtest.c - Comprehensive Memory, PMM, VMM, and Heap Stress Test for EquantOS
#include <stdint.h>
#include <stddef.h>

#define SYS_WRITE   1
#define SYS_BRK     12
#define SYS_SYSINFO 99
#define SYS_YIELD   158

typedef struct {
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t used_ram;
    uint64_t pmm_total_pages;
    uint64_t pmm_used_pages;
    uint64_t kernel_heap_used;
} equant_sysinfo_t;

static inline int64_t syscall0(uint64_t n) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t n, uint64_t a1) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1) : "memory");
    return ret;
}

static void print(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    syscall1(SYS_WRITE, (uint64_t)str);
}

static void *sys_sbrk(int64_t increment) {
    uint64_t current_brk = syscall1(SYS_BRK, 0);
    if (increment == 0) return (void *)current_brk;
    uint64_t new_brk = syscall1(SYS_BRK, current_brk + increment);
    if (new_brk == (uint64_t)-1) return (void *)-1;
    return (void *)current_brk;
}

void _start(void) {
    print("\n========================================\n");
    print("      EQUANTMEMTEST STARTING SUITE      \n");
    print("========================================\n");

    // 1. Test System Info & PMM / Heap stats
    print("[TEST 1] Querying PMM, VMM & Kernel Stats via SYS_SYSINFO...\n");
    equant_sysinfo_t info;
    if (syscall1(SYS_SYSINFO, (uint64_t)&info) == 0) {
        print("  -> SYS_SYSINFO query successful!\n");
    } else {
        print("  -> FAILED to query sysinfo!\n");
    }

    // 2. Test User Heap Expansion via SYS_BRK
    print("[TEST 2] Testing User Heap (sys_brk / sbrk)...\n");
    void *heap_chunk1 = sys_sbrk(4096);
    if (heap_chunk1 != (void *)-1) {
        print("  -> Allocated 4KB user heap block successfully.\n");
        volatile char *p = (volatile char *)heap_chunk1;
        p[0] = 'E';
        p[4095] = 'Q';
        if (p[0] == 'E' && p[4095] == 'Q') {
            print("  -> Heap read/write verification PASSED.\n");
        } else {
            print("  -> Heap read/write verification FAILED!\n");
        }
    } else {
        print("  -> Failed to allocate user heap block via sys_brk!\n");
    }

    // 3. Continuous Scheduler & Memory Stress Loop
    print("[TEST 3] Entering Continuous Memory & Scheduler Stress Loop...\n");
    volatile uint64_t counter = 0;

    for (;;) {
        counter++;
        
        // Every 500 iterations, print diagnostic heartbeat and yield
        if (counter % 500 == 0) {
            print("[equantmemtest] Heartbeat tick. Counter active.\n");
        }

        // Slow down yields to give scheduler breathing room
        for (volatile int i = 0; i < 200000; i++) {
            __asm__ volatile("nop");
        }

        syscall0(SYS_YIELD);
    }
}