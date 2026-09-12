#ifndef TCP_H
#define TCP_H

#include "net.h"
#include <stdbool.h>
#include <stdint.h>

#define TCP_MAX_SOCKETS        32
#define TCP_RTX_QUEUE_SIZE     8
#define TCP_REORDER_BUF_SIZE   8
#define TCP_RX_BUF_SIZE        16384
#define TCP_MAX_SEG_PAYLOAD    1400

#define TCP_INITIAL_RTO_MS     500
#define TCP_MAX_RTO_MS         8000
#define TCP_MAX_RETRIES        5
#define TCP_FIN_MAX_RETRIES    2
#define TCP_TIME_WAIT_MS       2000

typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

struct tcp_socket;
typedef void (*tcp_callback_t)(struct tcp_socket *sock, uint8_t *data, uint32_t len);

typedef struct {
    bool     in_use;
    uint32_t seq;
    uint32_t payload_len;
    uint8_t  flags;
    uint32_t send_time_ms;
    uint32_t rto_ms;
    uint8_t  retries;
    uint8_t *payload;
} tcp_rtx_entry_t;

typedef struct {
    bool     in_use;
    uint32_t seq;
    uint32_t len;
    uint8_t *data;
} tcp_reorder_entry_t;

typedef struct tcp_socket {
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    tcp_state_t state;
    tcp_state_t last_notified_state;

    uint32_t snd_nxt;
    uint32_t snd_una;
    uint32_t rcv_nxt;
    uint16_t rcv_wnd;

    tcp_callback_t on_data;

    tcp_rtx_entry_t rtx[TCP_RTX_QUEUE_SIZE];
    tcp_reorder_entry_t reorder[TCP_REORDER_BUF_SIZE];

    uint32_t time_wait_deadline_ms;
    bool active;
} tcp_socket_t;

void handle_tcp(net_interface_t *iface, uint8_t *packet, uint32_t ip_hdr_len);
void tcp_send_segment(net_interface_t *iface, tcp_socket_t *sock,
                      uint8_t flags, uint8_t *payload, uint32_t payload_len,
                      bool enqueue_for_retx);
void tcp_send_packet(net_interface_t *iface, tcp_socket_t *sock, uint8_t flags,
                     uint8_t *payload, uint32_t payload_len);
void tcp_tick(uint32_t now_ms);
void tcp_tick_with_iface(uint32_t now_ms);
void tcp_close(net_interface_t *iface, tcp_socket_t *sock);

void net_wget(net_interface_t *iface, uint32_t dest_ip);
tcp_socket_t *tcp_connect(net_interface_t *iface, uint32_t dest_ip,
                          uint16_t port, tcp_callback_t callback);

#endif // TCP_H