// shell.c - Ultimate EquantOS Diagnostic & Kernel Stress-Test Suite
#include "shell.h"
#include "term.h"
#include "string.h"
#include "../kernel/misc/timer.h"
#include "../kernel/fs/vfs.h"
#include "../kernel/fs/ramfs.h"
#include "../kernel/fs/mbr.h"
#include "../kernel/core/mem/pmm.h"
#include "../kernel/core/mem/memory.h"
#include "../kernel/drivers/pci/pci.h"
#include "../kernel/misc/power.h"
#include "../kernel/proc/loader.h"
#include "../kernel/proc/sched.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern uint64_t free_memory;
extern uint64_t total_pages;
extern size_t used_memory;

#define SHELL_PROMPT_COLOR 0x0000FF00
#define MAX_ARGS 8
#define LINE_BUF_SIZE 256
extern bool elf_load(void *elf_data, uint64_t size);

typedef void (*shell_handler_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *description;
    shell_handler_t handler;
} shell_command_t;

static char current_dir[256] = "/";

static void cmd_help(int argc, char **argv);
static void cmd_clear(int argc, char **argv);
static void cmd_echo(int argc, char **argv);
static void cmd_uptime(int argc, char **argv);
static void cmd_eqfetch(int argc, char **argv);
static void cmd_ver(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_mem(int argc, char **argv);
static void cmd_memstress(int argc, char **argv);
static void cmd_heapdump(int argc, char **argv);
static void cmd_diskinfo(int argc, char **argv);
static void cmd_hexdump(int argc, char **argv);
static void cmd_writefile(int argc, char **argv);
static void cmd_pciscan(int argc, char **argv);
static void cmd_sysinfo(int argc, char **argv);
static void cmd_panic_test(int argc, char **argv);
static void cmd_reboot(int argc, char **argv);
static void cmd_shutdown(int argc, char **argv);
static void resolve_path(const char *input, char *output, size_t max_len);
static void cmd_pwd(int argc, char **argv);
static void cmd_cd(int argc, char **argv);
static void cmd_run(int argc, char **argv);
static void cmd_cp(int argc, char **argv);

static const shell_command_t commands[] = {
    { "help",       "List all diagnostic & stress commands", cmd_help },
    { "clear",      "Clear the terminal screen",             cmd_clear },
    { "echo",       "Print text back to terminal",           cmd_echo },
    { "uptime",     "Show system uptime since boot",         cmd_uptime },
    { "eqfetch",    "Show EquantOS system banner",           cmd_eqfetch },
    { "ver",        "Show OS version details",               cmd_ver },
    { "ls",         "List files in VFS root directory",      cmd_ls },
    { "cat",        "Read and display file contents",        cmd_cat },
    { "mem",        "Show RAM and heap usage overview",      cmd_mem },
    { "memstress",  "Torture test kernel heap allocation",   cmd_memstress },
    { "heapdump",   "Dump internal kernel heap block map",   cmd_heapdump },
    { "diskinfo",   "Show ATA drive info and MBR partitions",cmd_diskinfo },
    { "hexdump",    "Dump raw file contents in Hex format",  cmd_hexdump },
    { "writefile",  "Create/write text file to RAMFS/FAT32", cmd_writefile },
    { "pciscan",    "Restore and print all PCI devices",      cmd_pciscan },
    { "sysinfo",    "Show detailed memory metrics struct",   cmd_sysinfo },
    { "panic_test", "Trigger Ring 0 #UD exception (panic)",  cmd_panic_test },
    { "reboot",   "Reboot the system hardware",              cmd_reboot },
    { "shutdown", "Power off the system hardware",           cmd_shutdown },
    { "pwd",    "Print current working directory",           cmd_pwd },
    { "cd",     "Change working directory",                  cmd_cd },
    { "run",    "Load and execute an ELF binary",            cmd_run },
    { "cp",     "Copy source file to destination",           cmd_cp },
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
            term_print("Unknown command: ");
            term_print(argv[0]);
            term_print(". Type 'help' for available commands.\n");
        }
    }

    print_prompt();
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== EquantOS Diagnostic & Stress Suite ===\n");
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
    uint32_t seconds = tick / 100;
    char num[24];
    itoa((int64_t)seconds, 10, num);
    term_print("System Uptime: ");
    term_print(num);
    term_print(" seconds\n");
}

static void cmd_eqfetch(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("    _/_/_/_/   | Equant OS Kernel\n");
    term_print("   _/          | Architecture: x86_64\n");
    term_print("  _/_/_/       | Version: 0.0.1 Alpha\n");
    term_print(" _/            | Subsystems: VFS, RAMFS, PMM, VMM\n");
    term_print("_/_/_/_/       | \n");
}

static void cmd_ver(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("EquantOS Kernel v0.0.1 (Build Target: x86_64-elf)\n");
}

static void cmd_ls(int argc, char **argv) {
    (void)argc; (void)argv;
    
    char resolved[256];
    resolve_path("", resolved, sizeof(resolved));

    vfs_node_t *dir = vfs_open(resolved, 0);
    if (!dir) {
        term_print("ls: cannot open directory: ");
        term_print(resolved);
        term_print("\n");
        return;
    }
    
    term_print("Listing directory ");
    term_print(resolved);
    term_print(":\n");

    uint32_t index = 0;
    vfs_node_t *child;
    while ((child = vfs_readdir(dir, index++)) != NULL) {
        term_print("  [");
        term_print((child->flags & FS_DIRECTORY) ? "DIR" : "FILE");
        term_print("] ");
        term_print(child->name);
        char sz[32];
        term_print(" (size: ");
        itoa((int64_t)child->length, 10, sz);
        term_print(sz);
        term_print(" bytes)\n");
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: cat <filename>\n");
        return;
    }
    
    char resolved[256];
    resolve_path(argv[1], resolved, sizeof(resolved));

    vfs_node_t *file = vfs_open(resolved, 0);
    if (!file) {
        term_print("cat: file not found: ");
        term_print(resolved);
        term_print("\n");
        return;
    }

    if (file->flags & FS_DIRECTORY) {
        term_print("cat: is a directory: ");
        term_print(resolved);
        term_print("\n");
        return;
    }

    uint8_t *buf = (uint8_t *)kmalloc(file->length + 1);
    if (!buf) {
        term_print("cat: out of memory\n");
        return;
    }

    int64_t bytes_read = vfs_read(file, 0, file->length, buf);
    if (bytes_read > 0) {
        buf[bytes_read] = '\0';
        term_print((char *)buf);
        term_print("\n");
    }

    kfree(buf);
}

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t total = pmm_get_total_memory();
    uint64_t free = free_memory;
    uint64_t used = pmm_get_used_memory();

    char buf[32];
    term_print("=== RAM & Heap Overview ===\n");
    term_print("Total RAM   : "); itoa((int64_t)(total / (1024 * 1024)), 10, buf); term_print(buf); term_print(" MB\n");
    term_print("Free RAM    : "); itoa((int64_t)(free / (1024 * 1024)), 10, buf); term_print(buf); term_print(" MB\n");
    term_print("Used RAM    : "); itoa((int64_t)(used / (1024 * 1024)), 10, buf); term_print(buf); term_print(" MB\n");
    term_print("Kernel Heap : "); itoa((int64_t)(used_memory / 1024), 10, buf); term_print(buf); term_print(" KB allocated\n");
}

static void cmd_memstress(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[STRESS] Starting kernel heap allocation torture test...\n");
    
    void *ptrs[64];
    int success_count = 0;

    for (int i = 0; i < 64; i++) {
        ptrs[i] = kmalloc(512);
        if (ptrs[i]) {
            memset(ptrs[i], 0xAA, 512);
            success_count++;
        }
    }

    char buf[16];
    term_print("[STRESS] Successfully allocated and wrote ");
    itoa(success_count, 10, buf);
    term_print(buf);
    term_print(" blocks of 512 bytes.\n");

    term_print("[STRESS] Freeing allocated memory blocks...\n");
    for (int i = 0; i < 64; i++) {
        if (ptrs[i]) {
            kfree(ptrs[i]);
        }
    }
    term_print("[STRESS] Heap stress test completed successfully without faults!\n");
}

static void cmd_heapdump(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[DEBUG] Dumping kernel heap block structure to serial console...\n");
    kheap_dump();
    term_print("[DEBUG] Heap dump finished. Check serial log.\n");
}

static void cmd_diskinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== ATA Drive & MBR Partition Info ===\n");
    int p_count = mbr_get_partition_count();
    char buf[16];
    term_print("Detected Partitions: ");
    itoa(p_count, 10, buf);
    term_print(buf);
    term_print("\n");

    for (int i = 0; i < p_count; i++) {
        partition_info_t *p = mbr_get_partition(i);
        if (p) {
            term_print("  Partition #");
            itoa(p->index, 10, buf);
            term_print(buf);
            term_print(" | Type: 0x");
            itoa_hex(p->type, buf);
            term_print(buf);
            term_print(" | Start LBA: ");
            itoa(p->start_lba, 10, buf);
            term_print(buf);
            term_print(" | Sectors: ");
            itoa(p->sector_count, 10, buf);
            term_print(buf);
            term_print("\n");
        }
    }
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: hexdump <filename>\n");
        return;
    }
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

    uint8_t buf[16];
    uint64_t offset = 0;
    int64_t bytes;
    term_print("Hexdump of ");
    term_print(path);
    term_print(":\n");

    while ((bytes = vfs_read(file, offset, sizeof(buf), buf)) > 0) {
        char hex[3];
        char addr_buf[16];
        itoa_hex(offset, addr_buf);
        term_print(addr_buf);
        term_print(": ");

        for (int i = 0; i < bytes; i++) {
            itoa_hex(buf[i], hex);
            if (buf[i] < 16) term_print("0");
            term_print(hex);
            term_print(" ");
        }
        term_print("\n");
        offset += bytes;
    }
}

static void cmd_writefile(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: writefile <filename> [text]\n");
        return;
    }

    char resolved[256];
    resolve_path(argv[1], resolved, sizeof(resolved));

    // FIX: Safely separate parent directory and filename without producing empty strings
    char parent_path[256];
    strcpy(parent_path, resolved);
    char *filename = parent_path;

    char *last_slash = strrchr(parent_path, '/');
    if (last_slash) {
        if (last_slash == parent_path) {
            // Path is in root, e.g. "/hello.txt"
            filename = last_slash + 1;
            parent_path[1] = '\0'; // parent_path becomes "/"
        } else {
            *last_slash = '\0';
            filename = last_slash + 1;
        }
    }

    vfs_node_t *dir = vfs_open(parent_path[0] == '\0' ? "/" : parent_path, 0);
    if (!dir || !(dir->flags & FS_DIRECTORY)) {
        term_print("writefile: invalid directory: ");
        term_print(parent_path);
        term_print("\n");
        return;
    }

    char text_buf[512] = {0};
    size_t pos = 0;
    for (int i = 2; i < argc; i++) {
        size_t arg_len = strlen(argv[i]);
        if (pos + arg_len >= sizeof(text_buf) - 1) break;
        if (i > 2) {
            text_buf[pos++] = ' ';
        }
        strcpy(text_buf + pos, argv[i]);
        pos += arg_len;
    }

    vfs_node_t *new_file = vfs_create(dir, filename, 0);
    if (!new_file) {
        new_file = vfs_open(resolved, 0);
    }

    if (!new_file) {
        term_print("writefile: failed to create file (filesystem may be read-only or full)\n");
        return;
    }

    int64_t written = vfs_write(new_file, 0, strlen(text_buf), (uint8_t *)text_buf);
    if (written >= 0) {
        term_print("File written successfully: ");
        term_print(resolved);
        term_print("\n");
    } else {
        term_print("writefile: write failed\n");
    }
}

static void cmd_pciscan(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[PCI] Re-scanning PCI bus topology...\n");
    pci_init();
    term_print("[PCI] Scan complete.\n");
}

static void cmd_sysinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== System Metrics Summary ===\n");
    term_print("PMM Total Pages : ");
    char buf[32];
    itoa(total_pages, 10, buf);
    term_print(buf);
    term_print("\nPMM Used Pages  : ");
    itoa(pmm_get_used_memory() / 4096, 10, buf);
    term_print(buf);
    term_print("\nKernel Heap Used: ");
    itoa(used_memory, 10, buf);
    term_print(buf);
    term_print(" bytes\n");
}

static void cmd_panic_test(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("CRITICAL: Triggering deliberate Ring 0 invalid opcode (ud2)...\n");
    __asm__ volatile("ud2");
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Initiating system reboot...\n");
    system_reboot();
}

static void cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Initiating system shutdown...\n");
    system_power_off();
}

static void resolve_path(const char *input, char *output, size_t max_len) {
    if (!input || input[0] == '\0') {
        strcpy(output, current_dir);
        return;
    }

    if (input[0] == '/') {
        strncpy(output, input, max_len);
    } else {
        strcpy(output, current_dir);
        if (output[strlen(output) - 1] != '/') {
            strcat(output, "/");
        }
        strcat(output, input);
    }
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print(current_dir);
    term_print("\n");
}

static void path_go_up(char *path) {
    int len = strlen(path);
    if (len <= 1) {
        strcpy(path, "/");
        return;
    }
    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
    for (int i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            if (i == 0) {
                path[1] = '\0';
            } else {
                path[i] = '\0';
            }
            break;
        }
    }
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        strcpy(current_dir, "/");
        return;
    }

    if (strcmp(argv[1], "..") == 0) {
        path_go_up(current_dir);
        return;
    }

    char resolved[256];
    resolve_path(argv[1], resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) {
        term_print("cd: no such file or directory: ");
        term_print(argv[1]);
        term_print("\n");
        return;
    }

    if (!(node->flags & FS_DIRECTORY)) {
        term_print("cd: not a directory: ");
        term_print(argv[1]);
        term_print("\n");
        return;
    }

    strcpy(current_dir, resolved);
}

static void cmd_run(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: run <executable>\n");
        return;
    }

    char resolved[256];
    resolve_path(argv[1], resolved, sizeof(resolved));

    vfs_node_t *file = vfs_open(resolved, 0);

    // Fallback: Check System Binary PATH (/sys/bin/)
    if (!file) {
        char path_buf[256];
        strcpy(path_buf, "/sys/bin/");
        strcat(path_buf, argv[1]);
        file = vfs_open(path_buf, 0);
        if (file) {
            strcpy(resolved, path_buf);
        }
    }

    if (!file) {
        term_print("run: executable not found in current directory or /sys/bin/: ");
        term_print(argv[1]);
        term_print("\n");
        return;
    }

    if (file->flags & FS_DIRECTORY) {
        term_print("run: path is a directory: ");
        term_print(resolved);
        term_print("\n");
        return;
    }

    uint8_t *elf_buf = (uint8_t *)kmalloc(file->length);
    if (!elf_buf) {
        term_print("run: out of memory for loading binary\n");
        return;
    }

    int64_t read_bytes = vfs_read(file, 0, file->length, elf_buf);
    if (read_bytes <= 0) {
        term_print("run: failed to read ELF file data\n");
        kfree(elf_buf);
        return;
    }

    term_print("Spawning process: ");
    term_print(resolved);
    term_print("\n");

    last_spawned_task = NULL;

    if (elf_load(elf_buf, file->length)) {
        // СИНХРОННОЕ ОЖИДАНИЕ: Шелл передает кванты времени и ждет, пока процесс станет ZOMBIE
        while (last_spawned_task && last_spawned_task->state != TASK_STATE_ZOMBIE) {
            sched_yield();
        }

        term_print("\nProcess execution finished.\n");
    } else {
        term_print("run: ELF load failed\n");
    }

    kfree(elf_buf);
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) {
        term_print("Usage: cp <source> <destination>\n");
        return;
    }

    char src_path[256], dst_path[256];
    resolve_path(argv[1], src_path, sizeof(src_path));
    resolve_path(argv[2], dst_path, sizeof(dst_path));

    vfs_node_t *src_file = vfs_open(src_path, 0);
    if (!src_file) {
        term_print("cp: source file not found: ");
        term_print(src_path);
        term_print("\n");
        return;
    }

    if (src_file->flags & FS_DIRECTORY) {
        term_print("cp: cannot copy directories\n");
        return;
    }

    char final_dst[256];
    vfs_node_t *dst_node = vfs_open(dst_path, 0);
    if (dst_node && (dst_node->flags & FS_DIRECTORY)) {
        strcpy(final_dst, dst_path);
        if (final_dst[strlen(final_dst) - 1] != '/') {
            strcat(final_dst, "/");
        }
        const char *src_name = src_path;
        for (int i = strlen(src_path) - 1; i >= 0; i--) {
            if (src_path[i] == '/') {
                src_name = &src_path[i + 1];
                break;
            }
        }
        strcat(final_dst, src_name);
    } else {
        strcpy(final_dst, dst_path);
    }

    char parent_path[256];
    strcpy(parent_path, final_dst);
    char *filename = parent_path;
    char *last_slash = strrchr(parent_path, '/');
    if (last_slash) {
        if (last_slash == parent_path) {
            filename = last_slash + 1;
            parent_path[1] = '\0';
        } else {
            *last_slash = '\0';
            filename = last_slash + 1;
        }
    }

    size_t alloc_len = src_file->length > 0 ? src_file->length : 1;
    uint8_t *buf = (uint8_t *)kmalloc(alloc_len);
    if (!buf) {
        term_print("cp: out of memory\n");
        return;
    }

    int64_t bytes_read = 0;
    if (src_file->length > 0) {
        bytes_read = vfs_read(src_file, 0, src_file->length, buf);
        if (bytes_read <= 0) {
            term_print("cp: failed to read source file\n");
            kfree(buf);
            return;
        }
    }

    vfs_node_t *parent_dir = vfs_open(parent_path[0] == '\0' ? "/" : parent_path, 0);
    if (!parent_dir || !(parent_dir->flags & FS_DIRECTORY)) {
        term_print("cp: invalid destination directory\n");
        kfree(buf);
        return;
    }

    vfs_node_t *new_file = vfs_create(parent_dir, filename, 0);
    if (!new_file) {
        new_file = vfs_open(final_dst, 0);
    }

    if (!new_file) {
        term_print("cp: failed to create destination file\n");
        kfree(buf);
        return;
    }

    int64_t bytes_written = vfs_write(new_file, 0, src_file->length, buf);
    if (bytes_written >= 0) {
        term_print("File copied successfully -> ");
        term_print(final_dst);
        term_print("\n");
    } else {
        term_print("cp: failed to write to destination file\n");
    }

    kfree(buf);
}