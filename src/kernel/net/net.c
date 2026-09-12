#include "net.h"
#include "arp.h"
#include "ipv4.h"
#include "../../../../syslibc/string.h"

extern void term_print(const char* str);

#define MAX_INTERFACES 4
static net_interface_t* interfaces[MAX_INTERFACES] = {0};
static int num_interfaces = 0;

void net_register_interface(net_interface_t* iface) {
    if (num_interfaces < MAX_INTERFACES) {
        interfaces[num_interfaces++] = iface;
    }
}

net_interface_t* net_get_interface(const char* name) {
    for (int i = 0; i < num_interfaces; i++) {
        if (strcmp(interfaces[i]->name, name) == 0) {
            return interfaces[i];
        }
    }
    return 0;
}

net_interface_t* net_get_primary_interface(void) {
    if (num_interfaces > 0) return interfaces[0];
    return 0;
}

void net_handle_packet(net_interface_t* iface, uint8_t* packet, uint16_t length) {
    (void)length;
    ethernet_header_t* eth = (ethernet_header_t*)packet;
    uint16_t type = HTONS(eth->ethertype);

    if (type == 0x0806) {
        handle_arp(iface, packet);
    } else if (type == 0x0800) {
        handle_ipv4(iface, packet);
    }
}

void net_send_packet(net_interface_t* iface, uint8_t* dest_mac, uint16_t ethertype, uint8_t* payload, uint32_t payload_len) {
    if (payload_len > 1500) return;
    if (!iface) return;
    
    uint8_t frame[1514];
    ethernet_header_t* header = (ethernet_header_t*)frame;

    memcpy(header->dest_mac, dest_mac, 6);
    memcpy(header->src_mac, iface->mac, 6);
    header->ethertype = HTONS(ethertype);

    memcpy(frame + sizeof(ethernet_header_t), payload, payload_len);

    if (iface->send) {
        iface->send(frame, sizeof(ethernet_header_t) + payload_len);
    }
}
