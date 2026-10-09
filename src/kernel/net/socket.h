#ifndef SOCKET_H
#define SOCKET_H

#include "tcp.h"
#include <stdbool.h>
#include <stdint.h>

// AF_INET sockets (TCP stream and UDP datagram) behind the BSD socket syscalls.
// All functions return 0/byte counts on success and a negative Linux errno on failure.

#define SOCK_MAX           128
#define SOCK_RX_RING_SIZE  262144   // TCP receive buffer, power of two
#define SOCK_UDP_QUEUE_MAX 64       // queued datagrams per UDP socket
#define SOCK_UDP_MAX_PAYLOAD 1472   // no IP fragmentation

#define SOCK_TYPE_STREAM   1
#define SOCK_TYPE_DGRAM    2

// poll() event bits (Linux values)
#define SOCK_POLLIN        0x001
#define SOCK_POLLOUT       0x004
#define SOCK_POLLERR       0x008
#define SOCK_POLLHUP       0x010
#define SOCK_POLLRDHUP     0x2000

#define SOCK_MSG_PEEK      0x02
#define SOCK_MSG_DONTWAIT  0x40

typedef enum {
    SOCK_STATE_FREE = 0,
    SOCK_STATE_IDLE,          // created / bound, not connected
    SOCK_STATE_CONNECTING,
    SOCK_STATE_CONNECTED,
    SOCK_STATE_CLOSED         // connection finished or failed
} sock_state_t;

typedef struct udp_dgram {
    struct udp_dgram *next;
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t len;
    uint8_t  data[];
} udp_dgram_t;

typedef struct {
    sock_state_t   state;
    int            type;
    tcp_socket_t  *tcb;

    uint8_t       *rx_ring;
    uint32_t       rx_head;
    uint32_t       rx_tail;
    bool           peer_closed;
    bool           shut_rd;
    bool           shut_wr;
    int            err;            // pending error for SO_ERROR (positive errno)

    uint16_t       local_port;
    uint32_t       remote_ip;
    uint16_t       remote_port;

    udp_dgram_t   *dq_head;
    udp_dgram_t   *dq_tail;
    uint32_t       dq_count;

    uint32_t       rcv_timeout_ms;
    uint32_t       snd_timeout_ms;
} socket_entry_t;

int     sock_create(int type);
int     sock_bind(int id, uint32_t ip, uint16_t port);
int     sock_connect(int id, uint32_t ip, uint16_t port, bool nonblock);
int64_t sock_sendto(int id, const uint8_t *buf, uint32_t len, const uint32_t *ip,
                    const uint16_t *port, bool nonblock);
int64_t sock_recvfrom(int id, uint8_t *buf, uint32_t len, uint32_t *ip, uint16_t *port,
                      int flags, bool nonblock);
int     sock_poll(int id);
int     sock_setsockopt(int id, int level, int optname, const void *val, uint32_t len);
int     sock_getsockopt(int id, int level, int optname, void *val, uint32_t *len);
int     sock_getname(int id, bool peer, uint32_t *ip, uint16_t *port);
int     sock_shutdown(int id, int how);
int     sock_close(int id);
int     sock_type(int id);
// Bytes (TCP) or next datagram size (UDP) readable without blocking
uint32_t sock_rx_available(int id);

// Called by the UDP layer for datagrams no kernel service claimed
bool    sock_udp_deliver(uint16_t dest_port, uint32_t src_ip, uint16_t src_port,
                         const uint8_t *data, uint32_t len);

#endif // SOCKET_H
