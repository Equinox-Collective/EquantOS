#include "ipv4.h"
#include "udp.h"
#include "tcp.h"
#include "arp.h"
#include "icmp.h"
#include "../../../../syslibc/string.h"

extern void term_print(const char* str);

uint16_t ip_checksum(void* vdata, uint32_t length) {
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)vdata;

    while (length > 1) { sum += *ptr++; length -= 2; }
    if (length > 0) { sum += *(uint8_t*)ptr; }
    while (sum >> 16) { sum = (sum & 0xFFFF) + (sum >> 16); }

    return (uint16_t)(~sum);
}

void handle_ipv4(net_interface_t* iface, uint8_t* packet) {
    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(ethernet_header_t));
    uint32_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
    
    // Debug log for any IP traffic
    if (ip->proto == 1 || ip->proto == 6 || ip->proto == 17) {
        // Only log ICMP/TCP/UDP to avoid spamming
        // char buf[64]; sprintf(buf, "[IPv4] Incoming Proto: %d\n", ip->proto); term_print(buf);
    }

    if (ip->proto == 1) { // ICMP
        handle_icmp(iface, packet, ip_hdr_len);
    }
    else if (ip->proto == 17) { // UDP
        handle_udp(iface, packet, ip_hdr_len);
    } 
    else if (ip->proto == 6) { // TCP
        handle_tcp(iface, packet, ip_hdr_len);
    }
}

void ipv4_send_packet(net_interface_t* iface, uint32_t dest_ip, uint8_t proto, uint8_t* payload, uint32_t payload_len) {
    if (!iface) return;

    // Routing Logic
    uint32_t target_ip = dest_ip;
    if ((dest_ip & iface->subnet_mask) != (iface->ip & iface->subnet_mask)) {
        target_ip = iface->gateway_ip;
    }

    // ARP Lookup
    uint8_t* dest_mac = arp_lookup(target_ip);
    if (!dest_mac) {
        send_arp_request(iface, target_ip);
        return; // Drop packet for now, first packet triggers ARP
    }

    uint8_t buffer[1600];
    memset(buffer, 0, sizeof(ipv4_header_t));
    ipv4_header_t* ip = (ipv4_header_t*)buffer;
    
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->len = HTONS(sizeof(ipv4_header_t) + payload_len);
    ip->id = HTONS(1);
    ip->flags_offset = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->src_ip = HTONL(iface->ip);
    ip->dest_ip = HTONL(dest_ip);
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip, sizeof(ipv4_header_t));

    memcpy(buffer + sizeof(ipv4_header_t), payload, payload_len);

    net_send_packet(iface, dest_mac, 0x0800, buffer, sizeof(ipv4_header_t) + payload_len);
}
