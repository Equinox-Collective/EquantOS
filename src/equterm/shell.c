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
#include <stdint.h>
#include <stddef.h>

extern uint64_t free_memory;
extern uint64_t total_pages;
extern size_t used_memory;

#define SHELL_PROMPT_COLOR 0x0000FF00 // Neon Green
#define MAX_ARGS 8
#define LINE_BUF_SIZE 256

typedef void (*shell_handler_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *description;
    shell_handler_t handler;
} shell_command_t;

// Command prototypes
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

// The Ultimate Command Registry
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
    { "writefile",  "Create/write text file to RAMFS",       cmd_writefile },
    { "pciscan",    "Resкан and print all PCI devices",      cmd_pciscan },
    { "sysinfo",    "Show detailed memory metrics struct",   cmd_sysinfo },
    { "panic_test", "Trigger Ring 0 #UD exception (panic)",  cmd_panic_test },
    { "reboot",   "Reboot the system hardware",              cmd_reboot },
    { "shutdown", "Power off the system hardware",           cmd_shutdown },
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
    vfs_node_t *dir = vfs_open("/", 0);
    if (!dir) {
        term_print("Error: failed to open root directory\n");
        return;
    }
    term_print("Listing VFS Root Directory (/):\n");
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

// TORTURE TEST: Heap allocation stress
static void cmd_memstress(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[STRESS] Starting kernel heap allocation torture test...\n");
    
    void *ptrs[64];
    int success_count = 0;

    for (int i = 0; i < 64; i++) {
        ptrs[i] = kmalloc(512); // Allocate 512 bytes chunks
        if (ptrs[i]) {
            memset(ptrs[i], 0xAA, 512); // Write pattern
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
    if (argc < 3) {
        term_print("Usage: writefile <filename> <text>\n");
        return;
    }

    // Reconstruct full text from argv[2] onwards, joining with spaces
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

    vfs_node_t *root = vfs_open("/", 0);
    if (!root) {
        term_print("Error opening root directory\n");
        return;
    }

    vfs_node_t *new_file = ramfs_create_file(root, argv[1], text_buf, strlen(text_buf));
    if (new_file) {
        term_print("File created successfully: /");
        term_print(argv[1]);
        term_print("\n");
    } else {
        term_print("Failed to create file\n");
    }
}

static void cmd_pciscan(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[PCI] Re-scanning PCI bus topology...\n");
    pci_init();
    term_print("[PCI] Scan complete. Check serial log for vendor/device entries.\n");
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

// CRASH TEST: Deliberate Ring 0 Invalid Opcode Exception (#UD)
static void cmd_panic_test(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("CRITICAL: Triggering deliberate Ring 0 invalid opcode (ud2)...\n");
    // Execute UD2 instruction which explicitly triggers #UD CPU exception vector 6
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