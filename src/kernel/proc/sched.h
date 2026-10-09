// src/kernel/proc/sched.h - O(1) Multilevel Priority Array Scheduler
#ifndef SCHED_H
#define SCHED_H

#include "task.h"

void sched_init(task_t *initial_task);
void sched_enqueue(task_t *task);
void sched_dequeue(task_t *task);
void sched_make_sleep(task_t *task, uint64_t sleep_until);
// Wake a task sleeping in sched_make_sleep() before its deadline
void sched_wake(task_t *task);

// I/O readiness waiting (poll/select/blocking sockets): a waiter registers
// before sleeping and any producer (socket, pipe, eventfd, input) wakes all
// waiters at once so they rescan their descriptors immediately.
// Drop every scheduler reference to a task that is about to be freed
void sched_forget(task_t *task);

void io_wait_prepare(void);
void io_wait_done(void);
void io_wake_all(void);
void sched_block(task_t *task);
void sched_unblock(task_t *task);

void sched_timer_tick(uint32_t current_tick);
uint64_t sched_switch(uint64_t current_rsp);
void sched_yield(void);

#endif // SCHED_H