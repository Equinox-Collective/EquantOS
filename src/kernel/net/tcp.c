// src/kernel/net/tcp.c - TCP client connections (RFC 793 subset)
//
// Each connection keeps its unacknowledged and unsent bytes in a ring buffer
// starting at snd_una; lost segments are resent go-back-N style from that
// buffer. The advertised window is the free space the socket layer reports,
// and bytes it refuses are not acknowledged, so a slow reader throttles the
// peer instead of silently losing data.
#include "tcp.h"
#include "ipv4.h"
#include "string.h"
#include "stdio.h"
#include "../core/mem/memory.h"
#include "../misc/timer.h"

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

#define TCP_OPT_MSS   2

static tcp_socket_t tcp_sockets[TCP_MAX_SOCKETS];
static uint16_t next_ephemeral = 0;

uint32_t tcp_now_ms(void) {
    return tick * (1000 / TIMER_HZ);
}

static uint32_t get_random_isn(void) {
    static uint32_t seed = 0x12345678;
    seed ^= (seed << 13) ^ (tick * 1103515245 + 12345);
    seed ^= (seed >> 17);
    seed ^= (seed << 5);
    return seed;
}

static uint16_t tcp_alloc_port(void) {
    uint32_t range = TCP_EPHEMERAL_LAST - TCP_EPHEMERAL_FIRST + 1;
    if (next_ephemeral == 0) {
        next_ephemeral = (uint16_t)(TCP_EPHEMERAL_FIRST + get_random_isn() % range);
    }
    for (uint32_t tries = 0; tries < range; tries++) {
        uint16_t port = next_ephemeral;
        next_ephemeral = (port >= TCP_EPHEMERAL_LAST) ? TCP_EPHEMERAL_FIRST : (uint16_t)(port + 1);

        bool used = false;
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (tcp_sockets[i].active && tcp_sockets[i].local_port == port) {
                used = true;
                break;
            }
        }
        if (!used) return port;
    }
    return 0;
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip, const uint8_t *seg, uint32_t len) {
    uint32_t sum = (src_ip >> 16) + (src_ip & 0xFFFF) +
                   (dest_ip >> 16) + (dest_ip & 0xFFFF) + 6 + len;
    uint32_t i = 0;
    for (; i + 1 < len; i += 2) sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
    if (len & 1) sum += (uint32_t)seg[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return HTONS((uint16_t)~sum);
}

static uint32_t tcp_rcv_window(tcp_socket_t *s) {
    uint32_t wnd = s->rx_space ? s->rx_space(s) : 65535;
    if (wnd > 65535) wnd = 65535;
    s->last_adv_wnd = wnd;
    return wnd;
}

// Build and transmit one segment. Payload comes from the send ring at
// tx_offset (bytes past snd_una) unless flags carry no data.
static void tcp_xmit(tcp_socket_t *s, uint32_t seq, uint8_t flags,
                     uint32_t tx_offset, uint32_t len) {
    net_interface_t *iface = net_get_primary_interface();
    if (!iface) return;

    uint8_t seg[sizeof(tcp_header_t) + 4 + TCP_MAX_SEG_PAYLOAD];
    uint32_t opt_len = (flags & TCP_SYN) ? 4 : 0;
    uint32_t hdr_len = sizeof(tcp_header_t) + opt_len;
    if (len > TCP_MAX_SEG_PAYLOAD) len = TCP_MAX_SEG_PAYLOAD;

    memset(seg, 0, hdr_len);
    tcp_header_t *th = (tcp_header_t *)seg;
    th->src_port    = HTONS(s->local_port);
    th->dest_port   = HTONS(s->remote_port);
    th->seq         = HTONL(seq);
    th->ack         = (flags & TCP_ACK) ? HTONL(s->rcv_nxt) : 0;
    th->data_offset = (uint8_t)((hdr_len / 4) << 4);
    th->flags       = flags;
    th->window_size = HTONS((uint16_t)tcp_rcv_window(s));

    if (opt_len) {
        seg[20] = TCP_OPT_MSS;
        seg[21] = 4;
        seg[22] = (uint8_t)(TCP_LOCAL_MSS >> 8);
        seg[23] = (uint8_t)(TCP_LOCAL_MSS & 0xFF);
    }

    for (uint32_t i = 0; i < len; i++) {
        seg[hdr_len + i] = s->tx_buf[(s->tx_head + tx_offset + i) % TCP_TX_BUF_SIZE];
    }

    th->checksum = 0;
    th->checksum = tcp_checksum(iface->ip, s->remote_ip, seg, hdr_len + len);
    ipv4_send_packet(iface, s->remote_ip, 6, seg, hdr_len + len);
}

static void tcp_send_ack(tcp_socket_t *s) {
    tcp_xmit(s, s->snd_nxt, TCP_ACK, 0, 0);
}

static void tcp_arm_rtx(tcp_socket_t *s) {
    s->rtx_armed = true;
    s->rtx_deadline_ms = tcp_now_ms() + s->rto_ms;
}

static void tcp_notify(tcp_socket_t *s) {
    if (s->owner && s->on_event) s->on_event(s);
}

static void tcp_reorder_flush(tcp_socket_t *s) {
    for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
        if (s->reorder[i].in_use && s->reorder[i].data) kfree(s->reorder[i].data);
        s->reorder[i].in_use = false;
        s->reorder[i].data = NULL;
    }
}

static void tcp_free(tcp_socket_t *s) {
    tcp_reorder_flush(s);
    if (s->tx_buf) kfree(s->tx_buf);
    s->tx_buf = NULL;
    s->state = TCP_CLOSED;
    tcp_notify(s); // last event: the owner must drop its pointer now
    s->active = false;
    s->owner = NULL;
}

static void tcp_fail(tcp_socket_t *s, tcp_error_t err) {
    s->error = err;
    tcp_free(s);
}

// Push as much queued data as the peer window allows, then the FIN
static void tcp_output(tcp_socket_t *s) {
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT &&
        s->state != TCP_FIN_WAIT_1 && s->state != TCP_CLOSING &&
        s->state != TCP_LAST_ACK) {
        return;
    }

    uint32_t mss = s->peer_mss ? s->peer_mss : TCP_DEFAULT_MSS;
    if (mss > TCP_MAX_SEG_PAYLOAD) mss = TCP_MAX_SEG_PAYLOAD;

    for (;;) {
        uint32_t sent = s->snd_nxt - s->snd_una;
        if (sent >= s->tx_len) break;
        if (sent >= s->snd_wnd) break;

        uint32_t n = s->tx_len - sent;
        if (n > s->snd_wnd - sent) n = s->snd_wnd - sent;
        if (n > mss) n = mss;

        uint8_t flags = TCP_ACK;
        if (sent + n == s->tx_len) flags |= TCP_PSH;
        tcp_xmit(s, s->snd_nxt, flags, sent, n);
        s->snd_nxt += n;
        if (!s->rtx_armed) tcp_arm_rtx(s);
    }

    uint32_t fin_seq = s->snd_una + s->tx_len;
    if (s->fin_pending && s->snd_nxt == fin_seq) {
        tcp_xmit(s, fin_seq, TCP_FIN | TCP_ACK, 0, 0);
        s->snd_nxt = fin_seq + 1;
        s->fin_sent = true;
        if (!s->rtx_armed) tcp_arm_rtx(s);
    }

    // Zero window with data waiting: the persist timer will probe it
    if (!s->rtx_armed && s->snd_wnd == 0 && s->tx_len > (s->snd_nxt - s->snd_una)) {
        tcp_arm_rtx(s);
    }
}

uint32_t tcp_send_space(const tcp_socket_t *s) {
    if (!s || !s->active || !s->tx_buf) return 0;
    return TCP_TX_BUF_SIZE - s->tx_len;
}

uint32_t tcp_write(tcp_socket_t *s, const uint8_t *data, uint32_t len) {
    if (!s || !s->active || !s->tx_buf || s->fin_pending) return 0;
    uint32_t space = TCP_TX_BUF_SIZE - s->tx_len;
    if (len > space) len = space;

    uint32_t tail = (s->tx_head + s->tx_len) % TCP_TX_BUF_SIZE;
    for (uint32_t i = 0; i < len; i++) {
        s->tx_buf[(tail + i) % TCP_TX_BUF_SIZE] = data[i];
    }
    s->tx_len += len;
    tcp_output(s);
    return len;
}

void tcp_window_update(tcp_socket_t *s) {
    if (!s || !s->active) return;
    if (s->state != TCP_ESTABLISHED && s->state != TCP_FIN_WAIT_1 && s->state != TCP_FIN_WAIT_2) return;

    uint32_t old = s->last_adv_wnd;
    uint32_t now = s->rx_space ? s->rx_space(s) : 65535;
    if (now > 65535) now = 65535;
    // Announce a reopened window once it grew by a full segment (avoids silly windows)
    if (now >= old + TCP_LOCAL_MSS || (old < TCP_LOCAL_MSS && now >= TCP_LOCAL_MSS)) {
        tcp_send_ack(s);
    }
}

// Deliver in-order bytes to the owner; returns how many were accepted
static uint32_t tcp_deliver(tcp_socket_t *s, uint8_t *data, uint32_t len) {
    uint32_t accepted = len;
    if (s->owner && s->on_data) accepted = s->on_data(s, data, len);
    if (accepted > len) accepted = len;
    s->rcv_nxt += accepted;
    return accepted;
}

static void tcp_reorder_insert(tcp_socket_t *s, uint32_t seq, const uint8_t *data, uint32_t len) {
    if (SEQ_GEQ(seq, s->rcv_nxt + 65535)) return;
    for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
        if (s->reorder[i].in_use && s->reorder[i].seq == seq) return;
    }
    for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
        tcp_reorder_entry_t *e = &s->reorder[i];
        if (e->in_use) continue;
        e->data = (uint8_t *)kmalloc(len);
        if (!e->data) return;
        memcpy(e->data, data, len);
        e->seq = seq;
        e->len = len;
        e->in_use = true;
        return;
    }
}

static void tcp_reorder_drain(tcp_socket_t *s) {
    bool progress;
    do {
        progress = false;
        for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
            tcp_reorder_entry_t *e = &s->reorder[i];
            if (!e->in_use) continue;

            uint32_t end = e->seq + e->len;
            if (SEQ_LEQ(end, s->rcv_nxt)) {
                // Already covered by in-order data
            } else if (SEQ_LEQ(e->seq, s->rcv_nxt)) {
                uint32_t skip = s->rcv_nxt - e->seq;
                uint32_t want = e->len - skip;
                uint32_t got = tcp_deliver(s, e->data + skip, want);
                progress = (got == want);
                if (!progress) {
                    // Receiver full: forget the rest, the peer will resend it
                }
            } else {
                continue;
            }
            kfree(e->data);
            e->data = NULL;
            e->in_use = false;
        }
    } while (progress);
}

static void tcp_parse_options(tcp_socket_t *s, const tcp_header_t *th, uint32_t hdr_len) {
    const uint8_t *opt = (const uint8_t *)th + sizeof(tcp_header_t);
    const uint8_t *end = (const uint8_t *)th + hdr_len;
    while (opt < end) {
        uint8_t kind = opt[0];
        if (kind == 0) break;
        if (kind == 1) { opt++; continue; }
        if (opt + 1 >= end) break;
        uint8_t olen = opt[1];
        if (olen < 2 || opt + olen > end) break;
        if (kind == TCP_OPT_MSS && olen == 4) {
            s->peer_mss = ((uint32_t)opt[2] << 8) | opt[3];
        }
        opt += olen;
    }
}

// Process an acknowledgement of our data/FIN; returns true if our FIN is acked
static bool tcp_process_ack(tcp_socket_t *s, uint32_t ack, uint32_t wnd) {
    uint32_t snd_max = s->snd_una + s->tx_len + (s->fin_sent ? 1 : 0);

    if (SEQ_GT(ack, s->snd_una) && SEQ_LEQ(ack, snd_max)) {
        uint32_t acked = ack - s->snd_una;
        uint32_t data_acked = acked > s->tx_len ? s->tx_len : acked;
        s->tx_head = (s->tx_head + data_acked) % TCP_TX_BUF_SIZE;
        s->tx_len -= data_acked;
        s->snd_una = ack;
        if (SEQ_LT(s->snd_nxt, s->snd_una)) s->snd_nxt = s->snd_una;

        s->retries = 0;
        s->rto_ms = TCP_INITIAL_RTO_MS;
        if (s->snd_una == s->snd_nxt && s->tx_len == 0) {
            s->rtx_armed = false;
        } else {
            tcp_arm_rtx(s);
        }
        s->snd_wnd = wnd;
        tcp_notify(s); // send space opened up
    } else if (ack == s->snd_una) {
        s->snd_wnd = wnd;
    }

    return s->fin_sent && s->tx_len == 0 && s->snd_una == s->snd_nxt;
}

void handle_tcp(net_interface_t *iface, uint8_t *packet, uint32_t ip_hdr_len) {
    (void)iface;
    ipv4_header_t *ip = (ipv4_header_t *)(packet + sizeof(ethernet_header_t));
    tcp_header_t  *th = (tcp_header_t *)(packet + sizeof(ethernet_header_t) + ip_hdr_len);

    uint16_t dest_port = HTONS(th->dest_port);
    uint16_t src_port  = HTONS(th->src_port);
    uint32_t src_ip    = HTONL(ip->src_ip);

    tcp_socket_t *s = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *c = &tcp_sockets[i];
        if (c->active && c->local_port == dest_port && c->remote_ip == src_ip &&
            c->remote_port == src_port) {
            s = c;
            break;
        }
    }
    if (!s) return;

    uint32_t hdr_len     = (uint32_t)(th->data_offset >> 4) * 4;
    uint32_t ip_len      = HTONS(ip->len);
    if (hdr_len < sizeof(tcp_header_t) || ip_len < ip_hdr_len + hdr_len) return;
    uint32_t payload_len = ip_len - ip_hdr_len - hdr_len;
    uint32_t seq         = HTONL(th->seq);
    uint32_t ack         = HTONL(th->ack);
    uint32_t wnd         = HTONS(th->window_size);
    uint8_t  flags       = th->flags;
    uint8_t *payload     = (uint8_t *)th + hdr_len;

    if (s->state == TCP_SYN_SENT) {
        bool ack_ok = (flags & TCP_ACK) && ack == s->snd_nxt;
        if (flags & TCP_RST) {
            if (ack_ok) tcp_fail(s, TCP_ERR_REFUSED);
            return;
        }
        if ((flags & TCP_SYN) && ack_ok) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = wnd;
            s->peer_mss = TCP_DEFAULT_MSS;
            tcp_parse_options(s, th, hdr_len);
            s->state = TCP_ESTABLISHED;
            s->rtx_armed = false;
            s->retries = 0;
            s->rto_ms = TCP_INITIAL_RTO_MS;
            tcp_send_ack(s);
            tcp_notify(s);
            tcp_output(s);
        }
        return;
    }

    if (flags & TCP_RST) {
        // Accept resets that fall inside the receive window
        if (SEQ_GEQ(seq, s->rcv_nxt) && SEQ_LT(seq, s->rcv_nxt + 65536)) {
            if (s->state == TCP_TIME_WAIT || s->state == TCP_LAST_ACK || s->state == TCP_CLOSING) {
                tcp_free(s);
            } else {
                tcp_fail(s, TCP_ERR_RESET);
            }
        }
        return;
    }

    bool fin_acked = false;
    if (flags & TCP_ACK) fin_acked = tcp_process_ack(s, ack, wnd);

    // Incoming data (still flows after we sent our FIN)
    bool need_ack = false;
    bool can_receive = (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 ||
                        s->state == TCP_FIN_WAIT_2);
    if (payload_len > 0) {
        need_ack = true;
        if (can_receive) {
            if (seq == s->rcv_nxt) {
                if (tcp_deliver(s, payload, payload_len) == payload_len) tcp_reorder_drain(s);
            } else if (SEQ_LT(seq, s->rcv_nxt)) {
                uint32_t end = seq + payload_len;
                if (SEQ_GT(end, s->rcv_nxt)) {
                    uint32_t skip = s->rcv_nxt - seq;
                    if (tcp_deliver(s, payload + skip, payload_len - skip) == payload_len - skip) {
                        tcp_reorder_drain(s);
                    }
                }
            } else {
                tcp_reorder_insert(s, seq, payload, payload_len);
            }
        }
    }

    // Peer FIN counts only once every byte before it has been accepted
    if ((flags & TCP_FIN) && seq + payload_len == s->rcv_nxt && !s->peer_fin) {
        s->rcv_nxt++;
        s->peer_fin = true;
        need_ack = true;

        switch (s->state) {
        case TCP_ESTABLISHED:
            s->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            if (fin_acked) {
                s->state = TCP_TIME_WAIT;
                s->deadline_ms = tcp_now_ms() + TCP_TIME_WAIT_MS;
            } else {
                s->state = TCP_CLOSING;
            }
            break;
        case TCP_FIN_WAIT_2:
            s->state = TCP_TIME_WAIT;
            s->deadline_ms = tcp_now_ms() + TCP_TIME_WAIT_MS;
            break;
        default:
            break;
        }
        tcp_notify(s);
    } else if ((flags & TCP_FIN) && s->peer_fin) {
        need_ack = true; // retransmitted FIN: our ACK got lost
    }

    if (fin_acked) {
        switch (s->state) {
        case TCP_FIN_WAIT_1:
            s->state = TCP_FIN_WAIT_2;
            s->deadline_ms = tcp_now_ms() + TCP_FIN_WAIT2_MS;
            break;
        case TCP_CLOSING:
            s->state = TCP_TIME_WAIT;
            s->deadline_ms = tcp_now_ms() + TCP_TIME_WAIT_MS;
            break;
        case TCP_LAST_ACK:
            tcp_free(s);
            return;
        default:
            break;
        }
    }

    if (need_ack) tcp_send_ack(s);
    tcp_output(s);
}

void tcp_shutdown(tcp_socket_t *s) {
    if (!s || !s->active || s->fin_pending) return;

    if (s->state == TCP_ESTABLISHED) {
        s->state = TCP_FIN_WAIT_1;
    } else if (s->state == TCP_CLOSE_WAIT) {
        s->state = TCP_LAST_ACK;
    } else {
        return;
    }
    s->fin_pending = true;
    tcp_output(s);
}

void tcp_close(net_interface_t *iface, tcp_socket_t *s) {
    (void)iface;
    if (!s || !s->active) return;

    // The application is gone: further data is accepted and dropped
    s->owner = NULL;
    s->on_data = NULL;
    s->rx_space = NULL;
    s->on_event = NULL;

    if (s->state == TCP_SYN_SENT) {
        tcp_free(s);
        return;
    }
    tcp_shutdown(s);
}

void tcp_abort(tcp_socket_t *s) {
    if (!s || !s->active) return;
    if (s->state != TCP_SYN_SENT && s->state != TCP_TIME_WAIT) {
        tcp_xmit(s, s->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
    }
    s->owner = NULL;
    tcp_free(s);
}

tcp_socket_t *tcp_connect(net_interface_t *iface, uint32_t dest_ip, uint16_t port,
                          tcp_callback_t on_data, tcp_space_t rx_space,
                          tcp_event_t on_event, void *owner) {
    if (!iface) return NULL;

    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *s = &tcp_sockets[i];
        if (s->active) continue;

        uint16_t lport = tcp_alloc_port();
        if (!lport) return NULL;

        memset(s, 0, sizeof(*s));
        s->tx_buf = (uint8_t *)kmalloc(TCP_TX_BUF_SIZE);
        if (!s->tx_buf) return NULL;

        s->remote_ip   = dest_ip;
        s->remote_port = port;
        s->local_port  = lport;
        s->snd_una     = get_random_isn();
        s->snd_nxt     = s->snd_una;
        s->peer_mss    = TCP_DEFAULT_MSS;
        s->rto_ms      = TCP_INITIAL_RTO_MS;
        s->state       = TCP_SYN_SENT;
        s->on_data     = on_data;
        s->rx_space    = rx_space;
        s->on_event    = on_event;
        s->owner       = owner;
        s->active      = true;

        tcp_xmit(s, s->snd_nxt, TCP_SYN, 0, 0);
        s->snd_nxt++;
        tcp_arm_rtx(s);
        return s;
    }
    return NULL;
}

static void tcp_timeout(tcp_socket_t *s, uint32_t now_ms) {
    if (s->state == TCP_SYN_SENT) {
        if (s->retries >= TCP_SYN_MAX_RETRIES) {
            tcp_fail(s, TCP_ERR_TIMEOUT);
            return;
        }
        tcp_xmit(s, s->snd_una, TCP_SYN, 0, 0);
    } else if (s->snd_nxt == s->snd_una && s->snd_wnd == 0 && s->tx_len > 0) {
        // Persist timer: squeeze one byte past the closed window to get a fresh ACK
        tcp_xmit(s, s->snd_nxt, TCP_ACK, 0, 1);
        s->snd_nxt++;
        s->rto_ms = (s->rto_ms * 2 > TCP_MAX_RTO_MS) ? TCP_MAX_RTO_MS : s->rto_ms * 2;
        s->rtx_deadline_ms = now_ms + s->rto_ms;
        return;
    } else {
        if (s->retries >= TCP_MAX_RETRIES) {
            tcp_xmit(s, s->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
            tcp_fail(s, TCP_ERR_TIMEOUT);
            return;
        }
        // Go back to the oldest unacknowledged byte and resend from there
        s->snd_nxt = s->snd_una;
        s->rtx_armed = false;
        tcp_output(s);
    }

    s->retries++;
    s->rto_ms = (s->rto_ms * 2 > TCP_MAX_RTO_MS) ? TCP_MAX_RTO_MS : s->rto_ms * 2;
    s->rtx_armed = true;
    s->rtx_deadline_ms = now_ms + s->rto_ms;
}

void tcp_tick_with_iface(uint32_t now_ms) {
    if (!net_get_primary_interface()) return;

    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *s = &tcp_sockets[i];
        if (!s->active) continue;

        if (s->state == TCP_TIME_WAIT || (s->state == TCP_FIN_WAIT_2 && !s->owner)) {
            if ((int32_t)(now_ms - s->deadline_ms) >= 0) {
                tcp_free(s);
                continue;
            }
        }

        if (s->rtx_armed && (int32_t)(now_ms - s->rtx_deadline_ms) >= 0) {
            tcp_timeout(s, now_ms);
        }
    }
}

// ============================================================================
// Kernel shell "wget": plain HTTP GET into a 64 KB buffer
// ============================================================================

uint8_t *http_response_buf = NULL;
uint32_t http_response_len = 0;
bool     http_finished     = false;

extern char last_queried_name[];

static uint32_t wget_on_data(tcp_socket_t *sock, uint8_t *data, uint32_t len) {
    (void)sock;
    if (!http_response_buf) {
        http_response_buf = (uint8_t *)kmalloc(65536);
        if (!http_response_buf) return len;
        memset(http_response_buf, 0, 65536);
    }
    if (http_response_len + len < 65536) {
        memcpy(http_response_buf + http_response_len, data, len);
        http_response_len += len;
    }
    return len;
}

static bool wget_request_sent = false;

static void wget_on_event(tcp_socket_t *sock) {
    if (sock->state == TCP_ESTABLISHED && !wget_request_sent) {
        wget_request_sent = true;
        char get[512];
        const char *host = last_queried_name[0] ? last_queried_name : "httpforever.com";
        snprintf(get, sizeof(get),
                 "GET / HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: EquantOS/1.0\r\n"
                 "Accept: text/html,*/*\r\n"
                 "Connection: close\r\n\r\n",
                 host);
        tcp_write(sock, (const uint8_t *)get, (uint32_t)strlen(get));
    } else if (sock->state == TCP_CLOSE_WAIT || sock->state == TCP_CLOSED) {
        http_finished = true;
        if (sock->state == TCP_CLOSE_WAIT) tcp_close(net_get_primary_interface(), sock);
    }
}

void net_wget(net_interface_t *iface, uint32_t dest_ip) {
    if (http_response_buf) {
        kfree(http_response_buf);
        http_response_buf = NULL;
    }
    http_response_len = 0;
    http_finished     = false;
    wget_request_sent = false;

    static int wget_owner;
    tcp_connect(iface, dest_ip, 80, wget_on_data, NULL, wget_on_event, &wget_owner);
}
