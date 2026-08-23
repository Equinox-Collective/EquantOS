// src/kernel/drivers/usb/usb.c - Documented USB Request Engine
#include "usb.h"

void usb_build_setup_packet(usb_setup_packet_t *pkt, uint8_t req_type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len) {
    if (!pkt) return;
    pkt->request_type = req_type;
    pkt->request      = req;
    pkt->value        = val;
    pkt->index        = idx;
    pkt->length       = len;
}