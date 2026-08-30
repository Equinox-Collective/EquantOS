// src/kernel/proc/pipe.h - Kernel Pipe (FIFO) Subsystem
#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../fs/vfs.h"

#define PIPE_BUF_SIZE 4096

// Внутренний буфер пайпа (разделяется между read-end и write-end)
typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;      // Индекс для чтения
    uint32_t write_pos;     // Индекс для записи
    uint32_t count;         // Количество байт в буфере

    bool write_closed;      // write-end закрыт?
    bool read_closed;       // read-end закрыт?

    int ref_count;          // Сколько FD ссылаются (2 при создании)

    // Задача, заблокированная на чтении (ждёт данных)
    struct task *blocked_reader;
} pipe_t;

// Создаёт пайп, возвращает 0 при успехе.
// pipefd[0] = read-end FD, pipefd[1] = write-end FD
int pipe_create(int pipefd[2]);

// Вызывается из sys_close_handler для pipe-нодов
void pipe_close_read(pipe_t *p);
void pipe_close_write(pipe_t *p);

#endif // PIPE_H
