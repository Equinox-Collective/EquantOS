#include "dns.h"
#include "udp.h"
#include "string.h"
#include "stdio.h"
#include "../../equterm/term.h"

uint32_t resolved_ip = 0;
char last_queried_name[128] = {0};

static void dns_format_name(uint8_t *qname, const char *host) {
    int lock = 0;
    char temp[256];

    if (strlen(host) > 250) return;

    strcpy(temp, host);
    strcat(temp, ".");

    for (int i = 0; i < (int)strlen(temp); i++) {
        if (temp[i] == '.') {
            *qname++ = i - lock;
            for (; lock < i; lock++) {
                *qname++ = temp[lock];
            }
            lock++;
        }
    }
    *qname++ = '\0';
}

static uint8_t *dns_skip_name(uint8_t *ptr, uint8_t *end) {
    while (ptr < end) {
        uint8_t b = *ptr;
        if (b == 0) return ptr + 1;
        if ((b & 0xC0) == 0xC0) {
            return (ptr + 2 <= end) ? ptr + 2 : end;
        }
        if (ptr + 1 + b >= end) return end;
        ptr += 1 + b;
    }
    return end;
}

static void dns_callback(uint32_t src_ip, uint16_t src_port, uint8_t *data, uint32_t len) {
    (void)src_ip; (void)src_port;
    if (!data || len < sizeof(dns_header_t)) return;

    dns_header_t *dns = (dns_header_t *)data;
    uint16_t nq  = HTONS(dns->questions);
    uint16_t nan = HTONS(dns->answers);

    if (nan == 0) {
        term_print("[DNS] Error: No answers in response.\n");
        return;
    }

    uint8_t *ptr = data + sizeof(dns_header_t);
    uint8_t *end = data + len;

    for (uint16_t i = 0; i < nq && ptr < end; i++) {
        ptr = dns_skip_name(ptr, end);
        ptr += 4; // Skip QTYPE + QCLASS
    }

    for (uint16_t i = 0; i < nan && ptr + 10 <= end; i++) {
        ptr = dns_skip_name(ptr, end);
        if (ptr + 10 > end) break;

        uint16_t type  = ((uint16_t)ptr[0] << 8) | ptr[1];
        uint16_t rdlen = ((uint16_t)ptr[8] << 8) | ptr[9];
        ptr += 10;

        if (ptr + rdlen > end) break;

        if (type == 1 /* Type A (IPv4) */ && rdlen == 4) {
            resolved_ip = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) |
                          ((uint32_t)ptr[2] << 8)  |  (uint32_t)ptr[3];
            char buf[128];
            snprintf(buf, sizeof(buf), "[DNS] Resolved %s -> %u.%u.%u.%u\n",
                     last_queried_name,
                     (unsigned)((resolved_ip >> 24) & 0xFF),
                     (unsigned)((resolved_ip >> 16) & 0xFF),
                     (unsigned)((resolved_ip >> 8)  & 0xFF),
                     (unsigned)(resolved_ip         & 0xFF));
            term_print(buf);
            return;
        }
        ptr += rdlen;
    }

    term_print("[DNS] No A record found in response.\n");
}

void dns_query(net_interface_t *iface, const char *hostname, uint32_t server_ip) {
    if (!iface) return;

    static bool bound = false;
    if (!bound) {
        udp_bind(53535, dns_callback);
        bound = true;
    }

    resolved_ip = 0;
    strncpy(last_queried_name, hostname, sizeof(last_queried_name) - 1);

    uint8_t buffer[512];
    memset(buffer, 0, sizeof(buffer));

    dns_header_t *dns = (dns_header_t *)buffer;
    dns->id = HTONS(0xBEEF);
    dns->flags = HTONS(0x0100); // Standard recursive query
    dns->questions = HTONS(1);

    uint8_t *qname = buffer + sizeof(dns_header_t);
    dns_format_name(qname, hostname);

    int qname_len = strlen((char *)qname) + 1;
    uint8_t *qinfo = qname + qname_len;
    qinfo[1] = 1; // Type A
    qinfo[3] = 1; // Class IN

    uint32_t total_len = sizeof(dns_header_t) + qname_len + 4;
    udp_send_packet(iface, server_ip, 53535, 53, buffer, total_len);

    char buf[128];
    snprintf(buf, sizeof(buf), "[DNS] Querying %u.%u.%u.%u for '%s'...\n",
             (unsigned)((server_ip >> 24) & 0xFF),
             (unsigned)((server_ip >> 16) & 0xFF),
             (unsigned)((server_ip >> 8)  & 0xFF),
             (unsigned)(server_ip         & 0xFF),
             hostname);
    term_print(buf);
}

uint32_t dns_get_result(const char *hostname) {
    if (strcmp(last_queried_name, hostname) == 0) return resolved_ip;
    return 0;
}