#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

#define HTONS(n) ((((uint16_t)(n) & 0xFF00) >> 8) | (((uint16_t)(n) & 0x00FF) << 8))
#define HTONL(n) ((((uint32_t)(n) & 0xFF000000) >> 24) | \
                  (((uint32_t)(n) & 0x00FF0000) >> 8)  | \
                  (((uint32_t)(n) & 0x0000FF00) << 8)  | \
                  (((uint32_t)(n) & 0x000000FF) << 24))

typedef struct {
    uint8_t  dest_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype; 
} __attribute__((packed)) ethernet_header_t;

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed)) arp_header_t;

typedef struct {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t flags_offset;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} __attribute__((packed)) ipv4_header_t;

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

typedef struct {
    uint8_t  mode;
    uint8_t  stratum;
    uint8_t  poll;
    uint8_t  precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint64_t ref_ts;
    uint64_t orig_ts;
    uint64_t recv_ts;
    uint64_t trans_ts;
} __attribute__((packed)) ntp_packet_t;

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

typedef struct net_interface {
    char name[16];
    uint8_t mac[6];
    uint32_t ip;
    uint32_t subnet_mask;
    uint32_t gateway_ip;
    void (*send)(void* data, uint32_t len);
} net_interface_t;

// --- Net Interface ---
void net_register_interface(net_interface_t* iface);
net_interface_t* net_get_interface(const char* name);
net_interface_t* net_get_primary_interface(void);

void net_handle_packet(net_interface_t* iface, uint8_t* packet, uint16_t length);
void net_send_packet(net_interface_t* iface, uint8_t* dest_mac, uint16_t ethertype, uint8_t* payload, uint32_t payload_len);

// --- Globals ---
// Removed global mac_addr/local_ip. Use net_get_primary_interface().

#endif
