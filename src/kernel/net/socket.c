// src/kernel/net/socket.c - AF_INET socket layer (TCP streams, UDP datagrams)
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "../core/mem/memory.h"
#include "../misc/timer.h"
#include "../proc/task.h"
#include "../proc/sched.h"
#include "../drivers/net/rtl8139.h"
#include "string.h"

// Linux errno values used here
#define S_EAGAIN        11
#define S_ENOMEM        12
#define S_EINVAL        22
#define S_EPIPE         32
#define S_EBADF          9
#define S_EDESTADDRREQ  89
#define S_EMSGSIZE      90
#define S_ENOPROTOOPT   92
#define S_EOPNOTSUPP    95
#define S_EADDRINUSE    98
#define S_ENETDOWN     100
#define S_ECONNRESET   104
#define S_ENOBUFS      105
#define S_EISCONN      106
#define S_ENOTCONN     107
#define S_ETIMEDOUT    110
#define S_ECONNREFUSED 111
#define S_EALREADY     114
#define S_EINPROGRESS  115

// setsockopt/getsockopt names (Linux values)
#define SOL_SOCKET_LVL  1
#define IPPROTO_TCP_LVL 6
#define SO_TYPE_OPT     3
#define SO_ERROR_OPT    4
#define SO_SNDBUF_OPT   7
#define SO_RCVBUF_OPT   8
#define SO_RCVTIMEO_OPT 20
#define SO_SNDTIMEO_OPT 21
#define SO_ACCEPTCONN_OPT 30

#define UDP_EPHEMERAL_FIRST 32768
#define UDP_EPHEMERAL_LAST  48999

static socket_entry_t sockets[SOCK_MAX];
static uint16_t next_udp_port = UDP_EPHEMERAL_FIRST;

static socket_entry_t *sock_get(int id) {
    if (id < 0 || id >= SOCK_MAX) return NULL;
    if (sockets[id].state == SOCK_STATE_FREE) return NULL;
    return &sockets[id];
}

// ----------------------------------------------------------------------------
// Receive ring (TCP)
// ----------------------------------------------------------------------------

static uint32_t ring_used(const socket_entry_t *e) {
    return e->rx_head - e->rx_tail;
}

static uint32_t ring_free(const socket_entry_t *e) {
    return e->rx_ring ? SOCK_RX_RING_SIZE - ring_used(e) : 0;
}

static uint32_t ring_push(socket_entry_t *e, const uint8_t *data, uint32_t len) {
    uint32_t space = ring_free(e);
    if (len > space) len = space;
    for (uint32_t i = 0; i < len; i++) {
        e->rx_ring[(e->rx_head + i) & (SOCK_RX_RING_SIZE - 1)] = data[i];
    }
    e->rx_head += len;
    return len;
}

static uint32_t ring_pop(socket_entry_t *e, uint8_t *out, uint32_t max, bool peek) {
    uint32_t used = ring_used(e);
    if (max > used) max = used;
    for (uint32_t i = 0; i < max; i++) {
        out[i] = e->rx_ring[(e->rx_tail + i) & (SOCK_RX_RING_SIZE - 1)];
    }
    if (!peek) e->rx_tail += max;
    return max;
}

// ----------------------------------------------------------------------------
// TCP callbacks
// ----------------------------------------------------------------------------

static uint32_t sock_tcp_on_data(tcp_socket_t *tcb, uint8_t *data, uint32_t len) {
    socket_entry_t *e = (socket_entry_t *)tcb->owner;
    if (!e || !e->rx_ring) return len;
    if (e->shut_rd) return len; // reader gone: swallow
    uint32_t n = ring_push(e, data, len);
    if (n) io_wake_all();
    return n;
}

static uint32_t sock_tcp_rx_space(tcp_socket_t *tcb) {
    socket_entry_t *e = (socket_entry_t *)tcb->owner;
    if (!e) return 65535;
    return ring_free(e);
}

static int tcp_error_to_errno(tcp_error_t err, bool connecting) {
    switch (err) {
        case TCP_ERR_REFUSED: return S_ECONNREFUSED;
        case TCP_ERR_RESET:   return connecting ? S_ECONNREFUSED : S_ECONNRESET;
        case TCP_ERR_TIMEOUT: return S_ETIMEDOUT;
        default:              return 0;
    }
}

static void sock_tcp_on_event(tcp_socket_t *tcb) {
    socket_entry_t *e = (socket_entry_t *)tcb->owner;
    if (!e) return;

    switch (tcb->state) {
        case TCP_ESTABLISHED:
            if (e->state == SOCK_STATE_CONNECTING) e->state = SOCK_STATE_CONNECTED;
            break;
        case TCP_CLOSED: {
            bool connecting = (e->state == SOCK_STATE_CONNECTING);
            int err = tcp_error_to_errno(tcb->error, connecting);
            if (err) e->err = err;
            e->peer_closed = true;
            e->tcb = NULL;
            e->state = SOCK_STATE_CLOSED;
            break;
        }
        default:
            break;
    }
    if (tcb->peer_fin) e->peer_closed = true;
    io_wake_all(); // connected, closed, or send space freed
}

// ----------------------------------------------------------------------------
// Waiting
// ----------------------------------------------------------------------------

// Sleep one scheduler tick while the timer interrupt keeps the NIC and TCP timers going
static void sock_wait_tick(void) {
    rtl8139_poll();
    if (current_task) {
        io_wait_prepare();
        sched_make_sleep(current_task, tick + 2);
        sched_yield();
        io_wait_done();
    }
}

static bool deadline_passed(uint32_t start, uint32_t timeout_ms) {
    if (timeout_ms == 0) return false;
    return (tick - start) * (1000 / TIMER_HZ) >= timeout_ms;
}

// ----------------------------------------------------------------------------
// Socket API
// ----------------------------------------------------------------------------

int sock_create(int type) {
    if (type != SOCK_TYPE_STREAM && type != SOCK_TYPE_DGRAM) return -S_EINVAL;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (sockets[i].state == SOCK_STATE_FREE) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].state = SOCK_STATE_IDLE;
            sockets[i].type = type;
            return i;
        }
    }
    return -S_ENOBUFS;
}

int sock_type(int id) {
    socket_entry_t *e = sock_get(id);
    return e ? e->type : -S_EBADF;
}

uint32_t sock_rx_available(int id) {
    socket_entry_t *e = sock_get(id);
    if (!e) return 0;
    if (e->type == SOCK_TYPE_DGRAM) return e->dq_head ? e->dq_head->len : 0;
    return e->rx_ring ? ring_used(e) : 0;
}

static bool udp_port_in_use(uint16_t port) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (sockets[i].state != SOCK_STATE_FREE && sockets[i].type == SOCK_TYPE_DGRAM &&
            sockets[i].local_port == port) {
            return true;
        }
    }
    return false;
}

static uint16_t udp_alloc_port(void) {
    for (int tries = 0; tries <= UDP_EPHEMERAL_LAST - UDP_EPHEMERAL_FIRST; tries++) {
        uint16_t port = next_udp_port;
        next_udp_port = (port >= UDP_EPHEMERAL_LAST) ? UDP_EPHEMERAL_FIRST : (uint16_t)(port + 1);
        if (!udp_port_in_use(port)) return port;
    }
    return 0;
}

int sock_bind(int id, uint32_t ip, uint16_t port) {
    (void)ip; // single interface: any local address means the NIC address
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;
    if (e->local_port) return -S_EINVAL;

    if (e->type == SOCK_TYPE_DGRAM) {
        if (port == 0) {
            port = udp_alloc_port();
            if (!port) return -S_EADDRINUSE;
        } else if (udp_port_in_use(port)) {
            return -S_EADDRINUSE;
        }
        e->local_port = port;
        return 0;
    }

    // TCP: the port is picked when connecting; only remember an explicit request
    e->local_port = port;
    return 0;
}

int sock_connect(int id, uint32_t ip, uint16_t port, bool nonblock) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;

    if (e->type == SOCK_TYPE_DGRAM) {
        if (!e->local_port) {
            e->local_port = udp_alloc_port();
            if (!e->local_port) return -S_EADDRINUSE;
        }
        e->remote_ip = ip;
        e->remote_port = port;
        e->state = (ip || port) ? SOCK_STATE_CONNECTED : SOCK_STATE_IDLE;
        return 0;
    }

    if (e->state == SOCK_STATE_CONNECTING) return nonblock ? -S_EALREADY : -S_EALREADY;
    if (e->state == SOCK_STATE_CONNECTED) return -S_EISCONN;
    if (e->state == SOCK_STATE_CLOSED) {
        int err = e->err;
        e->err = 0;
        return err ? -err : -S_EISCONN;
    }

    net_interface_t *iface = net_get_primary_interface();
    if (!iface) return -S_ENETDOWN;

    if (!e->rx_ring) {
        e->rx_ring = (uint8_t *)kmalloc(SOCK_RX_RING_SIZE);
        if (!e->rx_ring) return -S_ENOMEM;
        e->rx_head = e->rx_tail = 0;
    }

    e->remote_ip = ip;
    e->remote_port = port;
    e->err = 0;
    e->peer_closed = false;
    e->state = SOCK_STATE_CONNECTING;
    e->tcb = tcp_connect(iface, ip, port, sock_tcp_on_data, sock_tcp_rx_space,
                         sock_tcp_on_event, e);
    if (!e->tcb) {
        e->state = SOCK_STATE_IDLE;
        return -S_ENOBUFS;
    }
    e->local_port = e->tcb->local_port;

    if (nonblock) return -S_EINPROGRESS;

    while (e->state == SOCK_STATE_CONNECTING) {
        sock_wait_tick();
    }
    if (e->state == SOCK_STATE_CONNECTED) return 0;

    int err = e->err ? e->err : S_ECONNREFUSED;
    e->err = 0;
    return -err;
}

static int64_t udp_sendto(socket_entry_t *e, const uint8_t *buf, uint32_t len,
                          const uint32_t *ip, const uint16_t *port) {
    uint32_t dst_ip;
    uint16_t dst_port;
    if (ip && port) {
        dst_ip = *ip;
        dst_port = *port;
    } else if (e->state == SOCK_STATE_CONNECTED) {
        dst_ip = e->remote_ip;
        dst_port = e->remote_port;
    } else {
        return -S_EDESTADDRREQ;
    }
    if (len > SOCK_UDP_MAX_PAYLOAD) return -S_EMSGSIZE;

    if (!e->local_port) {
        e->local_port = udp_alloc_port();
        if (!e->local_port) return -S_EADDRINUSE;
    }

    net_interface_t *iface = net_get_primary_interface();
    if (!iface) return -S_ENETDOWN;
    udp_send_packet(iface, dst_ip, e->local_port, dst_port, (uint8_t *)buf, len);
    return len;
}

int64_t sock_sendto(int id, const uint8_t *buf, uint32_t len, const uint32_t *ip,
                    const uint16_t *port, bool nonblock) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;
    if (e->type == SOCK_TYPE_DGRAM) return udp_sendto(e, buf, len, ip, port);

    if (e->err) {
        int err = e->err;
        e->err = 0;
        return -err;
    }
    if (e->shut_wr) return -S_EPIPE;

    uint32_t start = tick;
    while (e->state == SOCK_STATE_CONNECTING) {
        if (nonblock) return -S_EAGAIN;
        sock_wait_tick();
    }
    if (e->state != SOCK_STATE_CONNECTED || !e->tcb) {
        return (e->state == SOCK_STATE_IDLE) ? -S_ENOTCONN : -S_EPIPE;
    }

    uint32_t sent = 0;
    while (sent < len) {
        if (!e->tcb || e->state != SOCK_STATE_CONNECTED) {
            if (sent) break;
            int err = e->err ? e->err : S_EPIPE;
            e->err = 0;
            return -err;
        }
        sent += tcp_write(e->tcb, buf + sent, len - sent);
        if (sent == len) break;
        if (nonblock || deadline_passed(start, e->snd_timeout_ms)) break;
        sock_wait_tick();
    }
    if (sent == 0 && len > 0) return -S_EAGAIN;
    return sent;
}

static int64_t udp_recvfrom(socket_entry_t *e, uint8_t *buf, uint32_t len, uint32_t *ip,
                            uint16_t *port, int flags, bool nonblock) {
    uint32_t start = tick;
    while (!e->dq_head) {
        if (nonblock || (flags & SOCK_MSG_DONTWAIT)) return -S_EAGAIN;
        if (deadline_passed(start, e->rcv_timeout_ms)) return -S_EAGAIN;
        sock_wait_tick();
    }

    udp_dgram_t *d = e->dq_head;
    uint32_t n = d->len < len ? d->len : len;
    memcpy(buf, d->data, n);
    if (ip) *ip = d->src_ip;
    if (port) *port = d->src_port;

    if (!(flags & SOCK_MSG_PEEK)) {
        e->dq_head = d->next;
        if (!e->dq_head) e->dq_tail = NULL;
        e->dq_count--;
        kfree(d);
    }
    return n;
}

int64_t sock_recvfrom(int id, uint8_t *buf, uint32_t len, uint32_t *ip, uint16_t *port,
                      int flags, bool nonblock) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;
    if (e->type == SOCK_TYPE_DGRAM) return udp_recvfrom(e, buf, len, ip, port, flags, nonblock);

    if (flags & SOCK_MSG_DONTWAIT) nonblock = true;
    bool peek = (flags & SOCK_MSG_PEEK) != 0;
    uint32_t start = tick;

    for (;;) {
        if (e->rx_ring && ring_used(e) > 0) {
            uint32_t n = ring_pop(e, buf, len, peek);
            if (!peek && e->tcb) tcp_window_update(e->tcb);
            if (ip) *ip = e->remote_ip;
            if (port) *port = e->remote_port;
            return n;
        }
        if (e->err) {
            int err = e->err;
            e->err = 0;
            return -err;
        }
        if (e->peer_closed || e->shut_rd) return 0;
        if (e->state == SOCK_STATE_IDLE) return -S_ENOTCONN;
        if (e->state == SOCK_STATE_CLOSED) return 0;
        if (nonblock) return -S_EAGAIN;
        if (deadline_passed(start, e->rcv_timeout_ms)) return -S_EAGAIN;
        sock_wait_tick();
    }
}

int sock_poll(int id) {
    socket_entry_t *e = sock_get(id);
    if (!e) return SOCK_POLLERR;

    int ev = 0;
    if (e->type == SOCK_TYPE_DGRAM) {
        if (e->dq_head) ev |= SOCK_POLLIN;
        ev |= SOCK_POLLOUT;
        return ev;
    }

    switch (e->state) {
        case SOCK_STATE_IDLE:
            ev |= SOCK_POLLOUT | SOCK_POLLHUP;
            break;
        case SOCK_STATE_CONNECTING:
            break;
        case SOCK_STATE_CONNECTED:
            if (e->rx_ring && ring_used(e) > 0) ev |= SOCK_POLLIN;
            if (e->peer_closed) ev |= SOCK_POLLIN | SOCK_POLLRDHUP;
            if (e->tcb && tcp_send_space(e->tcb) > 0 && !e->shut_wr) ev |= SOCK_POLLOUT;
            break;
        case SOCK_STATE_CLOSED:
            ev |= SOCK_POLLIN | SOCK_POLLHUP | SOCK_POLLRDHUP;
            if (e->rx_ring && ring_used(e) > 0) ev |= SOCK_POLLIN;
            break;
        default:
            break;
    }
    if (e->err) ev |= SOCK_POLLERR | SOCK_POLLOUT;
    return ev;
}

int sock_setsockopt(int id, int level, int optname, const void *val, uint32_t len) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;

    if (level == SOL_SOCKET_LVL && (optname == SO_RCVTIMEO_OPT || optname == SO_SNDTIMEO_OPT)) {
        if (!val || len < 16) return -S_EINVAL;
        const int64_t *tv = (const int64_t *)val; // struct timeval { tv_sec; tv_usec; }
        uint32_t ms = (uint32_t)(tv[0] * 1000 + tv[1] / 1000);
        if (optname == SO_RCVTIMEO_OPT) e->rcv_timeout_ms = ms;
        else e->snd_timeout_ms = ms;
        return 0;
    }
    // Keepalive, nodelay, buffer sizes, reuseaddr...: accepted and ignored
    if (level == SOL_SOCKET_LVL || level == IPPROTO_TCP_LVL || level == 0) return 0;
    return -S_ENOPROTOOPT;
}

int sock_getsockopt(int id, int level, int optname, void *val, uint32_t *len) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;
    if (!val || !len || *len < sizeof(int)) return -S_EINVAL;

    int out = 0;
    if (level == SOL_SOCKET_LVL) {
        switch (optname) {
            case SO_ERROR_OPT:
                out = e->err;
                e->err = 0;
                break;
            case SO_TYPE_OPT:
                out = e->type;
                break;
            case SO_RCVBUF_OPT:
                out = SOCK_RX_RING_SIZE;
                break;
            case SO_SNDBUF_OPT:
                out = TCP_TX_BUF_SIZE;
                break;
            case SO_ACCEPTCONN_OPT:
                out = 0;
                break;
            default:
                out = 0;
                break;
        }
    } else if (level == IPPROTO_TCP_LVL) {
        out = 0;
    } else {
        return -S_ENOPROTOOPT;
    }
    *(int *)val = out;
    *len = sizeof(int);
    return 0;
}

int sock_getname(int id, bool peer, uint32_t *ip, uint16_t *port) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;

    if (peer) {
        if (e->state != SOCK_STATE_CONNECTED && e->state != SOCK_STATE_CONNECTING &&
            !(e->type == SOCK_TYPE_STREAM && e->state == SOCK_STATE_CLOSED && e->remote_port)) {
            return -S_ENOTCONN;
        }
        *ip = e->remote_ip;
        *port = e->remote_port;
        return 0;
    }

    net_interface_t *iface = net_get_primary_interface();
    *ip = (iface && (e->local_port || e->state != SOCK_STATE_IDLE)) ? iface->ip : 0;
    *port = e->local_port;
    return 0;
}

int sock_shutdown(int id, int how) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;
    if (how < 0 || how > 2) return -S_EINVAL;
    if (e->type == SOCK_TYPE_STREAM && e->state != SOCK_STATE_CONNECTED &&
        e->state != SOCK_STATE_CLOSED) {
        return -S_ENOTCONN;
    }

    if (how == 0 || how == 2) e->shut_rd = true;
    if ((how == 1 || how == 2) && !e->shut_wr) {
        e->shut_wr = true;
        if (e->tcb) tcp_shutdown(e->tcb);
    }
    return 0;
}

int sock_close(int id) {
    socket_entry_t *e = sock_get(id);
    if (!e) return -S_EBADF;

    if (e->tcb) {
        tcp_socket_t *tcb = e->tcb;
        e->tcb = NULL;
        // Unread data at close time means the peer should hear about it (RFC 2525)
        if (e->rx_ring && ring_used(e) > 0) tcp_abort(tcb);
        else tcp_close(net_get_primary_interface(), tcb);
    }
    if (e->rx_ring) {
        kfree(e->rx_ring);
        e->rx_ring = NULL;
    }
    while (e->dq_head) {
        udp_dgram_t *d = e->dq_head;
        e->dq_head = d->next;
        kfree(d);
    }
    memset(e, 0, sizeof(*e));
    e->state = SOCK_STATE_FREE;
    return 0;
}

bool sock_udp_deliver(uint16_t dest_port, uint32_t src_ip, uint16_t src_port,
                      const uint8_t *data, uint32_t len) {
    for (int i = 0; i < SOCK_MAX; i++) {
        socket_entry_t *e = &sockets[i];
        if (e->state == SOCK_STATE_FREE || e->type != SOCK_TYPE_DGRAM) continue;
        if (e->local_port != dest_port) continue;
        if (e->state == SOCK_STATE_CONNECTED &&
            (e->remote_ip != src_ip || e->remote_port != src_port)) {
            continue;
        }
        if (e->dq_count >= SOCK_UDP_QUEUE_MAX) return true; // queue full: drop

        udp_dgram_t *d = (udp_dgram_t *)kmalloc(sizeof(udp_dgram_t) + len);
        if (!d) return true;
        d->next = NULL;
        d->src_ip = src_ip;
        d->src_port = src_port;
        d->len = len;
        memcpy(d->data, data, len);
        if (e->dq_tail) e->dq_tail->next = d;
        else e->dq_head = d;
        e->dq_tail = d;
        e->dq_count++;
        io_wake_all();
        return true;
    }
    return false;
}
