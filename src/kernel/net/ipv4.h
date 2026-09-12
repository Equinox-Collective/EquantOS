#ifndef IPV4_H
#define IPV4_H

#include "net.h"

uint16_t ip_checksum(void* vdata, uint32_t length);
void handle_ipv4(net_interface_t* iface, uint8_t* packet);
void ipv4_send_packet(net_interface_t* iface, uint32_t dest_ip, uint8_t proto, uint8_t* payload, uint32_t payload_len);

#endif
