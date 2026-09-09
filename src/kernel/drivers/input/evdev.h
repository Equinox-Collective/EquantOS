// src/kernel/drivers/input/evdev.h - Linux Evdev & Mousedev Core Subsystem
#ifndef EVDEV_H
#define EVDEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../fs/vfs.h"
#include "input.h"

// Linux Input Event Types
#define EV_SYN       0x00
#define EV_KEY       0x01
#define EV_REL       0x02
#define EV_ABS       0x03
#define EV_MSC       0x04

// Synchronization Events
#define SYN_REPORT   0

// Evdev Standard 24-byte struct for Linux x86_64
struct linux_input_event {
    struct {
        int64_t tv_sec;
        int64_t tv_usec;
    } time;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

// Evdev Device IDs
struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

// Evdev ioctl definitions
#define EVIOCGVERSION       0x80044501
#define EVIOCGID            0x80084502
#define EVIOCGNAME(len)     (0x80004506 | ((len) << 16))
#define EVIOCGPHYS(len)     (0x80004507 | ((len) << 16))
#define EVIOCGUNIQ(len)     (0x80004508 | ((len) << 16))
#define EVIOCGPROP(len)     (0x80004509 | ((len) << 16))
#define EVIOCGBIT(ev, len)  (0x80004520 | ((ev) << 8) | ((len) << 16))

#define EVDEV_BUFFER_SIZE   128
#define MOUSE_BUF_SIZE      256

typedef struct evdev_device {
    char name[64];
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;

    struct linux_input_event ring[EVDEV_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;

    struct task *blocked_reader;
    bool is_mouse;
} evdev_device_t;

void evdev_init(void);
void evdev_push_event(uint16_t type, uint16_t code, int32_t value);

bool evdev_mouse_has_data(void);
bool evdev_mouse_can_read(void);

extern vfs_file_operations_t g_evdev_kbd_fops;
extern vfs_file_operations_t g_evdev_mouse_fops;
extern vfs_file_operations_t g_mousedev_fops;

#endif // EVDEV_H