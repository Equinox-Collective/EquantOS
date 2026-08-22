// src/kernel/drivers/usb/usb.h - Universal USB Core Definitions
#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stddef.h>

// USB Request Types
#define USB_REQ_TYPE_STANDARD (0 << 5)
#define USB_REQ_TYPE_CLASS    (1 << 5)
#define USB_REQ_TYPE_VENDOR   (2 << 5)

// USB Request Recipients
#define USB_REQ_RECIP_DEVICE    0
#define USB_REQ_RECIP_INTERFACE 1
#define USB_REQ_RECIP_ENDPOINT  2

// Standard USB Requests
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

// USB Descriptor Types
#define USB_DESC_DEVICE           0x01
#define USB_DESC_CONFIGURATION    0x02
#define USB_DESC_STRING           0x03
#define USB_DESC_INTERFACE        0x04
#define USB_DESC_ENDPOINT         0x05
#define USB_DESC_HUB              0x29
#define USB_DESC_HID              0x21

// USB Device Classes
#define USB_CLASS_PER_INTERFACE   0x00
#define USB_CLASS_AUDIO           0x01
#define USB_CLASS_HID             0x03
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_CLASS_HUB             0x09

// USB Setup Packet Structure (8 Bytes)
typedef struct {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup_packet_t;

// USB Standard Device Descriptor (18 Bytes)
typedef struct {
    uint8_t  length;
    uint8_t  descriptor_type;
    uint16_t bcd_usb;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet_size0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint8_t  i_manufacturer;
    uint8_t  i_product;
    uint8_t  i_serial_number;
    uint8_t  num_configurations;
} __attribute__((packed)) usb_device_descriptor_t;

// USB Standard Configuration Descriptor (9 Bytes)
typedef struct {
    uint8_t  length;
    uint8_t  descriptor_type;
    uint16_t total_length;
    uint8_t  num_interfaces;
    uint8_t  configuration_value;
    uint8_t  i_configuration;
    uint8_t  attributes;
    uint8_t  max_power;
} __attribute__((packed)) usb_config_descriptor_t;

// USB Standard Interface Descriptor (9 Bytes)
typedef struct {
    uint8_t  length;
    uint8_t  descriptor_type;
    uint8_t  interface_number;
    uint8_t  alternate_setting;
    uint8_t  num_endpoints;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint8_t  i_interface;
} __attribute__((packed)) usb_interface_descriptor_t;

// USB Standard Endpoint Descriptor (7 Bytes)
typedef struct {
    uint8_t  length;
    uint8_t  descriptor_type;
    uint8_t  endpoint_address;
    uint8_t  attributes;
    uint16_t max_packet_size;
    uint8_t  interval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

#endif // USB_H