// src/kernel/proc/syscall.c - Native x86_64 Linux System Call Dispatcher
#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "pipe.h"
#include "loader.h"
#include "../core/mem/vmm.h"
#include "../core/mem/pmm.h"
#include "../core/mem/memory.h"
#include "../core/gen/cpu.h"
#include "../drivers/serial/serial.h"
#include "../../equterm/term.h"
#include "../misc/timer.h"
#include "string.h"
#include "../drivers/tty/tty.h"
#include "stdio.h"
#include "../fs/vfs.h"
#include "../core/initcall.h"
#include "../drivers/tty/tty.h"
#include "../../equterm/shell.h"
#include "../ipc/af_unix.h"
#include "../ipc/shm.h"
#include "../drivers/input/evdev.h"
#include <stdarg.h>

static void strace_log(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_puts(COM1, buf); // Пишет только в окно хоста MINGW64!
}

__attribute__((aligned(16))) uint64_t syscall_user_rsp = 0;

extern void syscall_entry_asm(void);
extern uint64_t pmm_get_total_memory(void);
extern uint64_t pmm_get_used_memory(void);
extern uint64_t total_pages;
extern size_t used_memory;

static uint64_t mmap_virtual_base = 0x700000000000ULL;

void linux_syscall_handler(void *regs_ptr) {
    syscall_handler(regs_ptr);
}

extern bool tty_has_char(void);

static bool tty_has_input(void) {
    return tty_has_char();
}

// ============================================================================
// Helper Functions: File Descriptors, Paths, and Buffers
// ============================================================================

static void resolve_user_path(const char *input, char *output, size_t max_len) {
    if (!input || input[0] == '\0') {
        if (current_task && current_task->process && current_task->process->cwd[0] != '\0') {
            strncpy(output, current_task->process->cwd, max_len - 1);
        } else {
            strcpy(output, "/");
        }
        output[max_len - 1] = '\0';
        return;
    }

    if (input[0] == '/') {
        strncpy(output, input, max_len - 1);
    } else {
        if (current_task && current_task->process && current_task->process->cwd[0] != '\0') {
            strncpy(output, current_task->process->cwd, max_len - 1);
        } else {
            strcpy(output, "/");
        }
        size_t cur_len = strlen(output);
        if (cur_len > 0 && output[cur_len - 1] != '/' && cur_len + 1 < max_len) {
            strcat(output, "/");
            cur_len++;
        }
        size_t remaining = (max_len > cur_len + 1) ? (max_len - cur_len - 1) : 0;
        strncpy(output + cur_len, input, remaining);
    }
    output[max_len - 1] = '\0';
}

static int alloc_fd(vfs_node_t *node, uint32_t flags) {
    if (!current_task || !current_task->process) return -EMFILE;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (current_task->process->files[i] == NULL) {
            current_task->process->files[i] = node;
            current_task->process->file_offsets[i] = 0;
            current_task->process->file_flags[i] = flags;
            return i;
        }
    }
    return -EMFILE;
}

// ============================================================================
// 1. File Descriptor & I/O Handlers
// ============================================================================
static int64_t sys_kill_handler(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    extern task_t *task_list;
    if (!task_list) return -ESRCH;

    task_t *curr = task_list;
    bool found = false;
    do {
        if (curr->process && (pid <= 0 || (int)curr->process->pid == pid)) {
            found = true;
            if (sig == SIGKILL || sig == SIGTERM) {
                curr->process->exit_code = 128 + sig;
                curr->process->exited = true;
                curr->state = TASK_STATE_ZOMBIE;
            } else if (sig == SIGSTOP) {
                sched_block(curr);
            } else if (sig == SIGCONT) {
                sched_unblock(curr);
            }
        }
        curr = curr->next;
    } while (curr && curr != task_list);

    return found ? 0 : -ESRCH;
}

static inline bool validate_user_memory(const void *addr, size_t size, bool writeable) {
    (void)writeable;
    if (!addr) return false;
    uintptr_t uaddr = (uintptr_t)addr;
    // Canonical user-space boundary check for x86_64
    if (uaddr >= 0x0000800000000000ULL || (uaddr + size) > 0x0000800000000000ULL) {
        return false;
    }
    return true;
}

static int64_t sys_read_handler(int fd, void *buf, size_t count) {
    if (count == 0) return 0;
    if (!validate_user_memory(buf, count, true)) return -EFAULT;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node || !node->ops) return -EBADF;

    bool nonblock = (current_task->process->file_flags[fd] & O_NONBLOCK) != 0;

    // Fast-path: AF_UNIX sockets with explicit non-blocking awareness
    if (node->ops == &unix_socket_vfs_ops && node->ptr) {
        return unix_socket_read((unix_socket_t *)node->ptr, buf, count, nonblock);
    }

    if (nonblock) {
        if (strncmp(node->name, "tty", 3) == 0 || 
            strcmp(node->name, "input0") == 0 || 
            strcmp(node->name, "mouse") == 0) {
            if (!tty_has_input()) {
                return -EAGAIN;
            }
        }
    }
    // Return -EAGAIN on non-blocking mouse read if buffer is empty
    if (nonblock && (node->ops == &g_mousedev_fops || node->ops == &g_evdev_mouse_fops)) {
        if (!evdev_mouse_can_read()) {
            return -EAGAIN;
        }
    }
    if (!node->ops->read) return -EBADF;
    uint64_t offset = current_task->process->file_offsets[fd];
    int64_t bytes = vfs_read(node, offset, count, (uint8_t *)buf);
    
    if (bytes > 0) {
        current_task->process->file_offsets[fd] += bytes;
    }
    return bytes;
}

static int64_t sys_write_handler(int fd, const void *user_buf, size_t count) {
    if (count == 0) return 0;
    if (!validate_user_memory(user_buf, count, false)) return -EFAULT;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node || !node->ops) return -EBADF;

    bool nonblock = (current_task->process->file_flags[fd] & O_NONBLOCK) != 0;

    // Fast-path: AF_UNIX sockets
    if (node->ops == &unix_socket_vfs_ops && node->ptr) {
        return unix_socket_write((unix_socket_t *)node->ptr, user_buf, count, nonblock);
    }

    if (!node->ops->write) return -EBADF;

    uint64_t offset = current_task->process->file_offsets[fd];
    if (current_task->process->file_flags[fd] & O_APPEND) {
        offset = node->length;
    }

    int64_t bytes = vfs_write(node, offset, count, (uint8_t *)user_buf);
    if (bytes > 0) {
        current_task->process->file_offsets[fd] = offset + bytes;
    }
    return bytes;
}

static int64_t sys_lseek_handler(int fd, int64_t offset, int whence) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    int64_t new_offset = 0;
    switch (whence) {
        case SEEK_SET: new_offset = offset; break;
        case SEEK_CUR: new_offset = (int64_t)current_task->process->file_offsets[fd] + offset; break;
        case SEEK_END: new_offset = (int64_t)node->length + offset; break;
        default: return -EINVAL;
    }

    if (new_offset < 0) return -EINVAL;
    current_task->process->file_offsets[fd] = (uint64_t)new_offset;
    return new_offset;
}

static int64_t sys_pread64_handler(int fd, void *buf, size_t count, int64_t offset) {
    if (count == 0) return 0;
    if (!buf || offset < 0) return -EINVAL;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    return vfs_read(node, (uint64_t)offset, count, (uint8_t *)buf);
}

static int64_t sys_pwrite64_handler(int fd, const void *buf, size_t count, int64_t offset) {
    if (count == 0) return 0;
    if (!buf || offset < 0) return -EINVAL;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    return vfs_write(node, (uint64_t)offset, count, (uint8_t *)buf);
}

static int64_t sys_readv_handler(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base && iov[i].iov_len > 0) {
            int64_t ret = sys_read_handler(fd, iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return (total > 0) ? total : ret;
            total += ret;
        }
    }
    return total;
}

static int64_t sys_writev_handler(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base && iov[i].iov_len > 0) {
            int64_t ret = sys_write_handler(fd, iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return (total > 0) ? total : ret;
            total += ret;
        }
    }
    return total;
}

static int64_t sys_openat_handler(int dirfd, const char *pathname, int flags, int mode) {
    (void)dirfd;
    if (!pathname) return -EINVAL;

    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);

    if (!node && (flags & O_CREAT)) {
        char parent_path[256];
        strncpy(parent_path, resolved, sizeof(parent_path) - 1);
        parent_path[sizeof(parent_path) - 1] = '\0';
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
            node = vfs_create(parent_dir, filename, mode ? mode : 0644);
        }
    }

    if (!node) return -ENOENT;
    if ((flags & O_DIRECTORY) && !(node->flags & FS_DIRECTORY)) {
        return -ENOTDIR;
    }

    if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR))) {
        node->length = 0;
    }

    return alloc_fd(node, (uint32_t)flags);
}

static int64_t sys_close_handler(int fd) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    vfs_close(node);
    current_task->process->files[fd] = NULL;
    current_task->process->file_offsets[fd] = 0;
    current_task->process->file_flags[fd] = 0;
    return 0;
}

static int64_t sys_dup_handler(int oldfd) {
    if (!current_task || !current_task->process) return -EBADF;
    if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || !current_task->process->files[oldfd]) return -EBADF;

    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (current_task->process->files[i] == NULL) {
            current_task->process->files[i] = current_task->process->files[oldfd];
            current_task->process->file_offsets[i] = current_task->process->file_offsets[oldfd];
            current_task->process->file_flags[i] = current_task->process->file_flags[oldfd];
            return i;
        }
    }
    return -EMFILE;
}

static int64_t sys_dup2_handler(int oldfd, int newfd) {
    if (!current_task || !current_task->process) return -EBADF;
    if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || !current_task->process->files[oldfd]) return -EBADF;
    if (newfd < 0 || newfd >= MAX_OPEN_FILES) return -EBADF;
    if (oldfd == newfd) return newfd;

    if (current_task->process->files[newfd]) {
        vfs_close(current_task->process->files[newfd]);
    }
    current_task->process->files[newfd] = current_task->process->files[oldfd];
    current_task->process->file_offsets[newfd] = current_task->process->file_offsets[oldfd];
    current_task->process->file_flags[newfd] = current_task->process->file_flags[oldfd];
    return newfd;
}

static int64_t sys_dup3_handler(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) return -EINVAL;
    int64_t ret = sys_dup2_handler(oldfd, newfd);
    if (ret >= 0 && (flags & O_CLOEXEC)) {
        current_task->process->file_flags[newfd] |= O_CLOEXEC;
    }
    return ret;
}

static int64_t sys_pipe2_handler(int *pipefd, int flags) {
    if (!pipefd) return -EFAULT;
    int res = pipe_create(pipefd);
    if (res == 0 && (flags & O_CLOEXEC)) {
        current_task->process->file_flags[pipefd[0]] |= O_CLOEXEC;
        current_task->process->file_flags[pipefd[1]] |= O_CLOEXEC;
    }
    return res;
}

static int64_t sys_fcntl_handler(int fd, int cmd, uint64_t arg) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !current_task->process->files[fd]) return -EBADF;

    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            for (int i = (int)arg; i < MAX_OPEN_FILES; i++) {
                if (current_task->process->files[i] == NULL) {
                    current_task->process->files[i] = current_task->process->files[fd];
                    current_task->process->file_offsets[i] = current_task->process->file_offsets[fd];
                    current_task->process->file_flags[i] = (cmd == F_DUPFD_CLOEXEC) ? 
                        (current_task->process->file_flags[fd] | O_CLOEXEC) : current_task->process->file_flags[fd];
                    return i;
                }
            }
            return -EMFILE;
        }
        case F_GETFD:
            return (current_task->process->file_flags[fd] & O_CLOEXEC) ? 1 : 0;
        case F_SETFD:
            if (arg & 1) current_task->process->file_flags[fd] |= O_CLOEXEC;
            else current_task->process->file_flags[fd] &= ~O_CLOEXEC;
            return 0;
        case F_GETFL:
            return current_task->process->file_flags[fd];
        case F_SETFL:
            current_task->process->file_flags[fd] = (uint32_t)arg;
            return 0;
        default:
            return -EINVAL;
    }
}

static int64_t sys_ioctl_handler(int fd, uint64_t req, void *arg) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];

    // Check if this fd is a terminal or console (fd 0..2 OR /dev/tty, /dev/tty0)
    bool is_tty = (fd >= 0 && fd <= 2);
    if (node && strncmp(node->name, "tty", 3) == 0) {
        is_tty = true;
    }

    if (is_tty) {
        // 1. Virtual Terminal Queries (Crucial for Xfbdev / Xorg startup!)
        if (req == VT_OPENQRY && arg) {
            *(int *)arg = 1; // Report VT 1 as free and available
            return 0;
        }
        if (req == VT_GETSTATE && arg) {
            struct { unsigned short v_active, v_signal, v_state; } *vts = arg;
            vts->v_active = 1;
            vts->v_state = 1;
            return 0;
        }
        if (req == VT_ACTIVATE || req == VT_WAITACTIVE || req == VT_SETMODE || req == VT_DISALLOCATE) {
            return 0; // VT switched successfully
        }
        if (req == KDSETMODE || req == KDSKBMODE) {
            return 0; // Graphics/Keyboard mode set
        }
        if (req == KDGETMODE && arg) {
            *(int *)arg = 0; // KD_TEXT
            return 0;
        }
        // Socket / Generic non-blocking control
    if (req == 0x5421 && arg) { // FIONBIO
        int on = *(int *)arg;
        if (on) current_task->process->file_flags[fd] |= O_NONBLOCK;
        else    current_task->process->file_flags[fd] &= ~O_NONBLOCK;
        return 0;
    }
        if (req == KDGKBMODE && arg) {
            *(int *)arg = 1; // K_XLATE
            return 0;
        }
         if (req == 0x4B46 || req == 0x4B47) { // KDGKBENT / KDSKBENT
            if (!arg) return -EFAULT;
            
            struct {
                unsigned char kb_table;
                unsigned char kb_index;
                unsigned short kb_value;
            } *kbe = arg;

            // Only support first 4 basic tables (plain, shift, altgr, ctrl) and 128 keys
            if (kbe->kb_table >= 4 || kbe->kb_index >= 128) {
                return -EINVAL; // Tells Xfbdev to stop looping!
            }

            // Return basic US keymap or let it break cleanly
            kbe->kb_value = 0;
            return -EINVAL; // Returning -EINVAL forces Xfbdev to fall back to its internal built-in keymap!
        }
        // 2. Window Size and Process Groups
        if (req == TIOCGWINSZ && arg) {
            struct winsize *ws = (struct winsize *)arg;
            uint64_t gw = (uint64_t)term_get_glyph_width();
            uint64_t gh = (uint64_t)term_get_glyph_height();
            if (gw == 0) gw = 8;
            if (gh == 0) gh = 16;
            ws->ws_col = (unsigned short)(term_get_fb_width() / gw);
            ws->ws_row = (unsigned short)(term_get_fb_height() / gh);
            ws->ws_xpixel = (unsigned short)term_get_fb_width();
            ws->ws_ypixel = (unsigned short)term_get_fb_height();
            return 0;
        }
        if (req == TIOCGPGRP && arg) {
            uint64_t pgid = (current_task->process->pgid) ? current_task->process->pgid : 1;
            *(int *)arg = (int)pgid;
            return 0;
        }
        if (req == TIOCSPGRP && arg) {
            int new_pgid = *(int *)arg;
            current_task->process->pgid = (uint64_t)(new_pgid > 0 ? new_pgid : current_task->process->pid);
            return 0;
        }
        if (req == FIONREAD && arg) {
            *(int *)arg = tty_has_input() ? 1 : 0;
            return 0;
        }
        if (req == TCGETS && arg) {
            struct termios *tio = (struct termios *)arg;
            memset(tio, 0, sizeof(struct termios));
            tio->c_iflag = 0x4500;
            tio->c_oflag = 0x0005;
            tio->c_cflag = 0x00BF;
            tio->c_lflag = 0x8A3B;
            return 0;
        }
        if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
            return 0;
        }
        return 0;
    }

    // Pass custom ioctls (e.g. /dev/fb0) to VFS node ops
    if (node && node->ops && node->ops->ioctl) {
        return node->ops->ioctl(node, req, arg);
    }

    return -ENOTTY;
}

static int64_t sys_link_handler(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EFAULT;
    char old_res[256], new_res[256];
    resolve_user_path(oldpath, old_res, sizeof(old_res));
    resolve_user_path(newpath, new_res, sizeof(new_res));

    vfs_node_t *old_node = vfs_open(old_res, 0);
    if (!old_node) return -ENOENT;

    char parent_path[256];
    strncpy(parent_path, new_res, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = '\0';
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
    if (!parent_dir) return -ENOENT;

    vfs_node_t *new_node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!new_node) return -ENOMEM;
    memcpy(new_node, old_node, sizeof(vfs_node_t));
    strncpy(new_node->name, filename, sizeof(new_node->name) - 1);
    new_node->parent = parent_dir;
    new_node->next = NULL;

    if (!parent_dir->children) {
        parent_dir->children = new_node;
    } else {
        vfs_node_t *curr = parent_dir->children;
        while (curr->next) curr = curr->next;
        curr->next = new_node;
    }

    return 0;
}
// ============================================================================
// 2. Filesystem Metadata & Directory Navigation
// ============================================================================

static void fill_linux_stat(vfs_node_t *node, struct linux_stat *statbuf) {
    memset(statbuf, 0, sizeof(struct linux_stat));
    statbuf->st_dev = 1;
    statbuf->st_ino = node->inode ? node->inode : 1;
    statbuf->st_nlink = 1;
    statbuf->st_uid = 0;
    statbuf->st_gid = 0;
    statbuf->st_size = node->length;
    statbuf->st_blksize = 4096;
    statbuf->st_blocks = (node->length + 511) / 512;

    if (node->flags & FS_DIRECTORY) {
        statbuf->st_mode = S_IFDIR | 0755;
    } else if (node->flags & FS_SOCKET) {
        statbuf->st_mode = S_IFSOCK | 0777; // Will show 's' in ls -la!
    } else {
        statbuf->st_mode = S_IFREG | 0777;
    }

    uint64_t cur_sec = tick / 100;
    statbuf->st_atim.tv_sec = cur_sec;
    statbuf->st_mtim.tv_sec = cur_sec;
    statbuf->st_ctim.tv_sec = cur_sec;
}

static int64_t sys_stat_handler(const char *pathname, struct linux_stat *statbuf) {
    if (!pathname || !statbuf) return -EFAULT;
    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;

    fill_linux_stat(node, statbuf);
    return 0;
}

static int64_t sys_fstat_handler(int fd, struct linux_stat *statbuf) {
    if (!statbuf) return -EFAULT;

    if (fd >= 0 && fd <= 2) {
        memset(statbuf, 0, sizeof(struct linux_stat));
        statbuf->st_mode = S_IFCHR | 0666;
        statbuf->st_rdev = 0x0501;
        statbuf->st_blksize = 4096;
        return 0;
    }

    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    fill_linux_stat(node, statbuf);
    return 0;
}

static int64_t sys_access_handler(const char *pathname, int mode) {
    (void)mode;
    if (!pathname) return -EFAULT;
    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));
    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;
    return 0;
}

static int64_t sys_getcwd_handler(char *buf, size_t size) {
    if (!buf || size == 0) return -EINVAL;
    const char *cwd = (current_task && current_task->process && current_task->process->cwd[0] != '\0')
                      ? current_task->process->cwd : "/";
    size_t len = strlen(cwd) + 1;
    if (size < len) return -ERANGE;
    memcpy(buf, cwd, len);
    return (int64_t)len;
}

static int64_t sys_chdir_handler(const char *path) {
    if (!path) return -EFAULT;
    char resolved[256];
    resolve_user_path(path, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node || !(node->flags & FS_DIRECTORY)) return -ENOENT;

    if (current_task && current_task->process) {
        strncpy(current_task->process->cwd, resolved, sizeof(current_task->process->cwd) - 1);
        current_task->process->cwd[sizeof(current_task->process->cwd) - 1] = '\0';
    }
    return 0;
}

static int64_t sys_mkdirat_handler(int dirfd, const char *pathname, int mode) {
    (void)dirfd;
    if (!pathname) return -EFAULT;
    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));

    char parent_path[256];
    strncpy(parent_path, resolved, sizeof(parent_path) - 1);
    parent_path[sizeof(parent_path) - 1] = '\0';
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

    vfs_node_t *parent = vfs_open(parent_path[0] == '\0' ? "/" : parent_path, 0);
    if (!parent) return -ENOENT;

    vfs_node_t *created = vfs_create(parent, dirname, FS_DIRECTORY | (mode ? mode : 0755));
    return created ? 0 : -EEXIST;
}

static int64_t sys_unlinkat_handler(int dirfd, const char *pathname, int flags) {
    (void)dirfd; (void)flags;
    if (!pathname) return -EFAULT;
    char resolved[256];
    resolve_user_path(pathname, resolved, sizeof(resolved));

    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;

    if (node->parent && node->parent->children) {
        vfs_node_t *curr = node->parent->children;
        vfs_node_t *prev = NULL;
        while (curr) {
            if (curr == node) {
                if (prev) prev->next = curr->next;
                else node->parent->children = curr->next;
                kfree(node);
                return 0;
            }
            prev = curr;
            curr = curr->next;
        }
    }
    return 0;
}

static int64_t sys_renameat_handler(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    (void)olddirfd; (void)newdirfd;
    if (!oldpath || !newpath) return -EFAULT;
    char old_res[256], new_res[256];
    resolve_user_path(oldpath, old_res, sizeof(old_res));
    resolve_user_path(newpath, new_res, sizeof(new_res));

    vfs_node_t *old_node = vfs_open(old_res, 0);
    if (!old_node) return -ENOENT;

    const char *new_name = strrchr(new_res, '/');
    new_name = new_name ? new_name + 1 : new_res;
    strncpy(old_node->name, new_name, sizeof(old_node->name) - 1);
    return 0;
}

static int64_t sys_getdents64_handler(int fd, void *dirp, size_t count) {
    if (!dirp || count == 0) return -EINVAL;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *dir = current_task->process->files[fd];
    if (!dir || !(dir->flags & FS_DIRECTORY)) return -ENOTDIR;

    uint8_t *out_buf = (uint8_t *)dirp;
    size_t written = 0;
    uint32_t idx = (uint32_t)current_task->process->file_offsets[fd];

    vfs_node_t *child = NULL;
    while ((child = vfs_readdir(dir, idx)) != NULL) {
        size_t name_len = strlen(child->name);
        size_t rec_len = (19 + name_len + 1 + 7) & ~7;

        if (written + rec_len > count) break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)(out_buf + written);
        d->d_ino = child->inode ? child->inode : (idx + 1);
        d->d_off = idx + 1;
        d->d_reclen = (unsigned short)rec_len;
        
        // 4 = Directory (d), 12 = Socket (s), 8 = Regular file (-)
        d->d_type = (child->flags & FS_DIRECTORY) ? 4 : ((child->flags & FS_SOCKET) ? 12 : 8);
        
        memcpy(d->d_name, child->name, name_len + 1);

        written += rec_len;
        idx++;
    }

    current_task->process->file_offsets[fd] = idx;
    return (int64_t)written;
}

static int64_t sys_umask_handler(int mask) {
    if (!current_task || !current_task->process) return 022;
    uint32_t old_mask = current_task->process->umask;
    current_task->process->umask = (uint32_t)(mask & 0777);
    return old_mask;
}

static int64_t sys_chmod_handler(const char *path, int mode) {
    if (!path) return -EFAULT;
    char resolved[256];
    resolve_user_path(path, resolved, sizeof(resolved));
    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;
    node->permissions = (uint32_t)mode;
    return 0;
}

static int64_t sys_chown_handler(const char *path, int uid, int gid) {
    (void)uid; (void)gid;
    if (!path) return -EFAULT;
    char resolved[256];
    resolve_user_path(path, resolved, sizeof(resolved));
    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;
    return 0;
}

static int64_t sys_truncate_handler(const char *path, int64_t length) {
    if (!path || length < 0) return -EINVAL;
    char resolved[256];
    resolve_user_path(path, resolved, sizeof(resolved));
    vfs_node_t *node = vfs_open(resolved, 0);
    if (!node) return -ENOENT;
    node->length = (uint64_t)length;
    return 0;
}

static int64_t sys_ftruncate_handler(int fd, int64_t length) {
    if (length < 0) return -EINVAL;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;
    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;
    node->length = (uint64_t)length;
    return 0;
}

// ============================================================================
// 3. Memory Subsystem Handlers (Paging, Heap, Mmap)
// ============================================================================

static int64_t sys_brk_handler(uint64_t new_brk) {
    if (!current_task || !current_task->process) return 0;
    uint64_t old_brk = current_task->process->brk;
    if (new_brk == 0) return old_brk;

    if (new_brk <= old_brk) {
        current_task->process->brk = new_brk;
        return new_brk;
    }

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    uint64_t start_page = (old_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end_page = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start_page; addr < end_page; addr += PAGE_SIZE) {
        if (vmm_get_phys(pml4, addr) != 0) continue;
        void *phys = pmm_alloc();
        if (!phys) return old_brk;
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);
        vmm_map(pml4, addr, (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    current_task->process->brk = new_brk;
    return new_brk;
}

static int64_t sys_mmap_handler(uint64_t addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    if (length == 0) return -EINVAL;
    if (offset < 0 || (offset & (PAGE_SIZE - 1)) != 0) return -EINVAL;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_len = page_count * PAGE_SIZE;

    uint64_t virt_addr = addr;
    if (virt_addr == 0 || !(flags & MAP_FIXED)) {
        virt_addr = mmap_virtual_base;
        mmap_virtual_base += aligned_len;
    }

    if (!current_task || !current_task->process) return -EINVAL;
    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);

    // CASE 1: Device Memory Mapping (e.g. /dev/fb0 via MAP_SHARED)
    if (fd >= 0 && fd < MAX_OPEN_FILES && current_task->process->files[fd]) {
        vfs_node_t *node = current_task->process->files[fd];
        if (node->ops && node->ops->mmap) {
            return node->ops->mmap(node, virt_addr, aligned_len, prot, flags, offset);
        }
    }

    // CASE 2: Anonymous Memory Mapping (MAP_ANONYMOUS)
    if (flags & MAP_ANONYMOUS) {
        for (size_t i = 0; i < page_count; i++) {
            void *phys = pmm_alloc();
            if (!phys) return -ENOMEM;
            memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);

            uint64_t pte_flags = PTE_PRESENT | PTE_USER;
            if (prot & PROT_WRITE) pte_flags |= PTE_WRITABLE;

            vmm_map(pml4, virt_addr + (i * PAGE_SIZE), (uint64_t)phys, pte_flags);
        }
        return (int64_t)virt_addr;
    }

    // CASE 3: Regular File Mapping (MAP_PRIVATE or MAP_SHARED with file node)
    if (fd >= 0 && fd < MAX_OPEN_FILES && current_task->process->files[fd]) {
        vfs_node_t *node = current_task->process->files[fd];
        for (size_t i = 0; i < page_count; i++) {
            void *phys = pmm_alloc();
            if (!phys) return -ENOMEM;
            memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);

            uint64_t file_pos = (uint64_t)offset + (i * PAGE_SIZE);
            if (file_pos < node->length) {
                uint64_t chunk = (node->length - file_pos > PAGE_SIZE) ? PAGE_SIZE : (node->length - file_pos);
                vfs_read(node, file_pos, chunk, (uint8_t *)VIRT((uint64_t)phys));
            }

            uint64_t pte_flags = PTE_PRESENT | PTE_USER;
            if (prot & PROT_WRITE) pte_flags |= PTE_WRITABLE;

            vmm_map(pml4, virt_addr + (i * PAGE_SIZE), (uint64_t)phys, pte_flags);
        }
        return (int64_t)virt_addr;
    }

    return -EINVAL;
}

static int64_t sys_munmap_handler(uint64_t addr, size_t length) {
    if (length == 0 || (addr & (PAGE_SIZE - 1)) != 0) return -EINVAL;
    if (!current_task || !current_task->process) return -EINVAL;

    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < page_count; i++) {
        uint64_t virt = addr + (i * PAGE_SIZE);
        uint64_t phys = vmm_get_phys(pml4, virt);
        if (phys) {
            pmm_free((void *)(phys & ~0xFFFULL));
            vmm_unmap(pml4, virt);
        }
    }
    return 0;
}

static int64_t sys_mprotect_handler(uint64_t addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

static int64_t sys_mremap_handler(uint64_t old_address, size_t old_size, size_t new_size, int flags) {
    (void)flags;
    if (new_size == 0) return -EINVAL;
    int64_t new_addr = sys_mmap_handler(0, new_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (new_addr < 0) return new_addr;

    if (old_address && old_size) {
        size_t copy_len = (old_size < new_size) ? old_size : new_size;
        memcpy((void *)new_addr, (void *)old_address, copy_len);
        sys_munmap_handler(old_address, old_size);
    }
    return new_addr;
}

// ============================================================================
// 4. Process Lifecycle, Multitasking & Architecture Setup
// ============================================================================

static int64_t sys_exit_handler(int code) {
    if (current_task && current_task->process) {
        current_task->process->exit_code = code;
        current_task->process->exited = true;

        if (current_task->process->clear_child_tid) {
            uint32_t *tid_ptr = (uint32_t *)current_task->process->clear_child_tid;
            *tid_ptr = 0;
            extern task_t *task_list;
            if (task_list) {
                task_t *curr = task_list;
                do {
                    if (curr->futex_addr == current_task->process->clear_child_tid) {
                        curr->futex_addr = 0;
                        sched_unblock(curr);
                    }
                    curr = curr->next;
                } while (curr && curr != task_list);
            }
        }

        // Wake up any parent process blocked in wait4
        extern task_t *task_list;
        if (task_list) {
            task_t *curr = task_list;
            do {
                if (curr->process && curr->process->pid == current_task->process->parent_pid) {
                    if (curr->state == TASK_STATE_BLOCKED) {
                        sched_unblock(curr);
                    }
                }
                curr = curr->next;
            } while (curr && curr != task_list);
        }

        current_task->state = TASK_STATE_ZOMBIE;
        current_task->running = false;
        // sched_switch will automatically dequeue this task safely!
    }

    sched_yield();
    for (;;) { __asm__ volatile("hlt"); }
    return 0;
}

struct msghdr {
    void *msg_name;
    uint32_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

static int64_t sys_sendmsg_handler(int fd, const struct msghdr *msg, int flags) {
    (void)flags;
    if (!msg || !msg->msg_iov || msg->msg_iovlen == 0) return -EINVAL;
    return sys_writev_handler(fd, msg->msg_iov, (int)msg->msg_iovlen);
}

static int64_t sys_recvmsg_handler(int fd, struct msghdr *msg, int flags) {
    (void)flags;
    if (!msg || !msg->msg_iov || msg->msg_iovlen == 0) return -EINVAL;
    return sys_readv_handler(fd, msg->msg_iov, (int)msg->msg_iovlen);
}

static int64_t sys_setsockopt_handler(int fd, int level, int optname, const void *optval, uint32_t optlen) {
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0; // Pretend all socket options are set successfully
}

static int64_t sys_getsockopt_handler(int fd, int level, int optname, void *optval, uint32_t *optlen) {
    (void)fd; (void)level; (void)optname;
    if (optval && optlen && *optlen >= sizeof(int)) {
        *(int *)optval = 0; // Return SO_ERROR = 0 (No error)
    }
    return 0;
}

static int64_t sys_getpeername_handler(int fd, struct sockaddr_un *addr, uint32_t *addrlen) {
    (void)fd;
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_un)) {
        addr->sun_family = AF_UNIX;
        strcpy(addr->sun_path, "/tmp/.X11-unix/X0");
        *addrlen = sizeof(struct sockaddr_un);
    }
    return 0;
}

static int64_t sys_getsockname_handler(int fd, struct sockaddr_un *addr, uint32_t *addrlen) {
    return sys_getpeername_handler(fd, addr, addrlen);
}

static int64_t sys_shutdown_handler(int fd, int how) {
    (void)fd; (void)how;
    return 0;
}

static int64_t sys_clone_handler(uint64_t flags, uint64_t stack_top, int *parent_tid, int *child_tid, uint64_t tls, syscall_regs_t *regs) {
    if (!current_task || !current_task->process) return -EAGAIN;

    if (pmm_get_free_pages() < PMM_RESERVE_PAGES) {
        return -EAGAIN;
    }

    // 1. Force save the current live FPU/SSE registers into current_task before cloning!
    __asm__ volatile("fxsave64 (%0)" :: "r"(task_fpu_area(current_task)) : "memory");

    page_table_t *child_pml4 = NULL;
    if (flags & CLONE_VM) {
        child_pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    } else {
        child_pml4 = vmm_clone_address_space(current_task->process->cr3);
        if (!child_pml4) {
            return -ENOMEM;
        }
    }

    process_t *child_proc = current_task->process;
    if (!(flags & CLONE_THREAD)) {
        child_proc = (process_t *)kmalloc(sizeof(process_t));
        if (!child_proc) {
            if (!(flags & CLONE_VM)) vmm_destroy_address_space(PHYS(child_pml4));
            return -ENOMEM;
        }
        memset(child_proc, 0, sizeof(process_t));
        child_proc->pid = next_pid++;
        child_proc->parent_pid = current_task->process->pid;
        child_proc->pgid = current_task->process->pgid;
        child_proc->cr3 = (flags & CLONE_VM) ? current_task->process->cr3 : PHYS(child_pml4);
        child_proc->brk = current_task->process->brk;
        child_proc->umask = current_task->process->umask;
        memcpy(child_proc->cwd, current_task->process->cwd, sizeof(child_proc->cwd));

        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            child_proc->files[i] = current_task->process->files[i];
            child_proc->file_offsets[i] = current_task->process->file_offsets[i];
            child_proc->file_flags[i] = current_task->process->file_flags[i];
        }
    }

    task_t *child_task = (task_t *)kmalloc(sizeof(task_t));
    if (!child_task) {
        if (!(flags & CLONE_THREAD)) kfree(child_proc);
        if (!(flags & CLONE_VM)) vmm_destroy_address_space(PHYS(child_pml4));
        return -ENOMEM;
    }
    memset(child_task, 0, sizeof(task_t));

    // Initialize FPU safely and copy verified parent state
    task_init_fpu(child_task);
    memcpy(task_fpu_area(child_task), task_fpu_area(current_task), 512);

    // Sanitize MXCSR to prevent #GP on fxrstor64
    uint32_t *mxcsr = (uint32_t *)((uint8_t *)task_fpu_area(child_task) + 24);
    *mxcsr &= 0xFFBF; // Mask out any illegal reserved bits

    child_task->id = (flags & CLONE_THREAD) ? next_pid++ : child_proc->pid;
    child_task->state = TASK_STATE_RUNNABLE;
    child_task->running = true;
    child_task->priority = current_task->priority;
    child_task->time_slice = 10;
    child_task->fs_base = (flags & CLONE_SETTLS) ? tls : current_task->fs_base;
    child_task->process = child_proc;

    child_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;
    if (!child_task->kstack_at_bottom) {
        kfree(child_task);
        if (!(flags & CLONE_THREAD)) kfree(child_proc);
        if (!(flags & CLONE_VM)) vmm_destroy_address_space(PHYS(child_pml4));
        return -ENOMEM;
    }

    // Exact System V AMD64 context switch frame matching RESTORE_REGS order
    uint64_t *stack = (uint64_t *)child_task->kstack_at_bottom;

    *--stack = 0x1B;                                // SS (User Data)
    *--stack = stack_top ? stack_top : regs->rsp;   // RSP
    *--stack = 0x202;                               // RFLAGS (IF=1)
    *--stack = 0x23;                                // CS (User Code)
    *--stack = (regs->rcx != 0) ? regs->rcx : regs->rip; // RIP

    *--stack = 0;                                   // error_code
    *--stack = 0;                                   // int_no

    // GPRs: High address to low address (so pop r15 is popped first!)
    *--stack = 0;                                   // RAX = 0 in child process (POSIX fork)
    *--stack = regs->rbx;
    *--stack = regs->rcx;
    *--stack = regs->rdx;
    *--stack = regs->rbp;
    *--stack = regs->rsi;
    *--stack = regs->rdi;
    *--stack = regs->r8;
    *--stack = regs->r9;
    *--stack = regs->r10;
    *--stack = regs->r11;
    *--stack = regs->r12;
    *--stack = regs->r13;
    *--stack = regs->r14;
    *--stack = regs->r15;

    child_task->rsp = (uint64_t)stack;

    if ((flags & CLONE_PARENT_SETTID) && parent_tid) *parent_tid = (int)child_task->id;
    if ((flags & CLONE_CHILD_SETTID) && child_tid) *child_tid = (int)child_task->id;
    if (flags & CLONE_CHILD_CLEARTID) child_proc->clear_child_tid = (uint64_t)child_tid;

    extern task_t *task_list;
    if (task_list) {
        child_task->next = task_list->next;
        child_task->prev = task_list;
        task_list->next->prev = child_task;
        task_list->next = child_task;
    } else {
        child_task->next = child_task;
        child_task->prev = child_task;
        task_list = child_task;
    }

    sched_enqueue(child_task);
    return (int64_t)child_task->id;
}

static int64_t sys_wait4_handler(int pid, int *wstatus, int options) {
    (void)options;
    if (!current_task || !current_task->process) return -ECHILD;

    extern task_t *task_list;

    for (;;) {
        bool have_child = false;
        task_t *child = NULL;

        if (task_list) {
            task_t *curr = task_list;
            do {
                if (curr->process && curr->process->parent_pid == current_task->process->pid) {
                    if (pid <= 0 || (int)curr->process->pid == pid) {
                        have_child = true;
                        if (curr->state == TASK_STATE_ZOMBIE || curr->process->exited) {
                            child = curr;
                            break;
                        }
                    }
                }
                curr = curr->next;
            } while (curr && curr != task_list);
        }

        if (child) {
            int child_pid = (int)child->process->pid;
            if (wstatus) {
                *wstatus = (child->process->exit_code & 0xFF) << 8;
            }
            child->process->parent_pid = 0; // Detach reaped child
            return child_pid;
        }

        if (!have_child) {
            return -ECHILD;
        }

        current_task->state = TASK_STATE_BLOCKED;
        current_task->running = false;
        sched_dequeue(current_task);
        sched_yield();
    }
}

static int64_t sys_arch_prctl_handler(int code, uint64_t addr) {
    if (!current_task) return -EINVAL;

    if (code == ARCH_SET_FS) {
        current_task->fs_base = addr;
        write_msr(0xC0000100, addr);
        return 0;
    } else if (code == ARCH_GET_FS) {
        if (!addr) return -EFAULT;
        *(uint64_t *)addr = current_task->fs_base;
        return 0;
    } else if (code == ARCH_SET_GS) {
        current_task->gs_base = addr;
        write_msr(0xC0000101, addr);
        return 0;
    } else if (code == ARCH_GET_GS) {
        if (!addr) return -EFAULT;
        *(uint64_t *)addr = current_task->gs_base;
        return 0;
    }
    return -EINVAL;
}

// ============================================================================
// 5. Signals & Synchronization (Futex, Signal Actions, RT Mask)
// ============================================================================

static int64_t sys_rt_sigaction_handler(int signum, const struct linux_sigaction *act, struct linux_sigaction *oldact, size_t sigsetsize) {
    (void)sigsetsize;
    if (signum <= 0 || signum >= NSIG || signum == SIGKILL || signum == SIGSTOP) return -EINVAL;
    if (!current_task || !current_task->process) return -EINVAL;

    sigaction_info_t *entry = &current_task->process->sigactions[signum];
    if (oldact) {
        oldact->sa_handler = (void (*)(int))entry->handler;
        oldact->sa_flags = entry->flags;
        oldact->sa_restorer = (void (*)(void))entry->restorer;
        oldact->sa_mask = entry->mask;
    }
    if (act) {
        entry->handler = (uint64_t)act->sa_handler;
        entry->flags = act->sa_flags;
        entry->restorer = (uint64_t)act->sa_restorer;
        entry->mask = act->sa_mask;
    }
    return 0;
}

static int64_t sys_rt_sigprocmask_handler(int how, const uint64_t *set, uint64_t *oldset, size_t sigsetsize) {
    (void)sigsetsize;
    if (!current_task || !current_task->process) return -EINVAL;

    if (oldset) {
        *oldset = current_task->process->sigmask;
    }
    if (set) {
        if (how == SIG_BLOCK) current_task->process->sigmask |= *set;
        else if (how == SIG_UNBLOCK) current_task->process->sigmask &= ~(*set);
        else if (how == SIG_SETMASK) current_task->process->sigmask = *set;
        else return -EINVAL;
    }
    return 0;
}

static int64_t sys_futex_handler(uint32_t *uaddr, int op, uint32_t val, const struct linux_timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    if (!uaddr) return -EFAULT;
    int cmd = op & 0x7F;

    if (cmd == FUTEX_WAIT) {
        if (*uaddr != val) {
            return -EAGAIN;
        }
        current_task->futex_addr = (uint64_t)uaddr;
        sched_block(current_task);
        sched_yield();
        current_task->futex_addr = 0;
        return 0;
    } else if (cmd == FUTEX_WAKE) {
        int woken = 0;
        extern task_t *task_list;
        if (task_list) {
            task_t *curr = task_list;
            do {
                if (curr->futex_addr == (uint64_t)uaddr) {
                    curr->futex_addr = 0;
                    sched_unblock(curr);
                    woken++;
                    if ((uint32_t)woken >= val) break;
                }
                curr = curr->next;
            } while (curr && curr != task_list);
        }
        return woken;
    }
    return -ENOSYS;
}

// ============================================================================
// 6. Time, Polling, and System Statistics
// ============================================================================

static int64_t sys_clock_gettime_handler(int clock_id, struct linux_timespec *tp) {
    (void)clock_id;
    if (!tp) return -EFAULT;
    uint64_t current_ticks = tick;
    tp->tv_sec = current_ticks / 100;
    tp->tv_nsec = (current_ticks % 100) * 10000000ULL;
    return 0;
}

static int64_t sys_gettimeofday_handler(struct linux_timeval *tv, struct linux_timezone *tz) {
    if (tv) {
        uint64_t current_ticks = tick;
        tv->tv_sec = current_ticks / 100;
        tv->tv_usec = (current_ticks % 100) * 10000ULL;
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

static int64_t sys_nanosleep_handler(const struct linux_timespec *req, struct linux_timespec *rem) {
    (void)rem;
    if (!req) return -EFAULT;
    uint64_t target_tick = tick + (req->tv_sec * 100 + req->tv_nsec / 10000000ULL);
    if (target_tick > tick) {
        sched_make_sleep(current_task, target_tick);
        sched_yield();
    }
    return 0;
}

#define POLLWRNORM 0x0100
#define POLLRDNORM 0x0040

static int poll_scan_fds(struct linux_pollfd *fds, uint64_t nfds) {
    int ready = 0;

    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        int fd = fds[i].fd;
        if (fd < 0 || fd >= MAX_OPEN_FILES) continue;

        // 1. TTY / Console Standard Input (fd 0)
        if (fd == 0) {
            if ((fds[i].events & (POLLIN | POLLRDNORM)) && tty_has_input()) {
                fds[i].revents |= (fds[i].events & (POLLIN | POLLRDNORM));
                ready++;
            }
        } 
        // 2. TTY / Console Standard Output (fd 1, 2)
        else if (fd == 1 || fd == 2) {
            if (fds[i].events & (POLLOUT | POLLWRNORM)) {
                fds[i].revents |= (fds[i].events & (POLLOUT | POLLWRNORM));
                ready++;
            }
        }

        // 3. File descriptors & Sockets
        if (current_task && current_task->process) {
            vfs_node_t *node = current_task->process->files[fd];
            if (!node) continue;

            // UNIX Domain Sockets
            if (node->ops == &unix_socket_vfs_ops && node->ptr) {
                unix_socket_t *s = (unix_socket_t *)node->ptr;

                if ((fds[i].events & (POLLIN | POLLRDNORM)) && unix_socket_can_read(s)) {
                    fds[i].revents |= (fds[i].events & (POLLIN | POLLRDNORM));
                    ready++;
                }
                if ((fds[i].events & (POLLOUT | POLLWRNORM)) && unix_socket_can_write(s)) {
                    fds[i].revents |= (fds[i].events & (POLLOUT | POLLWRNORM));
                    ready++;
                }
                if (s->peer_closed) {
                    fds[i].revents |= POLLHUP;
                    ready++;
                }
            }
            // Mouse device /dev/mouse or /dev/input/mice
            else if ((node->ops == &g_mousedev_fops || node->ops == &g_evdev_mouse_fops)) {
                if ((fds[i].events & (POLLIN | POLLRDNORM)) && evdev_mouse_has_data()) {
                    fds[i].revents |= (fds[i].events & (POLLIN | POLLRDNORM));
                    ready++;
                }
            }
        }
    }

    return ready;
}

static int64_t sys_poll_handler(struct linux_pollfd *fds, uint64_t nfds, int timeout) {
    if (!fds && nfds > 0) return -EFAULT;

    // Fast path: Check immediately without sleeping
    int ready = poll_scan_fds(fds, nfds);
    if (ready > 0 || timeout == 0) {
        return ready;
    }

    uint64_t start_tick = tick;
    uint64_t max_ticks = (timeout < 0) ? (uint64_t)-1 : ((uint64_t)timeout / 10);

    // Sleep path: Yield until events occur or timeout expires
    while (ready == 0) {
        if (timeout >= 0 && (tick - start_tick) >= max_ticks) {
            break;
        }

        __asm__ volatile("sti; pause");
        sched_yield();

        // Re-scan both TTY and Sockets on each iteration
        ready = poll_scan_fds(fds, nfds);
    }

    return ready;
}

static int select_scan_fds(int nfds, uint8_t *rfds, uint8_t *wfds) {
    int ready = 0;
    if (nfds > MAX_OPEN_FILES) nfds = MAX_OPEN_FILES;

    for (int fd = 0; fd < nfds; fd++) {
        int byte = fd / 8;
        int bit = fd % 8;

        // Check Read Readiness
        if (rfds && (rfds[byte] & (1 << bit))) {
            bool can_read = false;
            if (fd == 0 && tty_has_input()) {
                can_read = true;
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES && current_task && current_task->process) {
                vfs_node_t *node = current_task->process->files[fd];
                if (node && node->ops == &unix_socket_vfs_ops && node->ptr) {
                    can_read = unix_socket_can_read((unix_socket_t *)node->ptr);
                }
            }
            if (can_read) {
                ready++;
            } else {
                rfds[byte] &= ~(1 << bit); // Clear bit if not ready
            }
        }

        // Check Write Readiness
        if (wfds && (wfds[byte] & (1 << bit))) {
            bool can_write = false;
            if (fd == 1 || fd == 2) {
                can_write = true;
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES && current_task && current_task->process) {
                vfs_node_t *node = current_task->process->files[fd];
                if (node && node->ops == &unix_socket_vfs_ops && node->ptr) {
                    can_write = unix_socket_can_write((unix_socket_t *)node->ptr);
                }
            }
            if (can_write) {
                ready++;
            } else {
                wfds[byte] &= ~(1 << bit);
            }
        }
    }
    return ready;
}

static int64_t sys_pselect6_handler(int nfds, void *readfds, void *writefds, void *exceptfds, 
                                   const struct linux_timespec *timeout, const void *sigmask) {
    (void)exceptfds; (void)sigmask;
    if (nfds < 0) return -EINVAL;
    if (nfds > MAX_OPEN_FILES) nfds = MAX_OPEN_FILES;

    uint8_t *rfds = (uint8_t *)readfds;
    uint8_t *wfds = (uint8_t *)writefds;

    int bytes = (nfds + 7) / 8;
    uint8_t orig_rfds[MAX_OPEN_FILES / 8 + 1];
    uint8_t orig_wfds[MAX_OPEN_FILES / 8 + 1];
    memset(orig_rfds, 0, sizeof(orig_rfds));
    memset(orig_wfds, 0, sizeof(orig_wfds));

    if (rfds) memcpy(orig_rfds, rfds, bytes);
    if (wfds) memcpy(orig_wfds, wfds, bytes);

    uint64_t start_tick = tick;
    uint64_t max_ticks = (timeout == NULL) ? (uint64_t)-1 : (timeout->tv_sec * 100 + timeout->tv_nsec / 10000000ULL);

    for (;;) {
        int ready = 0;

        // Clear output bitmasks for current poll iteration
        if (rfds) memset(rfds, 0, bytes);
        if (wfds) memset(wfds, 0, bytes);

        for (int fd = 0; fd < nfds; fd++) {
            int byte = fd / 8;
            int bit = fd % 8;

            // 1. Check Read Readiness
            if (orig_rfds[byte] & (1 << bit)) {
                bool can_read = false;

                if (fd == 0 && tty_has_input()) {
                    can_read = true;
                } else if (current_task && current_task->process && fd < MAX_OPEN_FILES) {
                    vfs_node_t *node = current_task->process->files[fd];
                    if (node) {
                        // Sockets
                        if (node->ops == &unix_socket_vfs_ops && node->ptr) {
                            can_read = unix_socket_can_read((unix_socket_t *)node->ptr);
                        }
                        // Mouse (/dev/mouse, /dev/psaux, /dev/input/mice)
                        else if (node->ops == &g_mousedev_fops || node->ops == &g_evdev_mouse_fops) {
                            can_read = evdev_mouse_can_read();
                        }
                    }
                }

                if (can_read) {
                    if (rfds) rfds[byte] |= (1 << bit);
                    ready++;
                }
            }

            // 2. Check Write Readiness
            if (orig_wfds[byte] & (1 << bit)) {
                bool can_write = false;

                if (fd == 1 || fd == 2) {
                    can_write = true;
                } else if (current_task && current_task->process && fd < MAX_OPEN_FILES) {
                    vfs_node_t *node = current_task->process->files[fd];
                    if (node) {
                        if (node->ops == &unix_socket_vfs_ops && node->ptr) {
                            can_write = unix_socket_can_write((unix_socket_t *)node->ptr);
                        }
                    }
                }

                if (can_write) {
                    if (wfds) wfds[byte] |= (1 << bit);
                    ready++;
                }
            }
        }

        // Return immediately if events are active
        if (ready > 0) {
            return ready;
        }

        // Timeout checks
        if (timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0) {
            return 0;
        }

        if (timeout != NULL && (tick - start_tick) >= max_ticks) {
            return 0;
        }

        __asm__ volatile("sti; pause");
        sched_yield();
    }
}

static int64_t sys_sysinfo_handler(equant_sysinfo_t *info) {
    if (!info) return -EFAULT;
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

static int64_t sys_uname_handler(struct linux_utsname *buf) {
    if (!buf) return -EFAULT;
    memset(buf, 0, sizeof(struct linux_utsname));
    strcpy(buf->sysname, "Linux");
    strcpy(buf->nodename, "equant");
    strcpy(buf->release, "6.1.0-equantos");
    strcpy(buf->version, "EquantOS SMP Unix Kernel x86_64");
    strcpy(buf->machine, "x86_64");
    strcpy(buf->domainname, "localdomain");
    return 0;
}

static int64_t sys_getrusage_handler(int who, struct rusage *usage) {
    (void)who;
    if (!usage) return -EFAULT;
    memset(usage, 0, sizeof(struct rusage));
    if (current_task && current_task->process) {
        usage->ru_utime.tv_sec = current_task->process->utime / 100;
        usage->ru_utime.tv_usec = (current_task->process->utime % 100) * 10000;
        usage->ru_stime.tv_sec = current_task->process->stime / 100;
        usage->ru_stime.tv_usec = (current_task->process->stime % 100) * 10000;
    }
    usage->ru_maxrss = 4096;
    return 0;
}

static int64_t sys_times_handler(struct tms *buf) {
    if (!buf) return -EFAULT;
    if (current_task && current_task->process) {
        buf->tms_utime = current_task->process->utime;
        buf->tms_stime = current_task->process->stime;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return (int64_t)tick;
}

static int64_t sys_prlimit64_handler(int pid, int resource, const struct linux_rlimit *new_limit, struct linux_rlimit *old_limit) {
    (void)pid; (void)new_limit;
    if (old_limit) {
        if (resource == RLIMIT_NOFILE) {
            old_limit->rlim_cur = MAX_OPEN_FILES;
            old_limit->rlim_max = MAX_OPEN_FILES;
        } else if (resource == RLIMIT_STACK) {
            old_limit->rlim_cur = 8 * 1024 * 1024;
            old_limit->rlim_max = 8 * 1024 * 1024;
        } else {
            old_limit->rlim_cur = RLIM_INFINITY;
            old_limit->rlim_max = RLIM_INFINITY;
        }
    }
    return 0;
}

static int64_t sys_getpgid_handler(int pid) {
    if (!current_task || !current_task->process) return -ESRCH;
    if (pid == 0 || pid == (int)current_task->process->pid) {
        return current_task->process->pgid ? (int64_t)current_task->process->pgid : (int64_t)current_task->process->pid;
    }
    extern task_t *task_list;
    if (task_list) {
        task_t *curr = task_list;
        do {
            if (curr->process && (int)curr->process->pid == pid) {
                return curr->process->pgid ? (int64_t)curr->process->pgid : (int64_t)curr->process->pid;
            }
            curr = curr->next;
        } while (curr && curr != task_list);
    }
    return -ESRCH;
}

static int64_t sys_socket_handler(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_UNIX) {
        return -EAFNOSUPPORT; // Only UNIX domain sockets for local GUI IPC right now
    }

    unix_socket_t *sock = unix_socket_create(type);
    if (!sock) return -ENOMEM;

    vfs_node_t *node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!node) {
        unix_socket_close(sock);
        return -ENOMEM;
    }

    strcpy(node->name, "socket:unix");
    node->flags = FS_FILE;
    node->ops = &unix_socket_vfs_ops;
    node->ptr = (struct vfs_node *)sock;

    int fd = alloc_fd(node, O_RDWR);
    if (fd < 0) {
        kfree(node);
        unix_socket_close(sock);
        return fd;
    }

    return fd;
}

static int64_t sys_bind_handler(int fd, const struct sockaddr_un *addr, uint32_t addrlen) {
    (void)addrlen;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !current_task->process->files[fd]) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (node->ops != &unix_socket_vfs_ops || !node->ptr) return -ENOTSOCK;

    return unix_socket_bind((unix_socket_t *)node->ptr, addr);
}

static int64_t sys_listen_handler(int fd, int backlog) {
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !current_task->process->files[fd]) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (node->ops != &unix_socket_vfs_ops || !node->ptr) return -ENOTSOCK;

    return unix_socket_listen((unix_socket_t *)node->ptr, backlog);
}

static int64_t sys_accept_handler(int fd, struct sockaddr_un *addr, uint32_t *addrlen) {
    (void)addr; (void)addrlen;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !current_task->process->files[fd]) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (node->ops != &unix_socket_vfs_ops || !node->ptr) return -ENOTSOCK;

    unix_socket_t *client_sock = NULL;
    int err = unix_socket_accept((unix_socket_t *)node->ptr, &client_sock);
    if (err < 0) return err;

    vfs_node_t *client_node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!client_node) {
        unix_socket_close(client_sock);
        return -ENOMEM;
    }

    strcpy(client_node->name, "socket:unix_client");
    client_node->flags = FS_FILE;
    client_node->ops = &unix_socket_vfs_ops;
    client_node->ptr = (struct vfs_node *)client_sock;

    int new_fd = alloc_fd(client_node, O_RDWR);
    if (new_fd < 0) {
        kfree(client_node);
        unix_socket_close(client_sock);
        return new_fd;
    }

    return new_fd;
}

static int64_t sys_connect_handler(int fd, const struct sockaddr_un *addr, uint32_t addrlen) {
    (void)addrlen;
    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !current_task->process->files[fd]) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (node->ops != &unix_socket_vfs_ops || !node->ptr) return -ENOTSOCK;

    return unix_socket_connect((unix_socket_t *)node->ptr, addr);
}

// ============================================================================
// Master Syscall Dispatcher Table
// ============================================================================

// Implementation of socketpair(AF_UNIX, SOCK_STREAM, 0, sv)
static int64_t sys_socketpair_handler(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if (!sv || !validate_user_memory(sv, sizeof(int) * 2, true)) return -EFAULT;

    unix_socket_t *s1 = unix_socket_create(type);
    unix_socket_t *s2 = unix_socket_create(type);
    if (!s1 || !s2) {
        if (s1) unix_socket_close(s1);
        if (s2) unix_socket_close(s2);
        return -ENOMEM;
    }

    s1->peer = s2;
    s2->peer = s1;
    s1->state = UNIX_STATE_CONNECTED;
    s2->state = UNIX_STATE_CONNECTED;

    vfs_node_t *n1 = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    vfs_node_t *n2 = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!n1 || !n2) {
        if (n1) kfree(n1);
        if (n2) kfree(n2);
        unix_socket_close(s1);
        unix_socket_close(s2);
        return -ENOMEM;
    }

    strcpy(n1->name, "socketpair:0");
    n1->flags = FS_FILE;
    n1->ops = &unix_socket_vfs_ops;
    n1->ptr = (struct vfs_node *)s1;

    strcpy(n2->name, "socketpair:1");
    n2->flags = FS_FILE;
    n2->ops = &unix_socket_vfs_ops;
    n2->ptr = (struct vfs_node *)s2;

    int fd1 = alloc_fd(n1, O_RDWR);
    int fd2 = alloc_fd(n2, O_RDWR);
    if (fd1 < 0 || fd2 < 0) {
        if (fd1 >= 0) sys_close_handler(fd1);
        if (fd2 >= 0) sys_close_handler(fd2);
        return -EMFILE;
    }

    sv[0] = fd1;
    sv[1] = fd2;
    return 0;
}

// Fixed select handler handling struct timeval (microseconds)
static int64_t sys_select_handler(int nfds, void *rfds, void *wfds, void *efds, const struct linux_timeval *tv) {
    struct linux_timespec ts;
    struct linux_timespec *pts = NULL;
    if (tv) {
        ts.tv_sec = tv->tv_sec;
        ts.tv_nsec = tv->tv_usec * 1000ULL;
        pts = &ts;
    }
    return sys_pselect6_handler(nfds, rfds, wfds, efds, pts, NULL);
}

// Implementation of ppoll (syscall 271)
static int64_t sys_ppoll_handler(struct linux_pollfd *fds, uint64_t nfds, const struct linux_timespec *tmo_p, const void *sigmask, size_t sigsetsize) {
    (void)sigmask; (void)sigsetsize;
    int timeout_ms = -1;
    if (tmo_p) {
        timeout_ms = (int)(tmo_p->tv_sec * 1000 + tmo_p->tv_nsec / 1000000ULL);
    }
    return sys_poll_handler(fds, nfds, timeout_ms);
}

// Fixed select handler that converts struct timeval (microseconds) to timespec (nanoseconds)
static int64_t sys_select_compat_handler(int nfds, void *rfds, void *wfds, void *efds, const struct linux_timeval *tv) {
    struct linux_timespec ts;
    struct linux_timespec *pts = NULL;
    if (tv) {
        ts.tv_sec = tv->tv_sec;
        ts.tv_nsec = tv->tv_usec * 1000ULL;
        pts = &ts;
    }
    return sys_pselect6_handler(nfds, rfds, wfds, efds, pts, NULL);
}

static int64_t sys_execve_handler(const char *filename, char *const argv[], char *const envp[], syscall_regs_t *regs) {
    (void)envp;
    if (!filename || !current_task || !current_task->process) return -EINVAL;

    char resolved[256];
    resolve_user_path(filename, resolved, sizeof(resolved));

    vfs_node_t *file = vfs_open(resolved, 0);
    if (!file) {
        char alt[256];
        snprintf(alt, sizeof(alt), "/bin/%s", filename);
        file = vfs_open(alt, 0);
        if (!file) {
            snprintf(alt, sizeof(alt), "/bin/%s.elf", filename);
            file = vfs_open(alt, 0);
        }
        if (!file) {
            snprintf(alt, sizeof(alt), "%s.elf", filename);
            file = vfs_open(alt, 0);
        }
    }

    if (!file || (file->flags & FS_DIRECTORY)) {
        return -ENOENT;
    }

    uint8_t *file_buf = (uint8_t *)kmalloc(file->length);
    if (!file_buf) return -ENOMEM;

    if (vfs_read(file, 0, file->length, file_buf) <= 0) {
        kfree(file_buf);
        return -EIO;
    }

    // 1. Check for Shebang script execution (#!/bin/sh, #!/bin/bash)
    if (file->length >= 2 && file_buf[0] == '#' && file_buf[1] == '!') {
        char interp_line[128];
        size_t idx = 2;
        while (idx < file->length && (file_buf[idx] == ' ' || file_buf[idx] == '\t')) idx++;
        
        size_t l_idx = 0;
        while (idx < file->length && file_buf[idx] != '\n' && file_buf[idx] != '\r' && l_idx < sizeof(interp_line) - 1) {
            interp_line[l_idx++] = (char)file_buf[idx++];
        }
        interp_line[l_idx] = '\0';
        kfree(file_buf);

        // Tokenize interpreter and optional arguments
        char *interp_argv[18];
        int new_argc = 0;
        char *token = interp_line;
        while (*token) {
            while (*token == ' ') token++;
            if (!*token) break;
            interp_argv[new_argc++] = token;
            while (*token && *token != ' ') token++;
            if (*token) *token++ = '\0';
        }

        interp_argv[new_argc++] = (char *)filename;
        if (argv) {
            for (int i = 1; argv[i] != NULL && new_argc < 16; i++) {
                interp_argv[new_argc++] = argv[i];
            }
        }
        interp_argv[new_argc] = NULL;

        return sys_execve_handler(interp_argv[0], interp_argv, envp, regs);
    }

    // 2. Standard ELF Binary Loading
    int argc = 0;
    char k_argv_storage[16][128];
    char *exec_argv[17];

    if (argv) {
        while (argv[argc] && argc < 16) {
            strncpy(k_argv_storage[argc], argv[argc], 127);
            k_argv_storage[argc][127] = '\0';
            exec_argv[argc] = k_argv_storage[argc];
            argc++;
        }
    }
    exec_argv[argc] = NULL;

    uint64_t new_entry = 0;
    uint64_t new_rsp = 0;
    uint64_t new_cr3 = 0;

    bool ok = elf_execve_replace(file_buf, file->length, argc, exec_argv, &new_entry, &new_rsp, &new_cr3);
    kfree(file_buf);

    if (!ok) {
        return -ENOEXEC;
    }

    uint64_t old_cr3 = current_task->process->cr3;
    current_task->process->cr3 = new_cr3;

    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    vmm_destroy_address_space(old_cr3);

    regs->rip = new_entry;
    regs->rcx = new_entry; // Required for SYSRETQ
    regs->rsp = new_rsp;
    regs->cs = 0x23;
    regs->ss = 0x1B;
    regs->rflags = 0x202;
    regs->r11 = 0x202;    // Required for SYSRETQ

    regs->rax = 0;
    regs->rbx = 0;
    regs->rdx = 0;
    regs->rsi = 0;
    regs->rdi = 0;
    regs->rbp = 0;
    regs->r8  = 0;
    regs->r9  = 0;
    regs->r10 = 0;
    regs->r12 = 0;
    regs->r13 = 0;
    regs->r14 = 0;
    regs->r15 = 0;

    return 0;
}


static const char *get_syscall_name(uint64_t no) {
    switch (no) {
        case 0: return "read";
        case 1: return "write";
        case 2: return "open";
        case 3: return "close";
        case 4: return "stat";
        case 5: return "fstat";
        case 6: return "lstat";
        case 7: return "poll";
        case 8: return "lseek";
        case 9: return "mmap";
        case 10: return "mprotect";
        case 11: return "munmap";
        case 12: return "brk";
        case 13: return "rt_sigaction";
        case 14: return "rt_sigprocmask";
        case 15: return "rt_sigreturn";
        case 16: return "ioctl";
        case 17: return "pread64";
        case 18: return "pwrite64";
        case 19: return "readv";
        case 20: return "writev";
        case 21: return "access";
        case 22: return "pipe";
        case 23: return "select";
        case 24: return "sched_yield";
        case 25: return "mremap";
        case 29: return "shmget";
        case 30: return "shmat";
        case 31: return "shmctl";
        case 32: return "dup";
        case 33: return "dup2";
        case 35: return "nanosleep";
        case 36: return "getitimer";
        case 38: return "setitimer";
        case 39: return "getpid";
        case 41: return "socket";
        case 42: return "connect";
        case 43: return "accept";
        case 44: return "sendto";
        case 45: return "recvfrom";
        case 49: return "bind";
        case 50: return "listen";
        case 56: return "clone";
        case 57: return "fork";
        case 59: return "execve";
        case 60: return "exit";
        case 61: return "wait4";
        case 62: return "kill";
        case 63: return "uname";
        case 67: return "shmdt";
        case 72: return "fcntl";
        case 79: return "getcwd";
        case 80: return "chdir";
        case 83: return "mkdir";
        case 86: return "link";
        case 87: return "unlink";
        case 96: return "gettimeofday";
        case 97: return "getrlimit";
        case 98: return "getrusage";
        case 99: return "sysinfo";
        case 158: return "arch_prctl";
        case 186: return "gettid";
        case 202: return "futex";
        case 217: return "getdents64";
        case 218: return "set_tid_address";
        case 228: return "clock_gettime";
        case 231: return "exit_group";
        case 257: return "openat";
        case 258: return "mkdirat";
        case 262: return "newfstatat";
        case 263: return "unlinkat";
        case 265: return "linkat";
        case 268: return "fchmodat";
        case 269: return "faccessat";
        case 270: return "pselect6";
        case 292: return "dup3";
        case 293: return "pipe2";
        case 302: return "prlimit64";
        default: return "unknown";
    }
}

void syscall_handler(void *regs_ptr) {
    syscall_regs_t *regs = (syscall_regs_t *)regs_ptr;
    uint64_t syscall_no = regs->rax;
    int64_t ret = -ENOSYS;

    uint32_t pid = (current_task && current_task->process) ? (uint32_t)current_task->process->pid : 0;
    const char *name = get_syscall_name(syscall_no);

    // 1. LOG ENTRY (видно ДО того, как сисколл зависнет внутри!)
    bool quiet = (syscall_no == 16 && regs->rsi == 0x4B46) ||
                 (syscall_no == SYS_CLOCK_GETTIME) ||
                 (syscall_no == SYS_SELECT) ||
                 (syscall_no == SYS_PSELECT6) ||
                 (syscall_no == SYS_POLL) ||
                 (syscall_no == SYS_PPOLL) ||
                 (syscall_no == SYS_READ) ||
                 (syscall_no == SYS_WRITE) ||
                 (syscall_no == SYS_READV) ||
                 (syscall_no == SYS_WRITEV) ||
                 (syscall_no == SYS_RECVMSG) ||
                 (syscall_no == SYS_SENDMSG);

    if (!quiet) {
        strace_log("[STRACE %u] > %s(%d) args=(0x%llx, 0x%llx, 0x%llx)\n",
                   pid, name, (int)syscall_no, regs->rdi, regs->rsi, regs->rdx);
    }

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
        case SYS_LSTAT:
            ret = sys_stat_handler((const char *)regs->rdi, (struct linux_stat *)regs->rsi);
            break;
        case SYS_FSTAT:
            ret = sys_fstat_handler((int)regs->rdi, (struct linux_stat *)regs->rsi);
            break;
        case SYS_POLL:
            ret = sys_poll_handler((struct linux_pollfd *)regs->rdi, regs->rsi, (int)regs->rdx);
            break;
        case SYS_LSEEK:
            ret = sys_lseek_handler((int)regs->rdi, (int64_t)regs->rsi, (int)regs->rdx);
            break;
        case SYS_MMAP:
            ret = sys_mmap_handler(regs->rdi, (size_t)regs->rsi, (int)regs->rdx, (int)regs->r10, (int)regs->r8, (int64_t)regs->r9);
            break;
        case SYS_MPROTECT:
            ret = sys_mprotect_handler(regs->rdi, (size_t)regs->rsi, (int)regs->rdx);
            break;
        case SYS_MUNMAP:
            ret = sys_munmap_handler(regs->rdi, (size_t)regs->rsi);
            break;
        case SYS_BRK:
            ret = sys_brk_handler(regs->rdi);
            break;
        case SYS_RT_SIGACTION:
            ret = sys_rt_sigaction_handler((int)regs->rdi, (const struct linux_sigaction *)regs->rsi, (struct linux_sigaction *)regs->rdx, (size_t)regs->r10);
            break;
        case SYS_RT_SIGPROCMASK:
            ret = sys_rt_sigprocmask_handler((int)regs->rdi, (const uint64_t *)regs->rsi, (uint64_t *)regs->rdx, (size_t)regs->r10);
            break;
        case SYS_RT_SIGRETURN:
            ret = 0;
            break;
        case SYS_IOCTL:
            ret = sys_ioctl_handler((int)regs->rdi, regs->rsi, (void *)regs->rdx);
            break;
        case SYS_PREAD64:
            ret = sys_pread64_handler((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx, (int64_t)regs->r10);
            break;
        case SYS_PWRITE64:
            ret = sys_pwrite64_handler((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx, (int64_t)regs->r10);
            break;
        case SYS_READV:
            ret = sys_readv_handler((int)regs->rdi, (const struct iovec *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_WRITEV:
            ret = sys_writev_handler((int)regs->rdi, (const struct iovec *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_ACCESS:
            ret = sys_access_handler((const char *)regs->rdi, (int)regs->rsi);
            break;
        case SYS_PIPE:
            ret = sys_pipe2_handler((int *)regs->rdi, 0);
            break;
        case SYS_SELECT:
        case SYS_PSELECT6: // 270
            ret = sys_pselect6_handler((int)regs->rdi, (void *)regs->rsi, (void *)regs->rdx, 
                                       (void *)regs->r10, (const struct linux_timespec *)regs->r8, (const void *)regs->r9);
            break;
        case SYS_PPOLL:
            ret = sys_ppoll_handler((struct linux_pollfd *)regs->rdi, regs->rsi, 
                                    (const struct linux_timespec *)regs->rdx, (const void *)regs->r10, (size_t)regs->r8);
            break;
        case SYS_SOCKETPAIR:
            ret = sys_socketpair_handler((int)regs->rdi, (int)regs->rsi, (int)regs->rdx, (int *)regs->r10);
            break;
        case SYS_SCHED_YIELD:
            sched_yield();
            ret = 0;
            break;
        case SYS_SENDMSG:
            ret = sys_sendmsg_handler((int)regs->rdi, (const struct msghdr *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_RECVMSG:
            ret = sys_recvmsg_handler((int)regs->rdi, (struct msghdr *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_SETSOCKOPT:
            ret = sys_setsockopt_handler((int)regs->rdi, (int)regs->rsi, (int)regs->rdx, (const void *)regs->r10, (uint32_t)regs->r8);
            break;
        case SYS_GETSOCKOPT:
            ret = sys_getsockopt_handler((int)regs->rdi, (int)regs->rsi, (int)regs->rdx, (void *)regs->r10, (uint32_t *)regs->r8);
            break;
        case SYS_GETPEERNAME:
            ret = sys_getpeername_handler((int)regs->rdi, (struct sockaddr_un *)regs->rsi, (uint32_t *)regs->rdx);
            break;
        case SYS_GETSOCKNAME:
            ret = sys_getsockname_handler((int)regs->rdi, (struct sockaddr_un *)regs->rsi, (uint32_t *)regs->rdx);
            break;
        case SYS_SHUTDOWN:
            ret = sys_shutdown_handler((int)regs->rdi, (int)regs->rsi);
            break;
        case SYS_MREMAP:
            ret = sys_mremap_handler(regs->rdi, (size_t)regs->rsi, (size_t)regs->rdx, (int)regs->r10);
            break;
        case SYS_MSYNC:
        case SYS_MINCORE:
        case SYS_MADVISE:
            ret = 0;
            break;
        case SYS_DUP:
            ret = sys_dup_handler((int)regs->rdi);
            break;
        case SYS_DUP2:
            ret = sys_dup2_handler((int)regs->rdi, (int)regs->rsi);
            break;
            
        case SYS_NANOSLEEP:
            ret = sys_nanosleep_handler((const struct linux_timespec *)regs->rdi, (struct linux_timespec *)regs->rsi);
            break;
        case SYS_GETPID:
            ret = current_task ? current_task->process->pid : 1;
            break;
        case SYS_SOCKET:
            ret = sys_socket_handler((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
            break;
        case SYS_BIND:
            ret = sys_bind_handler((int)regs->rdi, (const struct sockaddr_un *)regs->rsi, (uint32_t)regs->rdx);
            break;
        case SYS_LISTEN:
            ret = sys_listen_handler((int)regs->rdi, (int)regs->rsi);
            break;
        case SYS_ACCEPT:
            ret = sys_accept_handler((int)regs->rdi, (struct sockaddr_un *)regs->rsi, (uint32_t *)regs->rdx);
            break;
        case SYS_LINK:
            ret = sys_link_handler((const char *)regs->rdi, (const char *)regs->rsi);
            break;
        case SYS_LINKAT:
            ret = sys_link_handler((const char *)regs->rsi, (const char *)regs->r10);
            break;
        case SYS_CONNECT:
            ret = sys_connect_handler((int)regs->rdi, (const struct sockaddr_un *)regs->rsi, (uint32_t)regs->rdx);
            break;
        case SYS_SENDTO:
            ret = sys_write_handler((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_GETITIMER:
        case SYS_SETITIMER:
            ret = 0; // Pretend interval timer is set
            break;
        case SYS_RECVFROM:
            ret = sys_read_handler((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_CLONE:
            ret = sys_clone_handler(regs->rdi, regs->rsi, (int *)regs->rdx, (int *)regs->r10, regs->r8, regs);
            break;
        case SYS_FORK:
        case SYS_VFORK:
            ret = sys_clone_handler(0, 0, NULL, NULL, 0, regs);
            break;
        case SYS_EXECVE:
            ret = sys_execve_handler((const char *)regs->rdi, (char *const *)regs->rsi, (char *const *)regs->rdx, regs);
            break;
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            ret = sys_exit_handler((int)regs->rdi);
            break;
        case SYS_WAIT4:
            ret = sys_wait4_handler((int)regs->rdi, (int *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_KILL:
        case SYS_TKILL:
        case SYS_TGKILL:
            ret = sys_kill_handler((int)regs->rdi, (int)regs->rsi);
            break;
        case SYS_UNAME:
            ret = sys_uname_handler((struct linux_utsname *)regs->rdi);
            break;
        case SYS_FCNTL:
            ret = sys_fcntl_handler((int)regs->rdi, (int)regs->rsi, regs->rdx);
            break;
        case SYS_TRUNCATE:
            ret = sys_truncate_handler((const char *)regs->rdi, (int64_t)regs->rsi);
            break;
        case SYS_FTRUNCATE:
            ret = sys_ftruncate_handler((int)regs->rdi, (int64_t)regs->rsi);
            break;
        case SYS_GETCWD:
            ret = sys_getcwd_handler((char *)regs->rdi, (size_t)regs->rsi);
            break;
        case SYS_CHDIR:
            ret = sys_chdir_handler((const char *)regs->rdi);
            break;
        case SYS_RENAME:
            ret = sys_renameat_handler(AT_FDCWD, (const char *)regs->rdi, AT_FDCWD, (const char *)regs->rsi);
            break;
        case SYS_MKDIR:
            ret = sys_mkdirat_handler(AT_FDCWD, (const char *)regs->rdi, (int)regs->rsi);
            break;
        case SYS_RMDIR:
        case SYS_UNLINK:
            ret = sys_unlinkat_handler(AT_FDCWD, (const char *)regs->rdi, 0);
            break;
        case SYS_READLINK:
            ret = -EINVAL;
            break;
        case SYS_CHMOD:
            ret = sys_chmod_handler((const char *)regs->rdi, (int)regs->rsi);
            break;
        case SYS_FCHMOD:
            ret = 0;
            break;
        case SYS_CHOWN:
            ret = sys_chown_handler((const char *)regs->rdi, (int)regs->rsi, (int)regs->rdx);
            break;
        case SYS_FCHOWN:
            ret = 0;
            break;
        case SYS_UMASK:
            ret = sys_umask_handler((int)regs->rdi);
            break;
        case SYS_GETTIMEOFDAY:
            ret = sys_gettimeofday_handler((struct linux_timeval *)regs->rdi, (struct linux_timezone *)regs->rsi);
            break;
        case SYS_GETRLIMIT:
            ret = sys_prlimit64_handler(0, (int)regs->rdi, NULL, (struct linux_rlimit *)regs->rsi);
            break;
        case SYS_GETRUSAGE:
            ret = sys_getrusage_handler((int)regs->rdi, (struct rusage *)regs->rsi);
            break;
        case SYS_SYSINFO:
            ret = sys_sysinfo_handler((equant_sysinfo_t *)regs->rdi);
            break;
        case SYS_TIMES:
            ret = sys_times_handler((struct tms *)regs->rdi);
            break;
        case SYS_SHMGET:
            ret = sys_shmget_handler((uint64_t)regs->rdi, (size_t)regs->rsi, (int)regs->rdx);
            break;
        case SYS_SHMAT:
            ret = sys_shmat_handler((int)regs->rdi, (uint64_t)regs->rsi, (int)regs->rdx);
            break;
        case SYS_SHMDT:
            ret = sys_shmdt_handler((uint64_t)regs->rdi);
            break;
        case SYS_SHMCTL:
            ret = sys_shmctl_handler((int)regs->rdi, (int)regs->rsi, (void *)regs->rdx);
            break;
        case SYS_GETUID:
        case SYS_GETEUID:
            ret = (current_task && current_task->process) ? (int64_t)current_task->process->uid : 0;
            break;
        case SYS_GETGID:
        case SYS_GETEGID:
            ret = (current_task && current_task->process) ? (int64_t)current_task->process->gid : 0;
            break;
            ret = 0;
            break;
        case SYS_GETPPID:
            ret = (current_task && current_task->process) ? (int64_t)current_task->process->parent_pid : 1;
            break;
        case SYS_GETPGID:
            ret = sys_getpgid_handler((int)regs->rdi);
            break;
        case SYS_GETPGRP:
            ret = sys_getpgid_handler(0);
            break;
        case SYS_SETPGID: {
            int pid = (int)regs->rdi;
            int pgid = (int)regs->rsi;
            if (current_task && current_task->process) {
                if (pid == 0) pid = (int)current_task->process->pid;
                if (pgid == 0) pgid = pid;
                current_task->process->pgid = (uint64_t)pgid;
            }
            ret = 0;
            break;
        }
        case SYS_SETSID:
            if (current_task && current_task->process) {
                current_task->process->pgid = current_task->process->pid;
                ret = (int64_t)current_task->process->pid;
            } else {
                ret = 1;
            }
            break;
        case SYS_ARCH_PRCTL:
            ret = sys_arch_prctl_handler((int)regs->rdi, regs->rsi);
            break;
        case SYS_GETTID:
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_FUTEX:
            ret = sys_futex_handler((uint32_t *)regs->rdi, (int)regs->rsi, (uint32_t)regs->rdx, (const struct linux_timespec *)regs->r10, (uint32_t *)regs->r8, (uint32_t)regs->r9);
            break;
        case SYS_GETDENTS64:
            ret = sys_getdents64_handler((int)regs->rdi, (void *)regs->rsi, (size_t)regs->rdx);
            break;
        case SYS_SET_TID_ADDRESS:
            if (current_task && current_task->process) {
                current_task->process->clear_child_tid = regs->rdi;
            }
            ret = current_task ? current_task->id : 1;
            break;
        case SYS_CLOCK_GETTIME:
            ret = sys_clock_gettime_handler((int)regs->rdi, (struct linux_timespec *)regs->rsi);
            break;
        case SYS_OPENAT:
            ret = sys_openat_handler((int)regs->rdi, (const char *)regs->rsi, (int)regs->rdx, (int)regs->r10);
            break;
        case SYS_MKDIRAT:
            ret = sys_mkdirat_handler((int)regs->rdi, (const char *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_NEWFSTATAT:
            ret = sys_stat_handler((const char *)regs->rsi, (struct linux_stat *)regs->rdx);
            break;
        case SYS_UNLINKAT:
            ret = sys_unlinkat_handler((int)regs->rdi, (const char *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_RENAMEAT:
            ret = sys_renameat_handler((int)regs->rdi, (const char *)regs->rsi, (int)regs->rdx, (const char *)regs->r10);
            break;
        case SYS_FACCESSAT:
            ret = sys_access_handler((const char *)regs->rsi, (int)regs->rdx);
            break;
        case SYS_DUP3:
            ret = sys_dup3_handler((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
            break;
        case SYS_PIPE2:
            ret = sys_pipe2_handler((int *)regs->rdi, (int)regs->rsi);
            break;
        case SYS_PRLIMIT64:
            ret = sys_prlimit64_handler((int)regs->rdi, (int)regs->rsi, (const struct linux_rlimit *)regs->rdx, (struct linux_rlimit *)regs->r10);
            break;
        case SYS_EQUANT_KDIAG: {
        const char *user_cmd = (const char *)regs->rdi;
        if (!user_cmd) {
            ret = -EFAULT;
            break;
        }
        // Execute cleanly without spilling the kernel prompt into Bash
        shell_execute_diag(user_cmd);
        ret = 0;
        break;
    }
        default:
            ret = -ENOSYS;
            break;
    }

    regs->rax = (uint64_t)ret;

    if (!quiet) {
        strace_log("[STRACE %u] < %s = %lld (0x%llx)\n",
                   pid, name, (long long)ret, (unsigned long long)ret);
    }
}

void init_syscalls(void) {
    uint64_t efer = read_msr(0xC0000080);
    write_msr(0xC0000080, efer | 1); // SCE (Syscall Enable)

    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(0xC0000081, star);

    write_msr(0xC0000082, (uint64_t)syscall_entry_asm);

    // Standard Linux x86_64 Syscall FMASK:
    // Clears TF (0x100), IF (0x200), DF (0x400), IOPL (0x3000), NT (0x4000), AC (0x40000)
    write_msr(0xC0000084, 0x257FD5);
}

static int __init init_syscalls_initcall(void) {
    init_syscalls();
    return 0;
}
arch_initcall(init_syscalls_initcall);