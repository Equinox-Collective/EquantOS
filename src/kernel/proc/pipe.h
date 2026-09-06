// src/kernel/proc/pipe.h - Kernel Pipe (FIFO) Subsystem
#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../fs/vfs.h"

#define PIPE_BUF_SIZE 4096

typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;

    bool write_closed;
    bool read_closed;
    int ref_count;

    struct task *blocked_reader;
    struct task *blocked_writer;
} pipe_t;

int pipe_create(int pipefd[2]);
void pipe_close_read(pipe_t *p);
void pipe_close_write(pipe_t *p);

#endif // PIPE_H