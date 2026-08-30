// src/kernel/proc/task.h - Thread & Process Control Block with POSIX Support
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_OPEN_FILES 32
#define NUM_PRIORITIES 32
#define NSIG           64

// Priority Levels
#define PRIO_REALTIME    0
#define PRIO_INTERACTIVE 8
#define PRIO_NORMAL      16
#define PRIO_BACKGROUND  24

struct vfs_node;

typedef enum {
    TASK_STATE_RUNNABLE,
    TASK_STATE_SLEEPING,
    TASK_STATE_BLOCKED,
    TASK_STATE_ZOMBIE
} task_state_t;

typedef struct sigaction_info {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} sigaction_info_t;

typedef struct process {
    uint64_t pid;
    uint64_t parent_pid;
    uint64_t pgid;
    uint64_t cr3;
    uint64_t brk;
    char cwd[256];

    // File Descriptor Table with individual seek offsets and open flags
    struct vfs_node *files[MAX_OPEN_FILES];
    uint64_t file_offsets[MAX_OPEN_FILES];
    uint32_t file_flags[MAX_OPEN_FILES];

    // POSIX Security & Permissions
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t umask;

    // Signal Dispatching Table
    sigaction_info_t sigactions[NSIG];
    uint64_t sigmask;
    uint64_t sigpending;

    // Thread & Exit Synchronization
    uint64_t clear_child_tid;
    int exit_code;
    bool exited;
    struct task *wait_parent;

    // CPU Accounting (Ticks)
    uint64_t utime;
    uint64_t stime;
} process_t;

typedef struct task {
    uint64_t rsp;               // offset 0 (ASM context switch)
    uint64_t kstack_at_bottom;  // offset 8
    uint64_t id;                // offset 16 (TID)
    task_state_t state;
    bool running;
    
    uint8_t priority;
    uint64_t time_slice;
    uint64_t sleep_until;
    uint64_t fs_base;
    uint64_t gs_base;
    
    // Futex wait queue link
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
extern task_t *task_list;
extern uint64_t next_pid;

void task_init(void);
void task_create(void (*entry)(), uint64_t arg1, uint64_t arg2);
void task_init_fpu(task_t *task);
void yield(void);

#endif // TASK_H