// src/kernel/drivers/usb/usb.c - Documented USB Request Engine
#include "usb.h"
#include "../serial/serial.h"
#include "stdio.h"

void usb_build_setup_packet(usb_setup_packet_t *pkt, uint8_t req_type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len) {
    if (!pkt) return;
    pkt->request_type = req_type;
    pkt->request      = req;
    pkt->value        = val;
    pkt->index        = idx;
    pkt->length       = len;

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), 
             "[USB-REQ] SetupPkt: Type=0x%02x | Req=0x%02x | Val=0x%04x | Idx=0x%04x | Len=%u\n",
             req_type, req, val, idx, len);
    serial_puts(COM1, log_buf);
}