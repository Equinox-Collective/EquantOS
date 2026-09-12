#include "tcp.h"
#include "string.h"
#include "../core/mem/memory.h"

bool tcp_rtx_enqueue(tcp_socket_t *sock, uint32_t seq, uint8_t flags,
                     const uint8_t *payload, uint32_t payload_len,
                     uint32_t now_ms) {
    for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
        tcp_rtx_entry_t *e = &sock->rtx[i];
        if (e->in_use) continue;

        e->in_use       = true;
        e->seq          = seq;
        e->payload_len  = payload_len;
        e->flags        = flags;
        e->send_time_ms = now_ms;
        e->rto_ms       = TCP_INITIAL_RTO_MS;
        e->retries      = 0;
        e->payload      = NULL;
        if (payload_len > 0 && payload) {
            e->payload = (uint8_t *)kmalloc(payload_len);
            if (!e->payload) {
                e->in_use = false;
                return false;
            }
            memcpy(e->payload, payload, payload_len);
        }
        return true;
    }
    return false;
}

void tcp_rtx_ack(tcp_socket_t *sock, uint32_t ack_nxt) {
    for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
        tcp_rtx_entry_t *e = &sock->rtx[i];
        if (!e->in_use) continue;

        uint32_t seg_end = e->seq + e->payload_len;
        if (e->flags & TCP_SYN) seg_end += 1;
        if (e->flags & TCP_FIN) seg_end += 1;

        int32_t diff = (int32_t)(ack_nxt - seg_end);
        if (diff >= 0) {
            if (e->payload) {
                kfree(e->payload);
                e->payload = NULL;
            }
            e->in_use = false;
        }
    }
}

void tcp_rtx_flush(tcp_socket_t *sock) {
    for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
        tcp_rtx_entry_t *e = &sock->rtx[i];
        if (e->in_use && e->payload) kfree(e->payload);
        e->in_use = false;
        e->payload = NULL;
    }
}

bool tcp_rtx_tick(net_interface_t *iface, tcp_socket_t *sock, uint32_t now_ms) {
    bool kill = false;

    for (int i = 0; i < TCP_RTX_QUEUE_SIZE; i++) {
        tcp_rtx_entry_t *e = &sock->rtx[i];
        if (!e->in_use) continue;

        uint32_t elapsed = now_ms - e->send_time_ms;
        if (elapsed < e->rto_ms) continue;

        bool is_fin_only = (e->payload_len == 0) && (e->flags & TCP_FIN);
        uint32_t retry_budget = is_fin_only ? TCP_FIN_MAX_RETRIES : TCP_MAX_RETRIES;

        if (e->retries >= retry_budget) {
            kill = true;
            continue;
        }

        uint32_t old_snd_nxt = sock->snd_nxt;
        sock->snd_nxt = e->seq;
        tcp_send_segment(iface, sock, e->flags, e->payload, e->payload_len, false);
        sock->snd_nxt = old_snd_nxt;

        e->retries++;
        e->send_time_ms = now_ms;
        e->rto_ms = (e->rto_ms * 2 > TCP_MAX_RTO_MS) ? TCP_MAX_RTO_MS : e->rto_ms * 2;
    }

    return kill;
}

bool tcp_reorder_insert(tcp_socket_t *sock, uint32_t seq, const uint8_t *data, uint32_t len) {
    for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
        tcp_reorder_entry_t *e = &sock->reorder[i];
        if (e->in_use && e->seq == seq && e->len == len) return true;
    }

    for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
        tcp_reorder_entry_t *e = &sock->reorder[i];
        if (e->in_use) continue;

        e->data = (uint8_t *)kmalloc(len);
        if (!e->data) return false;
        memcpy(e->data, data, len);
        e->seq = seq;
        e->len = len;
        e->in_use = true;
        return true;
    }
    return false;
}

uint32_t tcp_reorder_drain(tcp_socket_t *sock) {
    uint32_t delivered = 0;
    bool progress;

    do {
        progress = false;
        for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
            tcp_reorder_entry_t *e = &sock->reorder[i];
            if (!e->in_use) continue;

            if (e->seq == sock->rcv_nxt) {
                if (sock->on_data) sock->on_data(sock, e->data, e->len);
                sock->rcv_nxt += e->len;
                delivered     += e->len;
                kfree(e->data);
                e->data = NULL;
                e->in_use = false;
                progress = true;
            } else {
                int32_t diff = (int32_t)(sock->rcv_nxt - (e->seq + e->len));
                if (diff >= 0) {
                    kfree(e->data);
                    e->data = NULL;
                    e->in_use = false;
                }
            }
        }
    } while (progress);

    return delivered;
}

void tcp_reorder_flush(tcp_socket_t *sock) {
    for (int i = 0; i < TCP_REORDER_BUF_SIZE; i++) {
        tcp_reorder_entry_t *e = &sock->reorder[i];
        if (e->in_use && e->data) kfree(e->data);
        e->in_use = false;
        e->data = NULL;
    }
}