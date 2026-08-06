#ifndef SCHED_H
#define SCHED_H

#include "task.h"

// Инициализация очередей планировщика
void sched_init(task_t *initial_task);

// Добавить задачу в Run Queue (очередь готовых к исполнению)
void sched_enqueue(task_t *task);

// Удалить задачу из Run Queue (при переходе в режим блокировки или сна)
void sched_dequeue(task_t *task);

// Переместить задачу в очередь спящих (Sleep Queue) до тика sleep_ticks
void sched_make_sleep(task_t *task, uint64_t sleep_until);

// Проверка спящих задач (вызывается из timer_callback() в timer.c на каждом тике)
void sched_timer_tick(uint32_t current_tick);

// Выбор следующей задачи и переключение контекста (замена старого schedule)
uint64_t sched_switch(uint64_t current_rsp);

// Принудительный yield
void sched_yield(void);

#endif /* SCHED_H */