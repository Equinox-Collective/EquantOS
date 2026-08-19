#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include "elf.h"
#include "task.h"

/**
 * @brief Parse and load an ELF64 binary into a new process address space, 
 *        then spawn it as a runnable task.
 * @param elf_data Pointer to raw ELF file in memory (loaded via Limine module).
 * @param size Size of the ELF file in bytes.
 * @return true on success, false on failure.
 */
bool elf_load(void *elf_data, uint64_t size);

extern task_t *last_spawned_task;

#endif // LOADER_H