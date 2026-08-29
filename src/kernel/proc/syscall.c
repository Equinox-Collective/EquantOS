// syscall.c - System Call Dispatcher & Native x86_64 Syscall Setup
#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "../core/mem/vmm.h"
#include "../core/mem/pmm.h"
#include "../core/mem/memory.h"
#include "../core/gen/cpu.h"
#include "../drivers/serial/serial.h"
#include "../../equterm/term.h"
#include "../misc/timer.h"
#include "string.h"
#include "../fs/vfs.h"
#include "../core/initcall.h"
#include "../drivers/tty/tty.h"

__attribute__((aligned(16))) uint64_t syscall_user_rsp = 0;

#define IA32_EFER        0xC0000080
#define IA32_STAR        0xC0000081
#define IA32_LSTAR       0xC0000082
#define IA32_FMASK       0xC0000084
#define IA32_FS_BASE_MSR 0xC0000100

#define AT_FDCWD         -100
#define O_CREAT          0100

#define ARCH_SET_FS      0x1002
#define ARCH_GET_FS      0x1003

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

void linux_syscall_handler(void *regs_ptr) {
    syscall_handler(regs_ptr);
}



extern void syscall_entry_asm(void);

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} __attribute__((packed)) syscall_regs_t;

static uint64_t mmap_virtual_base = 0x700000000000ULL;

static void resolve_user_path(const char *input, char *output, size_t max_len) {
    if (!input || input[0] == '\0') {
        if (current_task && current_task->process && current_task->process->cwd[0] != '\0') {
            strncpy(output, current_task->process->cwd, max_len);
        } else {
            strcpy(output, "/");
        }
        return;
    }

    if (input[0] == '/') {
        strncpy(output, input, max_len);
    } else {
        if (current_task && current_task->process && current_task->process->cwd[0] != '\0') {
            strcpy(output, current_task->process->cwd);
        } else {
            strcpy(output, "/");
        }
        if (output[strlen(output) - 1] != '/') {
            strcat(output, "/");
        }
        strcat(output, input);
    }
}

static int alloc_fd(vfs_node_t *node) {
    if (!current_task || !current_task->process) return -EMFILE;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (current_task->process->files[i] == NULL) {
            current_task->process->files[i] = node;
            return i;
        }
    }
    return -EMFILE;
}

static int64_t sys_openat_handler(int dirfd, const char *pathname, int flags, int mode) {
    (void)dirfd;
    if (!pathname) return -EINVAL;

    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);

    // Поддержка создания файла при O_CREAT
    if (!node && (flags & O_CREAT)) {
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

        vfs_node_t *parent_dir = vfs_open(parent_path[0] == '\0' ? "/" : parent_path, 0);
        if (parent_dir) {
            node = vfs_create(parent_dir, filename, mode);
        }
    }

    if (!node) return -ENOENT;

    return alloc_fd(node);
}

#include "../drivers/tty/tty.h"

// Обработчик сисколла read (sys_read)
static int64_t sys_read_handler(int fd, void *buf, size_t count) {
    if (count == 0) return 0;
    if (!buf) return -EINVAL;

    // === STDIN (Клавиатура / Serial) ===
    if (fd == 0) {
        char *out = (char *)buf;
        char c = tty_getchar();

        // Превращаем возврат каретки \r в каноничный Unix \n
        if (c == '\r') {
            c = '\n';
        }

        out[0] = c;
        return 1;
    }

    // === Обычные файлы VFS ===
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    return vfs_read(node, 0, count, (uint8_t *)buf);
}

static int64_t sys_close_handler(int fd) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 3 || fd >= MAX_OPEN_FILES) return -EBADF;

    if (current_task->process->files[fd]) {
        vfs_close(current_task->process->files[fd]);
        current_task->process->files[fd] = NULL;
    }
    return 0;
}

static int64_t sys_getcwd_handler(char *buf, size_t size) {
    if (!buf || size == 0) return -EINVAL;
    const char *cwd = (current_task && current_task->process && current_task->process->cwd[0] != '\0')
                      ? current_task->process->cwd : "/";
    size_t len = strlen(cwd) + 1;
    if (size < len) return -ERANGE;

    memcpy(buf, cwd, len);
    return (int64_t)len; // <-- ВОЗВРАЩАЕМ ДЛИНУ СТРОКИ, А НЕ 0!
}

static int64_t sys_chdir_handler(const char *path) {
    if (!path) return -EINVAL;

    char resolved[256];
    resolve_user_path(path, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node || !(node->flags & FS_DIRECTORY)) return -ENOENT;

    if (current_task && current_task->process) {
        strncpy(current_task->process->cwd, resolved, sizeof(current_task->process->cwd) - 1);
    }
    return 0;
}

static int64_t sys_getdents64_handler(int fd, void *dirp, size_t count) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *dir = current_task->process->files[fd];
    if (!dir || !(dir->flags & FS_DIRECTORY)) return -ENOTDIR;

    uint8_t *out_buf = (uint8_t *)dirp;
    size_t written = 0;
    uint32_t idx = 0;

    vfs_node_t *child = NULL;
    while ((child = vfs_readdir(dir, idx++)) != NULL) {
        size_t name_len = strlen(child->name);
        size_t rec_len = (sizeof(struct linux_dirent64) + name_len + 1 + 7) & ~7;

        if (written + rec_len > count) break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)(out_buf + written);
        d->d_ino = child->inode ? child->inode : idx;
        d->d_off = idx * 32;
        d->d_reclen = (unsigned short)rec_len;
        d->d_type = (child->flags & FS_DIRECTORY) ? 4 : 8;
        strcpy(d->d_name, child->name);

        written += rec_len;
        kfree(child);
    }

    return (int64_t)written;
}

static int64_t sys_exit_handler(int code) {
    serial_puts(COM1, "[KERNEL] User process exited with code: ");
    char buf[32];
    itoa_hex(code, buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    if (current_task) {
        current_task->state = TASK_STATE_ZOMBIE;
        current_task->running = false;
        sched_dequeue(current_task); // <-- ВОТ ОНА! ВЫБРАСЫВАЕМ ЗОМБИ ИЗ ОЧЕРЕДИ!
    }
    
    sched_yield();
    for(;;);
    return 0;
}

static int64_t sys_stat_handler(const char *pathname, struct linux_stat *statbuf) {
    if (!pathname || !statbuf) return -EINVAL;

    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) {
        return -ENOENT; // ВАЖНО: возвращаем -2 (Файл не найден), а не -ENOSYS!
    }

    memset(statbuf, 0, sizeof(struct linux_stat));
    statbuf->st_ino = node->inode ? node->inode : 1;
    statbuf->st_nlink = 1;
    statbuf->st_uid = 0; // root
    statbuf->st_gid = 0; // root
    statbuf->st_size = node->length;
    statbuf->st_blksize = 4096;
    statbuf->st_blocks = (node->length + 511) / 512;

    if (node->flags & FS_DIRECTORY) {
        statbuf->st_mode = S_IFDIR | 0755;
    } else {
        statbuf->st_mode = S_IFREG | 0777; // Исполняемый файл
    }

    return 0;
}

static int64_t sys_fstat_handler(int fd, struct linux_stat *statbuf) {
    if (!statbuf) return -EINVAL;

    // Для STDIN (0), STDOUT (1), STDERR (2) сообщаем, что это TTY (терминал)
    if (fd >= 0 && fd <= 2) {
        memset(statbuf, 0, sizeof(struct linux_stat));
        statbuf->st_mode = S_IFCHR | 0666; // Символьное устройство
        statbuf->st_rdev = 0x0501;         // /dev/tty
        return 0;
    }

    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    memset(statbuf, 0, sizeof(struct linux_stat));
    statbuf->st_ino = node->inode ? node->inode : 1;
    statbuf->st_nlink = 1;
    statbuf->st_size = node->length;
    statbuf->st_blksize = 4096;
    statbuf->st_blocks = (node->length + 511) / 512;
    statbuf->st_mode = (node->flags & FS_DIRECTORY) ? (S_IFDIR | 0755) : (S_IFREG | 0777);

    return 0;
}

static int64_t sys_arch_prctl_handler(int code, uint64_t addr) {
    if (!current_task) return -EINVAL;

    if (code == ARCH_SET_FS) {
        current_task->fs_base = addr;
        write_msr(IA32_FS_BASE_MSR, addr);
        return 0;
    } else if (code == ARCH_GET_FS) {
        if (!addr) return -EINVAL;
        *(uint64_t *)addr = current_task->fs_base;
        return 0;
    }
    return -EINVAL;
}

static int64_t sys_brk_handler(uint64_t new_brk) {
    if (!current_task || !current_task->process) return 0;
    
    uint64_t old_brk = current_task->process->brk;
    if (new_brk == 0) {
        return old_brk;
    }

    if (new_brk <= old_brk) {
        current_task->process->brk = new_brk;
        return new_brk;
    }

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    
    uint64_t start_page = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start_page; addr < end_page; addr += PAGE_SIZE) {
        if (vmm_get_phys(pml4, addr) != 0) {
            continue;
        }

        void *phys = pmm_alloc();
        if (!phys) {
            return old_brk;
        }
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, addr, (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    current_task->process->brk = new_brk;
    return new_brk;
}

static int64_t sys_write_handler(int fd, const void *user_buf, size_t count) {
    if (fd == 1 || fd == 2) {
        const char *buf = (const char *)user_buf;
        for (size_t i = 0; i < count; i++) {
            char c = buf[i];
            serial_putchar(COM1, c);
            term_putchar_raw(c);
        }
        return count;
    }

    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 3 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    return vfs_write(node, 0, count, (uint8_t *)user_buf);
}

static int64_t sys_writev_handler(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base && iov[i].iov_len > 0) {
            int64_t ret = sys_write_handler(fd, iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return ret;
            total += ret;
        }
    }
    return total;
}

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

static int64_t sys_ioctl_handler(int fd, uint64_t req, void *arg) {
    if (fd >= 0 && fd <= 2) {
        if (req == TIOCGWINSZ && arg) {
            struct winsize *ws = (struct winsize *)arg;
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = 640;
            ws->ws_ypixel = 480;
            return 0;
        }
        // Заглушки для настроек терминала (TCGETS, TCSETS, FIONREAD)
        return 0;
    }
    return -EINVAL;
}

static int64_t sys_mmap_handler(uint64_t addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    (void)prot; (void)flags; (void)fd; (void)offset;
    if (length == 0) return -EINVAL;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_len = page_count * PAGE_SIZE;

    uint64_t virt_addr = addr;
    if (virt_addr == 0) {
        virt_addr = mmap_virtual_base;
        mmap_virtual_base += aligned_len;
    }

    if (!current_task || !current_task->process) return -EINVAL;

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);

    for (size_t i = 0; i < page_count; i++) {
        void *phys = pmm_alloc();
        if (!phys) return -ENOMEM;
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, virt_addr + (i * PAGE_SIZE), (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    return (int64_t)virt_addr;
}

static int64_t sys_munmap_handler(uint64_t addr, size_t length) {
    (void)addr; (void)length;
    return 0;
}

static int64_t sys_mprotect_handler(uint64_t addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

static int64_t sys_uname_handler(struct linux_utsname *buf) {
    if (!buf) return -EINVAL;

    memset(buf, 0, sizeof(struct linux_utsname));
    strcpy(buf->sysname, "EquantOS");
    strcpy(buf->nodename, "equant");
    strcpy(buf->release, "0.0.1-alpha");
    strcpy(buf->version, "EquantOS Kernel v0.0.1 (x86_64)");
    strcpy(buf->machine, "x86_64");
    strcpy(buf->domainname, "localdomain");

    return 0;
}

static int64_t sys_clock_gettime_handler(int clock_id, struct linux_timespec *tp) {
    (void)clock_id;
    if (!tp) return -EINVAL;

    uint64_t current_ticks = tick;
    tp->tv_sec = current_ticks / 100;
    tp->tv_nsec = (current_ticks % 100) * 10000000ULL;

    return 0;
}

static int64_t sys_nanosleep_handler(const struct linux_timespec *req, struct linux_timespec *rem) {
    (void)rem;
    if (!req) return -EINVAL;

    uint64_t target_tick = tick + (req->tv_sec * 100 + req->tv_nsec / 10000000ULL);
    
    if (target_tick > tick) {
        __asm__ volatile("sti");
        while (tick < target_tick) {
            __asm__ volatile("hlt");
        }
        __asm__ volatile("cli");
    }
    return 0;
}

static int64_t sys_rt_sigaction_handler(int signum, const void *act, void *oldact, size_t sigsetsize) {
    (void)signum; (void)act; (void)oldact; (void)sigsetsize;
    return 0;
}

struct linux_pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN  0x0001
#define POLLOUT 0x0004

static bool tty_has_input(void) {
    return serial_received(COM1) || input_has_events();
}

static int64_t sys_poll_handler(struct linux_pollfd *fds, uint64_t nfds, int timeout) {
    if (!fds && nfds > 0) return -EINVAL;

    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;

        if (fds[i].fd == 0) { // STDIN
            if (tty_has_input()) {
                fds[i].revents |= (fds[i].events & POLLIN);
                if (fds[i].revents) ready++;
            }
        } else if (fds[i].fd == 1 || fds[i].fd == 2) { // STDOUT / STDERR
            fds[i].revents |= (fds[i].events & POLLOUT);
            if (fds[i].revents) ready++;
        }
    }

    // Если данные есть ИЛИ это неблокирующий опрос (timeout == 0) — выходим сразу
    if (ready > 0 || timeout == 0) {
        return ready;
    }

    // Если timeout > 0 или -1 (бесконечно) — ждем появления символа
    uint64_t start_tick = tick;
    uint64_t max_ticks = (timeout < 0) ? (uint64_t)-1 : (timeout / 10);

    while (ready == 0) {
        if (timeout >= 0 && (tick - start_tick) >= max_ticks) {
            break; // Таймаут истек
        }

        if (tty_has_input()) {
            for (uint64_t i = 0; i < nfds; i++) {
                if (fds[i].fd == 0) {
                    fds[i].revents |= (fds[i].events & POLLIN);
                    if (fds[i].revents) ready++;
                }
            }
            break;
        }

        __asm__ volatile("sti");
        __asm__ volatile("pause");
        sched_yield();
    }

    return ready;
}
extern uint64_t pmm_get_total_memory(void);
extern uint64_t pmm_get_used_memory(void);
extern uint64_t total_pages;
extern size_t used_memory;

static int64_t sys_sysinfo_handler(equant_sysinfo_t *info) {
    if (!info) return -EINVAL;

    equant_sysinfo_t kinfo;
    memset(&kinfo, 0, sizeof(equant_sysinfo_t));

    kinfo.total_ram = pmm_get_total_memory();
    kinfo.used_ram = pmm_get_used_memory();
    kinfo.free_ram = (kinfo.total_ram > kinfo.used_ram) ? (kinfo.total_ram - kinfo.used_ram) : 0;
    kinfo.pmm_total_pages = total_pages;
    kinfo.pmm_used_pages = kinfo.used_ram / PAGE_SIZE;
    kinfo.kernel_heap_used = used_memory;

    memcpy(info, &kinfo, sizeof(equant_sysinfo_t));
    return 0;
}

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

static int64_t sys_fcntl_handler(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;
    switch (cmd) {
        case 0: /* F_DUPFD */
        case 1030: /* F_DUPFD_CLOEXEC */
            for (int i = (int)arg; i < MAX_OPEN_FILES; i++) {
                if (current_task->process->files[i] == NULL) {
                    current_task->process->files[i] = current_task->process->files[fd];
                    return i;
                }
            }
            return -EMFILE;
        case 1: /* F_GETFD */ return 0;
        case 2: /* F_SETFD */ return 0;
        case 3: /* F_GETFL */ return 2; // O_RDWR
        case 4: /* F_SETFL */ return 0;
        default: return 0;
    }
}

static int64_t sys_dup2_handler(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || !current_task->process->files[oldfd]) return -EBADF;
    if (newfd < 0 || newfd >= MAX_OPEN_FILES) return -EBADF;
    if (oldfd == newfd) return newfd;

    if (current_task->process->files[newfd]) {
        vfs_close(current_task->process->files[newfd]);
    }
    current_task->process->files[newfd] = current_task->process->files[oldfd];
    return newfd;
}

static int64_t sys_access_handler(const char *pathname, int mode) {
    (void)mode;
    if (!pathname) return -EINVAL;
    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));
    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;
    return 0; // Файл существует и доступен
}

// Таблица имен для отладки
// static const char *syscall_name(uint64_t num) {
//     switch (num) {
//         case SYS_READ: return "read";
//         case SYS_WRITE: return "write";
//         case SYS_OPEN: return "open";
//         case SYS_CLOSE: return "close";
//         case SYS_STAT: return "stat";
//         case SYS_FSTAT: return "fstat";
//         case SYS_POLL: return "poll";
//         case SYS_MMAP: return "mmap";
//         case SYS_MPROTECT: return "mprotect";
//         case SYS_MUNMAP: return "munmap";
//         case SYS_BRK: return "brk";
//         case SYS_RT_SIGACTION: return "rt_sigaction";
//         case SYS_RT_SIGPROCMASK: return "rt_sigprocmask";
//         case SYS_IOCTL: return "ioctl";
//         case SYS_WRITEV: return "writev";
//         case SYS_SCHED_YIELD: return "sched_yield";
//         case SYS_NANOSLEEP: return "nanosleep";
//         case SYS_GETPID: return "getpid";
//         case SYS_EXIT: return "exit";
//         case SYS_UNAME: return "uname";
//         case SYS_GETCWD: return "getcwd";
//         case SYS_CHDIR: return "chdir";
//         case SYS_GETUID: return "getuid";
//         case SYS_GETGID: return "getgid";
//         case SYS_GETEUID: return "geteuid";
//         case SYS_GETEGID: return "getegid";
//         case SYS_GETPPID: return "getppid";
//         case SYS_ARCH_PRCTL: return "arch_prctl";
//         case SYS_GETDENTS64: return "getdents64";
//         case SYS_SET_TID_ADDRESS: return "set_tid_address";
//         case SYS_CLOCK_GETTIME: return "clock_gettime";
//         case SYS_EXIT_GROUP: return "exit_group";
//         case SYS_OPENAT: return "openat";
//         default: return "UNKNOWN";
//     }
// }

void syscall_handler(void *regs_ptr) {
    syscall_regs_t *regs = (syscall_regs_t *)regs_ptr;
    uint64_t syscall_no = regs->rax;
    int64_t ret = -ENOSYS;

    // === ЛОГИРОВАНИЕ В COM1 (QEMU stdio) ===
    // serial_puts(COM1, "[STRACE] ");
    // serial_puts(COM1, syscall_name(syscall_no));
    // serial_puts(COM1, "(");
    
    // // Если сискол работает со строками путей (open, stat, chdir) — выводим строку:
    // if (syscall_no == SYS_OPEN || syscall_no == SYS_STAT || syscall_no == SYS_CHDIR) {
    //     serial_puts(COM1, "\"");
    //     serial_puts(COM1, (const char *)regs->rdi);
    //     serial_puts(COM1, "\"");
    // } else if (syscall_no == SYS_OPENAT) {
    //     serial_puts(COM1, "\"");
    //     serial_puts(COM1, (const char *)regs->rsi);
    //     serial_puts(COM1, "\"");
    // } else {
    //     char arg_buf[32];
    //     itoa_hex(regs->rdi, arg_buf);
    //     serial_puts(COM1, arg_buf);
    // }
    // serial_puts(COM1, ")\n");

    // === ДИСПЕТЧЕРИЗАЦИЯ СИСКОЛОВ ===
    switch (syscall_no) {
        case SYS_READ:
            ret = sys_read_handler((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_WRITE:
            ret = sys_write_handler((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_OPEN:
            ret = sys_openat_handler(AT_FDCWD, (const char *)regs->rdi, (int)regs->rsi, (int)regs->rdx);
            break;
        case SYS_CLOSE:
            ret = sys_close_handler((int)regs->rdi);
            break;
        case SYS_STAT:
            ret = sys_stat_handler((const char *)regs->rdi, (struct linux_stat *)regs->rsi);
            break;
        case SYS_FSTAT:
            ret = sys_fstat_handler((int)regs->rdi, (struct linux_stat *)regs->rsi);
            break;
        case SYS_WRITEV:
            ret = sys_writev_handler((int)regs->rdi, (const struct iovec *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_GETCWD:
            ret = sys_getcwd_handler((char *)regs->rdi, (size_t)regs->rsi);
            break;
        case SYS_POLL:
            ret = sys_poll_handler((struct linux_pollfd *)regs->rdi, regs->rsi, (int)regs->rdx);
            break;
        case SYS_GETEUID:
        case SYS_GETEGID:
            ret = 0;
            break;
        case SYS_FCNTL:
            ret = sys_fcntl_handler((int)regs->rdi, (int)regs->rsi, regs->rdx);
            break;
        case SYS_CHDIR:
            ret = sys_chdir_handler((const char *)regs->rdi);
            break;
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            ret = sys_exit_handler((int)regs->rdi);
            break;
        case SYS_BRK:
            ret = sys_brk_handler(regs->rdi);
            break;
        case SYS_MMAP:
            ret = sys_mmap_handler(regs->rdi, (size_t)regs->rsi, (int)regs->rdx, (int)regs->r10, (int)regs->r8, (int64_t)regs->r9);
            break;
        case SYS_MUNMAP:
            ret = sys_munmap_handler(regs->rdi, (size_t)regs->rsi);
            break;
        case SYS_MPROTECT:
            ret = sys_mprotect_handler(regs->rdi, (size_t)regs->rsi, (int)regs->rdx);
            break;
        case SYS_RT_SIGACTION:
            ret = sys_rt_sigaction_handler((int)regs->rdi, (const void *)regs->rsi, (void *)regs->rdx, (size_t)regs->r10);
            break;
        case SYS_RT_SIGPROCMASK:
            ret = 0;
            break;
        case SYS_SCHED_YIELD:
            sched_yield();
            ret = 0;
            break;
        case SYS_NANOSLEEP:
            ret = sys_nanosleep_handler((const struct linux_timespec *)regs->rdi, (struct linux_timespec *)regs->rsi);
            break;
        case SYS_ARCH_PRCTL:
            ret = sys_arch_prctl_handler((int)regs->rdi, regs->rsi);
            break;
        case SYS_TKILL:
        case SYS_TGKILL:
            ret = 0;
            break;
        case SYS_GETDENTS64:
            ret = sys_getdents64_handler((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_GETPID:
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_GETPPID:
            ret = 1;
            break;
        case SYS_GETUID:
        case SYS_GETGID:
            ret = 0;
            break;
        case SYS_UNAME:
            ret = sys_uname_handler((struct linux_utsname *)regs->rdi);
            break;
        case SYS_SET_TID_ADDRESS:
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_CLOCK_GETTIME:
            ret = sys_clock_gettime_handler((int)regs->rdi, (struct linux_timespec *)regs->rsi);
            break;
        case SYS_FUTEX:
            ret = 0;
            break;
        case SYS_OPENAT:
            ret = sys_openat_handler((int)regs->rdi, (const char *)regs->rsi, (int)regs->rdx, (int)regs->r10);
            break;
        case SYS_IOCTL:
            ret = sys_ioctl_handler((int)regs->rdi, regs->rsi, (void *)regs->rdx);
            break;
        case SYS_SYSINFO:
            ret = sys_sysinfo_handler((equant_sysinfo_t *)regs->rdi);
            break;
        default:
            serial_puts(COM1, "[KERNEL] Unknown Syscall: 0x");
            char buf[32];
            itoa_hex(syscall_no, buf);
            serial_puts(COM1, buf);
            serial_puts(COM1, "\n");
            ret = -ENOSYS;
            break;
    }

    regs->rax = (uint64_t)ret;
}

void init_syscalls(void) {
    uint64_t efer = read_msr(IA32_EFER);
    write_msr(IA32_EFER, efer | 1);

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(IA32_STAR, star);

    write_msr(IA32_LSTAR, (uint64_t)syscall_entry_asm);
    write_msr(IA32_FMASK, 0x200);

    serial_puts(COM1, "[KERNEL] Native x86_64 Hardware 'syscall' MSRs Initialized.\n");
}

static int __init init_syscalls_initcall(void) {
    init_syscalls();
    return 0;
}
arch_initcall(init_syscalls_initcall);