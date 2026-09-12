#include "tcp.h"
#include "ipv4.h"
#include "string.h"
#include "stdio.h"
#include "../core/mem/memory.h"
#include "../misc/timer.h"

// Forward decls from tcp_rtx.c
bool tcp_rtx_enqueue(tcp_socket_t *sock, uint32_t seq, uint8_t flags,
                     const uint8_t *payload, uint32_t payload_len,
                     uint32_t now_ms);
void tcp_rtx_ack(tcp_socket_t *sock, uint32_t ack_nxt);
void tcp_rtx_flush(tcp_socket_t *sock);
bool tcp_rtx_tick(net_interface_t *iface, tcp_socket_t *sock, uint32_t now_ms);
bool tcp_reorder_insert(tcp_socket_t *sock, uint32_t seq, const uint8_t *data, uint32_t len);
uint32_t tcp_reorder_drain(tcp_socket_t *sock);
void tcp_reorder_flush(tcp_socket_t *sock);

// Weak hook for socket layer state transitions (no link failure if socket.c is missing)
__attribute__((weak)) void socket_on_state_change(tcp_socket_t *sock) {
    (void)sock;
}

static tcp_socket_t tcp_sockets[TCP_MAX_SOCKETS];

static inline uint32_t get_random_isn(void) {
    extern volatile uint32_t tick;
    static uint32_t seed = 0x12345678;
    // Xorshift32 PRNG mixed with hardware timer ticks
    seed ^= (seed << 13) ^ (tick * 1103515245 + 12345);
    seed ^= (seed >> 17);
    seed ^= (seed << 5);
    return seed & 0x7FFFFFFF;
}

static void tcp_socket_free(tcp_socket_t *sock) {
    tcp_rtx_flush(sock);
    tcp_reorder_flush(sock);
    sock->state  = TCP_CLOSED;
    sock->active = false;
    sock->on_data = NULL;
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip,
                             tcp_header_t *tcp, uint8_t *payload,
                             uint32_t payload_len) {
    uint32_t sum = 0;
    uint16_t tcp_len = sizeof(tcp_header_t) + payload_len;

    uint16_t s_ip[2] = {(uint16_t)(src_ip >> 16),  (uint16_t)(src_ip & 0xFFFF)};
    uint16_t d_ip[2] = {(uint16_t)(dest_ip >> 16), (uint16_t)(dest_ip & 0xFFFF)};

    sum += HTONS(s_ip[0]);
    sum += HTONS(s_ip[1]);
    sum += HTONS(d_ip[0]);
    sum += HTONS(d_ip[1]);
    sum += HTONS(6);
    sum += HTONS(tcp_len);

    uint16_t *ptr = (uint16_t *)tcp;
    for (size_t i = 0; i < sizeof(tcp_header_t) / 2; i++) sum += ptr[i];

    uint16_t *p_ptr = (uint16_t *)payload;
    for (size_t i = 0; i < payload_len / 2; i++) sum += p_ptr[i];
    if (payload_len % 2) sum += (uint16_t)((uint8_t *)payload)[payload_len - 1];

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

void tcp_send_segment(net_interface_t *iface, tcp_socket_t *sock,
                      uint8_t flags, uint8_t *payload, uint32_t payload_len,
                      bool enqueue_for_retx) {
    if (!iface || !sock) return;

    uint32_t total_len = sizeof(tcp_header_t) + payload_len;
    uint8_t *buffer = (uint8_t *)kmalloc(total_len);
    if (!buffer) return;
    memset(buffer, 0, total_len);

    tcp_header_t *tcp = (tcp_header_t *)buffer;
    tcp->src_port    = HTONS(sock->local_port);
    tcp->dest_port   = HTONS(sock->remote_port);
    tcp->seq         = HTONL(sock->snd_nxt);
    tcp->ack         = HTONL(sock->rcv_nxt);
    tcp->data_offset = 0x50;
    tcp->flags       = flags;
    tcp->window_size = HTONS(sock->rcv_wnd ? sock->rcv_wnd : 8192);

    if (payload && payload_len > 0) {
        memcpy(buffer + sizeof(tcp_header_t), payload, payload_len);
    }

    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(iface->ip, sock->remote_ip, tcp, payload, payload_len);

    ipv4_send_packet(iface, sock->remote_ip, 6, buffer, total_len);

    if (enqueue_for_retx) {
        uint32_t consumes = payload_len;
        if (flags & TCP_SYN) consumes += 1;
        if (flags & TCP_FIN) consumes += 1;

        if (consumes > 0) {
            tcp_rtx_enqueue(sock, sock->snd_nxt, flags, payload, payload_len, tick * 10);
            sock->snd_nxt += consumes;
        }
    }

    kfree(buffer);
}

void tcp_send_packet(net_interface_t *iface, tcp_socket_t *sock, uint8_t flags,
                     uint8_t *payload, uint32_t payload_len) {
    tcp_send_segment(iface, sock, flags, payload, payload_len, true);
}

static void tcp_send_pure_ack(net_interface_t *iface, tcp_socket_t *sock) {
    tcp_send_segment(iface, sock, TCP_ACK, NULL, 0, false);
}

uint8_t *http_response_buf = NULL;
uint32_t http_response_len = 0;
bool     http_finished     = false;

static void wget_on_data(tcp_socket_t *sock, uint8_t *data, uint32_t len) {
    (void)sock;
    if (!http_response_buf) {
        http_response_buf = (uint8_t *)kmalloc(65536);
        if (!http_response_buf) return;
        memset(http_response_buf, 0, 65536);
    }
    if (http_response_len + len < 65536) {
        memcpy(http_response_buf + http_response_len, data, len);
        http_response_len += len;
    }
}

extern char last_queried_name[];

static void send_http_get(net_interface_t *iface, tcp_socket_t *sock) {
    char get[512];
    const char *host = last_queried_name[0] ? last_queried_name : "httpforever.com";
    snprintf(get, sizeof(get),
             "GET / HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: EquantOS/1.0\r\n"
             "Accept: text/html,*/*\r\n"
             "Connection: close\r\n\r\n",
             host);
    tcp_send_packet(iface, sock, TCP_PSH | TCP_ACK, (uint8_t *)get, (uint32_t)strlen(get));
}

void handle_tcp(net_interface_t *iface, uint8_t *packet, uint32_t ip_hdr_len) {
    ipv4_header_t *ip = (ipv4_header_t *)(packet + sizeof(ethernet_header_t));
    tcp_header_t  *th = (tcp_header_t *)(packet + sizeof(ethernet_header_t) + ip_hdr_len);

    uint16_t dest_port = HTONS(th->dest_port);
    uint32_t src_ip    = HTONL(ip->src_ip);

    tcp_socket_t *sock = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (tcp_sockets[i].active && tcp_sockets[i].local_port == dest_port &&
            tcp_sockets[i].remote_ip == src_ip) {
            sock = &tcp_sockets[i];
            break;
        }
    }

    if (!sock) return;

    uint32_t hdr_len    = (th->data_offset >> 4) * 4;
    uint32_t payload_len = HTONS(ip->len) - ip_hdr_len - hdr_len;
    uint32_t seq        = HTONL(th->seq);
    uint32_t ack        = HTONL(th->ack);
    uint8_t  flags      = th->flags;
    uint8_t *payload    = (uint8_t *)th + hdr_len;

    if (flags & TCP_RST) {
        tcp_socket_free(sock);
        if (sock->on_data == wget_on_data) http_finished = true;
        return;
    }

    if (flags & TCP_ACK) {
        tcp_rtx_ack(sock, ack);
        if ((int32_t)(ack - sock->snd_una) > 0) sock->snd_una = ack;
    }

    switch (sock->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            sock->rcv_nxt = seq + 1;
            sock->state   = TCP_ESTABLISHED;
            tcp_send_pure_ack(iface, sock);

            if (sock->on_data == wget_on_data) {
                send_http_get(iface, sock);
            }
        }
        break;

    case TCP_ESTABLISHED:
        if (payload_len > 0) {
            if (seq == sock->rcv_nxt) {
                if (sock->on_data) sock->on_data(sock, payload, payload_len);
                sock->rcv_nxt += payload_len;
                tcp_reorder_drain(sock);
                tcp_send_pure_ack(iface, sock);
            } else if ((int32_t)(seq - sock->rcv_nxt) > 0) {
                tcp_reorder_insert(sock, seq, payload, payload_len);
                tcp_send_pure_ack(iface, sock);
            } else {
                tcp_send_pure_ack(iface, sock);
            }
        }

        if (flags & TCP_FIN) {
            sock->rcv_nxt = seq + payload_len + 1;
            sock->state   = TCP_CLOSE_WAIT;
            tcp_send_pure_ack(iface, sock);

            sock->state = TCP_LAST_ACK;
            tcp_send_packet(iface, sock, TCP_FIN | TCP_ACK, NULL, 0);

            if (sock->on_data == wget_on_data) http_finished = true;
        }
        break;

    case TCP_FIN_WAIT_1: {
        bool our_fin_acked = ((int32_t)(ack - sock->snd_nxt) >= 0);

        if (payload_len > 0 && seq == sock->rcv_nxt) {
            if (sock->on_data) sock->on_data(sock, payload, payload_len);
            sock->rcv_nxt += payload_len;
            tcp_reorder_drain(sock);
            tcp_send_pure_ack(iface, sock);
        }

        if (flags & TCP_FIN) {
            sock->rcv_nxt = seq + payload_len + 1;
            tcp_send_pure_ack(iface, sock);

            if (our_fin_acked) {
                sock->state = TCP_TIME_WAIT;
                sock->time_wait_deadline_ms = (tick * 10) + TCP_TIME_WAIT_MS;
            } else {
                sock->state = TCP_CLOSING;
            }
        } else if (our_fin_acked) {
            sock->state = TCP_FIN_WAIT_2;
        }
        break;
    }

    case TCP_FIN_WAIT_2:
        if (payload_len > 0 && seq == sock->rcv_nxt) {
            if (sock->on_data) sock->on_data(sock, payload, payload_len);
            sock->rcv_nxt += payload_len;
            tcp_reorder_drain(sock);
            tcp_send_pure_ack(iface, sock);
        }
        if (flags & TCP_FIN) {
            sock->rcv_nxt = seq + payload_len + 1;
            tcp_send_pure_ack(iface, sock);
            sock->state = TCP_TIME_WAIT;
            sock->time_wait_deadline_ms = (tick * 10) + TCP_TIME_WAIT_MS;
        }
        break;

    case TCP_CLOSING:
        if ((int32_t)(ack - sock->snd_nxt) >= 0) {
            sock->state = TCP_TIME_WAIT;
            sock->time_wait_deadline_ms = (tick * 10) + TCP_TIME_WAIT_MS;
        }
        break;

    case TCP_LAST_ACK:
        if ((int32_t)(ack - sock->snd_nxt) >= 0) {
            tcp_socket_free(sock);
        }
        break;

    case TCP_TIME_WAIT:
        if (flags & TCP_FIN) tcp_send_pure_ack(iface, sock);
        break;

    default:
        break;
    }
}

void tcp_close(net_interface_t *iface, tcp_socket_t *sock) {
    if (!sock || !sock->active) return;

    switch (sock->state) {
    case TCP_ESTABLISHED:
    case TCP_SYN_RECEIVED:
        sock->state = TCP_FIN_WAIT_1;
        tcp_send_packet(iface, sock, TCP_FIN | TCP_ACK, NULL, 0);
        break;
    case TCP_CLOSE_WAIT:
        sock->state = TCP_LAST_ACK;
        tcp_send_packet(iface, sock, TCP_FIN | TCP_ACK, NULL, 0);
        break;
    default:
        break;
    }
}

tcp_socket_t *tcp_connect(net_interface_t *iface, uint32_t dest_ip,
                          uint16_t port, tcp_callback_t callback) {
    if (!iface) return NULL;

    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *s = &tcp_sockets[i];
        if (s->active) continue;

        memset(s, 0, sizeof(*s));
        s->remote_ip   = dest_ip;
        s->remote_port = port;
        s->local_port  = 50000 + i;
        s->snd_nxt     = get_random_isn();
        s->snd_una     = s->snd_nxt;
        s->rcv_nxt     = 0;
        s->rcv_wnd     = TCP_RX_BUF_SIZE;
        s->state       = TCP_SYN_SENT;
        s->on_data     = callback;
        s->active      = true;

        tcp_send_packet(iface, s, TCP_SYN, NULL, 0);
        return s;
    }
    return NULL;
}

void tcp_tick_with_iface(uint32_t now_ms) {
    net_interface_t *iface = net_get_primary_interface();
    if (!iface) return;

    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *s = &tcp_sockets[i];
        if (!s->active) {
            if (s->last_notified_state != TCP_CLOSED) {
                s->state = TCP_CLOSED;
                socket_on_state_change(s);
                s->last_notified_state = TCP_CLOSED;
            }
            continue;
        }

        if (s->state == TCP_TIME_WAIT) {
            if ((int32_t)(now_ms - s->time_wait_deadline_ms) >= 0) {
                tcp_socket_free(s);
            }
        } else if (tcp_rtx_tick(iface, s, now_ms)) {
            tcp_socket_free(s);
            if (s->on_data == wget_on_data) http_finished = true;
        }

        if (s->state != s->last_notified_state) {
            socket_on_state_change(s);
            s->last_notified_state = s->state;
        }
    }
}

void net_wget(net_interface_t *iface, uint32_t dest_ip) {
    if (http_response_buf) {
        kfree(http_response_buf);
        http_response_buf = NULL;
    }
    http_response_len = 0;
    http_finished     = false;

    tcp_connect(iface, dest_ip, 80, wget_on_data);
}