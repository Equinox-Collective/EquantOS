#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    TASK_STATE_RUNNABLE,
    TASK_STATE_SLEEPING,
    TASK_STATE_BLOCKED,
    TASK_STATE_ZOMBIE
} task_state_t;

// Minimal process structure required by sched.c for CR3 switching
typedef struct process {
    uint64_t pid;
    uint64_t cr3;
    uint64_t brk;
} process_t;

typedef struct task {
    uint64_t rsp;               // Saved stack pointer
    uint64_t id;                // Thread ID (TID)
    task_state_t state;         // Current state
    bool running;               // Legacy running flag
    
    uint64_t kstack_at_bottom;  // Kernel stack base
    uint64_t sleep_until;       // Sleep timer tick
    uint64_t fs_base;           // Thread Local Storage (TLS) base
    
    process_t *process;         // Pointer to process control block

    uint8_t fpu_state[512];     // FPU state buffer (aligned via macro)

    // Scheduler queues links
    struct task *sched_next;
    struct task *sched_prev;

    // Global task list links
    struct task *next;
    struct task *prev;
} task_t;

// Safe alignment macro for FPU state usage
#define task_fpu_area(t) (void*)(((uint64_t)(t)->fpu_state + 15) & ~(uint64_t)15)

extern task_t *current_task;

void task_init(void);
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2);
void yield(void);

#endif // TASK_H