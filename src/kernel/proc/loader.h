#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include "elf.h"
#include "task.h"

/**
 * @brief Parse and load an ELF64 binary with dynamic CLI arguments (argc, argv, envp).
 */
bool elf_load_args(void *elf_data, uint64_t size, int argc, char **argv);
bool elf_load(void *elf_data, uint64_t size);
bool elf_execve_replace(void *elf_data, uint64_t size, int argc, char **argv, uint64_t *out_entry, uint64_t *out_rsp, uint64_t *out_cr3);
extern task_t *last_spawned_task;

#endif // LOADER_H