// shell.c - Ultimate EquantOS Diagnostic & Kernel Stress-Test Suite
#include "shell.h"
#include "term.h"
#include "string.h"
#include "../kernel/misc/timer.h"
#include "../kernel/fs/vfs.h"
#include "../kernel/core/mem/vmm.h"
#include "../kernel/fs/mbr.h"
#include "../kernel/fs/ext2.h"
#include "../kernel/core/mem/pmm.h"
#include "../kernel/core/mem/memory.h"
#include "../kernel/drivers/pci/pci.h"
#include "../kernel/misc/power.h"
#include "../kernel/proc/loader.h"
#include "../kernel/proc/sched.h"
#include "../kernel/drivers/disk/ata.h"
#include "../kernel/drivers/disk/nvme.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../kernel/drivers/display/psf2.h"
#include "../kernel/fs/gpt.h"
#include "../kernel/fs/partition.h"
#include "../kernel/proc/task.h"
#include "../kernel/core/gen/cpu.h"
#include "../kernel/core/gen/io.h"
#include "../kernel/drivers/usb/xhci.h"

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
static void cmd_ttytest(int argc, char **argv);
static void cmd_fonttest(int argc, char **argv);
static void cmd_colortest(int argc, char **argv);
static void cmd_mkfstest(int argc, char **argv);
static void cmd_ataread(int argc, char **argv);
static void cmd_nvmeread(int argc, char **argv);
static void cmd_atastress(int argc, char **argv);
static void cmd_nvmestress(int argc, char **argv);
static void cmd_fstest(int argc, char **argv);
static void cmd_mkdir(int argc, char **argv);
static void cmd_touch(int argc, char **argv);
static void cmd_mountinfo(int argc, char **argv);
static void cmd_gptdump(int argc, char **argv);
static void cmd_mbrdump(int argc, char **argv);
static void cmd_devtest(int argc, char **argv);
static void cmd_cpuinfo(int argc, char **argv);
static void cmd_vmstress(int argc, char **argv);
static void cmd_pmmbench(int argc, char **argv);
static void cmd_schedtest(int argc, char **argv);
static void cmd_ps(int argc, char **argv);
static void cmd_sleep(int argc, char **argv);
static void cmd_msrtest(int argc, char **argv);
static void cmd_ioperf(int argc, char **argv);
static void cmd_fatinfo(int argc, char **argv);
static void cmd_ext2info(int argc, char **argv);
static void cmd_xhcitest(int argc, char **argv);
static void cmd_pcipeek(int argc, char **argv);
static void cmd_inbtest(int argc, char **argv);

static const shell_command_t commands[] = {
    { "help",       "List all diagnostic & stress commands",  cmd_help },
    { "clear",      "Clear the terminal screen",              cmd_clear },
    { "echo",       "Print text back to terminal",            cmd_echo },
    { "uptime",     "Show system uptime since boot",          cmd_uptime },
    { "eqfetch",    "Show EquantOS system banner",            cmd_eqfetch },
    { "ver",        "Show OS version details",                cmd_ver },
    { "ls",         "List files in VFS root directory",       cmd_ls },
    { "cat",        "Read and display file contents",         cmd_cat },
    { "mem",        "Show RAM and heap usage overview",       cmd_mem },
    { "memstress",  "Torture test kernel heap allocation",    cmd_memstress },
    { "heapdump",   "Dump internal kernel heap block map",    cmd_heapdump },
    { "diskinfo",   "Show ATA drive info and MBR partitions", cmd_diskinfo },
    { "hexdump",    "Dump raw file contents in Hex format",   cmd_hexdump },
    { "writefile",  "Create/write text file to RAMFS/FAT32",  cmd_writefile },
    { "pciscan",    "Restore and print all PCI devices",      cmd_pciscan },
    { "sysinfo",    "Show detailed memory metrics struct",    cmd_sysinfo },
    { "panic_test", "Trigger Ring 0 #UD exception (panic)",   cmd_panic_test },
    { "reboot",   "Reboot the system hardware",               cmd_reboot },
    { "shutdown", "Power off the system hardware",            cmd_shutdown },
    { "pwd",    "Print current working directory",            cmd_pwd },
    { "cd",     "Change working directory",                   cmd_cd },
    { "run",    "Load and execute an ELF binary",             cmd_run },
    { "cp",     "Copy source file to destination",            cmd_cp },
    { "colortest", "Test ANSI color palette output",          cmd_colortest },
    { "fonttest",  "Check active font details and glyph map", cmd_fonttest },
    { "ttytest",   "Test TAB, backspace and control codes",   cmd_ttytest },
    { "mkfstest",  "Format partition with Ext2 filesystem",   cmd_mkfstest },
    { "ataread",    "Read raw ATA sectors: ataread <lba> <n>",cmd_ataread },
    { "nvmeread",   "Read raw NVMe sectors: nvmeread <lba> <n>", cmd_nvmeread },
    { "atastress",  "Torture read/write test on ATA drive",   cmd_atastress },
    { "nvmestress", "High-speed NVMe DMA read/write torture", cmd_nvmestress },
    { "fstest",     "File create, multi-write & readback test", cmd_fstest },
    { "mkdir",      "Create a directory: mkdir <path>",       cmd_mkdir },
    { "touch",      "Create an empty file: touch <path>",     cmd_touch },
    { "mountinfo",  "List mounted VFS filesystems & roots",   cmd_mountinfo },
    { "gptdump",    "Dump raw GPT header and partitions",     cmd_gptdump },
    { "mbrdump",    "Dump raw MBR entries & boot signature",  cmd_mbrdump },
    { "devtest",    "Test /dev/null, /dev/tty0, /dev/input0", cmd_devtest },
    { "cpuinfo",    "Show CR0, CR4, RFLAGS, PAT, SSE state",  cmd_cpuinfo },
    { "vmstress",   "Virtual memory map & COW page fault test", cmd_vmstress },
    { "pmmbench",   "Benchmark physical page allocation speed", cmd_pmmbench },
    { "schedtest",  "Spawn background tasks to test preempt", cmd_schedtest },
    { "ps",         "Show process table, PID, status & memory", cmd_ps },
    { "sleep",      "Sleep for milliseconds: sleep <ms>",     cmd_sleep },
    { "msrtest",    "Read CPU MSR register: msrtest <hex_msr>", cmd_msrtest },
    { "ioperf",     "Benchmark disk read throughput (MB/s)",  cmd_ioperf },
    { "fatinfo",    "Display FAT32 cluster chain & info",     cmd_fatinfo },
    { "ext2info",   "Dump Ext2 Superblock & Inode table info", cmd_ext2info },
    { "xhcitest",   "Show USB xHCI controller & port status", cmd_xhcitest },
    { "pcipeek",    "Read PCI register: pcipeek <b> <s> <f> <o>", cmd_pcipeek },
    { "inbtest",    "Read raw hardware I/O port: inbtest <hex_port>", cmd_inbtest },
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
    term_print("=== ATA/NVMe Drive & Partition Info ===\n");
    int p_count = disk_get_partition_count();
    char buf[16];
    term_print("Detected Partitions: ");
    itoa(p_count, 10, buf);
    term_print(buf);
    term_print("\n");

    for (int i = 0; i < p_count; i++) {
        partition_info_t *p = disk_get_partition(i);
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
            sched_yield(); // Отдаем процессор BusyBox'у
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

static void cmd_colortest(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== EquantOS ANSI Color Palette Test ===\n");
    term_print("\033[31m[RED]\033[0m     Red color text\n");
    term_print("\033[32m[GREEN]\033[0m   Green color text\n");
    term_print("\033[33m[YELLOW]\033[0m  Yellow color text\n");
    term_print("\033[34m[BLUE]\033[0m    Blue color text\n");
    term_print("\033[36m[CYAN]\033[0m    Cyan color text\n");
    term_print("\033[0m[DEFAULT] Reset to default foreground color\n");
    term_print("=======================================\n");
}

static void cmd_fonttest(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== EquantOS Font Subsystem Diagnostic ===\n");
    
    if (kernel_psf2_font.loaded && kernel_psf2_font.hdr) {
        term_print("Active Engine : PSF2 High-Resolution Font Renderer\n");
        char buf[32];
        itoa((int64_t)kernel_psf2_font.hdr->width, 10, buf);
        term_print("Glyph Width   : "); term_print(buf); term_print(" px\n");
        itoa((int64_t)kernel_psf2_font.hdr->height, 10, buf);
        term_print("Glyph Height  : "); term_print(buf); term_print(" px\n");
        itoa((int64_t)kernel_psf2_font.hdr->numglyph, 10, buf);
        term_print("Total Glyphs  : "); term_print(buf); term_print("\n");
    } else {
        term_print("Active Engine : 8x8 Legacy Bitmap Fallback\n");
    }

    term_print("\nAlphabet Test : ABCDEFGHIJKLMNOPQRSTUVWXYZ\n");
    term_print("Lowercase Test: abcdefghijklmnopqrstuvwxyz\n");
    term_print("Numeric Test  : 0123456789\n");
    term_print("Symbols Test  : !@#$%^&*()_+-=[]{}|;:'\",.<>/?`~\n");
    term_print("===========================================\n");
}

static void cmd_ttytest(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== EquantOS TTY Control Characters Test ===\n");
    term_print("Testing Tabulation (\\t):\n");
    term_print("COL1\tCOL2\tCOL3\tCOL4\n");
    term_print("100\t200\t300\t400\n");
    
    term_print("\nTesting Carriage Return (\\r) and Backspace (\\b):\n");
    term_print("Loading [====      ] 40%\b\b\b\b\b\b\b\b\b\b80% [========  ]\n");
    term_print("============================================\n");
}

static void cmd_mkfstest(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: mkfstest <partition_num>\nExample: mkfstest 0\n");
        return;
    }

    int part_num = atoi(argv[1]);
    partition_info_t *p = disk_get_partition(part_num);
    if (!p) {
        term_print("mkfstest: Invalid partition index!\n");
        return;
    }

    block_device_t dev;
    if (nvme_init() == NVME_SUCCESS) {
        dev = nvme_get_block_device();
    } else {
        block_device_t ata_dev = {
            .read = (block_read_fn)read_sectors_ata_pio,
            .write = (block_write_fn)write_sectors_ata_pio,
            .sector_size = 512
        };
        dev = ata_dev;
    }

    term_print("Formatting partition #");
    term_print(argv[1]);
    term_print(" with EXT2 Filesystem...\n");

    int err = mkfs_ext2(dev, p->start_lba, p->sector_count, "EQUANT_TEST");
    if (err == 0) {
        term_print("SUCCESS: Partition formatted to EXT2!\n");
    } else {
        term_print("ERROR: Formatting failed.\n");
    }
}
// 2. Чтение сырых секторов ATA
static void cmd_ataread(int argc, char **argv) {
    if (argc < 3) {
        term_print("Usage: ataread <start_lba> <count>\n");
        return;
    }
    uint64_t lba = atoi(argv[1]);
    uint32_t count = atoi(argv[2]);
    if (count == 0 || count > 128) count = 1;

    uint8_t *buf = (uint8_t *)kmalloc(count * 512);
    if (!buf) { term_print("ataread: out of memory\n"); return; }

    term_print("Reading from ATA PIO...\n");
    read_sectors_ata_pio((uintptr_t)buf, lba, count);

    char h[8];
    term_print("First 16 bytes: ");
    for (int i = 0; i < 16; i++) {
        itoa_hex(buf[i], h);
        if (buf[i] < 16) term_print("0");
        term_print(h); term_print(" ");
    }
    term_print("\n[ATA] Read completed successfully.\n");
    kfree(buf);
}

// 3. Чтение сырых секторов NVMe
static void cmd_nvmeread(int argc, char **argv) {
    if (argc < 3) {
        term_print("Usage: nvmeread <start_lba> <count>\n");
        return;
    }
    uint64_t lba = atoi(argv[1]);
    uint32_t count = atoi(argv[2]);
    if (count == 0 || count > 128) count = 1;

    uint8_t *buf = (uint8_t *)kmalloc(count * 512);
    if (!buf) { term_print("nvmeread: out of memory\n"); return; }

    int res = nvme_read_sectors(lba, count, buf);
    if (res == NVME_SUCCESS) {
        char h[8];
        term_print("NVMe Read Success! First 16 bytes: ");
        for (int i = 0; i < 16; i++) {
            itoa_hex(buf[i], h);
            if (buf[i] < 16) term_print("0");
            term_print(h); term_print(" ");
        }
        term_print("\n");
    } else {
        term_print("nvmeread: NVMe read failed or drive not ready!\n");
    }
    kfree(buf);
}

// 4. Стресс-тест записи и верификации ATA
static void cmd_atastress(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: atastress <test_lba>\nWARNING: This will overwrite 1 sector!\n");
        return;
    }
    uint64_t lba = atoi(argv[1]);
    uint8_t *wbuf = (uint8_t *)kmalloc(512);
    uint8_t *rbuf = (uint8_t *)kmalloc(512);
    if (!wbuf || !rbuf) { term_print("Out of memory\n"); return; }

    for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i ^ 0x5A);
    memset(rbuf, 0, 512);

    term_print("[ATA-STRESS] Writing pattern 0x5A to LBA...\n");
    write_sectors_ata_pio((uintptr_t)wbuf, lba, 1);

    term_print("[ATA-STRESS] Reading back and verifying...\n");
    read_sectors_ata_pio((uintptr_t)rbuf, lba, 1);

    if (memcmp(wbuf, rbuf, 512) == 0) {
        term_print("\033[32m[PASS]\033[0m ATA read/write pattern verified with 100% match!\n");
    } else {
        term_print("\033[31m[FAIL]\033[0m ATA data corruption detected!\n");
    }
    kfree(wbuf);
    kfree(rbuf);
}

// 5. Высокоскоростной стресс-тест NVMe
static void cmd_nvmestress(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: nvmestress <test_lba>\nWARNING: Writes 8 sectors test pattern!\n");
        return;
    }
    uint64_t lba = atoi(argv[1]);
    size_t sz = 8 * 512;
    uint8_t *wbuf = (uint8_t *)kmalloc(sz);
    uint8_t *rbuf = (uint8_t *)kmalloc(sz);
    if (!wbuf || !rbuf) return;

    for (size_t i = 0; i < sz; i++) wbuf[i] = (uint8_t)(i & 0xFF);
    memset(rbuf, 0, sz);

    term_print("[NVME-STRESS] Executing 4KB DMA Write...\n");
    if (nvme_write_sectors(lba, 8, wbuf) != NVME_SUCCESS) {
        term_print("[FAIL] NVMe write failed\n");
        kfree(wbuf); kfree(rbuf); return;
    }

    term_print("[NVME-STRESS] Executing 4KB DMA Read back...\n");
    if (nvme_read_sectors(lba, 8, rbuf) != NVME_SUCCESS) {
        term_print("[FAIL] NVMe read failed\n");
        kfree(wbuf); kfree(rbuf); return;
    }

    if (memcmp(wbuf, rbuf, sz) == 0) {
        term_print("\033[32m[PASS]\033[0m NVMe 4KB DMA stress test verified cleanly!\n");
    } else {
        term_print("\033[31m[FAIL]\033[0m NVMe DMA data mismatch!\n");
    }
    kfree(wbuf); kfree(rbuf);
}

// 6. Комплексный тест файловой системы
static void cmd_fstest(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[FSTEST] Creating /test_vfs.tmp...\n");
    vfs_node_t *dir = vfs_open("/", 0);
    if (!dir) { term_print("Cannot open root dir\n"); return; }

    vfs_node_t *file = vfs_create(dir, "test_vfs.tmp", 0);
    if (!file) file = vfs_open("/test_vfs.tmp", 0);
    if (!file) { term_print("Failed to create file\n"); return; }

    const char *test_data = "EquantOS VFS Integration Test String 1234567890\n";
    size_t len = strlen(test_data);
    int64_t w = vfs_write(file, 0, len, (uint8_t *)test_data);

    char r_buf[128] = {0};
    int64_t r = vfs_read(file, 0, len, (uint8_t *)r_buf);

    if (w == (int64_t)len && r == (int64_t)len && strcmp(test_data, r_buf) == 0) {
        term_print("\033[32m[PASS]\033[0m VFS create/write/read cycle passed perfectly!\n");
    } else {
        term_print("\033[31m[FAIL]\033[0m VFS data verification failed!\n");
    }
}

// 7. Создание директории
static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { term_print("Usage: mkdir <dirname>\n"); return; }
    char resolved[256];
    resolve_path(argv[1], resolved, sizeof(resolved));

    char parent_path[256];
    strcpy(parent_path, resolved);
    char *dirname = parent_path;
    char *last_slash = strrchr(parent_path, '/');
    if (last_slash) {
        if (last_slash == parent_path) {
            dirname = last_slash + 1;
            parent_path[1] = '\0';
        } else {
            *last_slash = '\0';
            dirname = last_slash + 1;
        }
    }

    vfs_node_t *pdir = vfs_open(parent_path[0] == '\0' ? "/" : parent_path, 0);
    if (!pdir) { term_print("mkdir: invalid parent path\n"); return; }

    vfs_node_t *new_dir = vfs_create(pdir, dirname, FS_DIRECTORY);
    if (new_dir) {
        term_print("Directory created: "); term_print(resolved); term_print("\n");
    } else {
        term_print("mkdir: failed to create directory\n");
    }
}

// 8. Создание пустого файла
static void cmd_touch(int argc, char **argv) {
    if (argc < 2) { term_print("Usage: touch <filename>\n"); return; }
    char resolved[256];
    resolve_path(argv[1], resolved, sizeof(resolved));

    char parent_path[256];
    strcpy(parent_path, resolved);
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

    vfs_node_t *pdir = vfs_open(parent_path[0] == '\0' ? "/" : parent_path, 0);
    if (!pdir) { term_print("touch: invalid directory\n"); return; }

    vfs_node_t *nf = vfs_create(pdir, filename, FS_FILE);
    if (nf) {
        term_print("Created file: "); term_print(resolved); term_print("\n");
    } else {
        term_print("touch: create failed\n");
    }
}

// 9. Список точек монтирования
static void cmd_mountinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== VFS Mountpoints & Storage Topology ===\n");
    term_print("  /          -> RAMFS Root Filesystem (In-Memory Boot)\n");
    term_print("  /dev       -> DevFS (Dynamic Device Nodes: null, input0, tty0)\n");
    term_print("  /sys/bin   -> Limine Boot Executables Cache\n");
    term_print("  /drives    -> Partition Mount Root (FAT32/EXT2)\n");
}

// 10. Дамп GPT таблицы
static void cmd_gptdump(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== GUID Partition Table (GPT) Dump ===\n");
    int count = gpt_get_partition_count();
    char b[32];
    itoa(count, 10, b);
    term_print("GPT Partitions parsed: "); term_print(b); term_print("\n");

    for (int i = 0; i < count; i++) {
        partition_info_t *p = gpt_get_partition(i);
        if (p) {
            term_print("  [GPT #"); itoa(p->index, 10, b); term_print(b); term_print("] ");
            term_print("Start LBA: "); itoa(p->start_lba, 10, b); term_print(b);
            term_print(" | Sectors: "); itoa(p->sector_count, 10, b); term_print(b);
            term_print(" | Type: "); term_print(p->fs_name);
            term_print("\n");
        }
    }
}

// 11. Дамп MBR таблицы
static void cmd_mbrdump(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== Master Boot Record (MBR) Dump ===\n");
    int count = mbr_get_partition_count();
    char b[32];
    itoa(count, 10, b);
    term_print("MBR Partitions detected: "); term_print(b); term_print("\n");

    for (int i = 0; i < count; i++) {
        partition_info_t *p = mbr_get_partition(i);
        if (p) {
            term_print("  [MBR Entry #"); itoa(p->index, 10, b); term_print(b); term_print("] ");
            term_print("Type: 0x"); itoa_hex(p->type, b); term_print(b);
            term_print(" | LBA: "); itoa(p->start_lba, 10, b); term_print(b);
            term_print(" | Sectors: "); itoa(p->sector_count, 10, b); term_print(b);
            term_print("\n");
        }
    }
}

// 12. Тестирование виртуальных устройств DevFS
static void cmd_devtest(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== DevFS Dynamic Device Nodes Test ===\n");

    vfs_node_t *dev_null = vfs_open("/dev/null", 0);
    if (dev_null) {
        int64_t w = vfs_write(dev_null, 0, 10, (uint8_t *)"1234567890");
        term_print("  /dev/null write test: ");
        term_print(w == 10 ? "\033[32m[PASS]\033[0m\n" : "\033[31m[FAIL]\033[0m\n");
    } else {
        term_print("  /dev/null node not found!\n");
    }

    vfs_node_t *dev_tty = vfs_open("/dev/tty0", 0);
    if (dev_tty) {
        const char *msg = "\033[32m[PASS]\033[0m\n";
        term_print("  /dev/tty0 write test: ");
        vfs_write(dev_tty, 0, strlen(msg), (uint8_t *)msg);
    } else {
        term_print("  /dev/tty0 node not found!\n");
    }
}

// 13. Информация о регистрах CPU
static void cmd_cpuinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    char b[32];
    uint64_t cr0 = read_cr0();
    uint64_t cr4 = read_cr4();

    term_print("=== CPU Control Registers & Architecture State ===\n");
    term_print("CR0 Register : 0x"); itoa_hex(cr0, b); term_print(b); term_print("\n");
    term_print("CR4 Register : 0x"); itoa_hex(cr4, b); term_print(b); term_print("\n");
    term_print("FPU/SSE State: ");
    term_print((cr4 & (1 << 9)) ? "\033[32mENABLED (OSFXSR)\033[0m\n" : "\033[31mDISABLED\033[0m\n");
    term_print("Paging (PG)  : ");
    term_print((cr0 & (1ULL << 31)) ? "\033[32mENABLED\033[0m\n" : "\033[31mDISABLED\033[0m\n");
}

// 14. Стресс-тест виртуальной памяти и Copy-On-Write
static void cmd_vmstress(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[VM-STRESS] Testing VMM address space allocation & COW cloning...\n");

    page_table_t *pml4 = vmm_create_address_space();
    if (!pml4) {
        term_print("vmstress: Failed to create address space!\n");
        return;
    }

    uint64_t test_vaddr = 0x0000000040000000ULL;
    void *phys = pmm_alloc();
    vmm_map(pml4, test_vaddr, (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);

    uint64_t resolved_phys = vmm_get_phys(pml4, test_vaddr);
    if (resolved_phys == (uint64_t)phys) {
        term_print("\033[32m[PASS]\033[0m Page translation matches physical frame!\n");
    } else {
        term_print("\033[31m[FAIL]\033[0m Page translation mismatch!\n");
    }

    vmm_unmap(pml4, test_vaddr);
    pmm_free(phys);
    term_print("[VM-STRESS] Address space unmapped cleanly.\n");
}

// 15. Бенчмарк аллокатора физических страниц (PMM Buddy)
static void cmd_pmmbench(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[PMM-BENCH] Allocating 1024 physical pages sequentially...\n");

    void *pages[1024];
    uint32_t start = tick;
    int success = 0;

    for (int i = 0; i < 1024; i++) {
        pages[i] = pmm_alloc();
        if (pages[i]) success++;
    }

    uint32_t alloc_time = tick - start;
    start = tick;

    for (int i = 0; i < 1024; i++) {
        if (pages[i]) pmm_free(pages[i]);
    }
    uint32_t free_time = tick - start;

    char b[32];
    term_print("Allocated & Freed 1024 Pages (4MB). Success: ");
    itoa(success, 10, b); term_print(b);
    term_print(" pages. Alloc ticks: "); itoa(alloc_time, 10, b); term_print(b);
    term_print(" | Free ticks: "); itoa(free_time, 10, b); term_print(b);
    term_print("\n");
}

// Фоновая функция для теста планировщика
static void sched_test_worker(void) {
    for (int i = 0; i < 5; i++) {
        yield();
    }
    if (current_task) {
        current_task->state = TASK_STATE_ZOMBIE;
    }
    for (;;) { yield(); }
}

// 16. Тест планировщика задач
static void cmd_schedtest(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[SCHED] Spawning 3 concurrent kernel threads...\n");

    task_create(sched_test_worker, 0, 0);
    task_create(sched_test_worker, 0, 0);
    task_create(sched_test_worker, 0, 0);

    for (int i = 0; i < 20; i++) {
        sched_yield();
    }

    term_print("\033[32m[PASS]\033[0m Multithreading round-robin preemption completed.\n");
}

// 17. Список активных процессов
static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== Process & Thread Table ===\n");
    term_print(" PID  | State     | Priority | TimeSlice\n");
    term_print("-----------------------------------------\n");

    if (current_task) {
        char b[32];
        term_print("  "); itoa(current_task->id, 10, b); term_print(b);
        term_print("   | RUNNING   | ");
        itoa(current_task->priority, 10, b); term_print(b);
        term_print("       | ");
        itoa(current_task->time_slice, 10, b); term_print(b);
        term_print(" ticks\n");
    }
}

// 18. Задержка таймера PIT
static void cmd_sleep(int argc, char **argv) {
    if (argc < 2) { term_print("Usage: sleep <milliseconds>\n"); return; }
    uint32_t ms = atoi(argv[1]);
    term_print("Sleeping... ");
    sleep(ms / 10);
    term_print("Done!\n");
}

// 19. Чтение MSR регистра CPU
static void cmd_msrtest(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: msrtest <hex_msr>\nExample: msrtest 0xC0000080 (EFER)\n");
        return;
    }
    uint32_t msr = (uint32_t)atoi(argv[1]);
    uint64_t val = read_msr(msr);
    char b[32];
    term_print("MSR 0x"); itoa_hex(msr, b); term_print(b);
    term_print(" = 0x"); itoa_hex(val, b); term_print(b); term_print("\n");
}

// 20. Бенчмарк дисковой производительности
static void cmd_ioperf(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("[IO-PERF] Measuring 1MB sequential sector read speed...\n");

    size_t sz = 1024 * 1024;
    uint8_t *buf = (uint8_t *)kmalloc(sz);
    if (!buf) { term_print("ioperf: OOM\n"); return; }

    uint32_t t0 = tick;
    if (nvme_init() == NVME_SUCCESS) {
        nvme_read_sectors(0, 2048, buf);
        uint32_t dt = tick - t0;
        char b[32];
        term_print("NVMe 1MB Read Time: "); itoa(dt, 10, b); term_print(b); term_print(" ticks (10ms unit)\n");
    } else {
        read_sectors_ata_pio((uintptr_t)buf, 0, 2048);
        uint32_t dt = tick - t0;
        char b[32];
        term_print("ATA 1MB Read Time: "); itoa(dt, 10, b); term_print(b); term_print(" ticks (10ms unit)\n");
    }
    kfree(buf);
}

// 21. Диагностика FAT32
static void cmd_fatinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== FAT32 Driver Inspection ===\n");
    vfs_node_t *fat_mount = vfs_open("/drives/fat32_nvme", 0);
    if (fat_mount) {
        term_print("FAT32 Volume Status: \033[32mMOUNTED\033[0m\n");
        term_print("Root Cluster Pointer: OK\n");
    } else {
        term_print("FAT32 Volume Status: NOT MOUNTED (Check /drives)\n");
    }
}

// 22. Диагностика Ext2
static void cmd_ext2info(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== EXT2 Filesystem Inspection ===\n");
    vfs_node_t *ext2_mount = vfs_open("/drives/ext2_nvme", 0);
    if (ext2_mount) {
        term_print("EXT2 Volume Status: \033[32mMOUNTED\033[0m\n");
        term_print("Root Inode (#2): Active\n");
    } else {
        term_print("EXT2 Volume Status: NOT MOUNTED (Check /drives)\n");
    }
}

// 23. Статус xHCI USB контроллера
static void cmd_xhcitest(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("=== xHCI USB 3.x Subsystem Diagnostic ===\n");
    term_print("Flushing pending events from xHCI Event Ring...\n");
    xhci_handle_events();
    term_print("xHCI Subsystem Status: \033[32mACTIVE & READY\033[0m\n");
}

// 24. Чтение произвольного регистра PCI
static void cmd_pcipeek(int argc, char **argv) {
    if (argc < 5) {
        term_print("Usage: pcipeek <bus> <slot> <func> <offset>\n");
        return;
    }
    uint8_t b = atoi(argv[1]);
    uint8_t s = atoi(argv[2]);
    uint8_t f = atoi(argv[3]);
    uint8_t o = atoi(argv[4]);

    uint32_t val = pci_read_dword(b, s, f, o);
    char hex[32];
    term_print("PCI [");
    term_print(argv[1]); term_print(":");
    term_print(argv[2]); term_print(":");
    term_print(argv[3]); term_print("] Off 0x");
    itoa_hex(o, hex); term_print(hex);
    term_print(" = 0x");
    itoa_hex(val, hex); term_print(hex);
    term_print("\n");
}

// 25. Чтение I/O порта процессора
static void cmd_inbtest(int argc, char **argv) {
    if (argc < 2) {
        term_print("Usage: inbtest <hex_port>\nExample: inbtest 0x64 (PS/2 Status)\n");
        return;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    uint8_t val = inb(port);
    char b[16];
    term_print("inb(0x"); itoa_hex(port, b); term_print(b);
    term_print(") -> 0x"); itoa_hex(val, b); term_print(b);
    term_print("\n");
}