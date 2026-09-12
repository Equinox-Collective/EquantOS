#ifndef SOCKET_H
#define SOCKET_H

#include "tcp.h"
#include <stdbool.h>
#include <stdint.h>

#define SOCK_MAX           16
#define SOCK_RX_RING_SIZE  262144

#define SOCK_ERR_BADFD       -1
#define SOCK_ERR_NOMEM       -2
#define SOCK_ERR_NOTCONN     -3
#define SOCK_ERR_TIMEOUT     -4
#define SOCK_ERR_REFUSED     -5
#define SOCK_ERR_CLOSED      -6
#define SOCK_ERR_AGAIN       -7
#define SOCK_ERR_INVAL       -8

#define SOCK_LEVEL_SOCKET   1
#define SOCK_OPT_RCVTIMEO   1
#define SOCK_OPT_NODELAY    2

typedef enum {
    SOCK_STATE_FREE = 0,
    SOCK_STATE_CONNECTING,
    SOCK_STATE_CONNECTED,
    SOCK_STATE_PEER_CLOSED,
    SOCK_STATE_CLOSED,
    SOCK_STATE_ERROR
} sock_state_t;

typedef struct {
    sock_state_t   state;
    tcp_socket_t  *tcb;
    uint8_t       *rx_ring;
    uint32_t       rx_head;
    uint32_t       rx_tail;
    uint32_t       rx_size;
    uint32_t       rcv_timeout_ms;
    int32_t        err;
    uint64_t       owner_pid;
} socket_entry_t;

int sock_create(void);
int sock_connect(int fd, uint32_t ip_be, uint16_t port);
int sock_send(int fd, const uint8_t *buf, uint32_t len);
int sock_recv(int fd, uint8_t *buf, uint32_t len);
int sock_close(int fd);
int sock_setsockopt(int fd, int level, int optname, const void *val, uint32_t vallen);

void socket_rx_dispatch(tcp_socket_t *sock, uint8_t *data, uint32_t len);
void socket_on_state_change(tcp_socket_t *sock);

#endif // SOCKET_H