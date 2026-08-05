#include "sched.h"
#include "gdt.h"
#include "string.h"
#include "timer.h"

#define IA32_FS_BASE_MSR 0xC0000100

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

extern uint64_t hhdm_offset;
static uint64_t kernel_cr3 = 0;

// Очередь готовых задач (Run Queue)
static task_t *run_queue_head = NULL;
static task_t *run_queue_tail = NULL;

// Очередь спящих задач (Sleep Queue) — отсортирована по sleep_until
static task_t *sleep_queue_head = NULL;

// Ссылка на текущую исполняемую задачу
extern task_t *current_task;

void sched_init(task_t *initial_task) {
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    
    initial_task->state = TASK_STATE_RUNNABLE;
    initial_task->running = true;
    
    // Инициализируем ТОЛЬКО очереди планировщика!
    initial_task->sched_next = NULL;
    initial_task->sched_prev = NULL;
    
    run_queue_head = initial_task;
    run_queue_tail = initial_task;
    current_task = initial_task;
}

// Добавление в конец Run Queue за O(1)
void sched_enqueue(task_t *task) {
    if (!task) return;
    
    task->state = TASK_STATE_RUNNABLE;
    task->running = true;
    task->sched_next = NULL;
    task->sched_prev = run_queue_tail;
    
    if (run_queue_tail) {
        run_queue_tail->sched_next = task;
    } else {
        run_queue_head = task;
    }
    run_queue_tail = task;
}

// Удаление из любой позиции Run Queue за O(1)
void sched_dequeue(task_t *task) {
    if (!task) return;
    
    if (task->sched_prev) {
        task->sched_prev->sched_next = task->sched_next;
    } else {
        run_queue_head = task->sched_next;
    }
    
    if (task->sched_next) {
        task->sched_next->sched_prev = task->sched_prev;
    } else {
        run_queue_tail = task->sched_prev;
    }
    
    task->sched_next = NULL;
    task->sched_prev = NULL;
}

// Вставка в Sleep Queue по возрастанию времени пробуждения (O(N) при вставке, но O(1) при пробуждении)
void sched_make_sleep(task_t *task, uint64_t sleep_until) {
    if (!task) return;
    
    sched_dequeue(task); // Убираем из активных задач
    
    task->state = TASK_STATE_SLEEPING;
    task->running = false;
    task->sleep_until = sleep_until;
    task->sched_next = NULL;
    task->sched_prev = NULL;
    
    if (!sleep_queue_head) {
        sleep_queue_head = task;
        return;
    }
    
    // Вставка со связанной сортировкой
    task_t *curr = sleep_queue_head;
    task_t *prev_node = NULL;
    
    while (curr && curr->sleep_until <= sleep_until) {
        prev_node = curr;
        curr = curr->sched_next;
    }
    
    if (!prev_node) { // Вставка в самое начало
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

// Проверка Sleep Queue на каждом тике таймера — выполняется за O(1) в лучшем случае
void sched_timer_tick(uint32_t current_tick) {
    while (sleep_queue_head && current_tick >= sleep_queue_head->sleep_until) {
        task_t *task = sleep_queue_head;
        
        // Извлекаем из головы Sleep Queue
        sleep_queue_head = task->sched_next;
        if (sleep_queue_head) {
            sleep_queue_head->sched_prev = NULL;
        }
        
        task->sleep_until = 0;
        sched_enqueue(task); // Перемещаем в Run Queue
    }
}

// Алгоритм Round-Robin планирования за O(1)
uint64_t sched_switch(uint64_t current_rsp) {
    if (!current_task) return current_rsp;
    
    // Сохраняем указатель стека текущей задачи
    current_task->rsp = current_rsp;
    
    task_t *prev_task = current_task;
    
    // Если текущая задача все еще готова к выполнению, ротируем её в конец Run Queue
    if (current_task->state == TASK_STATE_RUNNABLE) {
        sched_dequeue(current_task);
        sched_enqueue(current_task);
    }
    
    // Если очередь пуста (все заблокированы), переключаемся на Idle/Init процесс ядра
    if (!run_queue_head) {
        // Защита: в системе всегда должен быть хотя бы один поток (Kernel Init)
        return current_rsp;
    }
    
    // Берём первую задачу из очереди готовых к исполнению
    current_task = run_queue_head;
    
    // DEBUG: print task switch info
    // printf("SCHED: Switching to Task ID %u (RSP: %x, CR3: %x)\n", 
    //         (unsigned int)current_task->id, current_task->rsp, 
    //         current_task->process ? current_task->process->cr3 : kernel_cr3);

    // Легковесное сохранение/восстановление FPU/SSE-контекста
    if (current_task != prev_task) {
        __asm__ volatile("fxsave64 (%0)"  :: "r"(task_fpu_area(prev_task))    : "memory");
        __asm__ volatile("fxrstor64 (%0)" :: "r"(task_fpu_area(current_task)) : "memory");
    }
    
    // Смена виртуального адресного пространства (CR3)
    uint64_t new_cr3 = (current_task->process && current_task->process->cr3 != 0) 
                       ? current_task->process->cr3 
                       : kernel_cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    
    gdt_set_tss_stack(current_task->kstack_at_bottom);
    
    // Обновление FS.base (Thread Local Storage) только при его наличии
    if (current_task->fs_base != 0) {
        wrmsr(IA32_FS_BASE_MSR, current_task->fs_base);
    }
    
    return current_task->rsp;
}

void sched_yield(void) {
    __asm__ volatile ("int $32");
}

// Wrapper called from assembly interrupt stub (interrupt.asm)
uint64_t schedule(uint64_t current_rsp) {
    return sched_switch(current_rsp);
}

// Temporary stubs for system calls until we implement Userland / POSIX syscalls
void syscall_handler(void *regs) {
    (void)regs;
}

void linux_syscall_handler(void *regs) {
    (void)regs;
}