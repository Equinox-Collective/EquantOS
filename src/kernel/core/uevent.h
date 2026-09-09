// src/kernel/core/uevent.h - Linux Uevent Netlink & Devtmpfs Subsystem
#ifndef UEVENT_H
#define UEVENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NETLINK_KOBJECT_UEVENT 15

typedef enum {
    KOBJ_ADD,
    KOBJ_REMOVE,
    KOBJ_CHANGE
} kobject_action_t;

void uevent_init(void);
void uevent_broadcast(kobject_action_t action, const char *subsystem, const char *devname, int major, int minor);

#endif // UEVENT_H