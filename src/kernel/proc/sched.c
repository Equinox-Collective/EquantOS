// src/kernel/proc/sched.c - O(1) Bitmask Priority Array Scheduler Implementation
#include "sched.h"
#include "../core/gen/gdt.h"
#include "string.h"
#include "stdio.h"

#define IA32_FS_BASE_MSR 0xC0000100

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static uint64_t kernel_cr3 = 0;

// O(1) Priority Array Queues
static task_t *run_queues[NUM_PRIORITIES] = {NULL};
static task_t *run_queues_tail[NUM_PRIORITIES] = {NULL};
static uint32_t active_priority_bitmap = 0; // 32-bit Mask for non-empty queues

static task_t *sleep_queue_head = NULL;
extern task_t *current_task;

void sched_init(task_t *initial_task) {
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    
    initial_task->state = TASK_STATE_RUNNABLE;
    initial_task->running = true;
    initial_task->priority = PRIO_INTERACTIVE;
    initial_task->time_slice = 10; // 10 ticks quantum
    
    initial_task->sched_next = NULL;
    initial_task->sched_prev = NULL;
    
    current_task = initial_task;
    sched_enqueue(initial_task);
}

// Add task to its respective priority queue in O(1)
void sched_enqueue(task_t *task) {
    if (!task) return;
    
    uint8_t prio = task->priority;
    if (prio >= NUM_PRIORITIES) prio = PRIO_NORMAL;

    task->state = TASK_STATE_RUNNABLE;
    task->running = true;
    task->sched_next = NULL;
    task->sched_prev = run_queues_tail[prio];
    
    if (run_queues_tail[prio]) {
        run_queues_tail[prio]->sched_next = task;
    } else {
        run_queues[prio] = task;
    }
    run_queues_tail[prio] = task;

    // Mark priority queue as active in bitmask
    active_priority_bitmap |= (1U << prio);
}

// Remove task from priority queue in O(1)
void sched_dequeue(task_t *task) {
    if (!task) return;
    
    uint8_t prio = task->priority;
    if (prio >= NUM_PRIORITIES) prio = PRIO_NORMAL;

    if (task->sched_prev) {
        task->sched_prev->sched_next = task->sched_next;
    } else {
        run_queues[prio] = task->sched_next;
    }
    
    if (task->sched_next) {
        task->sched_next->sched_prev = task->sched_prev;
    } else {
        run_queues_tail[prio] = task->sched_prev;
    }
    
    task->sched_next = NULL;
    task->sched_prev = NULL;

    // If queue is now empty, clear bit in bitmask
    if (run_queues[prio] == NULL) {
        active_priority_bitmap &= ~(1U << prio);
    }
}

void sched_block(task_t *task) {
    if (!task) return;
    sched_dequeue(task);
    task->state = TASK_STATE_BLOCKED;
    task->running = false;
}

void sched_unblock(task_t *task) {
    if (!task) return;
    sched_enqueue(task);
}

void sched_make_sleep(task_t *task, uint64_t sleep_until) {
    if (!task) return;
    
    sched_dequeue(task);
    
    task->state = TASK_STATE_SLEEPING;
    task->running = false;
    task->sleep_until = sleep_until;
    task->sched_next = NULL;
    task->sched_prev = NULL;
    
    if (!sleep_queue_head) {
        sleep_queue_head = task;
        return;
    }
    
    task_t *curr = sleep_queue_head;
    task_t *prev_node = NULL;
    
    while (curr && curr->sleep_until <= sleep_until) {
        prev_node = curr;
        curr = curr->sched_next;
    }
    
    if (!prev_node) {
        task->sched_next = sleep_queue_head;
        sleep_queue_head->sched_prev = task;
        sleep_queue_head = task;
    } else {
        task->sched_next = curr;
        task->sched_prev = prev_node;
        prev_node->sched_next = task;
        if (curr) {
            curr->sched_prev = task;
        }
    }
}

void sched_timer_tick(uint32_t current_tick) {
    while (sleep_queue_head && current_tick >= sleep_queue_head->sleep_until) {
        task_t *task = sleep_queue_head;
        sleep_queue_head = task->sched_next;
        if (sleep_queue_head) {
            sleep_queue_head->sched_prev = NULL;
        }
        task->sleep_until = 0;
        sched_enqueue(task);
    }
}

// O(1) Context Switch Engine using CPU Bit Scan Instruction
uint64_t sched_switch(uint64_t current_rsp) {
    if (!current_task) return current_rsp;
    
    current_task->rsp = current_rsp;
    task_t *prev_task = current_task;

    // Если задача завершилась (ZOMBIE) или заблокирована — удаляем её из очереди
    if (current_task->state != TASK_STATE_RUNNABLE) {
        sched_dequeue(current_task);
    } else {
        // Ротация текущей активной задачи
        sched_dequeue(current_task);
        sched_enqueue(current_task);
    }

    if (active_priority_bitmap == 0) {
        return current_rsp;
    }

    uint32_t highest_prio = (uint32_t)__builtin_ctz(active_priority_bitmap);
    current_task = run_queues[highest_prio];

    if (!current_task) return current_rsp;

    if (current_task != prev_task) {
        __asm__ volatile("fxsave64 (%0)"  :: "r"(task_fpu_area(prev_task))    : "memory");
        __asm__ volatile("fxrstor64 (%0)" :: "r"(task_fpu_area(current_task)) : "memory");
    }

    uint64_t new_cr3 = (current_task->process && current_task->process->cr3 != 0) 
                       ? current_task->process->cr3 
                       : kernel_cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

    gdt_set_tss_stack(current_task->kstack_at_bottom);

    if (current_task->fs_base != 0) {
        wrmsr(IA32_FS_BASE_MSR, current_task->fs_base);
    }

    return current_task->rsp;
}

void sched_yield(void) {
    __asm__ volatile ("int $32");
}

uint64_t schedule(uint64_t current_rsp) {
    return sched_switch(current_rsp);
}