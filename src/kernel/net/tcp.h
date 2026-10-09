#ifndef TCP_H
#define TCP_H

#include "net.h"
#include <stdbool.h>
#include <stdint.h>

#define TCP_MAX_SOCKETS        128
#define TCP_REORDER_BUF_SIZE   16
#define TCP_TX_BUF_SIZE        65536     // Unacknowledged + unsent bytes per connection
#define TCP_DEFAULT_MSS        536
#define TCP_LOCAL_MSS          1460
#define TCP_MAX_SEG_PAYLOAD    1460

#define TCP_INITIAL_RTO_MS     400
#define TCP_MIN_RTO_MS         200
#define TCP_MAX_RTO_MS         8000
#define TCP_MAX_RETRIES        8
#define TCP_SYN_MAX_RETRIES    5
#define TCP_TIME_WAIT_MS       2000
#define TCP_FIN_WAIT2_MS       15000

#define TCP_EPHEMERAL_FIRST    49152
#define TCP_EPHEMERAL_LAST     65535

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

// Why a connection died (reported to the socket layer as errno)
typedef enum {
    TCP_ERR_NONE = 0,
    TCP_ERR_REFUSED,
    TCP_ERR_RESET,
    TCP_ERR_TIMEOUT
} tcp_error_t;

struct tcp_socket;

// Receive callback: returns how many bytes it accepted (the rest is not ACKed)
typedef uint32_t (*tcp_callback_t)(struct tcp_socket *sock, uint8_t *data, uint32_t len);
// Free receive space, advertised as the TCP window
typedef uint32_t (*tcp_space_t)(struct tcp_socket *sock);
// Connection state changed (established, peer FIN, closed, error)
typedef void (*tcp_event_t)(struct tcp_socket *sock);

typedef struct {
    bool     in_use;
    uint32_t seq;
    uint32_t len;
    uint8_t *data;
} tcp_reorder_entry_t;

typedef struct tcp_socket {
    bool active;

    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    tcp_state_t state;
    tcp_error_t error;

    // Send side: tx_buf holds the bytes from snd_una onwards (tx_len of them)
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint32_t peer_mss;
    uint8_t *tx_buf;
    uint32_t tx_head;            // ring index of snd_una
    uint32_t tx_len;
    bool     fin_pending;        // application closed: FIN goes after the queued data
    bool     fin_sent;

    // Retransmission timer
    uint32_t rto_ms;
    uint32_t rtx_deadline_ms;
    bool     rtx_armed;
    uint8_t  retries;

    // Receive side
    uint32_t rcv_nxt;
    uint32_t last_adv_wnd;
    bool     peer_fin;
    tcp_reorder_entry_t reorder[TCP_REORDER_BUF_SIZE];

    uint32_t deadline_ms;        // TIME_WAIT / FIN_WAIT_2 expiry

    tcp_callback_t on_data;
    tcp_space_t    rx_space;
    tcp_event_t    on_event;
    void          *owner;        // socket layer entry, NULL once the application let go
} tcp_socket_t;

uint32_t tcp_now_ms(void);

void handle_tcp(net_interface_t *iface, uint8_t *packet, uint32_t ip_hdr_len);
void tcp_tick_with_iface(uint32_t now_ms);

tcp_socket_t *tcp_connect(net_interface_t *iface, uint32_t dest_ip, uint16_t port,
                          tcp_callback_t on_data, tcp_space_t rx_space,
                          tcp_event_t on_event, void *owner);
// Queue bytes for sending; returns how many fit into the send buffer
uint32_t tcp_write(tcp_socket_t *sock, const uint8_t *data, uint32_t len);
uint32_t tcp_send_space(const tcp_socket_t *sock);
// Application consumed received data: reopen the window if it was closing
void tcp_window_update(tcp_socket_t *sock);
// Send FIN after the queued data but keep receiving (shutdown(SHUT_WR))
void tcp_shutdown(tcp_socket_t *sock);
// Graceful close (FIN after queued data); the socket frees itself later
void tcp_close(net_interface_t *iface, tcp_socket_t *sock);
// Hard close (RST) and immediate free
void tcp_abort(tcp_socket_t *sock);

void net_wget(net_interface_t *iface, uint32_t dest_ip);

#endif // TCP_H
