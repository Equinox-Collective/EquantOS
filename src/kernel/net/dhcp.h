// src/kernel/net/dhcp.h
#ifndef DHCP_H
#define DHCP_H

#include "net.h"
#include <stdint.h>
#include <stdbool.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

typedef struct {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed)) dhcp_packet_t;

bool dhcp_discover(net_interface_t *iface);

#endif // DHCP_H