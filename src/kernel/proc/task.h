// src/kernel/proc/task.h - Thread & Process Control Block
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_OPEN_FILES 16
#define NUM_PRIORITIES 32

// Priority Levels (FreeBSD / Windows NT style)
#define PRIO_REALTIME    0   // High priority kernel threads
#define PRIO_INTERACTIVE 8   // User GUI, Keyboard, Shell
#define PRIO_NORMAL      16  // Standard user processes
#define PRIO_BACKGROUND  24  // Low priority batch processing

struct vfs_node;

typedef enum {
    TASK_STATE_RUNNABLE,
    TASK_STATE_SLEEPING,
    TASK_STATE_BLOCKED,
    TASK_STATE_ZOMBIE
} task_state_t;

typedef struct process {
    uint64_t pid;
    uint64_t parent_pid;        // PID родителя (для wait4)
    uint64_t cr3;
    uint64_t brk;
    char cwd[256];
    struct vfs_node *files[MAX_OPEN_FILES];

    // Для wait4: статус завершения
    int      exit_code;         // Код выхода из sys_exit
    bool     exited;            // true когда процесс вызвал exit()
    struct task *wait_parent;   // Задача родителя, ждущая нас через wait4
} process_t;

typedef struct task {
    uint64_t rsp;               // offset 0 (Used by ASM assembly context switch)
    uint64_t kstack_at_bottom;  // offset 8
    uint64_t id;                // offset 16
    task_state_t state;
    bool running;
    
    uint8_t priority;           // Priority level (0..31)
    uint64_t time_slice;        // Remaining execution quantum ticks
    uint64_t sleep_until;
    uint64_t fs_base;
    
    // Futex synchronization wait queue link
    uint64_t futex_addr;
    struct task *futex_next;

    process_t *process;

    uint8_t fpu_state[528] __attribute__((aligned(16)));

    struct task *sched_next;
    struct task *sched_prev;
    struct task *next;
    struct task *prev;
} task_t;

#define task_fpu_area(t) ((void*)(((uintptr_t)((t)->fpu_state) + 15) & ~15ULL))

extern task_t *current_task;
extern task_t *task_list; // Глобальный список всех задач (для fork)
extern uint64_t next_pid; // Глобальный счётчик PID (используется fork)

void task_init(void);
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2);
void task_init_fpu(task_t *task);
void yield(void);

#endif // TASK_H