// src/kernel/drivers/tty/pty.c - Pseudo-Terminal (PTY) Subsystem for EquantOS
#include "pty.h"
#include "../../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../../proc/task.h"
#include "../../proc/sched.h"
#include "../../proc/syscall.h"
#include "../../fs/vfs.h"
#include "../serial/serial.h"

extern int64_t sys_kill_handler(int pid, int sig);

static pty_pair_t g_pty_table[PTY_MAX_PAIRS];
static bool g_pty_initialized = false;

static vfs_file_operations_t pty_master_ops;
static vfs_file_operations_t pty_slave_ops;

static bool is_node_nonblocking(vfs_node_t *node) {
    if (!current_task || !current_task->process) return false;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (current_task->process->files[i] == node) {
            return (current_task->process->file_flags[i] & O_NONBLOCK) != 0;
        }
    }
    return false;
}

static void pty_free_pair(pty_pair_t *pair) {
    if (!pair) return;
    if (pair->master_node) {
        kfree(pair->master_node);
        pair->master_node = NULL;
    }
    if (pair->slave_node) {
        kfree(pair->slave_node);
        pair->slave_node = NULL;
    }
    memset(pair, 0, sizeof(pty_pair_t));
}

// -----------------------------------------------------------------------------
// Buffer Helper Routines
// -----------------------------------------------------------------------------

static void pty_buf_put(uint8_t *buf, uint32_t *head, uint32_t *count, uint8_t byte) {
    if (*count < PTY_BUF_SIZE) {
        buf[*head] = byte;
        *head = (*head + 1) % PTY_BUF_SIZE;
        (*count)++;
    }
}

static uint8_t pty_buf_get(uint8_t *buf, uint32_t *tail, uint32_t *count) {
    if (*count == 0) return 0;
    uint8_t byte = buf[*tail];
    *tail = (*tail + 1) % PTY_BUF_SIZE;
    (*count)--;
    return byte;
}

// -----------------------------------------------------------------------------
// Master Operations (Read from out_buf, Write into in_buf)
// -----------------------------------------------------------------------------

static int64_t pty_master_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return 0;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    while (pair->out_count == 0) {
        if (!pair->slave_open && pair->slave_refs == 0) {
            return 0; // EOF: slave is closed
        }
        if (is_node_nonblocking(node)) {
            return -EAGAIN;
        }
        if (current_task) {
            pair->blocked_master_read = current_task;
            sched_block(current_task);
            sched_yield();
            pair->blocked_master_read = NULL;
        } else {
            __asm__ volatile("pause");
        }
    }

    uint64_t to_read = (size < (uint64_t)pair->out_count) ? size : (uint64_t)pair->out_count;
    for (uint64_t i = 0; i < to_read; i++) {
        buffer[i] = pty_buf_get(pair->out_buf, &pair->out_tail, &pair->out_count);
    }

    if (pair->blocked_slave_write) {
        sched_unblock(pair->blocked_slave_write);
        pair->blocked_slave_write = NULL;
    }
    io_wake_all();

    return (int64_t)to_read;
}

static int64_t pty_master_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return 0;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    if (!pair->slave_open && pair->slave_refs == 0) {
        return -EIO;
    }

    uint64_t written = 0;
    while (written < size) {
        while (pair->in_count >= PTY_BUF_SIZE) {
            if (!pair->slave_open && pair->slave_refs == 0) {
                return written > 0 ? (int64_t)written : -EIO;
            }
            if (is_node_nonblocking(node)) {
                return written > 0 ? (int64_t)written : -EAGAIN;
            }
            if (current_task) {
                pair->blocked_master_write = current_task;
                sched_block(current_task);
                sched_yield();
                pair->blocked_master_write = NULL;
            } else {
                __asm__ volatile("pause");
            }
        }

        uint8_t c = buffer[written++];

        // ISIG: signal handling on Ctrl+C, Ctrl+\, Ctrl+Z
        if (pair->tio.c_lflag & 0x0001 /* ISIG */) {
            if (c == 0x03 /* Ctrl+C */) {
                if (pair->foreground_pgid > 0) {
                    sys_kill_handler(-(int)pair->foreground_pgid, SIGINT);
                }
                if (pair->tio.c_lflag & 0x0008 /* ECHO */) {
                    pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '^');
                    pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, 'C');
                    pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '\n');
                }
                continue;
            } else if (c == 0x1C /* Ctrl+\ */) {
                if (pair->foreground_pgid > 0) {
                    sys_kill_handler(-(int)pair->foreground_pgid, SIGQUIT);
                }
                continue;
            } else if (c == 0x1A /* Ctrl+Z */) {
                if (pair->foreground_pgid > 0) {
                    sys_kill_handler(-(int)pair->foreground_pgid, SIGTSTP);
                }
                continue;
            }
        }

        // ICRNL: map CR to NL on input
        if ((pair->tio.c_iflag & 0x0100 /* ICRNL */) && c == '\r') {
            c = '\n';
        }

        // ICANON backspace processing: DEL (0x7F) or Backspace (0x08)
        if ((pair->tio.c_lflag & 0x0002 /* ICANON */) && (c == 0x7F || c == '\b')) {
            if (pair->in_count > 0) {
                uint32_t last_idx = (pair->in_head - 1 + PTY_BUF_SIZE) % PTY_BUF_SIZE;
                if (pair->in_buf[last_idx] != '\n') {
                    pair->in_head = last_idx;
                    pair->in_count--;
                    if ((pair->tio.c_lflag & 0x0008 /* ECHO */) && (pair->tio.c_lflag & 0x0010 /* ECHOE */)) {
                        pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '\b');
                        pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, ' ');
                        pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '\b');
                    }
                }
            }
            continue;
        }

        // Echoing back to master output
        if (pair->tio.c_lflag & 0x0008 /* ECHO */) {
            if (c == '\n') {
                if (pair->tio.c_oflag & 0x0004 /* ONLCR */) {
                    pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '\r');
                }
                pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '\n');
            } else {
                pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, c);
            }
        }

        pty_buf_put(pair->in_buf, &pair->in_head, &pair->in_count, c);
    }

    if (pair->blocked_slave_read) {
        sched_unblock(pair->blocked_slave_read);
        pair->blocked_slave_read = NULL;
    }
    if (pair->blocked_master_read) {
        sched_unblock(pair->blocked_master_read);
        pair->blocked_master_read = NULL;
    }
    io_wake_all();

    return (int64_t)written;
}

static void pty_master_close(vfs_node_t *node) {
    if (!node || !node->ptr) return;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    pair->master_refs--;
    if (pair->master_refs <= 0) {
        pair->master_open = false;
        if (pair->blocked_slave_read) {
            sched_unblock(pair->blocked_slave_read);
            pair->blocked_slave_read = NULL;
        }
        if (pair->blocked_slave_write) {
            sched_unblock(pair->blocked_slave_write);
            pair->blocked_slave_write = NULL;
        }
        io_wake_all();

        if (!pair->slave_open && pair->slave_refs <= 0) {
            pty_free_pair(pair);
        }
    }
}

static int pty_master_ioctl(vfs_node_t *node, uint64_t req, void *arg) {
    req = (uint32_t)req;
    if (!node || !node->ptr) return -EBADF;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    switch (req) {
        case TIOCGPTN: {
            if (!arg) return -EFAULT;
            *(unsigned int *)arg = (unsigned int)pair->id;
            return 0;
        }
        case TIOCSPTLCK: {
            if (!arg) return -EFAULT;
            pair->locked = (*(int *)arg != 0);
            return 0;
        }
        case TIOCGPTLCK: {
            if (!arg) return -EFAULT;
            *(int *)arg = pair->locked ? 1 : 0;
            return 0;
        }
        case TIOCGWINSZ: {
            if (!arg) return -EFAULT;
            *(struct winsize *)arg = pair->ws;
            return 0;
        }
        case TIOCSWINSZ: {
            if (!arg) return -EFAULT;
            pair->ws = *(struct winsize *)arg;
            if (pair->foreground_pgid > 0) {
                sys_kill_handler(-(int)pair->foreground_pgid, SIGWINCH);
            }
            return 0;
        }
        case FIONREAD: {
            if (!arg) return -EFAULT;
            *(int *)arg = (int)pair->out_count;
            return 0;
        }
        case TCGETS: {
            if (!arg) return -EFAULT;
            *(struct termios *)arg = pair->tio;
            return 0;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            if (!arg) return -EFAULT;
            pair->tio = *(struct termios *)arg;
            return 0;
        }
        case TIOCGPGRP: {
            if (!arg) return -EFAULT;
            *(int *)arg = (int)pair->foreground_pgid;
            return 0;
        }
        case TIOCSPGRP: {
            if (!arg) return -EFAULT;
            pair->foreground_pgid = (uint64_t)(*(int *)arg);
            return 0;
        }
        default:
            return -ENOTTY;
    }
}

// -----------------------------------------------------------------------------
// Slave Operations (Read from in_buf, Write into out_buf)
// -----------------------------------------------------------------------------

static bool pty_buf_has_line(pty_pair_t *pair) {
    for (uint32_t i = 0; i < pair->in_count; i++) {
        uint8_t c = pair->in_buf[(pair->in_tail + i) % PTY_BUF_SIZE];
        if (c == '\n' || c == 0x04) return true;
    }
    return false;
}

static int64_t pty_slave_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return 0;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    while (pair->in_count == 0 || ((pair->tio.c_lflag & 0x0002 /* ICANON */) && !pty_buf_has_line(pair))) {
        if (!pair->master_open) {
            if (pair->in_count > 0) break;
            return 0; // EOF: master closed
        }
        if (is_node_nonblocking(node)) {
            return -EAGAIN;
        }
        if (current_task) {
            pair->blocked_slave_read = current_task;
            sched_block(current_task);
            sched_yield();
            pair->blocked_slave_read = NULL;
        } else {
            __asm__ volatile("pause");
        }
    }

    uint64_t bytes_read = 0;

    if (pair->tio.c_lflag & 0x0002 /* ICANON */) {
        // Canonical mode: read up to newline or EOF (0x04)
        while (bytes_read < size && pair->in_count > 0) {
            uint8_t c = pty_buf_get(pair->in_buf, &pair->in_tail, &pair->in_count);
            if (c == 0x04 /* Ctrl+D EOF */) {
                break;
            }
            buffer[bytes_read++] = c;
            if (c == '\n') {
                break;
            }
        }
    } else {
        // Raw / Non-canonical mode: read available bytes up to size
        uint64_t to_read = (size < (uint64_t)pair->in_count) ? size : (uint64_t)pair->in_count;
        for (uint64_t i = 0; i < to_read; i++) {
            buffer[bytes_read++] = pty_buf_get(pair->in_buf, &pair->in_tail, &pair->in_count);
        }
    }

    if (pair->blocked_master_write) {
        sched_unblock(pair->blocked_master_write);
        pair->blocked_master_write = NULL;
    }
    io_wake_all();

    return (int64_t)bytes_read;
}

static int64_t pty_slave_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr || !buffer || size == 0) return 0;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    if (!pair->master_open) {
        return -EPIPE;
    }

    uint64_t written = 0;
    while (written < size) {
        while (pair->out_count >= PTY_BUF_SIZE) {
            if (!pair->master_open) {
                return written > 0 ? (int64_t)written : -EPIPE;
            }
            if (is_node_nonblocking(node)) {
                return written > 0 ? (int64_t)written : -EAGAIN;
            }
            if (current_task) {
                pair->blocked_slave_write = current_task;
                sched_block(current_task);
                sched_yield();
                pair->blocked_slave_write = NULL;
            } else {
                __asm__ volatile("pause");
            }
        }

        uint8_t c = buffer[written++];

        // OPOST & ONLCR output mapping
        if (pair->tio.c_oflag & 0x0001 /* OPOST */) {
            if ((pair->tio.c_oflag & 0x0004 /* ONLCR */) && c == '\n') {
                if (pair->out_count + 2 <= PTY_BUF_SIZE) {
                    pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '\r');
                    pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, '\n');
                    continue;
                }
            }
        }

        pty_buf_put(pair->out_buf, &pair->out_head, &pair->out_count, c);
    }

    if (pair->blocked_master_read) {
        sched_unblock(pair->blocked_master_read);
        pair->blocked_master_read = NULL;
    }
    io_wake_all();

    return (int64_t)written;
}

static void pty_slave_open(vfs_node_t *node) {
    if (!node || !node->ptr) return;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;
    pair->slave_refs++;
    pair->slave_open = true;
}

static void pty_slave_close(vfs_node_t *node) {
    if (!node || !node->ptr) return;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    pair->slave_refs--;
    if (pair->slave_refs <= 0) {
        pair->slave_open = false;
        if (pair->blocked_master_read) {
            sched_unblock(pair->blocked_master_read);
            pair->blocked_master_read = NULL;
        }
        if (pair->blocked_master_write) {
            sched_unblock(pair->blocked_master_write);
            pair->blocked_master_write = NULL;
        }
        io_wake_all();

        if (!pair->master_open && pair->master_refs <= 0) {
            pty_free_pair(pair);
        }
    }
}

static int pty_slave_ioctl(vfs_node_t *node, uint64_t req, void *arg) {
    req = (uint32_t)req;
    if (!node || !node->ptr) return -EBADF;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;

    switch (req) {
        case TCGETS: {
            if (!arg) return -EFAULT;
            *(struct termios *)arg = pair->tio;
            return 0;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            if (!arg) return -EFAULT;
            pair->tio = *(struct termios *)arg;
            return 0;
        }
        case TIOCGWINSZ: {
            if (!arg) return -EFAULT;
            *(struct winsize *)arg = pair->ws;
            return 0;
        }
        case TIOCSWINSZ: {
            if (!arg) return -EFAULT;
            pair->ws = *(struct winsize *)arg;
            if (pair->foreground_pgid > 0) {
                sys_kill_handler(-(int)pair->foreground_pgid, SIGWINCH);
            }
            return 0;
        }
        case TIOCSCTTY: {
            if (current_task && current_task->process) {
                pair->session_sid = current_task->process->pid;
                pair->foreground_pgid = current_task->process->pgid ? current_task->process->pgid : current_task->process->pid;
                current_task->process->controlling_tty = pair->slave_node;
            }
            return 0;
        }
        case TIOCNOTTY: {
            pair->session_sid = 0;
            if (current_task && current_task->process) {
                current_task->process->controlling_tty = NULL;
            }
            return 0;
        }
        case TIOCGPGRP: {
            if (!arg) return -EFAULT;
            uint64_t pg = pair->foreground_pgid ? pair->foreground_pgid : (current_task && current_task->process ? current_task->process->pgid : 1);
            *(int *)arg = (int)pg;
            return 0;
        }
        case TIOCSPGRP: {
            if (!arg) return -EFAULT;
            int new_pg = *(int *)arg;
            pair->foreground_pgid = (new_pg > 0) ? (uint64_t)new_pg : (current_task && current_task->process ? current_task->process->pid : 1);
            return 0;
        }
        case FIONREAD: {
            if (!arg) return -EFAULT;
            *(int *)arg = (int)pair->in_count;
            return 0;
        }
        case TIOCGSID: {
            if (!arg) return -EFAULT;
            *(int *)arg = (int)pair->session_sid;
            return 0;
        }
        default:
            return -ENOTTY;
    }
}

// -----------------------------------------------------------------------------
// VFS Operations Tables
// -----------------------------------------------------------------------------

static vfs_file_operations_t pty_master_ops = {
    .read    = pty_master_read,
    .write   = pty_master_write,
    .open    = NULL,
    .close   = pty_master_close,
    .readdir = NULL,
    .finddir = NULL,
    .create  = NULL,
    .ioctl   = pty_master_ioctl,
    .mmap    = NULL
};

static vfs_file_operations_t pty_slave_ops = {
    .read    = pty_slave_read,
    .write   = pty_slave_write,
    .open    = pty_slave_open,
    .close   = pty_slave_close,
    .readdir = NULL,
    .finddir = NULL,
    .create  = NULL,
    .ioctl   = pty_slave_ioctl,
    .mmap    = NULL
};

// -----------------------------------------------------------------------------
// Public Subsystem API
// -----------------------------------------------------------------------------

void pty_subsystem_init(void) {
    if (g_pty_initialized) return;
    memset(g_pty_table, 0, sizeof(g_pty_table));
    g_pty_initialized = true;
    serial_puts(COM1, "[PTY] Pseudo-terminal subsystem initialized (max 32 pairs).\n");
}

vfs_node_t *pty_create_master_node(void) {
    if (!g_pty_initialized) pty_subsystem_init();

    int slot = -1;
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (!g_pty_table[i].allocated) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        serial_puts(COM1, "[PTY ERROR] Out of PTY slots!\n");
        return NULL;
    }

    pty_pair_t *pair = &g_pty_table[slot];
    memset(pair, 0, sizeof(pty_pair_t));

    pair->id = slot;
    pair->allocated = true;
    pair->locked = true; // Locked until unlocked via TIOCSPTLCK
    pair->master_open = true;
    pair->slave_open = false;
    pair->master_refs = 1;
    pair->slave_refs = 0;

    // Default 80x24 window geometry
    pair->ws.ws_row = 24;
    pair->ws.ws_col = 80;
    pair->ws.ws_xpixel = 0;
    pair->ws.ws_ypixel = 0;

    // Standard Linux termios default flags
    pair->tio.c_iflag = 0x4100; // ICRNL | IXON
    pair->tio.c_oflag = 0x0005; // OPOST | ONLCR
    pair->tio.c_cflag = 0x00BF; // B38400 | CS8 | CREAD
    pair->tio.c_lflag = 0x8A3B; // ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN
    pair->tio.c_cc[0]  = 0x03; // VINTR (Ctrl+C)
    pair->tio.c_cc[1]  = 0x1C; // VQUIT (Ctrl+\)
    pair->tio.c_cc[2]  = 0x7F; // VERASE (DEL/Backspace)
    pair->tio.c_cc[3]  = 0x15; // VKILL (Ctrl+U)
    pair->tio.c_cc[4]  = 0x04; // VEOF (Ctrl+D)
    pair->tio.c_cc[5]  = 0;    // VTIME
    pair->tio.c_cc[6]  = 1;    // VMIN
    pair->tio.c_cc[10] = 0x1A; // VSUSP (Ctrl+Z)

    vfs_node_t *m_node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    vfs_node_t *s_node = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!m_node || !s_node) {
        if (m_node) kfree(m_node);
        if (s_node) kfree(s_node);
        pair->allocated = false;
        return NULL;
    }

    snprintf(m_node->name, sizeof(m_node->name), "ptm%d", slot);
    m_node->flags = FS_FILE;
    m_node->ops = &pty_master_ops;
    m_node->ptr = (vfs_node_t *)pair;
    m_node->refcount = 0;
    pair->master_node = m_node;

    snprintf(s_node->name, sizeof(s_node->name), "%d", slot);
    s_node->flags = FS_FILE;
    s_node->ops = &pty_slave_ops;
    s_node->ptr = (vfs_node_t *)pair;
    s_node->refcount = 0;
    pair->slave_node = s_node;

    return m_node;
}

vfs_node_t *pty_get_slave_node(int id) {
    if (id < 0 || id >= PTY_MAX_PAIRS) return NULL;
    pty_pair_t *pair = &g_pty_table[id];
    if (!pair->allocated || !pair->master_open) return NULL;
    return pair->slave_node;
}

bool pty_is_master_node(vfs_node_t *node) {
    return (node && node->ops == &pty_master_ops);
}

bool pty_is_slave_node(vfs_node_t *node) {
    return (node && node->ops == &pty_slave_ops);
}

bool pty_poll_state(vfs_node_t *node, bool *readable, bool *writable, bool *hangup, bool *error) {
    if (!node || !node->ptr) return false;
    pty_pair_t *pair = (pty_pair_t *)node->ptr;
    if (!pair->allocated) return false;

    *hangup = false;
    *error = false;

    if (node->ops == &pty_master_ops) {
        *readable = (pair->out_count > 0);
        *writable = (pair->in_count < PTY_BUF_SIZE);
        if (!pair->slave_open && pair->slave_refs == 0) {
            *hangup = true;
        }
        return true;
    } else if (node->ops == &pty_slave_ops) {
        if (pair->tio.c_lflag & 0x0002 /* ICANON */) {
            *readable = pty_buf_has_line(pair);
        } else {
            *readable = (pair->in_count > 0);
        }
        *writable = (pair->out_count < PTY_BUF_SIZE);
        if (!pair->master_open && pair->master_refs == 0) {
            *hangup = true;
        }
        return true;
    }

    return false;
}
