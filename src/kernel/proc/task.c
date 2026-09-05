#include "task.h"
#include "sched.h"
#include "../core/mem/memory.h"
#include "../core/mem/pmm.h"
#include "string.h"
#include "stdio.h"
#include "../core/initcall.h"
#include "../drivers/serial/serial.h"

task_t *current_task = NULL;
task_t *idle_task = NULL;
task_t *task_list = NULL;  // Убрали static для использования в syscall.c (fork)
uint64_t next_pid = 1;

void task_init_fpu(task_t *task) {
    memset(task->fpu_state, 0, sizeof(task->fpu_state));
    uint16_t *fpu_cw = (uint16_t *)task_fpu_area(task);
    fpu_cw[0] = 0x037F; // Default FPU Control Word
    fpu_cw[2] = 0xFFFF; // Default FPU Tag Word
    uint32_t *fpu_mxcsr = (uint32_t *)((uint8_t *)task_fpu_area(task) + 24);
    *fpu_mxcsr = 0x1F80; // Default MXCSR state for SSE
}

static void idle_thread_entry(void) {
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

void task_create_idle(void) {
    idle_task = (task_t *)kmalloc(sizeof(task_t));
    memset(idle_task, 0, sizeof(task_t));

    task_init_fpu(idle_task);

    idle_task->id = 0;
    idle_task->priority = PRIO_BACKGROUND;
    idle_task->time_slice = 1;
    idle_task->running = true;
    idle_task->state = TASK_STATE_RUNNABLE;
    idle_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    uint64_t *stack = (uint64_t *)idle_task->kstack_at_bottom;

    *--stack = 0x10;                                // SS (Kernel Data)
    *--stack = idle_task->kstack_at_bottom;         // RSP
    *--stack = 0x202;                               // RFLAGS (IF=1)
    *--stack = 0x08;                                // CS (Kernel Code)
    *--stack = (uint64_t)idle_thread_entry;         // RIP

    *--stack = 0;                                   // Error code
    *--stack = 0;                                   // Interrupt number

    for (int i = 0; i < 15; i++) {
        *--stack = 0;
    }

    idle_task->rsp = (uint64_t)stack;
    idle_task->next = idle_task;
    idle_task->prev = idle_task;
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

    task_init_fpu(new_task); // <-- ОБЯЗАТЕЛЬНО инициализируем FPU/SSE контекст

    new_task->id = next_pid++;
    new_task->priority = PRIO_NORMAL; // <-- Выставляем нормальный приоритет (16), а не 0
    new_task->time_slice = 10;
    new_task->running = true;
    new_task->state = TASK_STATE_RUNNABLE;
    new_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    uint64_t *stack = (uint64_t *)new_task->kstack_at_bottom;

    *--stack = 0x10;                    // SS
    *--stack = new_task->kstack_at_bottom; // RSP
    *--stack = 0x202;                   // RFLAGS
    *--stack = 0x08;                    // CS
    *--stack = (uint64_t)entry;         // RIP

    *--stack = 0;                       // Error code
    *--stack = 0;                       // Int no

    for (int i = 0; i < 15; i++) {
        *--stack = 0;
    }

    (void)arg1;
    (void)arg2;

    new_task->rsp = (uint64_t)stack;

    new_task->next = task_list->next;
    new_task->prev = task_list;
    task_list->next->prev = new_task;
    task_list->next = new_task;

    sched_enqueue(new_task);
}

void yield(void) {
    __asm__ volatile ("int $32");
}

// // THIS SHOULD BELONG TO BOTTOM, DO NOT REWRITE IN ANY CASE // //

static int __init tasking_subsys_initcall(void) {
    task_init();
    sched_init(current_task);
    serial_puts(COM1, "[KERNEL] Multithreading & O(1) Scheduler Subsystem Initialized.\n");
    return 0;
}
subsys_initcall(tasking_subsys_initcall);