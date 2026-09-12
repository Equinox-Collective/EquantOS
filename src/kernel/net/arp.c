#include "arp.h"
#include "../../../../syslibc/string.h"
#include "../../../../syslibc/stdio.h"

extern void term_print(const char* str);

#define ARP_CACHE_SIZE 32
typedef struct {
    uint32_t ip; // NATIVE ORDER
    uint8_t mac[6];
    bool valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE] = {0};

void arp_update_cache(uint32_t ip, uint8_t* mac) {
    // Input 'ip' is in NATIVE order
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = true;
            return;
        }
    }
}

uint8_t* arp_lookup(uint32_t ip) {
    // Input 'ip' is in NATIVE order
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            return arp_cache[i].mac;
        }
    }
    return 0;
}

void arp_print_cache() {
    term_print("--- ARP CACHE ---\n");
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            char buf[64];
            uint32_t n = arp_cache[i].ip;
            sprintf(buf, "%d.%d.%d.%d -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                    (n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF,
                    arp_cache[i].mac[0], arp_cache[i].mac[1], arp_cache[i].mac[2],
                    arp_cache[i].mac[3], arp_cache[i].mac[4], arp_cache[i].mac[5]);
            term_print(buf);
        }
    }
}

void handle_arp(net_interface_t* iface, uint8_t* packet) {
    arp_header_t* arp = (arp_header_t*)(packet + sizeof(ethernet_header_t));
    
    // Convert to NATIVE for internal storage
    uint32_t spa = HTONL(arp->spa);
    arp_update_cache(spa, arp->sha);

    // If it's an ARP Request (1) for our IP
    if (arp->oper == HTONS(1) && HTONL(arp->tpa) == iface->ip) {
        send_arp_reply(iface, arp->sha, spa);
    }
}

void send_arp_request(net_interface_t* iface, uint32_t target_ip) {
    uint8_t arp_payload[sizeof(arp_header_t)]; 
    arp_header_t* arp = (arp_header_t*)arp_payload;

    arp->htype = HTONS(1);
    arp->ptype = HTONS(0x0800);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = HTONS(1); // 1 = REQUEST
    
    memcpy(arp->sha, iface->mac, 6);
    arp->spa = HTONL(iface->ip); 
    
    memset(arp->tha, 0, 6);
    arp->tpa = HTONL(target_ip);

    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    net_send_packet(iface, bcast, 0x0806, arp_payload, sizeof(arp_header_t));
}

void send_arp_reply(net_interface_t* iface, uint8_t* dest_mac, uint32_t dest_ip) {
    uint8_t arp_payload[sizeof(arp_header_t)];
    arp_header_t* arp = (arp_header_t*)arp_payload;

    arp->htype = HTONS(1);
    arp->ptype = HTONS(0x0800);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = HTONS(2); // 2 = REPLY 

    memcpy(arp->sha, iface->mac, 6); 
    arp->spa = HTONL(iface->ip);  

    memcpy(arp->tha, dest_mac, 6); 
    arp->tpa = HTONL(dest_ip); // dest_ip is NATIVE

    net_send_packet(iface, dest_mac, 0x0806, arp_payload, sizeof(arp_header_t));
}
