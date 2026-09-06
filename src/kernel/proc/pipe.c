// src/kernel/proc/pipe.c - Production-Grade POSIX Blocking FIFO Subsystem
#include "pipe.h"
#include "task.h"
#include "sched.h"
#include "syscall.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "../fs/vfs.h"

static int64_t pipe_read_op(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return -EBADF;

    pipe_t *p = (pipe_t *)node->ptr;

    // Wait until there is data to read or the write end has been completely closed
    while (p->count == 0) {
        if (p->write_closed) {
            return 0; // EOF (End of File)
        }
        if (current_task) {
            p->blocked_reader = current_task;
            sched_block(current_task);
            sched_yield();
            p->blocked_reader = NULL;
        } else {
            __asm__ volatile("pause");
        }
    }

    uint64_t to_read = (size < (uint64_t)p->count) ? size : (uint64_t)p->count;
    for (uint64_t i = 0; i < to_read; i++) {
        buffer[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
    }

    // Wake up blocked writer if space is now available in buffer
    if (p->blocked_writer) {
        sched_unblock(p->blocked_writer);
        p->blocked_writer = NULL;
    }

    return (int64_t)to_read;
}

static int64_t pipe_write_op(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return -EBADF;

    pipe_t *p = (pipe_t *)node->ptr;

    if (p->read_closed) {
        return -EPIPE; // Broken Pipe
    }

    uint64_t total_written = 0;

    while (total_written < size) {
        // If pipe buffer is full, block the writing task until reader drains bytes
        while (p->count >= PIPE_BUF_SIZE) {
            if (p->read_closed) {
                return total_written > 0 ? (int64_t)total_written : -EPIPE;
            }
            if (current_task) {
                p->blocked_writer = current_task;
                sched_block(current_task);
                sched_yield();
                p->blocked_writer = NULL;
            } else {
                __asm__ volatile("pause");
            }
        }

        p->buf[p->write_pos] = buffer[total_written++];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        p->count++;

        // Wake up blocked reader immediately on newly available bytes
        if (p->blocked_reader) {
            sched_unblock(p->blocked_reader);
            p->blocked_reader = NULL;
        }
    }

    return (int64_t)total_written;
}

static void pipe_read_close_op(vfs_node_t *node) {
    if (!node || !node->ptr) return;
    pipe_t *p = (pipe_t *)node->ptr;
    p->read_closed = true;
    p->ref_count--;

    if (p->blocked_writer) {
        sched_unblock(p->blocked_writer);
        p->blocked_writer = NULL;
    }
    if (p->ref_count <= 0) {
        kfree(p);
    }
}

static void pipe_write_close_op(vfs_node_t *node) {
    if (!node || !node->ptr) return;
    pipe_t *p = (pipe_t *)node->ptr;
    p->write_closed = true;
    p->ref_count--;

    if (p->blocked_reader) {
        sched_unblock(p->blocked_reader);
        p->blocked_reader = NULL;
    }
    if (p->ref_count <= 0) {
        kfree(p);
    }
}

static vfs_file_operations_t pipe_read_ops = {
    .read = pipe_read_op,
    .write = NULL,
    .open = NULL,
    .close = pipe_read_close_op,
    .readdir = NULL,
    .finddir = NULL,
    .create = NULL,
    .ioctl = NULL,
    .mmap = NULL
};

static vfs_file_operations_t pipe_write_ops = {
    .read = NULL,
    .write = pipe_write_op,
    .open = NULL,
    .close = pipe_write_close_op,
    .readdir = NULL,
    .finddir = NULL,
    .create = NULL,
    .ioctl = NULL,
    .mmap = NULL
};

int pipe_create(int pipefd[2]) {
    if (!pipefd || !current_task || !current_task->process) return -EINVAL;

    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;

    memset(p, 0, sizeof(pipe_t));
    p->ref_count = 2;
    p->blocked_reader = NULL;
    p->blocked_writer = NULL;

    vfs_node_t *r_node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    vfs_node_t *w_node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!r_node || !w_node) {
        if (r_node) kfree(r_node);
        if (w_node) kfree(w_node);
        kfree(p);
        return -ENOMEM;
    }

    strcpy(r_node->name, "pipe:r");
    r_node->flags = FS_FILE;
    r_node->ops = &pipe_read_ops;
    r_node->ptr = (struct vfs_node *)p;

    strcpy(w_node->name, "pipe:w");
    w_node->flags = FS_FILE;
    w_node->ops = &pipe_write_ops;
    w_node->ptr = (struct vfs_node *)p;

    int rfd = -1, wfd = -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!current_task->process->files[i]) {
            if (rfd == -1) rfd = i;
            else if (wfd == -1) { wfd = i; break; }
        }
    }

    if (rfd == -1 || wfd == -1) {
        kfree(r_node); kfree(w_node); kfree(p);
        return -EMFILE;
    }

    current_task->process->files[rfd] = r_node;
    current_task->process->file_offsets[rfd] = 0;
    current_task->process->file_flags[rfd] = O_RDONLY;

    current_task->process->files[wfd] = w_node;
    current_task->process->file_offsets[wfd] = 0;
    current_task->process->file_flags[wfd] = O_WRONLY;

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}