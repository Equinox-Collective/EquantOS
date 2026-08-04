// equantmemtest.c - Memory and Scheduler stress-test program for EquantOS
#include <stdint.h>

static inline void yield(void) {
    __asm__ volatile("int $32" ::: "memory");
}

void _start(void) {
    volatile uint64_t counter = 0;
    volatile char test_var = 0;

    for (;;) {
        counter++;
        test_var = (char)(counter & 0xFF);
        
        if (test_var != (char)(counter & 0xFF)) {
            for (;;);
        }

        // Slow down yields to give the scheduler breathing room
        for (volatile int i = 0; i < 5000000; i++) {
            __asm__ volatile("nop");
        }

        yield();
    }
}