#include "socket.h"
#include "tcp.h"
#include "net.h"
#include "../core/mem/memory.h"
#include "../misc/timer.h"
#include "../proc/task.h"
#include "string.h"
#include "stdio.h"

#define DEFAULT_CONNECT_TIMEOUT_MS  5000
#define DEFAULT_RECV_TIMEOUT_MS     5000

static socket_entry_t sockets[SOCK_MAX];

static bool ring_alloc(socket_entry_t *e) {
    if (e->rx_ring) return true;
    e->rx_ring = (uint8_t *)kmalloc(SOCK_RX_RING_SIZE);
    if (!e->rx_ring) return false;
    e->rx_size = SOCK_RX_RING_SIZE;
    e->rx_head = e->rx_tail = 0;
    return true;
}

static uint32_t ring_used(const socket_entry_t *e) {
    return e->rx_head - e->rx_tail;
}

static uint32_t ring_free(const socket_entry_t *e) {
    return e->rx_size - ring_used(e);
}

static void ring_push(socket_entry_t *e, const uint8_t *data, uint32_t len) {
    uint32_t free = ring_free(e);
    if (len > free) len = free;
    for (uint32_t i = 0; i < len; i++) {
        e->rx_ring[(e->rx_head + i) & (e->rx_size - 1)] = data[i];
    }
    e->rx_head += len;
}

static uint32_t ring_pop(socket_entry_t *e, uint8_t *out, uint32_t max) {
    uint32_t used = ring_used(e);
    if (max > used) max = used;
    for (uint32_t i = 0; i < max; i++) {
        out[i] = e->rx_ring[(e->rx_tail + i) & (e->rx_size - 1)];
    }
    e->rx_tail += max;
    return max;
}

static socket_entry_t *find_by_tcb(tcp_socket_t *tcb) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (sockets[i].state != SOCK_STATE_FREE && sockets[i].tcb == tcb) {
            return &sockets[i];
        }
    }
    return NULL;
}

void socket_rx_dispatch(tcp_socket_t *sock, uint8_t *data, uint32_t len) {
    socket_entry_t *e = find_by_tcb(sock);
    if (!e) return;
    if (!e->rx_ring && !ring_alloc(e)) return;
    ring_push(e, data, len);
}

void socket_on_state_change(tcp_socket_t *sock) {
    socket_entry_t *e = find_by_tcb(sock);
    if (!e) return;

    switch (sock->state) {
        case TCP_ESTABLISHED:
            if (e->state == SOCK_STATE_CONNECTING) e->state = SOCK_STATE_CONNECTED;
            break;
        case TCP_CLOSE_WAIT:
        case TCP_FIN_WAIT_2:
            if (e->state == SOCK_STATE_CONNECTED) e->state = SOCK_STATE_PEER_CLOSED;
            break;
        case TCP_CLOSED:
            e->state = SOCK_STATE_CLOSED;
            e->tcb   = NULL;
            break;
        default:
            break;
    }
}

int sock_create(void) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (sockets[i].state == SOCK_STATE_FREE) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].state = SOCK_STATE_CLOSED;
            sockets[i].owner_pid = current_task ? current_task->id : 0;
            return i;
        }
    }
    return SOCK_ERR_NOMEM;
}

static void hlt_until_tick(void) {
    __asm__ volatile("sti; hlt; cli");
}

int sock_connect(int fd, uint32_t ip_be, uint16_t port) {
    if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
    socket_entry_t *e = &sockets[fd];
    if (e->state != SOCK_STATE_CLOSED) return SOCK_ERR_INVAL;

    net_interface_t *iface = net_get_primary_interface();
    if (!iface) return SOCK_ERR_NOTCONN;

    e->state = SOCK_STATE_CONNECTING;
    e->tcb   = tcp_connect(iface, ip_be, port, socket_rx_dispatch);
    if (!e->tcb) {
        e->state = SOCK_STATE_CLOSED;
        return SOCK_ERR_NOMEM;
    }

    uint32_t start = tick;
    uint32_t deadline = start + (DEFAULT_CONNECT_TIMEOUT_MS / 10);

    while (tick < deadline) {
        if (e->state == SOCK_STATE_CONNECTED) return 0;
        if (e->state == SOCK_STATE_CLOSED || e->state == SOCK_STATE_ERROR) {
            return SOCK_ERR_REFUSED;
        }
        hlt_until_tick();
    }
    return SOCK_ERR_TIMEOUT;
}

int sock_send(int fd, const uint8_t *buf, uint32_t len) {
    if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
    socket_entry_t *e = &sockets[fd];
    if (e->state != SOCK_STATE_CONNECTED && e->state != SOCK_STATE_PEER_CLOSED) {
        return SOCK_ERR_NOTCONN;
    }
    if (!e->tcb) return SOCK_ERR_CLOSED;

    net_interface_t *iface = net_get_primary_interface();
    if (!iface) return SOCK_ERR_NOTCONN;

    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > TCP_MAX_SEG_PAYLOAD) chunk = TCP_MAX_SEG_PAYLOAD;
        tcp_send_segment(iface, e->tcb, TCP_ACK | TCP_PSH, (uint8_t *)(buf + sent), chunk, true);
        sent += chunk;
    }
    return (int)sent;
}

int sock_recv(int fd, uint8_t *buf, uint32_t len) {
    if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
    socket_entry_t *e = &sockets[fd];
    if (e->state == SOCK_STATE_FREE) return SOCK_ERR_BADFD;

    uint32_t timeout = e->rcv_timeout_ms ? (e->rcv_timeout_ms / 10) : (DEFAULT_RECV_TIMEOUT_MS / 10);
    uint32_t deadline = tick + timeout;

    while (tick < deadline) {
        if (e->rx_ring && ring_used(e) > 0) {
            return (int)ring_pop(e, buf, len);
        }
        if (e->state == SOCK_STATE_PEER_CLOSED || e->state == SOCK_STATE_CLOSED) {
            if (!e->rx_ring || ring_used(e) == 0) return 0;
        }
        if (e->state == SOCK_STATE_ERROR) return SOCK_ERR_CLOSED;
        hlt_until_tick();
    }
    return SOCK_ERR_TIMEOUT;
}

int sock_close(int fd) {
    if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
    socket_entry_t *e = &sockets[fd];
    if (e->state == SOCK_STATE_FREE) return SOCK_ERR_BADFD;

    if (e->tcb) {
        net_interface_t *iface = net_get_primary_interface();
        if (iface) tcp_close(iface, e->tcb);
    }
    if (e->rx_ring) {
        kfree(e->rx_ring);
        e->rx_ring = NULL;
    }
    memset(e, 0, sizeof(*e));
    e->state = SOCK_STATE_FREE;
    return 0;
}

int sock_setsockopt(int fd, int level, int optname, const void *val, uint32_t vallen) {
    if (fd < 0 || fd >= SOCK_MAX) return SOCK_ERR_BADFD;
    if (sockets[fd].state == SOCK_STATE_FREE) return SOCK_ERR_BADFD;
    if (level != SOCK_LEVEL_SOCKET) return SOCK_ERR_INVAL;

    switch (optname) {
        case SOCK_OPT_RCVTIMEO:
            if (vallen != sizeof(uint32_t) || !val) return SOCK_ERR_INVAL;
            sockets[fd].rcv_timeout_ms = *(const uint32_t *)val;
            return 0;
        case SOCK_OPT_NODELAY:
            return 0;
        default:
            return SOCK_ERR_INVAL;
    }
}