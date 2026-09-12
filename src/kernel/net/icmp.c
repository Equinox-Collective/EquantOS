#include "icmp.h"
#include "ipv4.h"
#include "../../../../syslibc/string.h"
#include "../../../../syslibc/stdio.h"

extern void term_print(const char* str);
uint16_t icmp_checksum(void* vdata, uint32_t length) {
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)vdata;
    while (length > 1) { sum += *ptr++; length -= 2; }
    if (length > 0) sum += *(uint8_t*)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

void handle_icmp(net_interface_t* iface, uint8_t* packet, uint32_t ip_hdr_len) {
    icmp_header_t* icmp = (icmp_header_t*)(packet + sizeof(ethernet_header_t) + ip_hdr_len);
    ipv4_header_t* ip = (ipv4_header_t*)(packet + sizeof(ethernet_header_t));

    if (icmp->type == 8) { // Echo Request
        term_print("[ICMP] Received Echo Request\n");
        // ... prep reply ...
        uint8_t buffer[64];
        memset(buffer, 0, 64);
        icmp_header_t* reply = (icmp_header_t*)buffer;
        reply->type = 0; reply->code = 0;
        reply->id = icmp->id; reply->seq = icmp->seq;
        reply->checksum = 0;
        reply->checksum = icmp_checksum(reply, sizeof(icmp_header_t));
        ipv4_send_packet(iface, HTONL(ip->src_ip), 1, buffer, sizeof(icmp_header_t));
    }
    else if (icmp->type == 0) { // Echo Reply
        uint32_t src = HTONL(ip->src_ip);
        char buf[80];
        sprintf(buf, "[ICMP] Reply from %d.%d.%d.%d: bytes=8 seq=%d\n",
                (src >> 24) & 0xFF, (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
                HTONS(icmp->seq));
        term_print(buf);
    } else {
        char buf[64];
        sprintf(buf, "[ICMP] Incoming packet type: %d\n", icmp->type);
        term_print(buf);
    }
}

void icmp_send_echo_request(net_interface_t* iface, uint32_t dest_ip) {
    if (!iface) return;

    icmp_header_t icmp;
    icmp.type = 8; // Echo Request
    icmp.code = 0;
    icmp.id = HTONS(0x1234);
    icmp.seq = HTONS(1);
    icmp.checksum = 0;
    icmp.checksum = icmp_checksum(&icmp, sizeof(icmp_header_t));

    ipv4_send_packet(iface, dest_ip, 1, (uint8_t*)&icmp, sizeof(icmp_header_t));
    term_print("[ICMP] Echo Request Sent.\n");
}
