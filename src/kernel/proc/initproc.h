// src/kernel/proc/initproc.h
#ifndef INITPROC_H
#define INITPROC_H

#include <stdbool.h>

/**
 * @brief Attempts to launch GNU Bash or BusyBox as the primary interactive shell.
 * Falls back to the diagnostic kernel shell if binary is missing.
 */
void kernel_start_userland(void);

#endif // INITPROC_H