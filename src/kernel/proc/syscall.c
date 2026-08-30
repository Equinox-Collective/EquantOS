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
#include "stdio.h"
#include "../fs/vfs.h"
#include "../fs/ramfs.h"
#include "../core/initcall.h"
#include "../drivers/tty/tty.h"

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

static bool tty_has_input(void) {
    return serial_received(COM1) || input_has_events();
}

// ============================================================================
// 1. File Descriptor & I/O Handlers
// ============================================================================

static int64_t sys_read_handler(int fd, void *buf, size_t count) {
    if (count == 0) return 0;
    if (!buf) return -EFAULT;

    if (fd == 0) {
        char *out = (char *)buf;
        char c = tty_getchar();
        if (c == '\r') c = '\n';
        out[0] = c;
        return 1;
    }

    if (!current_task || !current_task->process) return -EBADF;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

    uint64_t offset = current_task->process->file_offsets[fd];
    int64_t bytes = vfs_read(node, offset, count, (uint8_t *)buf);
    if (bytes > 0) {
        current_task->process->file_offsets[fd] += bytes;
    }
    return bytes;
}

static int64_t sys_write_handler(int fd, const void *user_buf, size_t count) {
    if (count == 0) return 0;
    if (!user_buf) return -EFAULT;

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
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_node_t *node = current_task->process->files[fd];
    if (!node) return -EBADF;

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
    if (fd >= 0 && fd <= 2) {
        if (req == TIOCGWINSZ && arg) {
            struct winsize *ws = (struct winsize *)arg;
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = 640;
            ws->ws_ypixel = 480;
            return 0;
        }
        if (req == TIOCGPGRP && arg) {
            uint64_t pgid = (current_task && current_task->process && current_task->process->pgid) 
                            ? current_task->process->pgid 
                            : (current_task ? current_task->process->pid : 1);
            *(int *)arg = (int)pgid;
            return 0;
        }
        if (req == TIOCSPGRP) {
            if (arg && current_task && current_task->process) {
                current_task->process->pgid = (uint64_t)(*(int *)arg);
            }
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
    return -ENOTTY;
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
        d->d_type = (child->flags & FS_DIRECTORY) ? 4 : 8;
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
    (void)prot;
    if (length == 0) return -EINVAL;

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_len = page_count * PAGE_SIZE;

    uint64_t virt_addr = addr;
    if (virt_addr == 0 || !(flags & MAP_FIXED)) {
        virt_addr = mmap_virtual_base;
        mmap_virtual_base += aligned_len;
    }

    if (!current_task || !current_task->process) return -EINVAL;
    page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);

    for (size_t i = 0; i < page_count; i++) {
        void *phys = pmm_alloc();
        if (!phys) return -ENOMEM;
        memset((void *)VIRT((uint64_t)phys), 0, PAGE_SIZE);

        if (fd >= 0 && fd < MAX_OPEN_FILES && current_task->process->files[fd]) {
            vfs_node_t *node = current_task->process->files[fd];
            uint64_t file_pos = (uint64_t)offset + (i * PAGE_SIZE);
            if (file_pos < node->length) {
                uint64_t chunk = (node->length - file_pos > PAGE_SIZE) ? PAGE_SIZE : (node->length - file_pos);
                vfs_read(node, file_pos, chunk, (uint8_t *)VIRT((uint64_t)phys));
            }
        }

        vmm_map(pml4, virt_addr + (i * PAGE_SIZE), (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    return (int64_t)virt_addr;
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
        sched_dequeue(current_task);
    }

    sched_yield();
    for (;;) { __asm__ volatile("hlt"); }
    return 0;
}

static int64_t sys_clone_handler(uint64_t flags, uint64_t stack_top, int *parent_tid, int *child_tid, uint64_t tls, syscall_regs_t *regs) {
    if (!current_task || !current_task->process) return -EAGAIN;

    page_table_t *child_pml4 = NULL;
    if (flags & CLONE_VM) {
        child_pml4 = (page_table_t *)VIRT(current_task->process->cr3);
    } else {
        child_pml4 = vmm_clone_address_space(current_task->process->cr3);
        if (!child_pml4) return -ENOMEM;
    }

    process_t *child_proc = current_task->process;
    if (!(flags & CLONE_THREAD)) {
        child_proc = (process_t *)kmalloc(sizeof(process_t));
        if (!child_proc) return -ENOMEM;
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
    if (!child_task) return -ENOMEM;
    memset(child_task, 0, sizeof(task_t));

    task_init_fpu(child_task);
    memcpy(task_fpu_area(child_task), task_fpu_area(current_task), 512);

    child_task->id = (flags & CLONE_THREAD) ? next_pid++ : child_proc->pid;
    child_task->state = TASK_STATE_RUNNABLE;
    child_task->running = true;
    child_task->priority = PRIO_NORMAL;
    child_task->time_slice = 10;
    child_task->fs_base = (flags & CLONE_SETTLS) ? tls : current_task->fs_base;
    child_task->process = child_proc;

    child_task->kstack_at_bottom = (uint64_t)kmalloc(16384) + 16384;
    uint64_t *stack = (uint64_t *)child_task->kstack_at_bottom;

    // Build iretq stack frame:
    *--stack = 0x1B;                                // SS
    *--stack = stack_top ? stack_top : regs->rsp;   // RSP
    *--stack = 0x202;                               // Clean User RFLAGS (IF=1)
    *--stack = 0x23;                                // CS
    *--stack = (regs->rcx != 0) ? regs->rcx : regs->rip; // Userland RIP from RCX

    *--stack = 0; // int_no
    *--stack = 0; // error_code
    *--stack = regs->r15;
    *--stack = regs->r14;
    *--stack = regs->r13;
    *--stack = regs->r12;
    *--stack = regs->r11;
    *--stack = regs->r10;
    *--stack = regs->r9;
    *--stack = regs->r8;
    *--stack = regs->rdi;
    *--stack = regs->rsi;
    *--stack = regs->rbp;
    *--stack = regs->rdx;
    *--stack = regs->rcx;
    *--stack = regs->rbx;
    *--stack = 0; // RAX = 0 in child (fork return value)

    child_task->rsp = (uint64_t)stack;

    if ((flags & CLONE_PARENT_SETTID) && parent_tid) {
        *parent_tid = (int)child_task->id;
    }
    if ((flags & CLONE_CHILD_SETTID) && child_tid) {
        *child_tid = (int)child_task->id;
    }
    if (flags & CLONE_CHILD_CLEARTID) {
        child_proc->clear_child_tid = (uint64_t)child_tid;
    }

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

static int64_t sys_poll_handler(struct linux_pollfd *fds, uint64_t nfds, int timeout) {
    if (!fds && nfds > 0) return -EFAULT;

    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd == 0) {
            if (tty_has_input()) {
                fds[i].revents |= (fds[i].events & POLLIN);
                if (fds[i].revents) ready++;
            }
        } else if (fds[i].fd == 1 || fds[i].fd == 2) {
            fds[i].revents |= (fds[i].events & POLLOUT);
            if (fds[i].revents) ready++;
        }
    }

    if (ready > 0 || timeout == 0) return ready;

    uint64_t start_tick = tick;
    uint64_t max_ticks = (timeout < 0) ? (uint64_t)-1 : ((uint64_t)timeout / 10);

    while (ready == 0) {
        if (timeout >= 0 && (tick - start_tick) >= max_ticks) break;
        if (tty_has_input()) {
            for (uint64_t i = 0; i < nfds; i++) {
                if (fds[i].fd == 0) {
                    fds[i].revents |= (fds[i].events & POLLIN);
                    if (fds[i].revents) ready++;
                }
            }
            break;
        }
        __asm__ volatile("sti; pause");
        sched_yield();
    }
    return ready;
}

static int64_t sys_pselect6_handler(int nfds, void *readfds, void *writefds, void *exceptfds, 
                                   const struct linux_timespec *timeout, const void *sigmask) {
    (void)writefds; (void)exceptfds; (void)sigmask;
    uint8_t *rfds = (uint8_t *)readfds;

    if (rfds && (rfds[0] & 1) && tty_has_input()) {
        return 1;
    }

    uint64_t start_tick = tick;
    uint64_t max_ticks = (timeout == NULL) ? (uint64_t)-1 : (timeout->tv_sec * 100 + timeout->tv_nsec / 10000000ULL);

    while (1) {
        if (tty_has_input()) {
            if (rfds) rfds[0] = 1;
            return 1;
        }
        if (timeout != NULL && (tick - start_tick) >= max_ticks) {
            if (rfds) rfds[0] = 0;
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
    (void)domain; (void)type; (void)protocol;
    int pipefd[2];
    if (pipe_create(pipefd) == 0) {
        return pipefd[0];
    }
    return -EAFNOSUPPORT;
}

// ============================================================================
// Master Syscall Dispatcher Table
// ============================================================================

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

    uint8_t *elf_buf = (uint8_t *)kmalloc(file->length);
    if (!elf_buf) return -ENOMEM;

    if (vfs_read(file, 0, file->length, elf_buf) <= 0) {
        kfree(elf_buf);
        return -EIO;
    }

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

    bool ok = elf_execve_replace(elf_buf, file->length, argc, exec_argv, &new_entry, &new_rsp, &new_cr3);
    kfree(elf_buf);

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

void syscall_handler(void *regs_ptr) {
    syscall_regs_t *regs = (syscall_regs_t *)regs_ptr;
    uint64_t syscall_no = regs->rax;
    int64_t ret = -ENOSYS;

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
        case SYS_PSELECT6:
            ret = sys_pselect6_handler((int)regs->rdi, (void *)regs->rsi, (void *)regs->rdx, 
                                       (void *)regs->r10, (const struct linux_timespec *)regs->r8, (const void *)regs->r9);
            break;
        case SYS_SCHED_YIELD:
            sched_yield();
            ret = 0;
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
        case SYS_CONNECT:
        case SYS_ACCEPT:
        case SYS_BIND:
        case SYS_LISTEN:
            ret = -ECONNREFUSED;
            break;
        case SYS_SENDTO:
            ret = sys_write_handler((int)regs->rdi, (const void *)regs->rsi, (size_t)regs->rdx);
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
        case SYS_GETUID:
        case SYS_GETGID:
        case SYS_GETEUID:
        case SYS_GETEGID:
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
        default:
            ret = -ENOSYS;
            break;
    }

    regs->rax = (uint64_t)ret;
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