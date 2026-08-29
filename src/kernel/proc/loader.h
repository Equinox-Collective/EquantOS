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

extern task_t *last_spawned_task;

#endif // LOADER_H