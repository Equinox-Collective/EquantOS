// shell.c - Interactive Shell with VFS, RAMFS and Memory diagnostics
#include "shell.h"
#include "term.h"
#include "string.h"
#include "timer.h"
#include "../fs/vfs.h"
#include "../mm/pmm.h"
#include "../mm/memory.h"
#include <stdint.h>
#include <stddef.h>

extern size_t used_memory;
extern uint64_t free_memory;

#define SHELL_PROMPT_COLOR 0x0000FF00 // Green
#define MAX_ARGS 8
#define LINE_BUF_SIZE 256

typedef void (*shell_handler_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *description;
    shell_handler_t handler;
} shell_command_t;

static void cmd_help(int argc, char **argv);
static void cmd_clear(int argc, char **argv);
static void cmd_echo(int argc, char **argv);
static void cmd_uptime(int argc, char **argv);
static void cmd_eqfetch(int argc, char **argv);
static void cmd_ver(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_mem(int argc, char **argv);

// Registered shell builtins
static const shell_command_t commands[] = {
    { "help",    "List available commands",         cmd_help },
    { "clear",   "Clear the terminal screen",       cmd_clear },
    { "echo",    "Print text back to the terminal", cmd_echo },
    { "uptime",  "Show time since boot",            cmd_uptime },
    { "eqfetch", "Show the system info banner",     cmd_eqfetch },
    { "ver",     "Show OS version",                 cmd_ver },
    { "ls",      "List files in VFS root directory", cmd_ls },
    { "cat",     "Display file contents",           cmd_cat },
    { "mem",     "Show RAM and heap usage stats",   cmd_mem },
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *token = strtok(line, " ");
    while (token && argc < max_args) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    return argc;
}

static void print_prompt(void) {
    uint32_t prev = term_get_color();
    term_set_color(SHELL_PROMPT_COLOR);
    term_print("EquantOS> ");
    term_set_color(prev);
}

void shell_init(void) {
    print_prompt();
}

void shell_execute(const char *cmd_line_in) {
    char line[LINE_BUF_SIZE];
    size_t len = strlen(cmd_line_in);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    for (size_t i = 0; i < len; i++) line[i] = cmd_line_in[i];
    line[len] = '\0';

    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv, MAX_ARGS);

    if (argc > 0) {
        int found = 0;
        for (size_t i = 0; i < NUM_COMMANDS; i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].handler(argc, argv);
                found = 1;
                break;
            }
        }
        if (!found) {
            term_print("Command not found: ");
            term_print(argv[0]);
            term_print("\n");
        }
    }

    print_prompt();
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Available commands:\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        term_print("  ");
        term_print(commands[i].name);
        term_print(" - ");
        term_print(commands[i].description);
        term_print("\n");
    }
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    term_clear();
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        term_print(argv[i]);
        if (i + 1 < argc) term_print(" ");
    }
    term_print("\n");
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t seconds = tick / 100; // PIT frequency is 100 Hz

    char num[24];
    itoa((int64_t)seconds, 10, num);

    term_print("System uptime: ");
    term_print(num);
    term_print("s\n");
}

static void cmd_eqfetch(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("    _/_/_/_/   | Equant OS\n");
    term_print("   _/          |  \n");
    term_print("  _/_/_/       | v0.0.1 Alpha\n");
    term_print(" _/            | VFS & RAMFS Active\n");
    term_print("_/_/_/_/       | Multi-tasking Ring 3\n");
}

static void cmd_ver(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Equant OS v0.0.1 Alpha (x86_64)\n");
}

// VFS Command: List files in root directory
static void cmd_ls(int argc, char **argv) {
    (void)argc; (void)argv;
    vfs_node_t *dir = vfs_open("/", 0);
    if (!dir) {
        term_print("Error: cannot open root directory\n");
        return;
    }
    term_print("Directory /:\n");
    uint32_t index = 0;
    vfs_node_t *child;
    while ((child = vfs_readdir(dir, index++)) != NULL) {
        term_print("  ");
        if (child->flags & FS_DIRECTORY) {
            term_print("[DIR]  ");
        } else {
            term_print("[FILE] ");
        }
        term_print(child->name);
        
        char size_buf[32];
        term_print(" (size: ");
        itoa((int64_t)child->length, 10, size_buf);
        term_print(size_buf);
        term_print(" bytes)\n");
    }
}

// VFS Command: Display file contents
static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: cat <filename>\n");
        return;
    }

    // Prepend leading slash if omitted
    char path[128];
    if (argv[1][0] != '/') {
        path[0] = '/';
        strcpy(path + 1, argv[1]);
    } else {
        strcpy(path, argv[1]);
    }

    vfs_node_t *file = vfs_open(path, 0);
    if (!file) {
        term_print("File not found: ");
        term_print(path);
        term_print("\n");
        return;
    }

    if (file->flags & FS_DIRECTORY) {
        term_print("Error: '");
        term_print(path);
        term_print("' is a directory\n");
        return;
    }

    uint8_t buf[256];
    uint64_t offset = 0;
    int64_t bytes_read;
    while ((bytes_read = vfs_read(file, offset, sizeof(buf) - 1, buf)) > 0) {
        buf[bytes_read] = '\0';
        term_print((char *)buf);
        offset += bytes_read;
    }
    term_print("\n");
}

// Memory Command: Show PMM and Heap stats
static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t total_bytes = pmm_get_total_memory();
    uint64_t free_bytes = free_memory;
    uint64_t used_bytes = pmm_get_used_memory();

    char buf[32];
    term_print("=== EquantOS Memory Statistics ===\n");
    
    term_print("Total RAM    : "); 
    itoa((int64_t)(total_bytes / (1024 * 1024)), 10, buf); 
    term_print(buf); term_print(" MB\n");

    term_print("Free RAM     : "); 
    itoa((int64_t)(free_bytes / (1024 * 1024)), 10, buf); 
    term_print(buf); term_print(" MB\n");

    term_print("Used RAM     : "); 
    itoa((int64_t)(used_bytes / (1024 * 1024)), 10, buf); 
    term_print(buf); term_print(" MB\n");

    term_print("Kernel Heap  : "); 
    itoa((int64_t)(used_memory / 1024), 10, buf); 
    term_print(buf); term_print(" KB used\n");
}