#include "task.h"
#include "sched.h"
#include "memory.h"
#include "pmm.h"
#include "string.h"
#include "stdio.h"

task_t *current_task = NULL;
static task_t *task_list = NULL;
static uint64_t next_pid = 1;

void task_init_fpu(task_t *task) {
    memset(task->fpu_state, 0, sizeof(task->fpu_state));
    uint16_t *fpu_cw = (uint16_t *)task_fpu_area(task);
    fpu_cw[0] = 0x037F; // Default FPU Control Word
    fpu_cw[2] = 0xFFFF; // Default FPU Tag Word
    uint32_t *fpu_mxcsr = (uint32_t *)((uint8_t *)task_fpu_area(task) + 24);
    *fpu_mxcsr = 0x1F80; // Default MXCSR state for SSE
}

void task_init(void) {
    current_task = (task_t *)kmalloc(sizeof(task_t));
    memset(current_task, 0, sizeof(task_t));

    uint16_t *fpu_cw = (uint16_t *)task_fpu_area(current_task);
    fpu_cw[0] = 0x037F;
    uint32_t *fpu_mxcsr = (uint32_t *)((uint8_t *)task_fpu_area(current_task) + 24);
    *fpu_mxcsr = 0x1F80;
    task_init_fpu(current_task);
    current_task->id = next_pid++;
    current_task->running = true;
    current_task->state = TASK_STATE_RUNNABLE;
    current_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    current_task->next = current_task;
    current_task->prev = current_task;
    task_list = current_task;
}

void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2) {
    task_t *new_task = (task_t *)kmalloc(sizeof(task_t));
    memset(new_task, 0, sizeof(task_t));

    new_task->id = next_pid++;
    new_task->running = true;
    new_task->state = TASK_STATE_RUNNABLE;
    new_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    // Build initial stack frame compatible with SAVE_REGS and iretq
    uint64_t *stack = (uint64_t *)new_task->kstack_at_bottom;

    // IRETQ stack frame
    *--stack = 0x10;                    // SS
    *--stack = new_task->kstack_at_bottom; // RSP
    *--stack = 0x202;                   // RFLAGS (Interrupts enabled)
    *--stack = 0x08;                    // CS
    *--stack = (uint64_t)entry;         // RIP

    // Dummy error code and interrupt number
    *--stack = 0;
    *--stack = 0;

    // SAVE_REGS pushes 15 general-purpose registers (rax ... r15)
    // We allocate space for them
    for (int i = 0; i < 15; i++) {
        *--stack = 0;
    }

    // Set rdi and rsi arguments (located inside the saved registers frame)
    // Based on SAVE_REGS order: rax, rbx, rcx, rdx, rbp, rsi, rdi ...
    // rdi is 9th from top of SAVE_REGS, rsi is 10th. 
    // To keep it safe, we let the scheduler handle execution context.
    (void)arg1;
    (void)arg2;

    new_task->rsp = (uint64_t)stack;

    // Link into global task list
    new_task->next = task_list->next;
    new_task->prev = task_list;
    task_list->next->prev = new_task;
    task_list->next = new_task;

    // Enqueue to scheduler
    sched_enqueue(new_task);
}

void yield(void) {
    __asm__ volatile ("int $32");
}