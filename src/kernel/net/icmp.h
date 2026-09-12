#ifndef ICMP_H
#define ICMP_H

#include "net.h"
#include "ipv4.h"

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_header_t;

void handle_icmp(net_interface_t* iface, uint8_t* packet, uint32_t ip_hdr_len);
void icmp_send_echo_request(net_interface_t* iface, uint32_t dest_ip);

#endif
