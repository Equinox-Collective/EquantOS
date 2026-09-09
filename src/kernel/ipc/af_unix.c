// src/kernel/ipc/af_unix.c - Fast Memory-Backed UNIX Domain Socket Engine
#include "af_unix.h"
#include "../core/mem/memory.h"
#include "../proc/task.h"
#include "../proc/sched.h"
#include "../proc/syscall.h"
#include "../fs/vfs.h"
#include "string.h"

// Forward declaration of VFS ops
static int64_t vfs_sock_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static int64_t vfs_sock_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer);
static void vfs_sock_close(vfs_node_t *node);

vfs_file_operations_t unix_socket_vfs_ops = {
    .read = vfs_sock_read,
    .write = vfs_sock_write,
    .open = NULL,
    .close = vfs_sock_close,
    .readdir = NULL,
    .finddir = NULL,
    .create = NULL,
    .ioctl = NULL,
    .mmap = NULL
};

unix_socket_t *unix_socket_create(int type) {
    unix_socket_t *sock = (unix_socket_t *)kzalloc(sizeof(unix_socket_t));
    if (!sock) return NULL;

    sock->state = UNIX_STATE_CREATED;
    sock->type = type;
    sock->ref_count = 1;
    return sock;
}

int unix_socket_bind(unix_socket_t *sock, const struct sockaddr_un *addr) {
    if (!sock || !addr) return -EINVAL;
    if (sock->state != UNIX_STATE_CREATED) return -EINVAL;

    // Check if path already exists
    vfs_node_t *existing = vfs_open(addr->sun_path, 0);
    if (existing) {
        return -EADDRINUSE;
    }

    // Resolve parent directory path (e.g. "/tmp/.X11-unix")
    char path_copy[UNIX_PATH_MAX];
    strncpy(path_copy, addr->sun_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *filename = path_copy;
    char *last_slash = strrchr(path_copy, '/');
    if (last_slash) {
        if (last_slash == path_copy) {
            filename = last_slash + 1;
            path_copy[1] = '\0';
        } else {
            *last_slash = '\0';
            filename = last_slash + 1;
        }
    }

    vfs_node_t *parent_dir = vfs_open(path_copy[0] == '\0' ? "/" : path_copy, 0);
    if (!parent_dir) return -ENOENT;

    // Create socket node in VFS with explicit FS_SOCKET flag
    vfs_node_t *sock_node = vfs_create(parent_dir, filename, FS_SOCKET | 0777);
    if (!sock_node) return -EIO;

    sock_node->flags = FS_SOCKET; // Guarantee not marked as directory
    sock_node->ops = &unix_socket_vfs_ops;
    sock_node->ptr = (vfs_node_t *)sock;

    strncpy(sock->path, addr->sun_path, sizeof(sock->path) - 1);
    sock->state = UNIX_STATE_BOUND;
    return 0;
}

int unix_socket_listen(unix_socket_t *sock, int backlog) {
    if (!sock) return -EINVAL;
    if (sock->state != UNIX_STATE_BOUND) return -EINVAL;

    sock->backlog_max = (backlog > UNIX_BACKLOG_MAX) ? UNIX_BACKLOG_MAX : (backlog <= 0 ? 1 : backlog);
    sock->backlog_count = 0;
    sock->state = UNIX_STATE_LISTENING;
    return 0;
}

int unix_socket_connect(unix_socket_t *client_sock, const struct sockaddr_un *addr) {
    if (!client_sock || !addr) return -EINVAL;
    if (client_sock->state == UNIX_STATE_CONNECTED) return -EISCONN;

    // Locate the bound listening socket in VFS
    vfs_node_t *node = vfs_open(addr->sun_path, 0);
    if (!node || !node->ptr) return -ECONNREFUSED;

    unix_socket_t *server_sock = (unix_socket_t *)node->ptr;
    if (server_sock->state != UNIX_STATE_LISTENING) {
        return -ECONNREFUSED;
    }

    // Check if server backlog is full
    if (server_sock->backlog_count >= server_sock->backlog_max) {
        return -EAGAIN;
    }

    // Create peer socket representing client on server side
    unix_socket_t *server_peer = unix_socket_create(client_sock->type);
    if (!server_peer) return -ENOMEM;

    // Interlink client and server sockets
    client_sock->peer = server_peer;
    server_peer->peer = client_sock;

    client_sock->state = UNIX_STATE_CONNECTED;
    server_peer->state = UNIX_STATE_CONNECTED;

    // Enqueue new connection into server backlog
    server_sock->backlog[server_sock->backlog_count++] = server_peer;

    // Wake up server accept thread if waiting
    if (server_sock->blocked_accept) {
        sched_unblock(server_sock->blocked_accept);
        server_sock->blocked_accept = NULL;
    }

    return 0;
}

int unix_socket_accept(unix_socket_t *server_sock, unix_socket_t **out_client) {
    if (!server_sock || !out_client) return -EINVAL;
    if (server_sock->state != UNIX_STATE_LISTENING) return -EINVAL;

    // Block until a client connects
    while (server_sock->backlog_count == 0) {
        if (current_task) {
            server_sock->blocked_accept = current_task;
            sched_block(current_task);
            sched_yield();
            server_sock->blocked_accept = NULL;
        } else {
            __asm__ volatile("pause");
        }
    }

    // Pop the first connection from backlog FIFO
    unix_socket_t *accepted = server_sock->backlog[0];
    for (uint32_t i = 0; i < server_sock->backlog_count - 1; i++) {
        server_sock->backlog[i] = server_sock->backlog[i + 1];
    }
    server_sock->backlog_count--;

    *out_client = accepted;
    return 0;
}

int64_t unix_socket_read(unix_socket_t *sock, void *buf, size_t count, bool nonblock) {
    if (!sock || !buf || count == 0) return 0;
    uint8_t *out = (uint8_t *)buf;

    // Wait for incoming data
    while (sock->count == 0) {
        if (sock->peer_closed) {
            return 0; // EOF
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (current_task) {
            sock->blocked_reader = current_task;
            sched_block(current_task);
            sched_yield();
            sock->blocked_reader = NULL;
        } else {
            __asm__ volatile("pause");
        }
    }

    uint64_t to_read = (count < sock->count) ? count : sock->count;
    for (uint64_t i = 0; i < to_read; i++) {
        out[i] = sock->buffer[sock->read_pos];
        sock->read_pos = (sock->read_pos + 1) % UNIX_SOCK_BUF_SIZE;
        sock->count--;
    }

    // Wake up peer writer if it was blocked waiting for free buffer space
    if (sock->peer && sock->peer->blocked_writer) {
        sched_unblock(sock->peer->blocked_writer);
        sock->peer->blocked_writer = NULL;
    }

    return (int64_t)to_read;
}

int64_t unix_socket_write(unix_socket_t *sock, const void *buf, size_t count, bool nonblock) {
    if (!sock || !buf || count == 0) return 0;
    if (!sock->peer || sock->peer_closed) return -EPIPE;

    unix_socket_t *dest = sock->peer;
    const uint8_t *in = (const uint8_t *)buf;
    size_t written = 0;

    while (written < count) {
        while (dest->count >= UNIX_SOCK_BUF_SIZE) {
            if (dest->peer_closed || sock->peer_closed) return -EPIPE;
            if (nonblock) return written > 0 ? (int64_t)written : -EAGAIN;

            if (current_task) {
                sock->blocked_writer = current_task;
                sched_block(current_task);
                sched_yield();
                sock->blocked_writer = NULL;
            } else {
                __asm__ volatile("pause");
            }
        }

        dest->buffer[dest->write_pos] = in[written++];
        dest->write_pos = (dest->write_pos + 1) % UNIX_SOCK_BUF_SIZE;
        dest->count++;

        // Wake up reading peer immediately
        if (dest->blocked_reader) {
            sched_unblock(dest->blocked_reader);
            dest->blocked_reader = NULL;
        }
    }

    return (int64_t)written;
}

void unix_socket_close(unix_socket_t *sock) {
    if (!sock) return;

    sock->state = UNIX_STATE_CLOSED;
    sock->peer_closed = true;

    if (sock->peer) {
        sock->peer->peer_closed = true;
        if (sock->peer->blocked_reader) {
            sched_unblock(sock->peer->blocked_reader);
            sock->peer->blocked_reader = NULL;
        }
        if (sock->peer->blocked_writer) {
            sched_unblock(sock->peer->blocked_writer);
            sock->peer->blocked_writer = NULL;
        }
    }

    sock->ref_count--;
    if (sock->ref_count <= 0) {
        kfree(sock);
    }
}

bool unix_socket_can_read(unix_socket_t *sock) {
    if (!sock) return false;
    if (sock->state == UNIX_STATE_LISTENING) {
        return sock->backlog_count > 0;
    }
    return (sock->count > 0 || sock->peer_closed);
}

bool unix_socket_can_write(unix_socket_t *sock) {
    if (!sock || !sock->peer || sock->peer_closed) return false;
    return (sock->peer->count < UNIX_SOCK_BUF_SIZE);
}

// VFS Adapter Callbacks
static int64_t vfs_sock_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr) return -EBADF;
    return unix_socket_read((unix_socket_t *)node->ptr, buffer, size, false);
}

static int64_t vfs_sock_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset;
    if (!node || !node->ptr) return -EBADF;
    return unix_socket_write((unix_socket_t *)node->ptr, buffer, size, false);
}

static void vfs_sock_close(vfs_node_t *node) {
    if (node && node->ptr) {
        unix_socket_close((unix_socket_t *)node->ptr);
        node->ptr = NULL;
    }
}