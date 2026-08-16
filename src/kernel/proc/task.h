// src/kernel/proc/task.h
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_OPEN_FILES 16

struct vfs_node;

typedef enum {
    TASK_STATE_RUNNABLE,
    TASK_STATE_SLEEPING,
    TASK_STATE_BLOCKED,
    TASK_STATE_ZOMBIE
} task_state_t;

typedef struct process {
    uint64_t pid;
    uint64_t cr3;
    uint64_t brk;
    char cwd[256];                         // Текущая рабочая директория процесса
    struct vfs_node *files[MAX_OPEN_FILES]; // Таблица открытых файлов
} process_t;

typedef struct task {
    uint64_t rsp;               // offset 0
    uint64_t kstack_at_bottom;  // offset 8
    uint64_t id;                // offset 16
    task_state_t state;
    bool running;
    
    uint64_t sleep_until;
    uint64_t fs_base;
    
    process_t *process;

    uint8_t fpu_state[528];

    struct task *sched_next;
    struct task *sched_prev;
    struct task *next;
    struct task *prev;
} task_t;

#define task_fpu_area(t) ((void*)(((uintptr_t)((t)->fpu_state) + 15) & ~15ULL))

extern task_t *current_task;

void task_init(void);
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2);
void task_init_fpu(task_t *task);
void yield(void);

#endif // TASK_H