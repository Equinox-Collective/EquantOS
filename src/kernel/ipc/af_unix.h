// src/kernel/ipc/af_unix.h - UNIX Domain Sockets (AF_UNIX / AF_LOCAL) for Local IPC
#ifndef AF_UNIX_H
#define AF_UNIX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../fs/vfs.h"

#define AF_UNIX             1
#define AF_LOCAL            1
#define UNIX_PATH_MAX       108
#define UNIX_SOCK_BUF_SIZE  16384 // 16KB circular buffer for high-bandwidth X11 packets
#define UNIX_BACKLOG_MAX    16

// POSIX sockaddr_un definition
struct sockaddr_un {
    uint16_t sun_family;               // AF_UNIX
    char sun_path[UNIX_PATH_MAX];      // Path in VFS (e.g. "/tmp/.X11-unix/X0")
};

typedef enum {
    UNIX_STATE_CREATED,
    UNIX_STATE_BOUND,
    UNIX_STATE_LISTENING,
    UNIX_STATE_CONNECTED,
    UNIX_STATE_CLOSED
} unix_sock_state_t;

struct unix_socket;

typedef struct unix_socket {
    unix_sock_state_t state;
    int type; // SOCK_STREAM or SOCK_DGRAM
    char path[UNIX_PATH_MAX];

    // Data buffer (Read by this socket, written by the peer)
    uint8_t buffer[UNIX_SOCK_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;

    // Paired Peer Socket
    struct unix_socket *peer;

    // Listen / Accept Connection Backlog
    struct unix_socket *backlog[UNIX_BACKLOG_MAX];
    uint32_t backlog_count;
    uint32_t backlog_max;

    // Blocking wait queues
    struct task *blocked_reader;
    struct task *blocked_writer;
    struct task *blocked_accept;

    int ref_count;
    bool peer_closed;
} unix_socket_t;

// Core Socket Lifecycle API
unix_socket_t *unix_socket_create(int type);
int unix_socket_bind(unix_socket_t *sock, const struct sockaddr_un *addr);
int unix_socket_listen(unix_socket_t *sock, int backlog);
int unix_socket_accept(unix_socket_t *server_sock, unix_socket_t **out_client);
int unix_socket_connect(unix_socket_t *client_sock, const struct sockaddr_un *addr);

int64_t unix_socket_read(unix_socket_t *sock, void *buf, size_t count, bool nonblock);
int64_t unix_socket_write(unix_socket_t *sock, const void *buf, size_t count, bool nonblock);
void unix_socket_close(unix_socket_t *sock);

// Poll readiness queries
bool unix_socket_can_read(unix_socket_t *sock);
bool unix_socket_can_write(unix_socket_t *sock);

// VFS Operations integration
extern vfs_file_operations_t unix_socket_vfs_ops;

#endif // AF_UNIX_H