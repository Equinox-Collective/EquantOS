// src/kernel/net/dhcp.c
#include "dhcp.h"
#include "udp.h"
#include "string.h"
#include "stdio.h"
#include "../misc/timer.h"
#include "../drivers/net/rtl8139.h"

static uint32_t dhcp_xid = 0x55AA1234;
static bool dhcp_completed = false;

static void dhcp_handle_response(uint32_t src_ip, uint16_t src_port, uint8_t *data, uint32_t len) {
    (void)src_ip; (void)src_port;
    if (len < sizeof(dhcp_packet_t)) return;

    dhcp_packet_t *pkt = (dhcp_packet_t *)data;
    if (pkt->magic != 0x63538263 || pkt->xid != dhcp_xid) return;

    net_interface_t *iface = net_get_primary_interface();
    if (!iface) return;

    uint8_t msg_type = 0;
    uint32_t server_id = 0;
    uint32_t subnet_mask = 0;
    uint32_t gateway = 0;
    uint32_t dns = 0;

    uint8_t *opt = pkt->options;
    while (*opt != 0xFF && (opt - data) < (int)len) {
        uint8_t code = *opt++;
        if (code == 0) continue;
        uint8_t opt_len = *opt++;

        if (code == 53) msg_type = *opt;
        else if (code == 1 && opt_len == 4) subnet_mask = *(uint32_t *)opt;
        else if (code == 3 && opt_len >= 4) gateway = *(uint32_t *)opt;
        else if (code == 6 && opt_len >= 4) dns = *(uint32_t *)opt;
        else if (code == 54 && opt_len == 4) server_id = *(uint32_t *)opt;

        opt += opt_len;
    }

    if (msg_type == DHCP_OFFER) {
        // Send DHCP Request
        dhcp_packet_t req;
        memset(&req, 0, sizeof(dhcp_packet_t));
        req.op = 1;
        req.htype = 1;
        req.hlen = 6;
        req.xid = dhcp_xid;
        req.flags = HTONS(0x8000); // Broadcast
        memcpy(req.chaddr, iface->mac, 6);
        req.magic = 0x63538263;

        uint8_t *ropt = req.options;
        *ropt++ = 53; *ropt++ = 1; *ropt++ = DHCP_REQUEST;
        *ropt++ = 50; *ropt++ = 4; memcpy(ropt, &pkt->yiaddr, 4); ropt += 4;
        *ropt++ = 54; *ropt++ = 4; memcpy(ropt, &server_id, 4); ropt += 4;
        *ropt++ = 0xFF;

        udp_send_packet(iface, 0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, (uint8_t *)&req, sizeof(req));
    } else if (msg_type == DHCP_ACK) {
        iface->ip = HTONL(pkt->yiaddr);
        if (subnet_mask) iface->subnet_mask = HTONL(subnet_mask);
        if (gateway) iface->gateway_ip = HTONL(gateway);

        char ip_str[64];
        snprintf(ip_str, sizeof(ip_str), "[DHCP] Configured IP: %u.%u.%u.%u via gateway %u.%u.%u.%u\n",
                 (iface->ip >> 24) & 0xFF, (iface->ip >> 16) & 0xFF,
                 (iface->ip >> 8) & 0xFF, iface->ip & 0xFF,
                 (iface->gateway_ip >> 24) & 0xFF, (iface->gateway_ip >> 16) & 0xFF,
                 (iface->gateway_ip >> 8) & 0xFF, iface->gateway_ip & 0xFF);
        serial_puts(COM1, ip_str);

        dhcp_completed = true;
    }
}

bool dhcp_discover(net_interface_t *iface) {
    if (!iface) return false;

    udp_bind(DHCP_CLIENT_PORT, dhcp_handle_response);
    dhcp_completed = false;

    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = 1;
    pkt.htype = 1;
    pkt.hlen = 6;
    pkt.xid = dhcp_xid;
    pkt.flags = HTONS(0x8000);
    memcpy(pkt.chaddr, iface->mac, 6);
    pkt.magic = 0x63538263;

    uint8_t *opt = pkt.options;
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_DISCOVER;
    *opt++ = 55; *opt++ = 3; *opt++ = 1; *opt++ = 3; *opt++ = 6; // Subnet, Router, DNS
    *opt++ = 0xFF;

    udp_send_packet(iface, 0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, (uint8_t *)&pkt, sizeof(pkt));

    uint32_t timeout = tick + 100; // 400ms timeout
    while (tick < timeout && !dhcp_completed) {
        rtl8139_poll();
        __asm__ volatile("pause");
    }

    return dhcp_completed;
}