// src/kernel/proc/sched.h - O(1) Multilevel Priority Array Scheduler
#ifndef SCHED_H
#define SCHED_H

#include "task.h"

void sched_init(task_t *initial_task);
void sched_enqueue(task_t *task);
void sched_dequeue(task_t *task);
void sched_make_sleep(task_t *task, uint64_t sleep_until);
void sched_block(task_t *task);
void sched_unblock(task_t *task);

void sched_timer_tick(uint32_t current_tick);
uint64_t sched_switch(uint64_t current_rsp);
void sched_yield(void);

#endif // SCHED_H