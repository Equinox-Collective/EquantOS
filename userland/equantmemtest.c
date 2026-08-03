// equantmemtest.c - Memory and Scheduler stress-test program for EquantOS

// External yield function (if linked or available via trap/syscall)
extern void yield(void);

void _start(void) {
    volatile uint64_t counter = 0;
    
    // Allocate/test a scratch memory region mapped in our address space
    volatile char *test_ptr = (volatile char *)0x600000; 

    for (;;) {
        counter++;
        
        // Write test pattern to memory to verify VMM/PMM integrity
        *test_ptr = (char)(counter & 0xFF);
        
        // Read back and verify
        if (*test_ptr != (char)(counter & 0xFF)) {
            // Memory corruption detected! Loop forever or trigger error
            for (;;);
        }

        // Yield CPU to test round-robin scheduler preemption
        // (In a real system this would call a syscall, for now it traps or yields)
        for (volatile int i = 0; i < 1000000; i++) {
            __asm__ volatile("nop");
        }
    }
}