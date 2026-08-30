// src/kernel/proc/pipe.c - Kernel Pipe (FIFO) Subsystem Implementation
//
// Архитектура:
//   pipe_t: кольцевой буфер 4KB в ядровой куче
//   read_node / write_node: два VFS-нода с кастомными ops
//   pipe_t хранится в node->ptr (у обоих нодов)
//
// Блокирующее чтение: если буфер пуст и write-end открыт —
//   задача блокируется (sched_block) и запоминается в p->blocked_reader.
//   Пишущий конец будит её через sched_unblock.
//

#include "pipe.h"
#include "task.h"
#include "sched.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "../fs/vfs.h"

// === Операции чтения из пайпа ===

static int64_t pipe_read_op(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return -9; // -EBADF

    pipe_t *p = (pipe_t *)node->ptr;

    // Блокирующее ожидание: ждём пока есть что читать или write-end закрыт
    while (p->count == 0) {
        if (p->write_closed) {
            return 0; // EOF — писатель закрылся, данных больше не будет
        }

        // Блокируемся: запоминаем себя как blocked_reader
        if (current_task) {
            p->blocked_reader = current_task;
            sched_block(current_task);
            sched_yield(); // Уступаем процессор
            p->blocked_reader = NULL;
        } else {
            // В ядровом контексте без задачи — спинлок
            __asm__ volatile("pause");
        }
    }

    // Читаем из кольцевого буфера
    uint64_t to_read = (size < (uint64_t)p->count) ? size : (uint64_t)p->count;
    for (uint64_t i = 0; i < to_read; i++) {
        buffer[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
    }

    return (int64_t)to_read;
}

// === Операции записи в пайп ===

static int64_t pipe_write_op(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return -9; // -EBADF

    pipe_t *p = (pipe_t *)node->ptr;

    // Если read-end закрыт — SIGPIPE/EPIPE
    if (p->read_closed) {
        return -32; // -EPIPE
    }

    // Пишем сколько влезает (без блокировки на переполнение для простоты)
    uint64_t written = 0;
    for (uint64_t i = 0; i < size; i++) {
        if (p->count >= PIPE_BUF_SIZE) break; // Буфер полон

        p->buf[p->write_pos] = buffer[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        p->count++;
        written++;
    }

    // Будим заблокированного читателя (если есть)
    if (p->blocked_reader) {
        sched_unblock(p->blocked_reader);
        p->blocked_reader = NULL;
    }

    return (int64_t)written;
}

// === Close-операции ===

static void pipe_read_close_op(vfs_node_t *node) {
    if (!node || !node->ptr) return;
    pipe_t *p = (pipe_t *)node->ptr;
    pipe_close_read(p);
}

static void pipe_write_close_op(vfs_node_t *node) {
    if (!node || !node->ptr) return;
    pipe_t *p = (pipe_t *)node->ptr;
    pipe_close_write(p);
}

// === VFS операции для read-end ===
static vfs_file_operations_t pipe_read_ops = {
    .read    = pipe_read_op,
    .write   = NULL,
    .open    = NULL,
    .close   = pipe_read_close_op,
    .readdir = NULL,
    .finddir = NULL,
    .create  = NULL,
};

// === VFS операции для write-end ===
static vfs_file_operations_t pipe_write_ops = {
    .read    = NULL,
    .write   = pipe_write_op,
    .open    = NULL,
    .close   = pipe_write_close_op,
    .readdir = NULL,
    .finddir = NULL,
    .create  = NULL,
};

// === Управление ref_count ===

void pipe_close_read(pipe_t *p) {
    if (!p) return;
    p->read_closed = true;
    p->ref_count--;

    // Будим заблокированного читателя (чтобы он увидел EOF)
    if (p->blocked_reader) {
        sched_unblock(p->blocked_reader);
        p->blocked_reader = NULL;
    }

    if (p->ref_count <= 0) {
        kfree(p);
    }
}

void pipe_close_write(pipe_t *p) {
    if (!p) return;
    p->write_closed = true;
    p->ref_count--;

    // Будим читателя — он увидит write_closed и вернёт EOF
    if (p->blocked_reader) {
        sched_unblock(p->blocked_reader);
        p->blocked_reader = NULL;
    }

    if (p->ref_count <= 0) {
        kfree(p);
    }
}

// === Создание пайпа ===

// Вспомогательная функция: выделяем FD начиная с fd_start (включая 0,1,2 для inherit)
static int alloc_fd_from(vfs_node_t *node, int fd_start) {
    if (!current_task || !current_task->process) return -24; // -EMFILE
    for (int i = fd_start; i < MAX_OPEN_FILES; i++) {
        if (current_task->process->files[i] == NULL) {
            current_task->process->files[i] = node;
            return i;
        }
    }
    return -24; // -EMFILE
}

int pipe_create(int pipefd[2]) {
    if (!pipefd) return -22; // -EINVAL

    // Выделяем буфер пайпа
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -12; // -ENOMEM

    memset(p, 0, sizeof(pipe_t));
    p->ref_count   = 2;   // read-end + write-end
    p->write_closed = false;
    p->read_closed  = false;
    p->blocked_reader = NULL;

    // Создаём read-end VFS нод
    vfs_node_t *read_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!read_node) { kfree(p); return -12; }
    memset(read_node, 0, sizeof(vfs_node_t));
    memcpy(read_node->name, "pipe:r", 7);
    read_node->flags = FS_FILE;
    read_node->ops   = &pipe_read_ops;
    read_node->ptr   = (vfs_node_t *)p; // Храним pipe_t в ptr

    // Создаём write-end VFS нод
    vfs_node_t *write_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!write_node) { kfree(read_node); kfree(p); return -12; }
    memset(write_node, 0, sizeof(vfs_node_t));
    memcpy(write_node->name, "pipe:w", 7);
    write_node->flags = FS_FILE;
    write_node->ops   = &pipe_write_ops;
    write_node->ptr   = (vfs_node_t *)p;

    // Выделяем FD (начиная с 3, stdin/stdout/stderr уже заняты)
    int rfd = alloc_fd_from(read_node, 3);
    if (rfd < 0) {
        kfree(read_node);
        kfree(write_node);
        kfree(p);
        return rfd;
    }

    int wfd = alloc_fd_from(write_node, 3);
    if (wfd < 0) {
        current_task->process->files[rfd] = NULL;
        kfree(read_node);
        kfree(write_node);
        kfree(p);
        return wfd;
    }

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}
