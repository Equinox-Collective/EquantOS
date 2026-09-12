#ifndef ARP_H
#define ARP_H

#include "net.h"

void handle_arp(net_interface_t* iface, uint8_t* packet);
void send_arp_request(net_interface_t* iface, uint32_t target_ip);
void send_arp_reply(net_interface_t* iface, uint8_t* dest_mac, uint32_t dest_ip);

// ARP Cache functions
uint8_t* arp_lookup(uint32_t ip);
void arp_print_cache();

#endif
