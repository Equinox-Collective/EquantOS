// equantmemtest.c - Memory and Scheduler stress-test program for EquantOS
#include <stdint.h>


// Local inline yield stub triggering the scheduler interrupt (vector 32)
static inline void yield(void) {
    __asm__ volatile("int $32" ::: "memory");
}

void _start(void) {
    volatile uint64_t counter = 0;
    volatile char test_var = 0;

    for (;;) {
        counter++;
        
        // Write test pattern to stack variable
        test_var = (char)(counter & 0xFF);
        
        // Read back and verify
        if (test_var != (char)(counter & 0xFF)) {
            for (;;); // Trap on memory corruption
        }

        // Yield CPU to test round-robin scheduler preemption
        yield();
    }
}