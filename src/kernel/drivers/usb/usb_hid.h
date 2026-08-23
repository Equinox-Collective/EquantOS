// src/kernel/drivers/usb/usb_hid.h - Layer 3: USB HID Device Driver
#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include <stddef.h>

// Parse raw 8-byte USB Boot Protocol HID Keyboard Report and push events to Layer 4
void usb_hid_parse_keyboard_report(const uint8_t *report, size_t len);

// Parse USB HID Mouse Report and push EV_REL events to Layer 4
void usb_hid_parse_mouse_report(const uint8_t *report, size_t len);
void xhci_timer_tick(void);

#endif // USB_HID_H