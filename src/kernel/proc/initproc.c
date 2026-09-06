// src/kernel/proc/initproc.c - System Userland Bootstrapper & Ring 3 Manager
#include "initproc.h"
#include "../proc/loader.h"
#include "sched.h"
#include "../fs/vfs.h"
#include "../core/mem/memory.h"
#include "../drivers/serial/serial.h"
#include "../../equterm/term.h"
#include "../../equterm/shell.h"

// Candidate paths to search for an interactive shell binary
static const char *shell_candidates[] = {
    "/bin/bash",
    "/bash.elf",
    "/sys/bin/bash.elf",
    "/bin/sh",
    "/busybox.elf",
    "/sys/bin/busybox.elf",
    NULL
};

void kernel_start_userland(void) {
    vfs_node_t *shell_file = NULL;
    const char *selected_path = NULL;

    // Search for available shell binary
    for (int i = 0; shell_candidates[i] != NULL; i++) {
        shell_file = vfs_open(shell_candidates[i], 0);
        if (shell_file && !(shell_file->flags & FS_DIRECTORY) && shell_file->length > 0) {
            selected_path = shell_candidates[i];
            break;
        }
    }

    if (!shell_file) {
        serial_puts(COM1, "[INIT] No userland shell found on VFS. Falling back to Kernel Rescue Shell.\n");
        term_set_color(0x00FFFF55); // Yellow warning
        term_print("[SYSTEM] No userland shell found in /bin/bash or /bash.elf.\n");
        term_print("[SYSTEM] Dropping to EquantOS Diagnostic Rescue Shell...\n\n");
        term_set_color(0x00FFFFFF);
        shell_init();
        return;
    }

    uint8_t *elf_buffer = (uint8_t *)kmalloc(shell_file->length);
    if (!elf_buffer) {
        serial_puts(COM1, "[INIT ERROR] Out of memory reading shell ELF.\n");
        shell_init();
        return;
    }

    int64_t read_bytes = vfs_read(shell_file, 0, shell_file->length, elf_buffer);
    if (read_bytes <= 0) {
        serial_puts(COM1, "[INIT ERROR] Failed to read shell binary data.\n");
        kfree(elf_buffer);
        shell_init();
        return;
    }

    // Configure argv for Bash
    char *argv[] = {
        (char *)selected_path,
        "-i",
        NULL
    };
    int argc = 2; 

    last_spawned_task = NULL;

    bool success = elf_load_args(elf_buffer, shell_file->length, argc, argv);
    kfree(elf_buffer);

    if (!success) {
        serial_puts(COM1, "[INIT ERROR] ELF loader failed to parse shell executable!\n");
        term_print("[INIT ERROR] Shell binary load failed. Launching Rescue Shell...\n");
        shell_init();
        return;
    }

    // Monitor shell lifecycle: if user types 'exit' or Bash faults, drop to Rescue Shell
    while (last_spawned_task && last_spawned_task->state != TASK_STATE_ZOMBIE) {
        current_task->state = TASK_STATE_BLOCKED;
        sched_dequeue(current_task);
        sched_yield();
    }

    term_set_color(0x00FF5555);
    term_print("\n[INIT] Interactive shell process terminated. Dropping to Rescue Shell.\n");
    term_set_color(0x00FFFFFF);
    shell_init();
}