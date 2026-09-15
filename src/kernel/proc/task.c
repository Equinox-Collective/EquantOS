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
task_t *task_list = NULL;
uint64_t next_pid = 1;

static process_t kernel_root_proc;

void task_init_fpu(task_t *task) {
    memset(task->fpu_state, 0, sizeof(task->fpu_state));
    uint16_t *fpu_cw = (uint16_t *)task_fpu_area(task);
    fpu_cw[0] = 0x037F;
    fpu_cw[2] = 0x00FF;
    uint32_t *fpu_mxcsr = (uint32_t *)((uint8_t *)task_fpu_area(task) + 24);
    *fpu_mxcsr = 0x1F80;
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

    *--stack = 0x10;
    *--stack = idle_task->kstack_at_bottom;
    *--stack = 0x202;
    *--stack = 0x08;
    *--stack = (uint64_t)idle_thread_entry;
    *--stack = 0;
    *--stack = 0;

    for (int i = 0; i < 15; i++) {
        *--stack = 0;
    }

    idle_task->rsp = (uint64_t)stack;
    idle_task->next = idle_task;
    idle_task->prev = idle_task;
}

void task_init(void) {
    memset(&kernel_root_proc, 0, sizeof(process_t));
    kernel_root_proc.pid = 0;
    strcpy(kernel_root_proc.cwd, "/");

    current_task = (task_t *)kmalloc(sizeof(task_t));
    memset(current_task, 0, sizeof(task_t));

    task_init_fpu(current_task);
    current_task->id = next_pid++;
    current_task->running = true;
    current_task->state = TASK_STATE_RUNNABLE;
    current_task->priority = PRIO_NORMAL;
    current_task->time_slice = 10;
    current_task->process = &kernel_root_proc;
    current_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    current_task->next = current_task;
    current_task->prev = current_task;
    task_list = current_task;
}

void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2) {
    task_t *new_task = (task_t *)kmalloc(sizeof(task_t));
    memset(new_task, 0, sizeof(task_t));

    task_init_fpu(new_task);

    new_task->id = next_pid++;
    new_task->priority = PRIO_NORMAL;
    new_task->time_slice = 10;
    new_task->running = true;
    new_task->state = TASK_STATE_RUNNABLE;
    new_task->process = &kernel_root_proc;
    new_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;

    uint64_t *stack = (uint64_t *)new_task->kstack_at_bottom;

    *--stack = 0x10;
    *--stack = new_task->kstack_at_bottom;
    *--stack = 0x202;
    *--stack = 0x08;
    *--stack = (uint64_t)entry;
    *--stack = 0;
    *--stack = 0;

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
    sched_yield();
}

static int __init tasking_subsys_initcall(void) {
    task_init();
    task_create_idle();
    sched_init(current_task);
    serial_puts(COM1, "[KERNEL] Multithreading & O(1) Scheduler Subsystem Initialized.\n");
    return 0;
}
subsys_initcall(tasking_subsys_initcall);