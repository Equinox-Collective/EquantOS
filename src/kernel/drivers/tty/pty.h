// src/kernel/drivers/tty/pty.h - Pseudo-Terminal (PTY) Subsystem for EquantOS
#ifndef PTY_H
#define PTY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../fs/vfs.h"
#include "../../proc/task.h"

#define PTY_MAX_PAIRS 32
#define PTY_BUF_SIZE  4096

// Terminal & PTY ioctls
#ifndef TCGETS
#define TCGETS       0x5401
#endif
#ifndef TCSETS
#define TCSETS       0x5402
#endif
#ifndef TCSETSW
#define TCSETSW      0x5403
#endif
#ifndef TCSETSF
#define TCSETSF      0x5404
#endif
#ifndef TIOCSCTTY
#define TIOCSCTTY    0x540E
#endif
#ifndef TIOCGPGRP
#define TIOCGPGRP    0x540F
#endif
#ifndef TIOCSPGRP
#define TIOCSPGRP    0x5410
#endif
#ifndef TIOCGWINSZ
#define TIOCGWINSZ   0x5413
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ   0x5414
#endif
#ifndef FIONREAD
#define FIONREAD     0x541B
#endif
#ifndef FIONBIO
#define FIONBIO      0x5421
#endif
#ifndef TIOCNOTTY
#define TIOCNOTTY    0x5422
#endif
#ifndef TIOCGSID
#define TIOCGSID     0x5429
#endif
#ifndef TIOCGPTN
#define TIOCGPTN     0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK   0x40045431
#endif
#ifndef TIOCGPTLCK
#define TIOCGPTLCK   0x80045439
#endif
#ifndef TIOCGPTPEER
#define TIOCGPTPEER  0x5441
#endif

// winsize struct
#ifndef WINSIZE_DEFINED
#define WINSIZE_DEFINED
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#endif

// termios struct
#ifndef TERMIOS_DEFINED
#define TERMIOS_DEFINED
struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};
#endif

// Forward declaration
typedef struct pty_pair {
    int id;
    bool allocated;
    bool locked;          // TIOCSPTLCK state (true = locked, false = unlocked)
    bool master_open;
    bool slave_open;
    int master_refs;
    int slave_refs;

    // Master -> Slave buffer (typed by user / emulator, read by shell)
    uint8_t in_buf[PTY_BUF_SIZE];
    uint32_t in_head;
    uint32_t in_tail;
    uint32_t in_count;

    // Slave -> Master buffer (printed by shell, read by emulator)
    uint8_t out_buf[PTY_BUF_SIZE];
    uint32_t out_head;
    uint32_t out_tail;
    uint32_t out_count;

    struct winsize ws;
    struct termios tio;

    uint64_t foreground_pgid;
    uint64_t session_sid;

    task_t *blocked_master_read;
    task_t *blocked_master_write;
    task_t *blocked_slave_read;
    task_t *blocked_slave_write;

    vfs_node_t *master_node;
    vfs_node_t *slave_node;
} pty_pair_t;

void pty_subsystem_init(void);
vfs_node_t *pty_create_master_node(void);
vfs_node_t *pty_get_slave_node(int id);
bool pty_is_master_node(vfs_node_t *node);
bool pty_is_slave_node(vfs_node_t *node);
bool pty_poll_state(vfs_node_t *node, bool *readable, bool *writable, bool *hangup, bool *error);

#endif // PTY_H
