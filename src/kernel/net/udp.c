#include "udp.h"
#include "ipv4.h"
#include "net.h"
#include "string.h"
#include "stdio.h"
#include "../core/mem/memory.h"
#include "../../equterm/term.h"

#define MAX_UDP_SOCKETS 64
static udp_socket_t udp_sockets[MAX_UDP_SOCKETS];

void udp_init(void) {
    memset(udp_sockets, 0, sizeof(udp_sockets));
}

bool udp_bind(uint16_t port, udp_callback_t callback) {
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (!udp_sockets[i].active) {
            udp_sockets[i].local_port = port;
            udp_sockets[i].callback = callback;
            udp_sockets[i].active = true;
            return true;
        }
    }
    return false;
}

void handle_udp(net_interface_t *iface, uint8_t *packet, uint32_t ip_hdr_len) {
    (void)iface;
    ipv4_header_t *ip = (ipv4_header_t *)(packet + sizeof(ethernet_header_t));
    udp_header_t *udp = (udp_header_t *)(packet + sizeof(ethernet_header_t) + ip_hdr_len);

    uint16_t src_port = HTONS(udp->src_port);
    uint16_t dest_port = HTONS(udp->dest_port);
    uint16_t len = HTONS(udp->len) - sizeof(udp_header_t);
    uint8_t *payload = (uint8_t *)udp + sizeof(udp_header_t);

    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_sockets[i].active && udp_sockets[i].local_port == dest_port) {
            if (udp_sockets[i].callback) {
                udp_sockets[i].callback(HTONL(ip->src_ip), src_port, payload, len);
            }
            return;
        }
    }
}

void udp_send_packet(net_interface_t *iface, uint32_t dest_ip,
                     uint16_t src_port, uint16_t dest_port, uint8_t *data,
                     uint32_t len) {
    if (!iface) return;

    uint32_t total_len = sizeof(udp_header_t) + len;
    uint8_t *buffer = (uint8_t *)kmalloc(total_len);
    if (!buffer) return;

    udp_header_t *udp = (udp_header_t *)buffer;
    udp->src_port = HTONS(src_port);
    udp->dest_port = HTONS(dest_port);
    udp->len = HTONS(total_len);
    udp->checksum = 0;

    memcpy(buffer + sizeof(udp_header_t), data, len);
    ipv4_send_packet(iface, dest_ip, 17, buffer, total_len);
    kfree(buffer);
}

static void ntp_callback(uint32_t src_ip, uint16_t src_port, uint8_t *data, uint32_t len) {
    (void)src_ip;
    (void)src_port;
    if (len < 48) return;

    uint32_t ntp_sec = ((uint32_t)data[40] << 24) | ((uint32_t)data[41] << 16) |
                       ((uint32_t)data[42] << 8)  |  (uint32_t)data[43];

    if (ntp_sec > 0) {
        uint32_t unix_timestamp = ntp_sec - 2208988800U;
        char buf[128];
        snprintf(buf, sizeof(buf), "[NTP] Time sync via UDP: Unix %u\n", (unsigned int)unix_timestamp);
        term_print(buf);
    }
}

void send_ntp_request(net_interface_t *iface) {
    if (!iface) return;

    static bool bound = false;
    if (!bound) {
        udp_bind(1234, ntp_callback);
        bound = true;
    }

    uint8_t ntp_payload[48];
    memset(ntp_payload, 0, 48);
    ntp_payload[0] = 0x23;

    // Send to Cloudflare NTP (162.159.200.1 = 0xA29FC801)
    udp_send_packet(iface, 0xA29FC801, 1234, 123, ntp_payload, 48);
}