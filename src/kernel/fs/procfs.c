// src/kernel/fs/procfs.c - Linux-compatible /proc pseudo filesystem
//
// File contents are generated when the file is opened, so every open() sees a fresh
// snapshot that read() then serves sequentially, like Linux seq_file.
#include "procfs.h"
#include "vfs.h"
#include "../core/mem/memory.h"
#include "../core/mem/pmm.h"
#include "../core/initcall.h"
#include "../drivers/serial/serial.h"
#include "../misc/timer.h"
#include "../proc/task.h"
#include "../proc/syscall.h"
#include "../equant_version.h"
#include "string.h"
#include "stdio.h"
#include <stdarg.h>
#include <stdbool.h>

#define PROCFS_SYS_BUF_SIZE 4096
#define PROCFS_PID_BUF_SIZE 512
#define PROCFS_PID_SLOTS    16

typedef struct procfs_buf {
    char *data;
    size_t cap;
    size_t len;
} procfs_buf_t;

typedef void (*procfs_gen_t)(procfs_buf_t *out, process_t *proc);

typedef struct procfs_file {
    procfs_gen_t gen;
    uint64_t pid;           // 0 for system-wide files
    procfs_buf_t buf;
} procfs_file_t;

static void pb_printf(procfs_buf_t *out, const char *fmt, ...) {
    if (out->len + 1 >= out->cap) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out->data + out->len, out->cap - out->len, fmt, args);
    va_end(args);
    if (n > 0) out->len += (size_t)n;
}

// ---------------------------------------------------------------------------
// System-wide files
// ---------------------------------------------------------------------------

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

typedef struct cpu_flag {
    uint8_t bit;
    const char *name;
} cpu_flag_t;

static const cpu_flag_t flags_1_edx[] = {
    {0, "fpu"}, {1, "vme"}, {2, "de"}, {3, "pse"}, {4, "tsc"}, {5, "msr"}, {6, "pae"}, {7, "mce"},
    {8, "cx8"}, {9, "apic"}, {11, "sep"}, {12, "mtrr"}, {13, "pge"}, {14, "mca"}, {15, "cmov"},
    {16, "pat"}, {17, "pse36"}, {19, "clflush"}, {23, "mmx"}, {24, "fxsr"}, {25, "sse"},
    {26, "sse2"}, {28, "ht"},
};
static const cpu_flag_t flags_1_ecx[] = {
    {0, "pni"}, {1, "pclmulqdq"}, {9, "ssse3"}, {12, "fma"}, {13, "cx16"}, {19, "sse4_1"},
    {20, "sse4_2"}, {21, "x2apic"}, {22, "movbe"}, {23, "popcnt"}, {25, "aes"}, {26, "xsave"},
    {28, "avx"}, {29, "f16c"}, {30, "rdrand"}, {31, "hypervisor"},
};
static const cpu_flag_t flags_ext_edx[] = {
    {11, "syscall"}, {20, "nx"}, {27, "rdtscp"}, {29, "lm"},
};
static const cpu_flag_t flags_ext_ecx[] = {
    {0, "lahf_lm"}, {5, "abm"},
};

static void pb_flags(procfs_buf_t *out, uint32_t reg, const cpu_flag_t *flags, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (reg & (1U << flags[i].bit)) pb_printf(out, " %s", flags[i].name);
    }
}

static void gen_cpuinfo(procfs_buf_t *out, process_t *proc) {
    (void)proc;
    uint32_t a, b, c, d;

    char vendor[13];
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';

    uint32_t sig, ecx1, edx1;
    cpuid(1, &sig, &b, &ecx1, &edx1);
    uint32_t family = (sig >> 8) & 0xF;
    uint32_t model = (sig >> 4) & 0xF;
    if (family == 0xF) family += (sig >> 20) & 0xFF;
    if (family == 0x6 || family >= 0xF) model += ((sig >> 16) & 0xF) << 4;

    char brand[49] = "Unknown CPU";
    uint32_t ecx_ext = 0, edx_ext = 0;
    cpuid(0x80000000, &a, &b, &c, &d);
    uint32_t max_ext = a;
    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, &a, &b, &ecx_ext, &edx_ext);
    }
    if (max_ext >= 0x80000004) {
        uint32_t *w = (uint32_t *)brand;
        for (uint32_t leaf = 0; leaf < 3; leaf++) {
            cpuid(0x80000002 + leaf, &w[leaf * 4 + 0], &w[leaf * 4 + 1], &w[leaf * 4 + 2], &w[leaf * 4 + 3]);
        }
        brand[48] = '\0';
    }
    const char *model_name = brand;
    while (*model_name == ' ') model_name++;

    // EquantOS schedules on the bootstrap processor only
    pb_printf(out, "processor\t: 0\n");
    pb_printf(out, "vendor_id\t: %s\n", vendor);
    pb_printf(out, "cpu family\t: %u\n", family);
    pb_printf(out, "model\t\t: %u\n", model);
    pb_printf(out, "model name\t: %s\n", model_name);
    pb_printf(out, "stepping\t: %u\n", sig & 0xF);
    pb_printf(out, "physical id\t: 0\n");
    pb_printf(out, "siblings\t: 1\n");
    pb_printf(out, "core id\t\t: 0\n");
    pb_printf(out, "cpu cores\t: 1\n");
    pb_printf(out, "fpu\t\t: yes\n");
    pb_printf(out, "flags\t\t:");
    pb_flags(out, edx1, flags_1_edx, sizeof(flags_1_edx) / sizeof(flags_1_edx[0]));
    pb_flags(out, edx_ext, flags_ext_edx, sizeof(flags_ext_edx) / sizeof(flags_ext_edx[0]));
    pb_flags(out, ecx1, flags_1_ecx, sizeof(flags_1_ecx) / sizeof(flags_1_ecx[0]));
    pb_flags(out, ecx_ext, flags_ext_ecx, sizeof(flags_ext_ecx) / sizeof(flags_ext_ecx[0]));
    pb_printf(out, "\naddress sizes\t: 48 bits virtual\n\n");
}

static void gen_meminfo(procfs_buf_t *out, process_t *proc) {
    (void)proc;
    uint64_t total_kb = pmm_get_total_memory() / 1024;
    uint64_t used_kb = pmm_get_used_memory() / 1024;
    uint64_t free_kb = total_kb > used_kb ? total_kb - used_kb : 0;

    pb_printf(out, "MemTotal:       %8lu kB\n", total_kb);
    pb_printf(out, "MemFree:        %8lu kB\n", free_kb);
    pb_printf(out, "MemAvailable:   %8lu kB\n", free_kb);
    pb_printf(out, "Buffers:        %8lu kB\n", (uint64_t)0);
    pb_printf(out, "Cached:         %8lu kB\n", (uint64_t)0);
    pb_printf(out, "SwapCached:     %8lu kB\n", (uint64_t)0);
    pb_printf(out, "Shmem:          %8lu kB\n", (uint64_t)0);
    pb_printf(out, "SReclaimable:   %8lu kB\n", (uint64_t)0);
    pb_printf(out, "SwapTotal:      %8lu kB\n", (uint64_t)0);
    pb_printf(out, "SwapFree:       %8lu kB\n", (uint64_t)0);
}

static void gen_uptime(procfs_buf_t *out, process_t *proc) {
    (void)proc;
    uint64_t now = tick;
    uint64_t hundredths = (now % TIMER_HZ) * 100 / TIMER_HZ;
    pb_printf(out, "%lu.%02lu 0.00\n", now / TIMER_HZ, hundredths);
}

static void gen_version(procfs_buf_t *out, process_t *proc) {
    (void)proc;
    pb_printf(out, "%s version %s (x86_64-elf-gcc) %s\n",
              EQUANT_OS_NAME, EQUANT_KERNEL_RELEASE, EQUANT_KERNEL_VERSION);
}

static void gen_swaps(procfs_buf_t *out, process_t *proc) {
    (void)proc;
    pb_printf(out, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
}

static void gen_loadavg(procfs_buf_t *out, process_t *proc) {
    (void)proc;
    uint32_t procs = 0;
    if (task_list) {
        task_t *curr = task_list;
        do {
            if (curr->process && curr->state != TASK_STATE_ZOMBIE) procs++;
            curr = curr->next;
        } while (curr && curr != task_list);
    }
    pb_printf(out, "0.00 0.00 0.00 1/%u %lu\n", procs, next_pid > 0 ? next_pid - 1 : 0);
}

// ---------------------------------------------------------------------------
// Per-process files
// ---------------------------------------------------------------------------

static const char *proc_comm(process_t *proc) {
    const char *slash = strrchr(proc->exe_path, '/');
    return slash ? slash + 1 : proc->exe_path;
}

static char proc_state(process_t *proc, uint32_t *threads) {
    char state = 'Z';
    uint32_t count = 0;
    if (task_list) {
        task_t *curr = task_list;
        do {
            if (curr->process == proc && curr->state != TASK_STATE_ZOMBIE) {
                count++;
                if (curr->state == TASK_STATE_RUNNABLE) state = 'R';
                else if (state != 'R') state = 'S';
            }
            curr = curr->next;
        } while (curr && curr != task_list);
    }
    if (threads) *threads = count;
    return state;
}

// Linux tty_nr encoding for /dev/tty1 (major 4, minor 1); EquantOS has a single console
static uint32_t proc_tty_nr(process_t *proc) {
    vfs_node_t *in = proc->files[0];
    return (in && strncmp(in->name, "tty", 3) == 0) ? 0x0401 : 0;
}

static void pb_comm(procfs_buf_t *out, process_t *proc) {
    char comm[16];
    strncpy(comm, proc_comm(proc), sizeof(comm) - 1);
    comm[sizeof(comm) - 1] = '\0';
    pb_printf(out, "%s", comm);
}

static void gen_pid_stat(procfs_buf_t *out, process_t *proc) {
    uint32_t threads = 0;
    char state = proc_state(proc, &threads);
    pb_printf(out, "%lu (", proc->pid);
    pb_comm(out, proc);
    // state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime
    // cutime cstime priority nice num_threads itrealvalue starttime vsize rss
    pb_printf(out, ") %c %lu %lu %lu %u %lu 0 0 0 0 0 %lu %lu 0 0 20 0 %u 0 0 0 0\n",
              state, proc->parent_pid, proc->pgid, proc->pgid, proc_tty_nr(proc), proc->pgid,
              proc->utime, proc->stime, threads);
}

static void gen_pid_cmdline(procfs_buf_t *out, process_t *proc) {
    size_t len = proc->cmdline_len;
    if (len > out->cap) len = out->cap;
    memcpy(out->data, proc->cmdline, len);
    out->len = len;
}

static void gen_pid_comm(procfs_buf_t *out, process_t *proc) {
    pb_comm(out, proc);
    pb_printf(out, "\n");
}

static void gen_pid_status(procfs_buf_t *out, process_t *proc) {
    uint32_t threads = 0;
    char state = proc_state(proc, &threads);
    pb_printf(out, "Name:\t");
    pb_comm(out, proc);
    pb_printf(out, "\nState:\t%c\n", state);
    pb_printf(out, "Tgid:\t%lu\nPid:\t%lu\nPPid:\t%lu\n", proc->pid, proc->pid, proc->parent_pid);
    pb_printf(out, "Uid:\t%u\t%u\t%u\t%u\n", proc->uid, proc->euid, proc->euid, proc->euid);
    pb_printf(out, "Gid:\t%u\t%u\t%u\t%u\n", proc->gid, proc->egid, proc->egid, proc->egid);
    pb_printf(out, "Threads:\t%u\n", threads);
}

// ---------------------------------------------------------------------------
// VFS glue
// ---------------------------------------------------------------------------

static void procfs_file_open(vfs_node_t *node) {
    procfs_file_t *file = (procfs_file_t *)node->ptr;
    if (!file || !file->gen) return;

    process_t *proc = NULL;
    if (file->pid) {
        proc = process_find(file->pid);
        if (!proc) {
            file->buf.len = 0;
            node->length = 0;
            return;
        }
    }

    if (!file->buf.data) {
        file->buf.data = (char *)kmalloc(file->buf.cap);
        if (!file->buf.data) return;
    }
    file->buf.len = 0;
    file->gen(&file->buf, proc);
    node->length = file->buf.len;
}

static int64_t procfs_file_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    procfs_file_t *file = (procfs_file_t *)node->ptr;
    if (!file || !file->buf.data || offset >= file->buf.len) return 0;
    uint64_t n = file->buf.len - offset;
    if (n > size) n = size;
    memcpy(buffer, file->buf.data + offset, n);
    return (int64_t)n;
}

static vfs_file_operations_t procfs_file_ops = {
    .read = procfs_file_read,
    .open = procfs_file_open,
};

static void procfs_init_file(vfs_node_t *node, procfs_file_t *file, const char *name,
                             procfs_gen_t gen, uint64_t pid, size_t cap, vfs_node_t *parent) {
    memset(node, 0, sizeof(*node));
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->flags = FS_FILE;
    node->permissions = 0444;
    node->ops = &procfs_file_ops;
    node->ptr = (vfs_node_t *)file;
    node->parent = parent;

    file->gen = gen;
    file->pid = pid;
    file->buf.cap = cap;
    file->buf.len = 0;
}

// Per-pid directories come from a small recycled pool: lookups never allocate,
// and a slot is only reused for another pid once 16 newer pids have been looked up.
typedef struct procfs_pid_entry {
    const char *name;
    procfs_gen_t gen;
} procfs_pid_entry_t;

static const procfs_pid_entry_t pid_entries[] = {
    {"stat", gen_pid_stat},
    {"cmdline", gen_pid_cmdline},
    {"comm", gen_pid_comm},
    {"status", gen_pid_status},
};
#define PROCFS_PID_FILES (sizeof(pid_entries) / sizeof(pid_entries[0]))

typedef struct procfs_pid_slot {
    uint64_t pid;
    vfs_node_t dir;
    vfs_node_t nodes[PROCFS_PID_FILES];
    procfs_file_t files[PROCFS_PID_FILES];
} procfs_pid_slot_t;

static procfs_pid_slot_t pid_slots[PROCFS_PID_SLOTS];
static uint32_t pid_slot_next = 0;
static vfs_node_t *procfs_root = NULL;

static vfs_node_t *procfs_pid_readdir(vfs_node_t *node, uint32_t index) {
    procfs_pid_slot_t *slot = (procfs_pid_slot_t *)node->ptr;
    if (!slot || index >= PROCFS_PID_FILES) return NULL;
    return &slot->nodes[index];
}

static vfs_node_t *procfs_pid_finddir(vfs_node_t *node, const char *name) {
    procfs_pid_slot_t *slot = (procfs_pid_slot_t *)node->ptr;
    if (!slot) return NULL;
    for (uint32_t i = 0; i < PROCFS_PID_FILES; i++) {
        if (strcmp(slot->nodes[i].name, name) == 0) return &slot->nodes[i];
    }
    return NULL;
}

static vfs_file_operations_t procfs_pid_dir_ops = {
    .readdir = procfs_pid_readdir,
    .finddir = procfs_pid_finddir,
};

static vfs_node_t *procfs_pid_dir(uint64_t pid) {
    for (uint32_t i = 0; i < PROCFS_PID_SLOTS; i++) {
        if (pid_slots[i].dir.ops && pid_slots[i].pid == pid) return &pid_slots[i].dir;
    }

    procfs_pid_slot_t *slot = &pid_slots[pid_slot_next];
    pid_slot_next = (pid_slot_next + 1) % PROCFS_PID_SLOTS;

    // Generated-content buffers (buf.data) are kept across reuse: same size for every pid
    for (uint32_t i = 0; i < PROCFS_PID_FILES; i++) {
        procfs_init_file(&slot->nodes[i], &slot->files[i], pid_entries[i].name, pid_entries[i].gen,
                         pid, PROCFS_PID_BUF_SIZE, &slot->dir);
    }

    memset(&slot->dir, 0, sizeof(slot->dir));
    snprintf(slot->dir.name, sizeof(slot->dir.name), "%lu", pid);
    slot->dir.flags = FS_DIRECTORY;
    slot->dir.permissions = 0555;
    slot->dir.ops = &procfs_pid_dir_ops;
    slot->dir.ptr = (vfs_node_t *)slot;
    slot->dir.parent = procfs_root;
    slot->pid = pid;
    return &slot->dir;
}

static bool parse_pid(const char *s, uint64_t *out) {
    if (!s || *s < '0' || *s > '9') return false;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    *out = v;
    return true;
}

// The n-th distinct live process in the task list (threads share one process)
static process_t *nth_process(uint32_t n) {
    if (!task_list) return NULL;
    task_t *curr = task_list;
    uint32_t seen = 0;
    do {
        process_t *p = curr->process;
        if (p && p->pid != 0) {
            bool first = true;
            for (task_t *prev = task_list; prev != curr; prev = prev->next) {
                if (prev->process == p) { first = false; break; }
            }
            if (first) {
                if (seen == n) return p;
                seen++;
            }
        }
        curr = curr->next;
    } while (curr && curr != task_list);
    return NULL;
}

static const struct {
    const char *name;
    procfs_gen_t gen;
} sys_entries[] = {
    {"cpuinfo", gen_cpuinfo},
    {"meminfo", gen_meminfo},
    {"uptime", gen_uptime},
    {"version", gen_version},
    {"swaps", gen_swaps},
    {"loadavg", gen_loadavg},
};
#define PROCFS_SYS_FILES (sizeof(sys_entries) / sizeof(sys_entries[0]))

static vfs_node_t sys_nodes[PROCFS_SYS_FILES];
static procfs_file_t sys_files[PROCFS_SYS_FILES];

static vfs_node_t *procfs_root_readdir(vfs_node_t *node, uint32_t index) {
    (void)node;
    if (index < PROCFS_SYS_FILES) return &sys_nodes[index];
    process_t *p = nth_process(index - (uint32_t)PROCFS_SYS_FILES);
    return p ? procfs_pid_dir(p->pid) : NULL;
}

static vfs_node_t *procfs_root_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    for (uint32_t i = 0; i < PROCFS_SYS_FILES; i++) {
        if (strcmp(sys_nodes[i].name, name) == 0) return &sys_nodes[i];
    }

    if (strcmp(name, "self") == 0 || strcmp(name, "thread-self") == 0) {
        if (!current_task || !current_task->process) return NULL;
        return procfs_pid_dir(current_task->process->pid);
    }

    uint64_t pid;
    if (parse_pid(name, &pid) && pid != 0 && process_find(pid)) {
        return procfs_pid_dir(pid);
    }
    return NULL;
}

static vfs_file_operations_t procfs_root_ops = {
    .readdir = procfs_root_readdir,
    .finddir = procfs_root_finddir,
};

// ---------------------------------------------------------------------------
// Magic links
// ---------------------------------------------------------------------------

static void node_path(vfs_node_t *node, char *out, size_t cap) {
    // Pipes and sockets are anonymous, like Linux "pipe:[ino]"
    if (!node->parent) {
        if (strncmp(node->name, "pipe:", 5) == 0) {
            snprintf(out, cap, "pipe:[%lu]", (uint64_t)(uintptr_t)node);
        } else {
            snprintf(out, cap, "/dev/%s", node->name);
        }
        return;
    }

    const char *parts[16];
    int depth = 0;
    for (vfs_node_t *n = node; n && depth < 16; n = n->parent) {
        if (n->parent == n || strcmp(n->name, "/") == 0) break;
        parts[depth++] = n->name;
        if (!n->parent) break;
    }

    size_t len = 0;
    out[0] = '\0';
    for (int i = depth - 1; i >= 0 && len + 1 < cap; i--) {
        len += (size_t)snprintf(out + len, cap - len, "/%s", parts[i]);
    }
    if (len == 0) snprintf(out, cap, "/");
}

int64_t procfs_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || strncmp(path, "/proc/", 6) != 0) return -EINVAL;

    const char *p = path + 6;
    process_t *proc = NULL;
    if (strncmp(p, "self/", 5) == 0) {
        proc = current_task ? current_task->process : NULL;
        p += 5;
    } else {
        uint64_t pid;
        if (!parse_pid(p, &pid)) return -EINVAL;
        while (*p >= '0' && *p <= '9') p++;
        if (*p != '/') return -EINVAL;
        p++;
        proc = process_find(pid);
    }
    if (!proc) return -ENOENT;

    char target[256];
    if (strcmp(p, "exe") == 0) {
        if (!proc->exe_path[0]) return -ENOENT;
        strncpy(target, proc->exe_path, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    } else if (strcmp(p, "cwd") == 0) {
        strncpy(target, proc->cwd, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    } else if (strncmp(p, "fd/", 3) == 0) {
        uint64_t fd;
        if (!parse_pid(p + 3, &fd) || fd >= MAX_OPEN_FILES || !proc->files[fd]) return -ENOENT;
        node_path(proc->files[fd], target, sizeof(target));
    } else {
        return -EINVAL;
    }

    size_t len = strlen(target);
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, target, len);
    return (int64_t)len;
}

static int __init procfs_initcall(void) {
    procfs_root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!procfs_root) return -1;
    strcpy(procfs_root->name, "proc");
    procfs_root->flags = FS_DIRECTORY;
    procfs_root->permissions = 0555;
    procfs_root->ops = &procfs_root_ops;

    for (uint32_t i = 0; i < PROCFS_SYS_FILES; i++) {
        procfs_init_file(&sys_nodes[i], &sys_files[i], sys_entries[i].name, sys_entries[i].gen,
                         0, PROCFS_SYS_BUF_SIZE, procfs_root);
    }

    if (!vfs_mount("/proc", procfs_root)) {
        serial_puts(COM1, "[PROCFS] Failed to mount /proc.\n");
        return -1;
    }
    serial_puts(COM1, "[PROCFS] Mounted /proc.\n");
    return 0;
}
late_initcall(procfs_initcall);
