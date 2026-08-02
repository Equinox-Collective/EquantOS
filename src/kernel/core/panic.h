#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

// CPU state structure matching the stack layout pushed by interrupts.asm
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, user_rsp, ss;
} __attribute__((packed)) cpu_state_t;

// Macro to trigger a software-level kernel panic
#define PANIC(msg) kernel_panic(NULL, __FILE__, __LINE__, msg)

/**
 * @brief Unified kernel panic handler supporting both CPU exceptions and software panics.
 * @param state Pointer to CPU register state if called from IDT, or NULL for software panics.
 * @param file Source file name (optional).
 * @param line Source line number (optional).
 * @param message Error description message.
 */
void __attribute__((noreturn)) kernel_panic(cpu_state_t *state, const char *file, int line, const char *message);

#endif // PANIC_H